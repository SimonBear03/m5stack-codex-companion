import json
import os
import stat
import tempfile
import unittest
from pathlib import Path

from sticks3_bridge.pairing import APP_SUPPORT_DIR, DEFAULT_PAIRING_STORE_PATH, PairedDevice, PairingStore


class PairingStoreTests(unittest.TestCase):
    def test_store_writes_file_mode_0600_and_round_trips_devices(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "paired-devices.json"
            store = PairingStore(path)
            device = PairedDevice(
                device_id="dev-1",
                label="Desk StickS3",
                board="sticks3",
                address="AA:BB",
                name="Codex-S3-0001",
                secret="a" * 64,
                enabled=True,
                paired_at="2026-06-14T00:00:00Z",
            )

            store.upsert_device(device)

            mode = stat.S_IMODE(os.stat(path).st_mode)
            self.assertEqual(0o600, mode)
            self.assertEqual("dev-1", store.devices()[0].device_id)

    def test_corrupt_store_raises(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "paired-devices.json"
            path.write_text("{not-json", encoding="utf-8")

            with self.assertRaises(RuntimeError):
                PairingStore(path).devices()

    def test_remove_device_preserves_host_id(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "paired-devices.json"
            store = PairingStore(path)
            host_id = store.host_id()
            store.upsert_device(
                PairedDevice(
                    device_id="dev-1",
                    label="Cardputer",
                    board="cardputer_adv",
                    address=None,
                    name="Codex-CP-0001",
                    secret="b" * 64,
                    enabled=True,
                    paired_at="2026-06-14T00:00:00Z",
                )
            )

            removed = store.remove_device("dev-1")

            self.assertIsNotNone(removed)
            payload = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(host_id, payload["host_id"])
            self.assertEqual([], payload["devices"])

    def test_default_store_uses_m5stack_support_directory(self) -> None:
        self.assertEqual("M5Stack Codex Companion", APP_SUPPORT_DIR.name)
        self.assertEqual(APP_SUPPORT_DIR / "paired-devices.json", DEFAULT_PAIRING_STORE_PATH)

    def test_migrates_legacy_store_by_moving_existing_host_secret(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            legacy_path = root / "old" / "paired-devices.json"
            new_path = root / "new" / "paired-devices.json"
            legacy_path.parent.mkdir()
            legacy_payload = {
                "host_id": "existing-host",
                "devices": [
                    {
                        "device_id": "dev-1",
                        "label": "Desk",
                        "board": "sticks3",
                        "address": "AA:BB",
                        "name": "Codex-S3-0001",
                        "secret": "c" * 64,
                        "enabled": True,
                        "paired_at": "2026-06-14T00:00:00Z",
                    }
                ],
            }
            legacy_path.write_text(json.dumps(legacy_payload), encoding="utf-8")

            store = PairingStore(new_path, legacy_path=legacy_path)

            self.assertFalse(legacy_path.exists())
            self.assertEqual("existing-host", store.host_id())
            self.assertEqual("dev-1", store.devices()[0].device_id)
            mode = stat.S_IMODE(os.stat(new_path).st_mode)
            self.assertEqual(0o600, mode)

    def test_migration_does_not_overwrite_existing_store(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            legacy_path = root / "old" / "paired-devices.json"
            new_path = root / "new" / "paired-devices.json"
            legacy_path.parent.mkdir()
            new_path.parent.mkdir()
            legacy_path.write_text(json.dumps({"host_id": "legacy", "devices": []}), encoding="utf-8")
            new_path.write_text(json.dumps({"host_id": "current", "devices": []}), encoding="utf-8")

            store = PairingStore(new_path, legacy_path=legacy_path)

            self.assertTrue(legacy_path.exists())
            self.assertEqual("current", store.host_id())


if __name__ == "__main__":
    unittest.main()
