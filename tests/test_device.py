import unittest
from types import SimpleNamespace

from sticks3_bridge.device import BleStickS3Device, advertisement_matches_prefix
from sticks3_bridge.protocol import NUS_RX_UUID, NUS_SERVICE_UUID


class FakeBleClient:
    def __init__(self) -> None:
        self.writes: list[tuple[str, bytes, bool]] = []

    async def write_gatt_char(self, uuid: str, chunk: bytes, *, response: bool) -> None:
        self.writes.append((uuid, bytes(chunk), response))


class DeviceTests(unittest.IsolatedAsyncioTestCase):
    def test_ble_scan_predicate_requires_codex_s3_name(self) -> None:
        service_only_device = SimpleNamespace(name=None)
        service_only_adv = SimpleNamespace(local_name=None, service_uuids=[NUS_SERVICE_UUID])
        named_device = SimpleNamespace(name="Codex-S3-0470")
        unnamed_adv = SimpleNamespace(local_name=None, service_uuids=[])
        unnamed_device = SimpleNamespace(name=None)
        local_name_adv = SimpleNamespace(local_name="Codex-S3-ABCD", service_uuids=[])

        self.assertFalse(advertisement_matches_prefix(service_only_device, service_only_adv, "Codex-S3-"))
        self.assertTrue(advertisement_matches_prefix(named_device, unnamed_adv, "Codex-S3-"))
        self.assertTrue(advertisement_matches_prefix(unnamed_device, local_name_adv, "Codex-S3-"))

    async def test_send_json_uses_acknowledged_ble_writes(self) -> None:
        client = FakeBleClient()
        device = BleStickS3Device(chunk_size=8)
        device._client = client

        await device.send_json({"msg": "hello"})

        self.assertGreater(len(client.writes), 1)
        self.assertTrue(all(uuid == NUS_RX_UUID for uuid, _, _ in client.writes))
        self.assertTrue(all(response for _, _, response in client.writes))


if __name__ == "__main__":
    unittest.main()
