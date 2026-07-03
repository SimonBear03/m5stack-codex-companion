from __future__ import annotations

import json
import os
import secrets
import shutil
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


APP_SUPPORT_DIR = Path.home() / "Library" / "Application Support" / "M5Stack Codex Companion"
LEGACY_APP_SUPPORT_DIR = Path.home() / "Library" / "Application Support" / "StickS3 Codex Companion"
DEFAULT_PAIRING_STORE_PATH = APP_SUPPORT_DIR / "paired-devices.json"
LEGACY_PAIRING_STORE_PATH = LEGACY_APP_SUPPORT_DIR / "paired-devices.json"


def utc_now_iso() -> str:
    return datetime.utcnow().replace(microsecond=0).isoformat() + "Z"


@dataclass(slots=True)
class PairedDevice:
    device_id: str
    label: str
    board: str
    address: str | None
    name: str
    secret: str
    enabled: bool
    paired_at: str
    last_seen: str | None = None

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "PairedDevice":
        device_id = str(data.get("device_id") or "").strip()
        secret = str(data.get("secret") or "").strip()
        if not device_id or not secret:
            raise ValueError("paired device is missing device_id or secret")
        return cls(
            device_id=device_id,
            label=str(data.get("label") or data.get("name") or device_id),
            board=str(data.get("board") or "unknown"),
            address=str(data["address"]) if data.get("address") else None,
            name=str(data.get("name") or ""),
            secret=secret,
            enabled=bool(data.get("enabled", True)),
            paired_at=str(data.get("paired_at") or utc_now_iso()),
            last_seen=str(data["last_seen"]) if data.get("last_seen") else None,
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "device_id": self.device_id,
            "label": self.label,
            "board": self.board,
            "address": self.address,
            "name": self.name,
            "secret": self.secret,
            "enabled": self.enabled,
            "paired_at": self.paired_at,
            "last_seen": self.last_seen,
        }


class PairingStore:
    def __init__(
        self,
        path: Path | None = None,
        *,
        legacy_path: Path | None = None,
        migrate_legacy: bool = True,
    ) -> None:
        self.path = (path or DEFAULT_PAIRING_STORE_PATH).expanduser()
        if legacy_path is not None:
            self.legacy_path: Path | None = legacy_path.expanduser()
        elif self.path == DEFAULT_PAIRING_STORE_PATH:
            self.legacy_path = LEGACY_PAIRING_STORE_PATH
        else:
            self.legacy_path = None
        if migrate_legacy:
            self.migrate_legacy_store()

    def migrate_legacy_store(self) -> bool:
        legacy_path = self.legacy_path
        if legacy_path is None or self.path.exists() or not legacy_path.exists():
            return False
        self.path.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(legacy_path), str(self.path))
        os.chmod(self.path, 0o600)
        try:
            legacy_path.parent.rmdir()
        except OSError:
            pass
        return True

    def load(self) -> dict[str, Any]:
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return {"host_id": secrets.token_hex(16), "devices": []}
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"Pairing store is not valid JSON: {self.path}") from exc
        if not isinstance(raw, dict):
            raise RuntimeError(f"Pairing store root must be an object: {self.path}")
        raw.setdefault("host_id", secrets.token_hex(16))
        raw.setdefault("devices", [])
        if not isinstance(raw["devices"], list):
            raise RuntimeError(f"Pairing store devices field must be a list: {self.path}")
        return raw

    def save(self, data: dict[str, Any]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        payload = json.dumps(data, indent=2, sort_keys=True) + "\n"
        temp_path = self.path.with_suffix(self.path.suffix + ".tmp")
        temp_path.write_text(payload, encoding="utf-8")
        os.chmod(temp_path, 0o600)
        temp_path.replace(self.path)
        os.chmod(self.path, 0o600)

    def host_id(self) -> str:
        data = self.load()
        host_id = str(data.get("host_id") or "").strip()
        if host_id and self.path.exists():
            return host_id
        if not host_id:
            host_id = secrets.token_hex(16)
            data["host_id"] = host_id
        self.save(data)
        return host_id

    def devices(self, *, enabled_only: bool = False) -> list[PairedDevice]:
        devices: list[PairedDevice] = []
        for item in self.load().get("devices", []):
            if not isinstance(item, dict):
                continue
            device = PairedDevice.from_dict(item)
            if enabled_only and not device.enabled:
                continue
            devices.append(device)
        return devices

    def upsert_device(self, device: PairedDevice) -> None:
        data = self.load()
        next_devices: list[dict[str, Any]] = []
        replaced = False
        for item in data.get("devices", []):
            if not isinstance(item, dict):
                continue
            if str(item.get("device_id") or "") == device.device_id:
                next_devices.append(device.to_dict())
                replaced = True
            else:
                next_devices.append(item)
        if not replaced:
            next_devices.append(device.to_dict())
        data["devices"] = next_devices
        self.save(data)

    def remove_device(self, device_id: str) -> PairedDevice | None:
        data = self.load()
        removed: PairedDevice | None = None
        next_devices: list[dict[str, Any]] = []
        for item in data.get("devices", []):
            if not isinstance(item, dict):
                continue
            if str(item.get("device_id") or "") == device_id:
                removed = PairedDevice.from_dict(item)
            else:
                next_devices.append(item)
        data["devices"] = next_devices
        self.save(data)
        return removed

    def mark_seen(self, device_id: str, *, address: str | None = None, name: str | None = None, board: str | None = None) -> None:
        data = self.load()
        changed = False
        for item in data.get("devices", []):
            if not isinstance(item, dict) or str(item.get("device_id") or "") != device_id:
                continue
            item["last_seen"] = utc_now_iso()
            if address:
                item["address"] = address
            if name:
                item["name"] = name
            if board:
                item["board"] = board
            changed = True
        if changed:
            self.save(data)
