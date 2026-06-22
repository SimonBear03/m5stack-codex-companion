from __future__ import annotations

import abc
import asyncio
import hashlib
import hmac
import logging
import secrets
from dataclasses import dataclass, field
from typing import Any, Callable

from .pairing import PairedDevice, PairingStore, utc_now_iso
from .protocol import (
    DEFAULT_BLE_CHUNK_SIZE,
    NUS_RX_UUID,
    NUS_TX_UUID,
    JsonLineDecoder,
    Snapshot,
    chunk_bytes,
    encode_json_line,
)

LOGGER = logging.getLogger(__name__)
DEFAULT_DEVICE_PREFIXES = "Codex-S3-,Codex-CP-"
AUTH_VERSION = 1
DEFAULT_BLE_WRITE_CHUNK_DELAY_SECONDS = 0.02
DEFAULT_BLE_WRITE_TIMEOUT_SECONDS = 4.0
DEFAULT_BLE_WRITE_WITH_RESPONSE = False
BOARD_DEVICE_PREFIXES = {
    "sticks3": "Codex-S3-",
    "cardputer_adv": "Codex-CP-",
}


def _device_prefixes(device_prefix: str) -> tuple[str, ...]:
    return tuple(prefix.strip() for prefix in device_prefix.split(",") if prefix.strip())


def advertisement_names(dev: Any, adv: Any) -> tuple[str, ...]:
    """Return advertised names exposed by Bleak without service-only fallbacks."""
    names: list[str] = []
    for raw_name in (getattr(dev, "name", None), getattr(adv, "local_name", None)):
        name = str(raw_name or "").strip()
        if name and name not in names:
            names.append(name)
    return tuple(names)


def advertisement_matches_prefix(dev: Any, adv: Any, device_prefix: str) -> bool:
    """Return true only for explicitly named Codex companion advertisements."""
    return any(name.startswith(prefix) for prefix in _device_prefixes(device_prefix) for name in advertisement_names(dev, adv))


def _paired_device_prefix(paired: PairedDevice) -> str | None:
    board = (paired.board or "").strip()
    if board in BOARD_DEVICE_PREFIXES:
        return BOARD_DEVICE_PREFIXES[board]
    device_id = paired.device_id or ""
    for known_board, prefix in BOARD_DEVICE_PREFIXES.items():
        if device_id.startswith(f"{known_board}-"):
            return prefix
    return None


def _paired_device_name_from_id(paired: PairedDevice) -> str | None:
    prefix = _paired_device_prefix(paired)
    if not prefix:
        return None
    parts = (paired.device_id or "").split("-", 2)
    if len(parts) < 2 or not parts[1]:
        return None
    return f"{prefix}{parts[1]}"


def advertisement_matches_paired_device(dev: Any, adv: Any, paired: PairedDevice, device_prefix: str) -> bool:
    """Return true when an advertisement can belong to this paired device."""
    names = advertisement_names(dev, adv)
    if not names:
        return False

    expected_names = [name for name in (paired.name, _paired_device_name_from_id(paired)) if name]
    if expected_names:
        return any(name == expected_name for name in names for expected_name in expected_names)

    paired_prefix = _paired_device_prefix(paired)
    if paired_prefix:
        return any(name.startswith(paired_prefix) for name in names)

    return advertisement_matches_prefix(dev, adv, device_prefix)


def auth_message(*, device_id: str, device_nonce: str, host_nonce: str, host_id: str) -> bytes:
    return f"auth:v{AUTH_VERSION}:{device_id}:{device_nonce}:{host_nonce}:{host_id}".encode("utf-8")


def auth_digest(secret_hex: str, *, device_id: str, device_nonce: str, host_nonce: str, host_id: str) -> str:
    secret = bytes.fromhex(secret_hex)
    return hmac.new(secret, auth_message(device_id=device_id, device_nonce=device_nonce, host_nonce=host_nonce, host_id=host_id), hashlib.sha256).hexdigest()


class StickS3Device(abc.ABC):
    @abc.abstractmethod
    async def connect(self) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    async def close(self) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    async def send_snapshot(self, snapshot: Snapshot) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    async def wait_for_interaction(self, interaction_id: str, timeout: float) -> dict[str, Any]:
        raise NotImplementedError

    @abc.abstractmethod
    async def wait_for_control(self) -> dict[str, Any]:
        raise NotImplementedError

    @abc.abstractmethod
    async def request_status(self) -> None:
        raise NotImplementedError

    def is_connected(self) -> bool:
        return False


class BleStickS3Device(StickS3Device):
    def __init__(
        self,
        *,
        device_prefix: str = DEFAULT_DEVICE_PREFIXES,
        address: str | None = None,
        scan_timeout: float = 10.0,
        chunk_size: int = DEFAULT_BLE_CHUNK_SIZE,
        write_chunk_delay: float = DEFAULT_BLE_WRITE_CHUNK_DELAY_SECONDS,
        write_timeout: float = DEFAULT_BLE_WRITE_TIMEOUT_SECONDS,
        write_with_response: bool = DEFAULT_BLE_WRITE_WITH_RESPONSE,
    ) -> None:
        self.device_prefix = device_prefix
        self.address = address
        self.scan_timeout = scan_timeout
        self.chunk_size = chunk_size
        self.write_chunk_delay = max(0.0, write_chunk_delay)
        self.write_timeout = max(1.0, write_timeout)
        self.write_with_response = write_with_response
        self._client: Any | None = None
        self._target: Any | None = None
        self._decoder = JsonLineDecoder()
        self._ack_queues: dict[str, asyncio.Queue[dict[str, Any]]] = {}
        self._interaction_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._control_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self.last_status_ack: dict[str, Any] | None = None
        self.always_connected = False
        self.on_disconnect: Callable[[], None] | None = None
        self._closing = False

    def _reset_session_state(self) -> None:
        self._decoder = JsonLineDecoder()
        self._ack_queues.clear()
        self._interaction_queue = asyncio.Queue()
        self._control_queue = asyncio.Queue()

    @staticmethod
    def _drain_queue(queue: asyncio.Queue[dict[str, Any]]) -> None:
        while True:
            try:
                queue.get_nowait()
            except asyncio.QueueEmpty:
                return

    async def connect(self) -> None:
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError as exc:
            raise RuntimeError("Install bridge dependencies with `python -m pip install -e .`") from exc

        target = self.address
        if not target:
            device = await BleakScanner.find_device_by_filter(
                lambda dev, adv: advertisement_matches_prefix(dev, adv, self.device_prefix),
                timeout=self.scan_timeout,
            )
            if device is None:
                raise RuntimeError(f"No Codex companion device found with prefix {self.device_prefix!r}")
            target = device

        await self.connect_target(target)

    async def connect_target(self, target: Any) -> None:
        from bleak import BleakClient

        self._reset_session_state()
        address = getattr(target, "address", target)
        timeout = min(max(3.0, float(self.scan_timeout)), 6.0)
        client = BleakClient(target, timeout=timeout, disconnected_callback=self._handle_disconnect)
        try:
            await asyncio.wait_for(client.connect(), timeout=timeout + 2.0)
            await asyncio.wait_for(client.start_notify(NUS_TX_UUID, self._handle_notification), timeout=5.0)
        except Exception:
            try:
                if getattr(client, "is_connected", False):
                    await client.disconnect()
            except Exception as exc:
                LOGGER.debug("Ignoring BLE connect cleanup error: %s", exc)
            raise
        self._client = client
        self._target = target
        if isinstance(address, str):
            self.address = address

    @property
    def connected_address(self) -> str | None:
        if self.address:
            return self.address
        target = self._target
        address = getattr(target, "address", None)
        return str(address) if address else None

    async def close(self) -> None:
        if self._client is None:
            return
        self._closing = True
        try:
            try:
                await self._client.stop_notify(NUS_TX_UUID)
            except Exception as exc:
                LOGGER.debug("Ignoring BLE stop_notify cleanup error: %s", exc)
            try:
                await self._client.disconnect()
            except Exception as exc:
                LOGGER.debug("Ignoring BLE disconnect cleanup error: %s", exc)
        finally:
            self._client = None
            self._target = None
            self._reset_session_state()
            self._closing = False

    def _handle_disconnect(self, _client: Any) -> None:
        if self._closing:
            return
        address = self.connected_address or "unknown"
        self._client = None
        self._target = None
        self._reset_session_state()
        LOGGER.info("BLE link disconnected: %s", address)
        if self.on_disconnect is not None:
            self.on_disconnect()

    async def send_snapshot(self, snapshot: Snapshot) -> None:
        await self.send_json(snapshot.to_wire())

    async def send_json(self, data: dict[str, Any]) -> None:
        if self._client is None:
            raise RuntimeError("StickS3 BLE client is not connected")
        chunks = chunk_bytes(encode_json_line(data), self.chunk_size)
        for index, chunk in enumerate(chunks):
            await asyncio.wait_for(
                self._client.write_gatt_char(NUS_RX_UUID, chunk, response=self.write_with_response),
                timeout=self.write_timeout,
            )
            if index + 1 < len(chunks) and self.write_chunk_delay > 0:
                await asyncio.sleep(self.write_chunk_delay)

    async def send_command(self, command: str, data: dict[str, Any] | None = None, *, timeout: float = 5.0) -> dict[str, Any]:
        payload = {"cmd": command}
        if data:
            payload.update(data)
        queue = self._ack_queues.setdefault(command, asyncio.Queue())
        self._drain_queue(queue)
        await self.send_json(payload)
        while True:
            message = await asyncio.wait_for(queue.get(), timeout=timeout)
            if message.get("ack") == command:
                return message

    async def hello(self, *, timeout: float = 5.0) -> dict[str, Any]:
        ack = await self.send_command("hello", {"nonce": secrets.token_hex(16)}, timeout=timeout)
        if not ack.get("ok"):
            raise RuntimeError(str(ack.get("error") or "hello rejected"))
        data = ack.get("data")
        if not isinstance(data, dict):
            raise RuntimeError("hello response did not include data")
        return data

    async def authenticate(self, paired: PairedDevice, *, host_id: str, timeout: float = 5.0) -> dict[str, Any]:
        hello = await self.hello(timeout=timeout)
        device_id = str(hello.get("device_id") or "")
        if device_id != paired.device_id:
            raise RuntimeError(f"connected to {device_id or 'unknown device'}, expected {paired.device_id}")
        device_nonce = str(hello.get("nonce") or "")
        if not device_nonce:
            raise RuntimeError("device hello did not include auth nonce")
        host_nonce = secrets.token_hex(16)
        mac = auth_digest(
            paired.secret,
            device_id=paired.device_id,
            device_nonce=device_nonce,
            host_nonce=host_nonce,
            host_id=host_id,
        )
        ack = await self.send_command(
            "auth",
            {"host_id": host_id, "nonce": host_nonce, "mac": mac},
            timeout=timeout,
        )
        if not ack.get("ok"):
            raise RuntimeError(str(ack.get("error") or "auth rejected"))
        return hello

    async def pair(
        self,
        *,
        host_id: str,
        label: str | None = None,
        timeout: float = 65.0,
    ) -> PairedDevice:
        hello = await self.hello()
        if hello.get("paired"):
            raise RuntimeError("device is already paired; unpair it first")
        device_id = str(hello.get("device_id") or "")
        if not device_id:
            raise RuntimeError("device hello did not include device_id")
        secret = secrets.token_hex(32)
        begin = await self.send_command(
            "pair_begin",
            {"host_id": host_id, "secret": secret, "label": label or ""},
            timeout=5.0,
        )
        if not begin.get("ok"):
            raise RuntimeError(str(begin.get("error") or "pair_begin rejected"))
        code = ""
        if isinstance(begin.get("data"), dict):
            code = str(begin["data"].get("code") or "")
        LOGGER.info("Confirm pairing code %s on device %s", code or "shown", device_id)

        deadline = asyncio.get_running_loop().time() + timeout
        last_error = "pairing timed out"
        while asyncio.get_running_loop().time() < deadline:
            commit = await self.send_command("pair_commit", {"code": code}, timeout=5.0)
            if commit.get("ok"):
                data = commit.get("data") if isinstance(commit.get("data"), dict) else {}
                return PairedDevice(
                    device_id=device_id,
                    label=label or str(data.get("name") or hello.get("name") or device_id),
                    board=str(data.get("board") or hello.get("board") or "unknown"),
                    address=self.connected_address,
                    name=str(data.get("name") or hello.get("name") or ""),
                    secret=secret,
                    enabled=True,
                    paired_at=utc_now_iso(),
                    last_seen=utc_now_iso(),
                )
            last_error = str(commit.get("error") or last_error)
            await asyncio.sleep(1.0)
        raise RuntimeError(last_error)

    async def request_status(self) -> None:
        ack = await self.send_command("status", timeout=5.0)
        data = ack.get("data")
        if ack.get("ok") and isinstance(data, dict):
            self.last_status_ack = data

    def is_connected(self) -> bool:
        return bool(self._client is not None and getattr(self._client, "is_connected", False))

    async def wait_for_interaction(self, interaction_id: str, timeout: float) -> dict[str, Any]:
        while True:
            message = await asyncio.wait_for(self._interaction_queue.get(), timeout=timeout)
            if message.get("cmd") not in {"permission", "interaction"}:
                continue
            if str(message.get("id")) != interaction_id:
                LOGGER.debug("Ignoring interaction for stale id %s", message.get("id"))
                continue
            if message.get("cmd") == "permission":
                decision = message.get("decision")
                if decision in {"once", "session", "deny", "cancel"}:
                    return {"cmd": "interaction", "id": interaction_id, "action": "submit", "value": str(decision)}
                continue
            return message

    async def wait_for_control(self) -> dict[str, Any]:
        return await self._control_queue.get()

    def _handle_notification(self, _: int, data: bytearray) -> None:
        for message in self._decoder.feed(bytes(data)):
            LOGGER.debug("StickS3 -> host: %s", message)
            ack = message.get("ack")
            if isinstance(ack, str):
                self._ack_queues.setdefault(ack, asyncio.Queue()).put_nowait(message)
            if message.get("cmd") in {"permission", "interaction"}:
                self._interaction_queue.put_nowait(message)
            elif message.get("cmd") == "control":
                self._control_queue.put_nowait(message)
            elif message.get("ack") == "status" and message.get("ok"):
                data = message.get("data")
                if isinstance(data, dict):
                    self.last_status_ack = data


@dataclass
class CompanionTarget:
    paired: PairedDevice
    client: BleStickS3Device
    state: str = "disconnected"
    error: str | None = None
    last_status_ack: dict[str, Any] | None = None
    last_wire_payload: dict[str, Any] | None = None
    last_activity_seq_sent: int = 0
    task: asyncio.Task[None] | None = None
    last_seen: str | None = None
    initial_sync_needed: bool = False
    initial_full_sync_pending: bool = False
    last_scan_attempt_monotonic: float = 0.0
    lock: asyncio.Lock = field(default_factory=asyncio.Lock)

    @property
    def device_id(self) -> str:
        return self.paired.device_id

    @property
    def label(self) -> str:
        return self.paired.label or self.paired.device_id

    def is_connected(self) -> bool:
        return self.state == "connected" and self.client.is_connected()

    async def send_snapshot(self, snapshot: Snapshot) -> None:
        await self.client.send_snapshot(snapshot)

    async def request_status(self) -> None:
        await self.client.request_status()
        self.last_status_ack = self.client.last_status_ack

    async def close(self) -> None:
        await self.client.close()

    def status(self) -> dict[str, Any]:
        return {
            "device_id": self.device_id,
            "label": self.label,
            "board": self.paired.board,
            "name": self.paired.name,
            "address": self.paired.address,
            "state": self.state,
            "error": self.error,
            "last_seen": self.last_seen or self.paired.last_seen,
            "detail_mode": self.detail_mode(),
        }

    def detail_mode(self) -> int | None:
        if not isinstance(self.last_status_ack, dict):
            return None
        settings = self.last_status_ack.get("settings")
        if not isinstance(settings, dict):
            return None
        detail = settings.get("detail")
        return detail if isinstance(detail, int) else None


class BleCompanionFleet(StickS3Device):
    def __init__(
        self,
        *,
        store: PairingStore,
        device_prefix: str = DEFAULT_DEVICE_PREFIXES,
        scan_timeout: float = 10.0,
        chunk_size: int = DEFAULT_BLE_CHUNK_SIZE,
        write_chunk_delay: float = DEFAULT_BLE_WRITE_CHUNK_DELAY_SECONDS,
        write_timeout: float = DEFAULT_BLE_WRITE_TIMEOUT_SECONDS,
        write_with_response: bool = DEFAULT_BLE_WRITE_WITH_RESPONSE,
        reconnect_delay: float = 1.0,
        max_reconnect_delay: float = 3.0,
        connected_reconnect_delay: float = 20.0,
    ) -> None:
        self.store = store
        self.device_prefix = device_prefix
        self.scan_timeout = scan_timeout
        self.chunk_size = chunk_size
        self.write_chunk_delay = write_chunk_delay
        self.write_timeout = write_timeout
        self.write_with_response = write_with_response
        self.reconnect_delay = max(0.5, reconnect_delay)
        self.max_reconnect_delay = max(self.reconnect_delay, max_reconnect_delay)
        self.connected_reconnect_delay = max(self.max_reconnect_delay, connected_reconnect_delay)
        self.host_id = store.host_id()
        paired_devices = sorted(
            store.devices(enabled_only=True),
            key=lambda device: device.last_seen or device.paired_at or "",
            reverse=True,
        )
        self.targets: list[CompanionTarget] = [
            CompanionTarget(
                paired=device,
                client=BleStickS3Device(
                    device_prefix=device_prefix,
                    address=device.address,
                    scan_timeout=scan_timeout,
                    chunk_size=chunk_size,
                    write_chunk_delay=write_chunk_delay,
                    write_timeout=write_timeout,
                    write_with_response=write_with_response,
                ),
            )
            for device in paired_devices
        ]
        for target in self.targets:
            target.client.on_disconnect = lambda target=target: self._mark_target_link_closed(target)
        self.always_connected = False
        self._started = False
        self._scan_lock = asyncio.Lock()
        self._connect_lock = asyncio.Lock()
        self._control_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()

    async def connect(self) -> None:
        if self._started:
            return
        self._started = True
        for target in self.targets:
            target.task = asyncio.create_task(self._connection_loop(target))

    async def _connection_loop(self, target: CompanionTarget) -> None:
        delay = self.reconnect_delay
        while True:
            if target.is_connected():
                await asyncio.sleep(1.0)
                continue
            if target.state == "connected":
                LOGGER.info("Companion %s link closed; reconnecting", target.label)
            target.state = "scanning"
            target.error = None
            try:
                await self._connect_target(target)
            except asyncio.CancelledError:
                raise
            except Exception as exc:
                other_connected = self._has_other_connected_target(target)
                target.state = "retry_deferred" if other_connected else "disconnected"
                target.error = str(exc)
                LOGGER.debug("Companion %s connection attempt failed: %s", target.label, exc)
                await target.close()
                if other_connected:
                    await asyncio.sleep(self.connected_reconnect_delay)
                    delay = self.reconnect_delay
                else:
                    await asyncio.sleep(delay)
                    delay = min(self.max_reconnect_delay, max(delay * 1.6, delay + 1.0))
                continue

            if not target.client.is_connected():
                target.state = "disconnected"
                target.error = "BLE link closed before session became ready"
                await target.close()
                await asyncio.sleep(delay)
                delay = min(self.max_reconnect_delay, max(delay * 1.6, delay + 1.0))
                continue

            target.state = "connected"
            target.error = None
            target.last_seen = utc_now_iso()
            target.initial_sync_needed = True
            target.initial_full_sync_pending = False
            self.store.mark_seen(
                target.device_id,
                address=target.client.connected_address,
                name=target.paired.name,
                board=target.paired.board,
            )
            delay = self.reconnect_delay

    def _mark_target_link_closed(self, target: CompanionTarget) -> None:
        target.state = "disconnected"
        target.error = "BLE link closed"
        target.initial_sync_needed = False
        target.initial_full_sync_pending = False

    async def _connect_target(self, target: CompanionTarget) -> None:
        await target.close()
        other_connected = self._has_other_connected_target(target)
        if target.paired.address:
            try:
                hello = await self._connect_and_auth_target(target, target.paired.address)
                self._apply_hello(target, hello)
                return
            except Exception as exc:
                await target.close()
                if other_connected:
                    raise RuntimeError(
                        f"direct connect failed for paired device {target.device_id}; "
                        "scan fallback deferred while another companion is connected"
                    ) from exc

        if other_connected:
            raise RuntimeError(f"scan fallback deferred for paired device {target.device_id} while another companion is connected")

        async with self._scan_lock:
            try:
                from bleak import BleakScanner
            except ImportError as exc:
                raise RuntimeError("Install bridge dependencies with `python -m pip install -e .`") from exc
            scan_timeout = self.scan_timeout
            target.last_scan_attempt_monotonic = asyncio.get_running_loop().time()
            devices = await BleakScanner.discover(timeout=scan_timeout, return_adv=True)

        candidates = []
        if isinstance(devices, dict):
            iterable = devices.values()
        else:
            iterable = devices
        for item in iterable:
            if isinstance(item, tuple) and len(item) == 2:
                dev, adv = item
            else:
                dev, adv = item, None
            if advertisement_matches_paired_device(dev, adv, target.paired, self.device_prefix):
                candidates.append(dev)

        last_error = f"paired device {target.device_id} not found"
        for dev in candidates:
            try:
                hello = await self._connect_and_auth_target(target, dev)
            except Exception as exc:
                last_error = str(exc)
                await target.close()
                continue
            self._apply_hello(target, hello)
            return
        raise RuntimeError(last_error)

    async def _connect_and_auth_target(self, target: CompanionTarget, ble_target: Any, *, serialize: bool = True) -> dict[str, Any]:
        if serialize:
            async with self._connect_lock:
                await target.client.connect_target(ble_target)
                hello = await target.client.authenticate(target.paired, host_id=self.host_id)
        else:
            await target.client.connect_target(ble_target)
            hello = await target.client.authenticate(target.paired, host_id=self.host_id)
        LOGGER.info("Connected to companion %s (%s)", target.label, target.device_id)
        return hello

    def _apply_hello(self, target: CompanionTarget, hello: dict[str, Any]) -> None:
        target.paired.address = target.client.connected_address or target.paired.address
        target.paired.name = str(hello.get("name") or target.paired.name)
        target.paired.board = str(hello.get("board") or target.paired.board)

    def _has_other_connected_target(self, target: CompanionTarget) -> bool:
        return any(other is not target and other.is_connected() for other in self.targets)

    def is_connected(self) -> bool:
        return any(target.is_connected() for target in self.targets)

    def connected_targets(self) -> list[CompanionTarget]:
        return [target for target in self.targets if target.is_connected()]

    def all_targets(self) -> list[CompanionTarget]:
        return list(self.targets)

    def needs_initial_sync(self) -> bool:
        return any(target.is_connected() and (target.initial_sync_needed or target.initial_full_sync_pending) for target in self.targets)

    async def close(self) -> None:
        for target in self.targets:
            if target.task is not None:
                target.task.cancel()
        for target in self.targets:
            if target.task is not None:
                try:
                    await target.task
                except asyncio.CancelledError:
                    pass
                target.task = None
            await target.close()
            if target.state != "connected":
                continue
            target.state = "disconnected"

    async def send_snapshot(self, snapshot: Snapshot) -> None:
        for target in self.connected_targets():
            await target.send_snapshot(snapshot)

    async def request_status(self) -> None:
        for target in self.connected_targets():
            await target.request_status()

    async def wait_for_interaction(self, interaction_id: str, timeout: float) -> dict[str, Any]:
        future: asyncio.Future[dict[str, Any]] = asyncio.Future()
        return await asyncio.wait_for(future, timeout=timeout)

    async def wait_for_control(self) -> dict[str, Any]:
        return await self._control_queue.get()

    def aggregate_state(self) -> str:
        if self.connected_targets():
            return "connected"
        if any(target.state == "scanning" for target in self.targets):
            return "scanning"
        return "disconnected"

    def status_summary(self) -> dict[str, Any]:
        connected = len(self.connected_targets())
        devices = [target.status() for target in self.targets]
        first_error = next((target.error for target in self.targets if target.error), None)
        return {
            "state": self.aggregate_state(),
            "device_count": len(self.targets),
            "connected_device_count": connected,
            "device_error": None if connected else first_error,
            "devices": devices,
        }


class FakeStickS3Device(StickS3Device):
    def __init__(self, *, auto_decision: str = "deny") -> None:
        if auto_decision not in {"once", "session", "deny", "cancel"}:
            raise ValueError("auto_decision must be one of: once, session, deny, cancel")
        self.auto_decision = auto_decision
        self.snapshots: list[Snapshot] = []
        self.last_status_ack: dict[str, Any] | None = None
        self.always_connected = True

    async def connect(self) -> None:
        LOGGER.info("Using fake StickS3 device with auto decision: %s", self.auto_decision)

    async def close(self) -> None:
        return None

    async def send_snapshot(self, snapshot: Snapshot) -> None:
        self.snapshots.append(snapshot)
        LOGGER.info("Snapshot: %s", snapshot.to_wire())

    async def wait_for_interaction(self, interaction_id: str, timeout: float) -> dict[str, Any]:
        await asyncio.sleep(0)
        LOGGER.info("Auto-answering interaction %s with %s", interaction_id, self.auto_decision)
        return {"cmd": "interaction", "id": interaction_id, "action": "submit", "value": self.auto_decision}

    async def wait_for_control(self) -> dict[str, Any]:
        future: asyncio.Future[dict[str, Any]] = asyncio.Future()
        return await future

    async def request_status(self) -> None:
        return None

    def is_connected(self) -> bool:
        return self.always_connected
