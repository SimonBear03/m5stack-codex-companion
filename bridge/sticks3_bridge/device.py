from __future__ import annotations

import abc
import asyncio
import logging
from typing import Any

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


def advertisement_matches_prefix(dev: Any, adv: Any, device_prefix: str) -> bool:
    """Return true only for explicitly named StickS3 companion advertisements."""
    return bool(
        (getattr(dev, "name", None) or "").startswith(device_prefix)
        or (getattr(adv, "local_name", None) or "").startswith(device_prefix)
    )


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
        device_prefix: str = "Codex-S3-",
        address: str | None = None,
        scan_timeout: float = 10.0,
        chunk_size: int = DEFAULT_BLE_CHUNK_SIZE,
    ) -> None:
        self.device_prefix = device_prefix
        self.address = address
        self.scan_timeout = scan_timeout
        self.chunk_size = chunk_size
        self._client: Any | None = None
        self._decoder = JsonLineDecoder()
        self._interaction_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self._control_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
        self.last_status_ack: dict[str, Any] | None = None
        self.always_connected = False

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
                raise RuntimeError(f"No StickS3 device found with prefix {self.device_prefix!r}")
            target = device.address

        client = BleakClient(target)
        await client.connect()
        await client.start_notify(NUS_TX_UUID, self._handle_notification)
        self._client = client

    async def close(self) -> None:
        if self._client is None:
            return
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

    async def send_snapshot(self, snapshot: Snapshot) -> None:
        await self.send_json(snapshot.to_wire())

    async def send_json(self, data: dict[str, Any]) -> None:
        if self._client is None:
            raise RuntimeError("StickS3 BLE client is not connected")
        for chunk in chunk_bytes(encode_json_line(data), self.chunk_size):
            await self._client.write_gatt_char(NUS_RX_UUID, chunk, response=True)
            await asyncio.sleep(0.004)

    async def request_status(self) -> None:
        await self.send_json({"cmd": "status"})

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
            if message.get("cmd") in {"permission", "interaction"}:
                self._interaction_queue.put_nowait(message)
            elif message.get("cmd") == "control":
                self._control_queue.put_nowait(message)
            elif message.get("ack") == "status" and message.get("ok"):
                data = message.get("data")
                if isinstance(data, dict):
                    self.last_status_ack = data


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
