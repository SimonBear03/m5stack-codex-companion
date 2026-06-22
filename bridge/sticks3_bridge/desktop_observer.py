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
from .protocol import Snapshot, codex_activity, codex_state_from_activity, normalize_rate_limits, short_text

LOGGER = logging.getLogger(__name__)

DESKTOP_ACTIVITY_HISTORY = 4
DESKTOP_ACTIVITY_TEXT_LIMIT = 1000
DEFAULT_ACTIVE_HEARTBEAT_INTERVAL = 10.0
DEFAULT_IDLE_HEARTBEAT_INTERVAL = 45.0
ACTIVE_WORK_WATCHDOG_SECONDS = 600.0
RECENT_WORK_EVENT_MAX_AGE_SECONDS = 120.0
ROLLOUT_ACTIVITY_SCAN_FILE_LIMIT = 32
ROLLOUT_ACTIVITY_TAIL_BYTES = 262144
ROLLOUT_SWITCH_ACTIVITY_MARGIN_SECONDS = 20.0
ROLLOUT_SWITCH_MIN_DWELL_SECONDS = 20.0
USAGE_REFRESH_INTERVAL_SECONDS = 5.0
USAGE_SCAN_FILE_LIMIT = 32
USAGE_INCREMENTAL_LOOKBACK_BYTES = 65536
DETAIL_FULL = 0
DETAIL_STATUS = 1
DETAIL_USAGE = 2
TRANSIENT_WORK_STATUS_KINDS = {
    "message",
    "output",
    "patch",
    "replying",
    "started",
    "thinking",
    "working",
}
WORK_RESPONSE_ITEM_TYPES = {
    "function_call",
    "custom_tool_call",
    "function_call_output",
    "message",
    "reasoning",
}
WORK_EVENT_PAYLOAD_TYPES = {
    "patch_apply_end",
    "task_started",
    "user_message",
}
TERMINAL_EVENT_PAYLOAD_TYPES = {
    "canceled",
    "cancelled",
    "interrupted",
    "task_aborted",
    "task_complete",
    "turn_aborted",
}


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


@dataclass(frozen=True)
class DesktopUsageSnapshot:
    tokens_total: int | None = None
    rate_limits: dict[str, dict[str, Any]] | None = None
    event_ts: float | None = None
    path: Path | None = None

    @property
    def has_usage(self) -> bool:
        return self.tokens_total is not None or self.rate_limits is not None


@dataclass(frozen=True)
class UsageFileCacheEntry:
    size: int
    mtime_ns: int
    usage: DesktopUsageSnapshot | None = None


@dataclass(frozen=True)
class RolloutActivity:
    path: Path
    thread_id: str | None
    cwd: str | None
    mtime: float
    active: bool
    last_active_ts: float | None = None
    last_terminal_ts: float | None = None


def usage_from_token_count_payload(payload: dict[str, Any], *, event_ts: float | None = None, path: Path | None = None) -> DesktopUsageSnapshot:
    tokens_total: int | None = None
    info = payload.get("info") or {}
    if isinstance(info, dict):
        usage = info.get("total_token_usage") or {}
        if isinstance(usage, dict) and isinstance(usage.get("total_tokens"), int):
            tokens_total = max(0, int(usage["total_tokens"]))
    return DesktopUsageSnapshot(
        tokens_total=tokens_total,
        rate_limits=compact_desktop_rate_limits(payload.get("rate_limits")),
        event_ts=event_ts,
        path=path,
    )


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


def sorted_rollout_files(sessions_dir: Path) -> list[Path]:
    try:
        candidates = list(sessions_dir.glob("**/*.jsonl"))
    except OSError:
        return []
    files: list[tuple[float, str, Path]] = []
    for path in candidates:
        try:
            mtime = path.stat().st_mtime
        except OSError:
            continue
        files.append((mtime, str(path), path))
    files.sort(key=lambda item: (item[0], item[1]), reverse=True)
    return [path for _, _, path in files]


def recent_rollout_lines(path: Path, *, size: int) -> list[str]:
    try:
        with path.open("rb") as handle:
            if size > ROLLOUT_ACTIVITY_TAIL_BYTES:
                handle.seek(max(0, size - ROLLOUT_ACTIVITY_TAIL_BYTES))
                handle.readline()
            data = handle.read()
    except OSError:
        return []
    return data.decode("utf-8", errors="ignore").splitlines()


def rollout_event_activity_kind(event: dict[str, Any]) -> str | None:
    payload = event.get("payload")
    if not isinstance(payload, dict):
        return None

    event_type = event.get("type")
    payload_type = payload.get("type")
    if event_type == "turn_context":
        return "active"
    if not isinstance(payload_type, str):
        return None
    if event_type == "response_item":
        return "active" if payload_type in WORK_RESPONSE_ITEM_TYPES else None
    if event_type != "event_msg":
        return None
    if payload_type in TERMINAL_EVENT_PAYLOAD_TYPES:
        return "terminal"
    if payload_type == "agent_message":
        message = payload.get("message")
        return "active" if isinstance(message, str) and message.strip() else None
    if payload_type in WORK_EVENT_PAYLOAD_TYPES:
        return "active"
    return None


def scan_rollout_activity(path: Path, *, now: float | None = None) -> RolloutActivity | None:
    metadata = rollout_metadata(path)
    try:
        stat = path.stat()
    except OSError:
        return None

    current = time.time() if now is None else now
    last_active_index: int | None = None
    last_terminal_index: int | None = None
    last_active_ts: float | None = None
    last_terminal_ts: float | None = None

    for index, line in enumerate(recent_rollout_lines(path, size=stat.st_size)):
        stripped = line.strip()
        if not stripped:
            continue
        try:
            event = json.loads(stripped)
        except json.JSONDecodeError:
            continue
        kind = rollout_event_activity_kind(event)
        if kind is None:
            continue
        event_ts = event_timestamp_seconds(event)
        event_time = event_ts if event_ts is not None else stat.st_mtime
        if kind == "active":
            if last_active_index is None or index > last_active_index:
                last_active_index = index
                last_active_ts = event_time
        elif last_terminal_index is None or index > last_terminal_index:
            last_terminal_index = index
            last_terminal_ts = event_time

    active = False
    if last_active_index is not None and (last_terminal_index is None or last_active_index > last_terminal_index):
        assert last_active_ts is not None
        active = current - last_active_ts <= ACTIVE_WORK_WATCHDOG_SECONDS

    return RolloutActivity(
        path=path,
        thread_id=str(metadata.get("id")) if metadata.get("id") else None,
        cwd=str(metadata.get("cwd")) if metadata.get("cwd") else None,
        mtime=stat.st_mtime,
        active=active,
        last_active_ts=last_active_ts,
        last_terminal_ts=last_terminal_ts,
    )


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


def latest_user_rollout(
    sessions_dir: Path,
    *,
    current_path: Path | None = None,
    switch_margin_seconds: float = 0.0,
    now: float | None = None,
) -> Path | None:
    files = sorted_rollout_files(sessions_dir)
    user_files = [path for path in files if is_user_rollout(path)]
    current = time.time() if now is None else now
    active_rollouts: list[RolloutActivity] = []
    current_activity: RolloutActivity | None = None
    for path in user_files[:ROLLOUT_ACTIVITY_SCAN_FILE_LIMIT]:
        activity = scan_rollout_activity(path, now=current)
        if current_path is not None and path == current_path:
            current_activity = activity
        if activity is not None and activity.active and activity.last_active_ts is not None:
            active_rollouts.append(activity)
    if active_rollouts:
        active_rollouts.sort(key=lambda activity: (activity.last_active_ts or 0.0, activity.mtime, str(activity.path)), reverse=True)
        winner = active_rollouts[0]
        if (
            current_path is not None
            and current_activity is not None
            and current_activity.active
            and current_activity.last_active_ts is not None
            and winner.path != current_path
            and winner.last_active_ts is not None
            and winner.last_active_ts - current_activity.last_active_ts < switch_margin_seconds
        ):
            return current_path
        return winner.path
    for path in user_files:
        return path
    return files[0] if files else None


def recent_rollout_files(sessions_dir: Path, *, limit: int = USAGE_SCAN_FILE_LIMIT) -> list[Path]:
    return sorted_rollout_files(sessions_dir)[:limit]


def latest_usage_from_rollout(path: Path, *, start_offset: int = 0) -> DesktopUsageSnapshot | None:
    latest: DesktopUsageSnapshot | None = None
    try:
        with path.open("r", encoding="utf-8") as handle:
            if start_offset > 0:
                handle.seek(start_offset)
                handle.readline()
            for line in handle:
                if '"token_count"' not in line:
                    continue
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if event.get("type") != "event_msg":
                    continue
                payload = event.get("payload")
                if not isinstance(payload, dict) or payload.get("type") != "token_count":
                    continue
                usage = usage_from_token_count_payload(payload, event_ts=event_timestamp_seconds(event), path=path)
                if usage.has_usage:
                    latest = usage
    except OSError:
        return None
    return latest


def latest_account_usage(
    sessions_dir: Path,
    *,
    limit: int = USAGE_SCAN_FILE_LIMIT,
    cache: dict[Path, UsageFileCacheEntry] | None = None,
) -> DesktopUsageSnapshot | None:
    best: DesktopUsageSnapshot | None = None
    recent_paths = recent_rollout_files(sessions_dir, limit=limit)
    for path in recent_paths:
        usage: DesktopUsageSnapshot | None = None
        if cache is None:
            usage = latest_usage_from_rollout(path)
        else:
            try:
                stat = path.stat()
            except OSError:
                cache.pop(path, None)
                continue
            cached = cache.get(path)
            if cached and cached.size == stat.st_size and cached.mtime_ns == stat.st_mtime_ns:
                usage = cached.usage
            else:
                start_offset = 0
                if cached and stat.st_size >= cached.size:
                    start_offset = max(0, cached.size - USAGE_INCREMENTAL_LOOKBACK_BYTES)
                scanned = latest_usage_from_rollout(path, start_offset=start_offset)
                usage = scanned or (cached.usage if cached and stat.st_size >= cached.size else None)
                cache[path] = UsageFileCacheEntry(size=stat.st_size, mtime_ns=stat.st_mtime_ns, usage=usage)
        if usage is None:
            continue
        if best is None:
            best = usage
            continue
        if usage.event_ts is not None and best.event_ts is not None:
            if usage.event_ts > best.event_ts:
                best = usage
        elif usage.event_ts is not None and best.event_ts is None:
            best = usage
    if cache is not None:
        recent_set = set(recent_paths)
        for cached_path in list(cache):
            if cached_path not in recent_set:
                cache.pop(cached_path, None)
    return best


def clean_tool_name(value: Any) -> str:
    text = short_text(value or "tool", 40)
    if "." in text:
        text = text.rsplit(".", 1)[-1]
    return text or "tool"


@dataclass
class DesktopObserverState:
    thread_id: str | None = None
    thread_name: str | None = None
    cwd: str | None = None
    active_turn_id: str | None = None
    active: bool = False
    last_message: str = "Desktop observer connected"
    status: dict[str, Any] = field(
        default_factory=lambda: {"speaker": "System", "kind": "connected", "text": "Desktop observer connected"}
    )
    activity: deque[dict[str, Any]] = field(default_factory=lambda: deque(maxlen=DESKTOP_ACTIVITY_HISTORY))
    next_seq: int = 1
    tokens_total: int | None = None
    rate_limits: dict[str, dict[str, Any]] | None = None
    usage_event_ts: float | None = None
    last_event_ts: float | None = None
    activity_updated_at: float = field(default_factory=time.time)

    def snapshot(self, activity: tuple[dict[str, Any], ...] | None = None, *, working: bool | None = None) -> Snapshot:
        is_working = self.active if working is None else working
        label = self.thread_name or (self.thread_id[:8] if self.thread_id else "Desktop")
        project_label = Path(self.cwd).name if self.cwd else None
        msg = self.last_message
        if not is_working and self.thread_id and msg == "Desktop observer connected":
            msg = short_text(f"Idle: {label}", 80)
        ordered_activity = tuple(reversed(self.activity)) if activity is None else activity
        entries = tuple(
            short_text(f"{item.get('speaker', 'Codex')}: {item.get('text', '')}", 48)
            for item in reversed(ordered_activity)
        )
        current_activity = self.codex_activity(working=is_working, label=label, project_label=project_label)
        return Snapshot(
            total=1 if self.thread_id else 0,
            running=1 if is_working else 0,
            waiting=0,
            msg=msg,
            entries=entries[:3],
            status=self.status,
            codex_activity=current_activity,
            activity=ordered_activity,
            tokens=self.tokens_total,
            rate_limits=self.rate_limits,
        )

    def codex_activity(self, *, working: bool, label: str, project_label: str | None) -> Any:
        status = self.activity_status(working=working)
        subtitle = self.status.get("text") if isinstance(self.status, dict) else self.last_message
        if not subtitle:
            subtitle = "Working" if status == "running" else "Idle"
        return codex_activity(
            status,
            title=label,
            subtitle=subtitle,
            updated_at=self.activity_updated_at,
            thread_label=label if self.thread_id else None,
            project_label=project_label,
        )

    def activity_status(self, *, working: bool) -> str:
        kind = str(self.status.get("kind") or "").lower() if isinstance(self.status, dict) else ""
        if kind == "error":
            return "failed"
        if working:
            return "running"
        if kind in {"completed", "complete", "task_complete"}:
            return "review"
        return "idle"


class DesktopObserverBridge:
    def __init__(
        self,
        *,
        device: StickS3Device,
        sessions_dir: Path | None = None,
        rollout_path: Path | None = None,
        thread_id: str | None = None,
        poll_interval: float = 2.0,
        heartbeat_interval: float = DEFAULT_ACTIVE_HEARTBEAT_INTERVAL,
        idle_heartbeat_interval: float = DEFAULT_IDLE_HEARTBEAT_INTERVAL,
        status_file: Path | None = None,
        reconnect: bool = True,
        reconnect_delay: float = 1.0,
        max_reconnect_delay: float = 3.0,
    ) -> None:
        self.device = device
        self.sessions_dir = sessions_dir or (default_codex_home() / "sessions")
        self.fixed_rollout_path = rollout_path
        self.rollout_path = rollout_path
        self.thread_id = thread_id
        self.poll_interval = poll_interval
        self.heartbeat_interval = heartbeat_interval
        self.idle_heartbeat_interval = idle_heartbeat_interval
        self.status_file = status_file
        self.reconnect = reconnect
        self.reconnect_delay = max(0.5, float(reconnect_delay))
        self.max_reconnect_delay = max(self.reconnect_delay, float(max_reconnect_delay))
        self.state = DesktopObserverState()
        self._offset = 0
        self._last_size = 0
        self._last_sent = 0.0
        self._last_activity_seq_sent = 0
        self._active_work_deadline = 0.0
        self._last_sent_working = False
        self._last_usage_scan = 0.0
        self._usage_file_cache: dict[Path, UsageFileCacheEntry] = {}
        self._last_wire_payload: dict[str, Any] | None = None
        self._rollout_selected_at = time.monotonic() if rollout_path is not None else 0.0
        self._poll_lock = asyncio.Lock()
        self._snapshot_lock = asyncio.Lock()
        self._device_connected = bool(getattr(device, "always_connected", False))
        self._device_state = "connected" if self._device_connected else "disconnected"
        self._device_error: str | None = None

    async def run(self) -> None:
        self.write_status(self._device_state, detail="Codex observer running")
        async with asyncio.TaskGroup() as task_group:
            task_group.create_task(self.device_connection_loop())
            task_group.create_task(self.handle_device_controls())
            task_group.create_task(self.observer_loop())

    async def observer_loop(self) -> None:
        while True:
            changed = await self.refresh_observed_state(force_usage=self._last_usage_scan <= 0.0)
            now = time.monotonic()
            working = self.is_working(now)
            heartbeat_interval = self.heartbeat_interval if working else self.idle_heartbeat_interval
            work_expired = self._last_sent_working and not working
            status_expired = self.expire_transient_work_status() if not working else False
            changed = changed or status_expired
            fleet_needs_sync = bool(getattr(self.device, "needs_initial_sync", lambda: False)())
            force_send = now - self._last_sent >= heartbeat_interval
            if changed or work_expired or force_send or fleet_needs_sync:
                await self.send_snapshot(force=force_send)
            await asyncio.sleep(self.poll_interval)

    async def device_connection_loop(self) -> None:
        if hasattr(self.device, "all_targets"):
            await self.device.connect()
            while True:
                state = getattr(self.device, "aggregate_state", lambda: "disconnected")()
                self._device_connected = state == "connected"
                self.write_status(state, detail=self.state.last_message)
                await asyncio.sleep(1.0)

        if self._device_connected and getattr(self.device, "always_connected", False):
            return

        delay = self.reconnect_delay
        while True:
            if self._device_connected:
                if not self.device.is_connected():
                    self._device_connected = False
                    self._device_state = "disconnected"
                    self._device_error = "BLE link closed"
                    await self.device.close()
                    self.write_status(self._device_state, detail="StickS3 disconnected", error=self._device_error)
                    delay = self.reconnect_delay
                    continue
                await asyncio.sleep(1.0)
                continue

            self._device_state = "scanning"
            self._device_error = None
            self.write_status(self._device_state, detail="Scanning for StickS3")
            try:
                await self.device.connect()
            except Exception as exc:
                self._device_connected = False
                self._device_state = "disconnected"
                self._device_error = str(exc)
                self.write_status(self._device_state, detail=f"Retrying BLE in {delay:.0f}s", error=str(exc))
                if not self.reconnect:
                    raise
                await self.device.close()
                await asyncio.sleep(delay)
                delay = min(self.max_reconnect_delay, max(delay * 1.6, delay + 1.0))
                continue

            delay = self.reconnect_delay
            self._device_connected = True
            self._device_state = "connected"
            self._device_error = None
            await self.sync_connected_device()

    async def refresh_observed_state(self, *, force_usage: bool = False) -> bool:
        async with self._poll_lock:
            changed = await self.poll_once()
            usage_changed = self.refresh_account_usage(force=force_usage)
            return changed or usage_changed

    async def sync_connected_device(self) -> None:
        await self.refresh_observed_state(force_usage=True)
        self.write_status(self._device_state, detail="BLE connected; syncing")
        if await self.send_initial_sync_snapshot():
            await self.send_snapshot(force=True)

    async def poll_once(self) -> bool:
        path = self.resolve_rollout()
        if path is None:
            if self.state.last_message != "No Codex Desktop rollout found":
                self.state.last_message = "No Codex Desktop rollout found"
                self.set_status("System", "disconnected", "No Codex Desktop rollout found")
                return True
            return False

        if self.rollout_path != path:
            tokens_total = self.state.tokens_total
            rate_limits = self.state.rate_limits
            usage_event_ts = self.state.usage_event_ts
            self.rollout_path = path
            self._rollout_selected_at = time.monotonic()
            self._offset = 0
            self._last_size = 0
            self._last_activity_seq_sent = 0
            self._active_work_deadline = 0.0
            self._last_sent_working = False
            self._last_wire_payload = None
            self.reset_device_send_state()
            self.state = DesktopObserverState(tokens_total=tokens_total, rate_limits=rate_limits, usage_event_ts=usage_event_ts)
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
        return self.refresh_account_usage(force=self._last_usage_scan <= 0.0) or changed

    def resolve_rollout(self) -> Path | None:
        if self.fixed_rollout_path is not None:
            return self.fixed_rollout_path if self.fixed_rollout_path.exists() else None
        if self.thread_id:
            return find_rollout_by_thread_id(self.sessions_dir, self.thread_id)
        candidate = latest_user_rollout(
            self.sessions_dir,
            current_path=self.rollout_path,
            switch_margin_seconds=ROLLOUT_SWITCH_ACTIVITY_MARGIN_SECONDS,
        )
        if candidate is None or self.rollout_path is None or candidate == self.rollout_path:
            return candidate
        current_activity = scan_rollout_activity(self.rollout_path)
        candidate_activity = scan_rollout_activity(candidate)
        if (
            current_activity is not None
            and current_activity.active
            and candidate_activity is not None
            and candidate_activity.active
        ):
            return self.rollout_path
        if time.monotonic() - self._rollout_selected_at >= ROLLOUT_SWITCH_MIN_DWELL_SECONDS:
            return candidate
        return candidate

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
            self.set_status("System", "connected", "Desktop observer connected")
            return True

        if event_type == "turn_context":
            turn_id = payload.get("turn_id")
            if not self.mark_active_work():
                return False
            if turn_id:
                self.state.active_turn_id = str(turn_id)
            self.set_status("Codex", "working", "Working")
            return True

        if event_type == "response_item":
            return self.apply_response_item(payload)

        if event_type != "event_msg":
            return False

        return self.apply_event_payload(payload)

    def apply_response_item(self, payload: dict[str, Any]) -> bool:
        payload_type = payload.get("type")
        if payload_type in {"function_call", "custom_tool_call"}:
            if not self.mark_active_work():
                return False
            name = clean_tool_name(payload.get("name"))
            self.set_status("Tool", "started", name)
            self.add_activity("Tool", "started", name)
            return True
        if payload_type == "function_call_output":
            if not self.mark_active_work():
                return False
            self.set_status("Tool", "output", "output ready")
            self.add_activity("Tool", "output", "output ready")
            return True
        if payload_type == "reasoning":
            if self.mark_active_work():
                self.set_status("Codex", "thinking", "Thinking")
                return True
            return False
        if payload_type == "message":
            if self.mark_active_work():
                self.set_status("Codex", "replying", "Replying")
                return True
            return False
        return False

    def apply_event_payload(self, payload: dict[str, Any]) -> bool:
        payload_type = payload.get("type")

        if payload_type == "task_started":
            turn_id = payload.get("turn_id")
            if not self.mark_active_work():
                return False
            if turn_id:
                self.state.active_turn_id = str(turn_id)
            self.state.last_message = "Codex working"
            self.set_status("Codex", "working", "Working")
            self.add_activity("System", "started", "Turn started")
            return True

        if payload_type in TERMINAL_EVENT_PAYLOAD_TYPES:
            self.clear_active_work()
            summary = payload.get("last_agent_message") or "Turn completed"
            completed = payload_type == "task_complete"
            self.state.last_message = "Turn completed" if completed else "Turn stopped"
            self.set_status("Codex", "completed" if completed else "stopped", self.state.last_message)
            self.add_activity("Codex", "completed" if completed else "stopped", summary if completed else self.state.last_message)
            return True

        if payload_type == "agent_message":
            message = payload.get("message")
            if isinstance(message, str) and message.strip():
                if not self.mark_active_work():
                    return False
                self.state.last_message = short_text(message, 80)
                self.set_status("Codex", "message", message)
                self.add_activity("Codex", "message", message)
                return True
            return False

        if payload_type == "user_message":
            if not self.mark_active_work():
                return False
            message = payload.get("message")
            self.state.last_message = "User message"
            self.set_status("User", "message", message or "User message")
            self.add_activity("User", "message", message or "User message")
            return True

        if payload_type == "token_count":
            return self.apply_usage(usage_from_token_count_payload(payload, event_ts=self.state.last_event_ts))

        if payload_type == "patch_apply_end":
            if not self.mark_active_work():
                return False
            success = payload.get("success")
            self.state.last_message = "Patch applied" if success else "Patch failed"
            text = "Patch applied" if success else "Patch failed"
            self.set_status("Tool", "patch", text)
            self.add_activity("Tool", "patch", text)
            return True

        return False

    def mark_active_work(self) -> bool:
        event_ts = self.state.last_event_ts
        if event_ts is not None and time.time() - event_ts > RECENT_WORK_EVENT_MAX_AGE_SECONDS:
            return False
        self.state.active = True
        self._active_work_deadline = max(
            self._active_work_deadline,
            time.monotonic() + ACTIVE_WORK_WATCHDOG_SECONDS,
        )
        return True

    def mark_recent_work(self) -> bool:
        return self.mark_active_work()

    def clear_active_work(self) -> None:
        self.state.active = False
        self.state.active_turn_id = None
        self._active_work_deadline = 0.0

    def clear_recent_work(self) -> None:
        self.clear_active_work()

    def expire_transient_work_status(self) -> bool:
        if self.is_working():
            return False
        changed = False
        if self.state.active or self.state.active_turn_id is not None or self._active_work_deadline > 0:
            self.clear_active_work()
            changed = True
        status = self.state.status if isinstance(self.state.status, dict) else {}
        kind = str(status.get("kind") or "").lower()
        if kind not in TRANSIENT_WORK_STATUS_KINDS:
            return changed
        self.set_status("Codex", "idle", "Idle")
        return True

    def is_working(self, now: float | None = None) -> bool:
        if not self.state.active:
            return False
        if self._active_work_deadline <= 0:
            return True
        current = time.monotonic() if now is None else now
        return current < self._active_work_deadline

    def build_snapshot(self, activity: tuple[dict[str, Any], ...] | None = None) -> Snapshot:
        return self.state.snapshot(activity=activity, working=self.is_working())

    def apply_usage(self, usage: DesktopUsageSnapshot | None) -> bool:
        if usage is None or not usage.has_usage:
            return False
        if (
            usage.event_ts is not None
            and self.state.usage_event_ts is not None
            and usage.event_ts < self.state.usage_event_ts
        ):
            return False
        changed = False
        if usage.tokens_total is not None and usage.tokens_total != self.state.tokens_total:
            self.state.tokens_total = usage.tokens_total
            changed = True
        if usage.rate_limits is not None and usage.rate_limits != self.state.rate_limits:
            self.state.rate_limits = usage.rate_limits
            changed = True
        if usage.event_ts is not None and (self.state.usage_event_ts is None or usage.event_ts > self.state.usage_event_ts):
            self.state.usage_event_ts = usage.event_ts
        return changed

    def refresh_account_usage(self, *, force: bool = False) -> bool:
        now = time.monotonic()
        if not force and now - self._last_usage_scan < USAGE_REFRESH_INTERVAL_SECONDS:
            return False
        self._last_usage_scan = now
        return self.apply_usage(latest_account_usage(self.sessions_dir, cache=self._usage_file_cache))

    def reset_device_send_state(self) -> None:
        if not hasattr(self.device, "all_targets"):
            return
        for target in self.device.all_targets():
            target.last_wire_payload = None
            target.last_activity_seq_sent = 0
            target.initial_sync_needed = False
            if hasattr(target, "initial_full_sync_pending"):
                target.initial_full_sync_pending = False

    def device_detail_mode(self, target: Any | None = None) -> int:
        source = target if target is not None else self.device
        device_status = getattr(source, "last_status_ack", None)
        if not isinstance(device_status, dict):
            return DETAIL_FULL
        settings = device_status.get("settings")
        if not isinstance(settings, dict):
            return DETAIL_FULL
        detail = settings.get("detail")
        if isinstance(detail, int) and detail in {DETAIL_FULL, DETAIL_STATUS, DETAIL_USAGE}:
            return detail
        return DETAIL_FULL

    @staticmethod
    def sanitize_status(status: dict[str, Any] | None, *, detail: int, working: bool) -> dict[str, str] | None:
        if not isinstance(status, dict):
            return None

        speaker = short_text(status.get("speaker") or "Codex", 16)
        kind = short_text(status.get("kind") or "status", 24)
        text = short_text(status.get("text") or kind, 96)
        if detail == DETAIL_FULL:
            return {"speaker": speaker, "kind": kind, "text": text}

        normalized_kind = kind.lower()
        normalized_speaker = speaker.lower()
        if detail == DETAIL_STATUS:
            if normalized_speaker == "user":
                text = "User message"
            elif normalized_speaker == "codex" and normalized_kind == "message":
                text = "Codex message"
            elif normalized_speaker == "system" and normalized_kind in {"connected", "disconnected", "error"}:
                text = text
            elif normalized_speaker == "tool":
                text = short_text(text or "Tool activity", 40)
            elif working:
                text = "Working"
            return {"speaker": speaker, "kind": kind, "text": text}

        if working:
            return {"speaker": "Codex", "kind": "working", "text": "Working"}
        if normalized_kind in {"completed", "complete", "task_complete"}:
            return {"speaker": "Codex", "kind": "completed", "text": "Turn completed"}
        if normalized_kind == "error":
            return {"speaker": "System", "kind": "error", "text": "Bridge error"}
        if normalized_kind == "disconnected":
            return {"speaker": "System", "kind": "disconnected", "text": "Disconnected"}
        if normalized_kind == "connected":
            return {"speaker": "System", "kind": "connected", "text": "Connected"}
        return {"speaker": "Codex", "kind": "idle", "text": "Idle"}

    @staticmethod
    def sanitize_codex_activity(activity: Any, *, detail: int, working: bool) -> Any:
        if detail == DETAIL_FULL or activity is None:
            return activity
        wire = activity.to_wire() if hasattr(activity, "to_wire") else dict(activity)
        status = str(wire.get("status") or ("running" if working else "idle"))
        subtitle = str(wire.get("subtitle") or "")
        if detail == DETAIL_STATUS:
            if status == "running":
                subtitle = "Working"
            elif status == "waiting":
                subtitle = "Waiting"
            elif status == "failed":
                subtitle = "Bridge error"
            elif status == "review":
                subtitle = "Ready for review"
            else:
                subtitle = "Idle"
        else:
            subtitle = "Working" if working else "Usage synced"
        return codex_activity(
            status,
            title=wire.get("title") or "Codex",
            subtitle=subtitle,
            waiting_kind=wire.get("waiting_kind"),
            updated_at=wire.get("updated_at"),
            thread_label=wire.get("thread_label"),
            project_label=wire.get("project_label"),
        )

    def snapshot_for_detail(self, snapshot: Snapshot, *, detail: int, working: bool) -> Snapshot:
        if detail == DETAIL_FULL:
            return snapshot
        status = self.sanitize_status(snapshot.status, detail=detail, working=working)
        msg = status["text"] if status else ("Working" if working else "Usage synced")
        current_activity = self.sanitize_codex_activity(snapshot.codex_activity, detail=detail, working=working)
        return Snapshot(
            total=snapshot.total,
            running=snapshot.running,
            waiting=snapshot.waiting,
            msg=msg,
            entries=(),
            status=status,
            codex_activity=current_activity,
            activity=(),
            tokens=snapshot.tokens,
            tokens_today=snapshot.tokens_today,
            remaining_pct=snapshot.remaining_pct,
            rate_limits=snapshot.rate_limits,
            legacy_text=False,
        )

    def initial_sync_snapshot(self, *, detail: int | None = None) -> Snapshot:
        working = self.is_working()
        detail = self.device_detail_mode() if detail is None else detail
        base = self.state.snapshot(activity=(), working=working)
        status = self.sanitize_status(base.status, detail=detail, working=working)
        if status is None:
            status = {
                "speaker": "Codex" if working else "System",
                "kind": "working" if working else "sync",
                "text": "Working" if working else "Syncing",
            }
        current_activity = self.sanitize_codex_activity(base.codex_activity, detail=detail, working=working)
        if current_activity is None:
            current_activity = codex_activity(
                "running" if working else "idle",
                title="Codex",
                subtitle="Working" if working else "Syncing",
                updated_at=self.state.last_event_ts,
            )
        return Snapshot(
            total=base.total,
            running=base.running,
            waiting=base.waiting,
            msg=status["text"],
            entries=(),
            status=status,
            codex_activity=current_activity,
            activity=(),
            tokens=base.tokens,
            tokens_today=base.tokens_today,
            remaining_pct=base.remaining_pct,
            rate_limits=base.rate_limits,
            legacy_text=False,
        )

    def set_status(self, speaker: str, kind: str, text: Any) -> None:
        clean = short_text(text, 96)
        self.state.status = {
            "speaker": short_text(speaker or "Codex", 16),
            "kind": short_text(kind or "status", 24),
            "text": clean,
        }
        self.state.activity_updated_at = self.state.last_event_ts or time.time()
        self.state.last_message = clean

    def add_activity(self, speaker: str, kind: str, value: Any) -> None:
        text = short_text(value, DESKTOP_ACTIVITY_TEXT_LIMIT)
        if text:
            self.state.activity.appendleft(
                {
                    "seq": f"d{self.state.next_seq}",
                    "speaker": short_text(speaker or "Codex", 16),
                    "kind": short_text(kind or "message", 24),
                    "text": text,
                }
            )
            self.state.next_seq += 1

    @staticmethod
    def activity_seq_value(item: dict[str, Any]) -> int:
        seq = str(item.get("seq") or "")
        if seq.startswith("d") and seq[1:].isdigit():
            return int(seq[1:])
        return 0

    def pending_activity(self) -> tuple[dict[str, Any], ...]:
        ordered = tuple(reversed(self.state.activity))
        if self._last_activity_seq_sent <= 0:
            return ordered
        return tuple(item for item in ordered if self.activity_seq_value(item) > self._last_activity_seq_sent)

    def pending_activity_for_target(self, target: Any) -> tuple[dict[str, Any], ...]:
        ordered = tuple(reversed(self.state.activity))
        last_sent = int(getattr(target, "last_activity_seq_sent", 0) or 0)
        if last_sent <= 0:
            return ordered
        return tuple(item for item in ordered if self.activity_seq_value(item) > last_sent)

    async def mark_device_send_failure(self, exc: Exception) -> None:
        LOGGER.warning("StickS3 send failed: %s", exc)
        self._device_connected = False
        self._device_state = "disconnected"
        self._device_error = str(exc)
        await self.device.close()
        self.write_status(self._device_state, detail="StickS3 disconnected", error=str(exc))

    async def mark_target_send_failure(self, target: Any, exc: Exception) -> None:
        LOGGER.warning("Companion send failed for %s: %s", getattr(target, "label", "device"), exc)
        target.state = "disconnected"
        target.error = str(exc)
        if hasattr(target, "initial_sync_needed"):
            target.initial_sync_needed = False
        if hasattr(target, "initial_full_sync_pending"):
            target.initial_full_sync_pending = False
        await target.close()
        self.write_status(getattr(self.device, "aggregate_state", lambda: "disconnected")(), detail="Companion disconnected", error=str(exc))

    async def send_initial_sync_snapshot(self) -> bool:
        if hasattr(self.device, "all_targets"):
            return await self.send_initial_sync_to_fleet()

        async with self._snapshot_lock:
            working = self.is_working()
            snapshot = self.initial_sync_snapshot()
            if not self._device_connected:
                self._last_sent_working = working
                self.write_status(self._device_state, detail=self.state.last_message, error=self._device_error)
                self._last_sent = time.monotonic()
                return False
            try:
                await self.device.send_snapshot(snapshot)
            except Exception as exc:
                await self.mark_device_send_failure(exc)
                return False
            self._last_sent_working = working
            self._last_sent = time.monotonic()
            self.write_status(self._device_state, detail="Initial sync sent")
            return True

    async def send_initial_sync_to_fleet(self) -> bool:
        async with self._snapshot_lock:
            await self.refresh_observed_state(force_usage=True)
            sent = False
            for target in self.device.connected_targets():
                detail = self.device_detail_mode(target)
                snapshot = self.initial_sync_snapshot(detail=detail)
                try:
                    await target.send_snapshot(snapshot)
                    await target.request_status()
                except Exception as exc:
                    await self.mark_target_send_failure(target, exc)
                    continue
                target.last_wire_payload = snapshot.to_wire()
                target.initial_sync_needed = False
                sent = True
            self._last_sent_working = self.is_working()
            self._last_sent = time.monotonic()
            self.write_status(getattr(self.device, "aggregate_state", lambda: "disconnected")(), detail="Initial sync sent" if sent else self.state.last_message)
            return sent

    async def send_snapshot(self, *, force: bool = False) -> None:
        if hasattr(self.device, "all_targets"):
            await self.send_snapshot_to_fleet(force=force)
            return

        async with self._snapshot_lock:
            activity = self.pending_activity()
            working = self.is_working()
            detail = self.device_detail_mode()
            snapshot = self.snapshot_for_detail(self.state.snapshot(activity=activity, working=working), detail=detail, working=working)
            wire_payload = snapshot.to_wire()
            self._last_sent_working = working
            if not self._device_connected:
                self.write_status(self._device_state, detail=self.state.last_message, error=self._device_error)
                self._last_sent = time.monotonic()
                return
            if not force and wire_payload == self._last_wire_payload:
                self._last_sent_working = working
                self.write_status(self._device_state, detail=self.state.last_message)
                return
            try:
                await self.device.send_snapshot(snapshot)
            except Exception as exc:
                await self.mark_device_send_failure(exc)
                return
            if activity:
                self._last_activity_seq_sent = max(self.activity_seq_value(item) for item in activity)
            await asyncio.sleep(0.05)
            self._last_sent = time.monotonic()
            self._last_wire_payload = wire_payload
            self.write_status(self._device_state, detail=self.state.last_message)

    async def send_snapshot_to_fleet(self, *, force: bool = False) -> None:
        async with self._snapshot_lock:
            working = self.is_working()
            sent_any = False
            for target in self.device.connected_targets():
                activity = self.pending_activity_for_target(target)
                detail = self.device_detail_mode(target)
                snapshot = self.snapshot_for_detail(self.state.snapshot(activity=activity, working=working), detail=detail, working=working)
                wire_payload = snapshot.to_wire()
                if not force and not target.initial_sync_needed and wire_payload == target.last_wire_payload:
                    continue
                try:
                    if target.initial_sync_needed:
                        initial = self.initial_sync_snapshot(detail=detail)
                        initial_wire_payload = initial.to_wire()
                        await target.send_snapshot(initial)
                        target.initial_sync_needed = False
                        target.last_wire_payload = initial_wire_payload
                        if self.initial_sync_covers_snapshot(initial_wire_payload, wire_payload):
                            target.last_wire_payload = wire_payload
                            if hasattr(target, "initial_full_sync_pending"):
                                target.initial_full_sync_pending = False
                            sent_any = True
                            continue
                        if hasattr(target, "initial_full_sync_pending"):
                            target.initial_full_sync_pending = True
                        await asyncio.sleep(0.08)
                    await target.send_snapshot(snapshot)
                except Exception as exc:
                    await self.mark_target_send_failure(target, exc)
                    continue
                if activity:
                    target.last_activity_seq_sent = max(self.activity_seq_value(item) for item in activity)
                target.last_wire_payload = wire_payload
                if hasattr(target, "initial_full_sync_pending"):
                    target.initial_full_sync_pending = False
                sent_any = True
            self._last_sent_working = working
            self._last_sent = time.monotonic()
            self.write_status(getattr(self.device, "aggregate_state", lambda: "disconnected")(), detail=self.state.last_message)
            if sent_any:
                await asyncio.sleep(0.05)

    @staticmethod
    def initial_sync_covers_snapshot(initial_wire_payload: dict[str, Any], wire_payload: dict[str, Any]) -> bool:
        if wire_payload.get("activity"):
            return False
        compact_payload = dict(wire_payload)
        compact_payload.pop("msg", None)
        compact_payload.pop("entries", None)
        compact_payload.pop("activity", None)
        return compact_payload == initial_wire_payload

    async def handle_device_controls(self) -> None:
        while True:
            message = await self.device.wait_for_control()
            LOGGER.info("Ignoring StickS3 control in desktop-observer mode: %s", message)
            self.state.last_message = "Observer is read-only"
            await self.send_snapshot()

    def write_status(self, state: str, *, detail: str | None = None, error: str | None = None) -> None:
        if self.status_file is None:
            return

        fleet_summary = getattr(self.device, "status_summary", lambda: None)()
        if isinstance(fleet_summary, dict):
            state = str(fleet_summary.get("state") or state)
            if error is None:
                error = fleet_summary.get("device_error")
        self._device_state = state
        self._device_error = error
        current_activity = self.state.snapshot(activity=(), working=self.is_working()).codex_activity
        current_activity_wire = current_activity.to_wire() if hasattr(current_activity, "to_wire") else current_activity
        codex_state = codex_state_from_activity(current_activity_wire)
        payload: dict[str, Any] = {
            "state": state,
            "updated_at": datetime.now().astimezone().isoformat(timespec="seconds"),
            "pid": os.getpid(),
            "bridge_state": "running",
            "supervisor_state": "running",
            "supervisor_pid": os.getpid(),
            "mode": codex_state,
            "menu_mode": codex_state,
            "codex_state": codex_state,
            "device_state": self._device_state,
            "device_error": self._device_error,
            "detail": detail,
            "error": error,
            "rollout": str(self.rollout_path) if self.rollout_path else None,
            "thread_id": self.state.thread_id,
            "thread_name": self.state.thread_name,
            "active": self.is_working(),
            "task_active": self.state.active,
            "detail_mode": self.device_detail_mode(),
            "last_message": self.state.last_message,
            "status": self.state.status,
            "codex_activity": current_activity_wire,
            "tokens": self.state.tokens_total,
            "rate_limits": self.state.rate_limits,
        }
        if isinstance(fleet_summary, dict):
            payload["device_count"] = fleet_summary.get("device_count", 0)
            payload["connected_device_count"] = fleet_summary.get("connected_device_count", 0)
            payload["devices"] = fleet_summary.get("devices", [])
        device_status = getattr(self.device, "last_status_ack", None)
        if isinstance(device_status, dict):
            payload["device"] = device_status

        try:
            self.status_file.parent.mkdir(parents=True, exist_ok=True)
            temp_path = self.status_file.with_suffix(self.status_file.suffix + ".tmp")
            temp_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            temp_path.replace(self.status_file)
        except OSError as exc:
            LOGGER.debug("Could not write status file %s: %s", self.status_file, exc)

    def codex_state(self) -> str:
        return codex_state_from_activity(self.state.snapshot(activity=(), working=self.is_working()).codex_activity)
