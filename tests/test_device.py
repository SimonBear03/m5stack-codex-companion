import asyncio
import unittest
from types import SimpleNamespace

from sticks3_bridge.device import (
    DEFAULT_DEVICE_PREFIXES,
    BleCompanionFleet,
    BleStickS3Device,
    advertisement_matches_paired_device,
    advertisement_matches_prefix,
    auth_digest,
)
from sticks3_bridge.pairing import PairedDevice
from sticks3_bridge.protocol import NUS_RX_UUID, NUS_SERVICE_UUID
from sticks3_bridge.protocol import encode_json_line


class FakeBleClient:
    def __init__(self) -> None:
        self.writes: list[tuple[str, bytes, bool]] = []
        self.is_connected = True

    async def write_gatt_char(self, uuid: str, chunk: bytes, *, response: bool) -> None:
        self.writes.append((uuid, bytes(chunk), response))


class FakePairingStore:
    def __init__(self, devices: list[PairedDevice]) -> None:
        self._devices = devices

    def host_id(self) -> str:
        return "host-id"

    def devices(self, *, enabled_only: bool = False) -> list[PairedDevice]:
        if enabled_only:
            return [device for device in self._devices if device.enabled]
        return list(self._devices)

    def mark_seen(self, *_args, **_kwargs) -> None:
        return None


class RecordingFleet(BleCompanionFleet):
    def __init__(self, *, store: FakePairingStore, fail_ble_targets: set[str] | None = None) -> None:
        super().__init__(store=store)
        self.connect_attempts = []
        self.fail_ble_targets = fail_ble_targets or set()

    async def _connect_and_auth_target(self, target, ble_target, *, serialize: bool = True):
        self.connect_attempts.append((target.device_id, ble_target))
        if ble_target in self.fail_ble_targets:
            raise RuntimeError("connect failed")
        return {"device_id": target.device_id, "name": target.paired.name, "board": target.paired.board}


class DeviceTests(unittest.IsolatedAsyncioTestCase):
    def _paired_device(self, *, device_id: str, board: str, name: str, address: str | None = None) -> PairedDevice:
        return PairedDevice(
            device_id=device_id,
            label=name,
            board=board,
            address=address,
            name=name,
            secret="00" * 32,
            enabled=True,
            paired_at="2026-06-15T00:00:00Z",
        )

    def test_ble_scan_predicate_requires_named_companion_device(self) -> None:
        service_only_device = SimpleNamespace(name=None)
        service_only_adv = SimpleNamespace(local_name=None, service_uuids=[NUS_SERVICE_UUID])
        sticks3_device = SimpleNamespace(name="Codex-S3-0470")
        cardputer_device = SimpleNamespace(name="Codex-CP-1234")
        unnamed_adv = SimpleNamespace(local_name=None, service_uuids=[])
        unnamed_device = SimpleNamespace(name=None)
        local_name_adv = SimpleNamespace(local_name="Codex-S3-ABCD", service_uuids=[])

        self.assertFalse(advertisement_matches_prefix(service_only_device, service_only_adv, DEFAULT_DEVICE_PREFIXES))
        self.assertTrue(advertisement_matches_prefix(sticks3_device, unnamed_adv, DEFAULT_DEVICE_PREFIXES))
        self.assertTrue(advertisement_matches_prefix(cardputer_device, unnamed_adv, DEFAULT_DEVICE_PREFIXES))
        self.assertTrue(advertisement_matches_prefix(unnamed_device, local_name_adv, DEFAULT_DEVICE_PREFIXES))

    def test_paired_device_scan_predicate_requires_target_identity(self) -> None:
        cardputer = self._paired_device(
            device_id="cardputer_adv-8428-205e9473",
            board="cardputer_adv",
            name="Codex-CP-8428",
        )
        sticks3 = self._paired_device(
            device_id="sticks3-0470-6a77b282",
            board="sticks3",
            name="Codex-S3-0470",
        )
        sticks3_adv = SimpleNamespace(local_name="Codex-S3-0470")
        cardputer_adv = SimpleNamespace(local_name="Codex-CP-8428")

        self.assertTrue(advertisement_matches_paired_device(SimpleNamespace(name="Codex-CP-8428"), None, cardputer, DEFAULT_DEVICE_PREFIXES))
        self.assertFalse(advertisement_matches_paired_device(SimpleNamespace(name="Codex-S3-0470"), sticks3_adv, cardputer, DEFAULT_DEVICE_PREFIXES))
        self.assertTrue(advertisement_matches_paired_device(SimpleNamespace(name="Codex-S3-0470"), None, sticks3, DEFAULT_DEVICE_PREFIXES))
        self.assertFalse(advertisement_matches_paired_device(SimpleNamespace(name="Codex-CP-8428"), cardputer_adv, sticks3, DEFAULT_DEVICE_PREFIXES))

    def test_paired_device_scan_predicate_can_derive_name_from_device_id(self) -> None:
        paired = self._paired_device(
            device_id="cardputer_adv-8428-205e9473",
            board="cardputer_adv",
            name="",
        )

        self.assertTrue(advertisement_matches_paired_device(SimpleNamespace(name="Codex-CP-8428"), None, paired, DEFAULT_DEVICE_PREFIXES))
        self.assertFalse(advertisement_matches_paired_device(SimpleNamespace(name="Codex-CP-9999"), None, paired, DEFAULT_DEVICE_PREFIXES))

    async def test_send_json_uses_paced_write_without_response(self) -> None:
        client = FakeBleClient()
        device = BleStickS3Device(chunk_size=8, write_chunk_delay=0.0)
        device._client = client

        await device.send_json({"msg": "hello"})

        self.assertGreater(len(client.writes), 1)
        self.assertTrue(all(uuid == NUS_RX_UUID for uuid, _, _ in client.writes))
        self.assertTrue(all(not response for _, _, response in client.writes))

    async def test_fleet_tries_stored_address_even_when_another_device_is_connected(self) -> None:
        connected = self._paired_device(
            device_id="cardputer_adv-8428-205e9473",
            board="cardputer_adv",
            name="Codex-CP-8428",
            address="CARDPUTER-ADDR",
        )
        reconnecting = self._paired_device(
            device_id="sticks3-0470-6a77b282",
            board="sticks3",
            name="Codex-S3-0470",
            address="STICKS3-ADDR",
        )
        fleet = RecordingFleet(store=FakePairingStore([connected, reconnecting]))
        targets = {target.device_id: target for target in fleet.targets}
        targets[connected.device_id].state = "connected"
        targets[connected.device_id].client._client = FakeBleClient()

        await fleet._connect_target(targets[reconnecting.device_id])

        self.assertIn((reconnecting.device_id, "STICKS3-ADDR"), fleet.connect_attempts)

    async def test_fleet_defers_scan_fallback_when_other_device_is_connected(self) -> None:
        connected = self._paired_device(
            device_id="cardputer_adv-8428-205e9473",
            board="cardputer_adv",
            name="Codex-CP-8428",
            address="CARDPUTER-ADDR",
        )
        reconnecting = self._paired_device(
            device_id="sticks3-0470-6a77b282",
            board="sticks3",
            name="Codex-S3-0470",
            address="STICKS3-ADDR",
        )
        fleet = RecordingFleet(store=FakePairingStore([connected, reconnecting]), fail_ble_targets={"STICKS3-ADDR"})
        targets = {target.device_id: target for target in fleet.targets}
        targets[connected.device_id].state = "connected"
        targets[connected.device_id].client._client = FakeBleClient()
        targets[reconnecting.device_id].last_scan_attempt_monotonic = asyncio.get_running_loop().time()

        with self.assertRaisesRegex(RuntimeError, "scan fallback deferred"):
            await fleet._connect_target(targets[reconnecting.device_id])

        self.assertEqual([(reconnecting.device_id, "STICKS3-ADDR")], fleet.connect_attempts)

    async def test_send_command_discards_stale_ack_before_write(self) -> None:
        device = BleStickS3Device(chunk_size=64)
        device._client = FakeBleClient()
        queue = device._ack_queues.setdefault("status", asyncio.Queue())
        queue.put_nowait({"ack": "status", "ok": True, "data": {"stale": True}})

        task = asyncio.create_task(device.send_command("status", timeout=1.0))
        await asyncio.sleep(0)
        device._handle_notification(0, bytearray(encode_json_line({"ack": "status", "ok": True, "data": {"fresh": True}})))

        ack = await task

        self.assertEqual({"fresh": True}, ack["data"])

    def test_auth_digest_is_stable(self) -> None:
        digest = auth_digest(
            "00" * 32,
            device_id="dev",
            device_nonce="device-nonce",
            host_nonce="host-nonce",
            host_id="host",
        )

        self.assertEqual(64, len(digest))
        self.assertEqual(
            digest,
            auth_digest(
                "00" * 32,
                device_id="dev",
                device_nonce="device-nonce",
                host_nonce="host-nonce",
                host_id="host",
            ),
        )


if __name__ == "__main__":
    unittest.main()
