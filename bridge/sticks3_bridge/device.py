from __future__ import annotations

import abc
import asyncio
import logging
from typing import Any

from .protocol import (
    DEFAULT_BLE_CHUNK_SIZE,
    NUS_RX_UUID,
    NUS_SERVICE_UUID,
    NUS_TX_UUID,
    JsonLineDecoder,
    Snapshot,
    chunk_bytes,
    encode_json_line,
)

LOGGER = logging.getLogger(__name__)


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
    async def wait_for_permission(self, prompt_id: str, timeout: float) -> str:
        raise NotImplementedError


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
        self._permission_queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()

    async def connect(self) -> None:
        try:
            from bleak import BleakClient, BleakScanner
        except ImportError as exc:
            raise RuntimeError("Install bridge dependencies with `python -m pip install -e .`") from exc

        target = self.address
        if not target:
            device = await BleakScanner.find_device_by_filter(
                lambda dev, adv: (
                    (dev.name or "").startswith(self.device_prefix)
                    or (adv.local_name or "").startswith(self.device_prefix)
                    or NUS_SERVICE_UUID.lower() in [uuid.lower() for uuid in adv.service_uuids]
                ),
                timeout=self.scan_timeout,
            )
            if device is None:
                raise RuntimeError(f"No StickS3 device found with prefix {self.device_prefix!r}")
            target = device.address

        client = BleakClient(target)
        await client.connect()
        await client.start_notify(NUS_TX_UUID, self._handle_notification)
        self._client = client
        await self.send_json({"cmd": "status"})

    async def close(self) -> None:
        if self._client is None:
            return
        try:
            await self._client.stop_notify(NUS_TX_UUID)
        finally:
            await self._client.disconnect()
            self._client = None

    async def send_snapshot(self, snapshot: Snapshot) -> None:
        await self.send_json(snapshot.to_wire())

    async def send_json(self, data: dict[str, Any]) -> None:
        if self._client is None:
            raise RuntimeError("StickS3 BLE client is not connected")
        for chunk in chunk_bytes(encode_json_line(data), self.chunk_size):
            await self._client.write_gatt_char(NUS_RX_UUID, chunk, response=False)
            await asyncio.sleep(0.008)

    async def wait_for_permission(self, prompt_id: str, timeout: float) -> str:
        while True:
            message = await asyncio.wait_for(self._permission_queue.get(), timeout=timeout)
            if message.get("cmd") != "permission":
                continue
            if str(message.get("id")) != prompt_id:
                LOGGER.debug("Ignoring permission for stale prompt id %s", message.get("id"))
                continue
            decision = message.get("decision")
            if decision in {"once", "deny"}:
                return str(decision)

    def _handle_notification(self, _: int, data: bytearray) -> None:
        for message in self._decoder.feed(bytes(data)):
            LOGGER.debug("StickS3 -> host: %s", message)
            if message.get("cmd") == "permission":
                self._permission_queue.put_nowait(message)


class FakeStickS3Device(StickS3Device):
    def __init__(self, *, auto_decision: str = "deny") -> None:
        if auto_decision not in {"once", "deny"}:
            raise ValueError("auto_decision must be 'once' or 'deny'")
        self.auto_decision = auto_decision
        self.snapshots: list[Snapshot] = []

    async def connect(self) -> None:
        LOGGER.info("Using fake StickS3 device with auto decision: %s", self.auto_decision)

    async def close(self) -> None:
        return None

    async def send_snapshot(self, snapshot: Snapshot) -> None:
        self.snapshots.append(snapshot)
        LOGGER.info("Snapshot: %s", snapshot.to_wire())

    async def wait_for_permission(self, prompt_id: str, timeout: float) -> str:
        await asyncio.sleep(0)
        LOGGER.info("Auto-answering prompt %s with %s", prompt_id, self.auto_decision)
        return self.auto_decision
