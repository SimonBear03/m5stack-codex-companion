from __future__ import annotations

import asyncio
import json
import logging
import os
import time
from collections import deque
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

from .device import StickS3Device
from .protocol import Snapshot, latest_entries, normalize_rate_limits, short_text

LOGGER = logging.getLogger(__name__)


def default_codex_home() -> Path:
    return Path(os.environ.get("CODEX_HOME", Path.home() / ".codex")).expanduser()


def event_timestamp_seconds(event: dict[str, Any]) -> float | None:
    timestamp = event.get("timestamp")
    if not isinstance(timestamp, str):
        return None
    try:
        return float(datetime.fromisoformat(timestamp.replace("Z", "+00:00")).timestamp())
    except ValueError:
        return None


def compact_desktop_rate_limits(rate_limits: dict[str, Any] | None) -> dict[str, dict[str, Any]] | None:
    if not isinstance(rate_limits, dict):
        return None

    def convert(window: dict[str, Any] | None) -> dict[str, Any] | None:
        if not isinstance(window, dict):
            return None
        used = window.get("usedPercent", window.get("used_percent"))
        if used is None:
            return None
        window_mins = window.get("windowDurationMins", window.get("window_minutes"))
        try:
            used_percent = int(round(float(used)))
        except (TypeError, ValueError):
            return None
        converted: dict[str, Any] = {"usedPercent": used_percent}
        if isinstance(window_mins, (float, int)):
            converted["windowDurationMins"] = int(window_mins)
        resets_at = window.get("resetsAt", window.get("resets_at"))
        if isinstance(resets_at, (float, int)):
            converted["resetsAt"] = int(resets_at)
        return converted

    converted = {
        "primary": convert(rate_limits.get("primary")),
        "secondary": convert(rate_limits.get("secondary")),
    }
    return normalize_rate_limits(converted)


def rollout_metadata(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as handle:
            first_line = handle.readline()
    except OSError:
        return {}
    if not first_line:
        return {}
    try:
        event = json.loads(first_line)
    except json.JSONDecodeError:
        return {}
    if event.get("type") != "session_meta":
        return {}
    payload = event.get("payload")
    return payload if isinstance(payload, dict) else {}


def is_user_rollout(path: Path) -> bool:
    metadata = rollout_metadata(path)
    if not metadata:
        return False
    if metadata.get("thread_source") == "subagent":
        return False
    source = metadata.get("source")
    return not (isinstance(source, dict) and "subagent" in source)


def find_rollout_by_thread_id(sessions_dir: Path, thread_id: str) -> Path | None:
    matches = sorted(
        sessions_dir.glob(f"**/*{thread_id}*.jsonl"),
        key=lambda path: path.stat().st_mtime,
        reverse=True,
    )
    if matches:
        return matches[0]
    for path in sorted(sessions_dir.glob("**/*.jsonl"), key=lambda path: path.stat().st_mtime, reverse=True):
        metadata = rollout_metadata(path)
        if metadata.get("id") == thread_id:
            return path
    return None


def latest_user_rollout(sessions_dir: Path) -> Path | None:
    files = sorted(sessions_dir.glob("**/*.jsonl"), key=lambda path: path.stat().st_mtime, reverse=True)
    for path in files:
        if is_user_rollout(path):
            return path
    return files[0] if files else None


@dataclass
class DesktopObserverState:
    thread_id: str | None = None
    thread_name: str | None = None
    cwd: str | None = None
    active_turn_id: str | None = None
    active: bool = False
    last_message: str = "Desktop observer connected"
    entries: deque[str] = field(default_factory=lambda: deque(maxlen=3))
    tokens_total: int | None = None
    rate_limits: dict[str, dict[str, Any]] | None = None
    last_event_ts: float | None = None

    def snapshot(self) -> Snapshot:
        label = self.thread_name or (self.thread_id[:8] if self.thread_id else "Desktop")
        msg = self.last_message
        if not self.active and self.thread_id and msg == "Desktop observer connected":
            msg = short_text(f"Idle: {label}", 80)
        return Snapshot(
            total=1 if self.thread_id else 0,
            running=1 if self.active else 0,
            waiting=0,
            msg=msg,
            entries=latest_entries(self.entries),
            tokens=self.tokens_total,
            rate_limits=self.rate_limits,
        )


class DesktopObserverBridge:
    def __init__(
        self,
        *,
        device: StickS3Device,
        sessions_dir: Path | None = None,
        rollout_path: Path | None = None,
        thread_id: str | None = None,
        poll_interval: float = 2.0,
        heartbeat_interval: float = 10.0,
    ) -> None:
        self.device = device
        self.sessions_dir = sessions_dir or (default_codex_home() / "sessions")
        self.rollout_path = rollout_path
        self.thread_id = thread_id
        self.poll_interval = poll_interval
        self.heartbeat_interval = heartbeat_interval
        self.state = DesktopObserverState()
        self._offset = 0
        self._last_size = 0
        self._last_sent = 0.0
        self._snapshot_lock = asyncio.Lock()

    async def run(self) -> None:
        await self.device.connect()
        asyncio.create_task(self.handle_device_controls())

        while True:
            changed = await self.poll_once()
            now = time.monotonic()
            if changed or now - self._last_sent >= self.heartbeat_interval:
                await self.send_snapshot()
            await asyncio.sleep(self.poll_interval)

    async def poll_once(self) -> bool:
        path = self.resolve_rollout()
        if path is None:
            if self.state.last_message != "No Codex Desktop rollout found":
                self.state.last_message = "No Codex Desktop rollout found"
                return True
            return False

        if self.rollout_path != path:
            self.rollout_path = path
            self._offset = 0
            self._last_size = 0
            self.state = DesktopObserverState()
            LOGGER.info("Observing Codex Desktop rollout: %s", path)

        try:
            stat = path.stat()
        except OSError as exc:
            LOGGER.warning("Could not stat rollout %s: %s", path, exc)
            return False

        if stat.st_size < self._offset:
            self._offset = 0
        if stat.st_size == self._offset and stat.st_size == self._last_size:
            return False

        changed = False
        try:
            with path.open("r", encoding="utf-8") as handle:
                handle.seek(self._offset)
                for line in handle:
                    if self.apply_line(line):
                        changed = True
                self._offset = handle.tell()
        except OSError as exc:
            LOGGER.warning("Could not read rollout %s: %s", path, exc)
            return False

        self._last_size = stat.st_size
        return changed

    def resolve_rollout(self) -> Path | None:
        if self.rollout_path and self.rollout_path.exists():
            return self.rollout_path
        if self.thread_id:
            return find_rollout_by_thread_id(self.sessions_dir, self.thread_id)
        return latest_user_rollout(self.sessions_dir)

    def apply_line(self, line: str) -> bool:
        stripped = line.strip()
        if not stripped:
            return False
        try:
            event = json.loads(stripped)
        except json.JSONDecodeError:
            LOGGER.debug("Ignoring invalid rollout JSON line")
            return False

        timestamp = event_timestamp_seconds(event)
        if timestamp is not None:
            self.state.last_event_ts = timestamp

        event_type = event.get("type")
        payload = event.get("payload")
        if not isinstance(payload, dict):
            return False

        if event_type == "session_meta":
            thread_id = payload.get("id")
            if thread_id:
                self.state.thread_id = str(thread_id)
                self.state.thread_name = self.state.thread_name or str(thread_id)[:8]
            self.state.cwd = str(payload.get("cwd") or "") or None
            self.state.last_message = "Desktop observer connected"
            return True

        if event_type == "turn_context":
            turn_id = payload.get("turn_id")
            if turn_id:
                self.state.active_turn_id = str(turn_id)
            return False

        if event_type == "response_item":
            return self.apply_response_item(payload)

        if event_type != "event_msg":
            return False

        return self.apply_event_payload(payload)

    def apply_response_item(self, payload: dict[str, Any]) -> bool:
        payload_type = payload.get("type")
        if payload_type in {"function_call", "custom_tool_call"}:
            name = payload.get("name") or "tool"
            self.add_entry(f"Tool: {name}")
            return True
        if payload_type == "function_call_output":
            self.add_entry("Tool output")
            return True
        return False

    def apply_event_payload(self, payload: dict[str, Any]) -> bool:
        payload_type = payload.get("type")

        if payload_type == "task_started":
            turn_id = payload.get("turn_id")
            if turn_id:
                self.state.active_turn_id = str(turn_id)
            self.state.active = True
            self.add_entry("Turn started")
            self.state.last_message = "Codex working"
            return True

        if payload_type == "task_complete":
            self.state.active = False
            self.state.active_turn_id = None
            summary = payload.get("last_agent_message") or "Turn completed"
            self.add_entry(summary)
            self.state.last_message = "Turn completed"
            return True

        if payload_type == "agent_message":
            message = payload.get("message")
            if isinstance(message, str) and message.strip():
                self.add_entry(message)
                self.state.last_message = short_text(message, 80)
                return True
            return False

        if payload_type == "user_message":
            message = payload.get("message")
            self.add_entry(f"User: {message}" if message else "User message")
            self.state.last_message = "User message"
            return True

        if payload_type == "token_count":
            info = payload.get("info") or {}
            if isinstance(info, dict):
                usage = info.get("total_token_usage") or {}
                if isinstance(usage, dict) and isinstance(usage.get("total_tokens"), int):
                    self.state.tokens_total = max(0, int(usage["total_tokens"]))
            self.state.rate_limits = compact_desktop_rate_limits(payload.get("rate_limits")) or self.state.rate_limits
            self.state.last_message = "Usage updated"
            return True

        if payload_type == "patch_apply_end":
            success = payload.get("success")
            self.add_entry("Patch applied" if success else "Patch failed")
            self.state.last_message = "Patch applied" if success else "Patch failed"
            return True

        return False

    def add_entry(self, value: Any) -> None:
        text = short_text(value, 48)
        if text:
            self.state.entries.appendleft(text)

    async def send_snapshot(self) -> None:
        async with self._snapshot_lock:
            await self.device.send_snapshot(self.state.snapshot())
            self._last_sent = time.monotonic()

    async def handle_device_controls(self) -> None:
        while True:
            message = await self.device.wait_for_control()
            LOGGER.info("Ignoring StickS3 control in desktop-observer mode: %s", message)
            self.state.last_message = "Observer is read-only"
            await self.send_snapshot()
