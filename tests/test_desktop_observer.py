import json
import os
import tempfile
import time
import unittest
from datetime import datetime, timezone
from pathlib import Path

from sticks3_bridge.desktop_observer import (
    DesktopObserverBridge,
    DesktopUsageSnapshot,
    compact_desktop_rate_limits,
    latest_account_usage,
    latest_user_rollout,
)
from sticks3_bridge.device import FakeStickS3Device


def event_line(event: dict) -> str:
    return json.dumps(event, separators=(",", ":")) + "\n"


def datetime_now_iso_for_test() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def write_rollout(path: Path, *, thread_id: str, thread_source: str = "user") -> None:
    source = "vscode" if thread_source == "user" else {"subagent": {"other": "guardian"}}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        event_line(
            {
                "timestamp": "2026-06-10T14:20:00.000Z",
                "type": "session_meta",
                "payload": {
                    "id": thread_id,
                    "cwd": "/tmp/project",
                    "originator": "Codex Desktop",
                    "source": source,
                    "thread_source": thread_source,
                },
            }
        ),
        encoding="utf-8",
    )


class StatusRequestFailingDevice(FakeStickS3Device):
    async def request_status(self) -> None:
        raise RuntimeError("status ack write failed")


class FakeFleetTarget:
    def __init__(self, device_id: str, *, detail: int = 0, connected: bool = True) -> None:
        self.device_id = device_id
        self.label = device_id
        self.state = "connected" if connected else "disconnected"
        self.error = None
        self.snapshots = []
        self.last_status_ack = {"settings": {"detail": detail}}
        self.last_wire_payload = None
        self.last_activity_seq_sent = 0
        self.initial_sync_needed = False

    def is_connected(self) -> bool:
        return self.state == "connected"

    async def send_snapshot(self, snapshot) -> None:
        self.snapshots.append(snapshot)

    async def request_status(self) -> None:
        return None

    async def close(self) -> None:
        self.state = "disconnected"

    def status(self) -> dict:
        return {"device_id": self.device_id, "label": self.label, "state": self.state, "error": self.error}


class FakeFleetDevice:
    def __init__(self, targets: list[FakeFleetTarget]) -> None:
        self.targets = targets
        self.always_connected = False

    async def connect(self) -> None:
        return None

    async def close(self) -> None:
        return None

    def all_targets(self) -> list[FakeFleetTarget]:
        return self.targets

    def connected_targets(self) -> list[FakeFleetTarget]:
        return [target for target in self.targets if target.is_connected()]

    def needs_initial_sync(self) -> bool:
        return any(target.initial_sync_needed for target in self.connected_targets())

    def aggregate_state(self) -> str:
        return "connected" if self.connected_targets() else "disconnected"

    def status_summary(self) -> dict:
        return {
            "state": self.aggregate_state(),
            "device_count": len(self.targets),
            "connected_device_count": len(self.connected_targets()),
            "devices": [target.status() for target in self.targets],
            "device_error": None,
        }

    async def wait_for_control(self) -> dict:
        raise RuntimeError("not used")


class DesktopObserverTests(unittest.IsolatedAsyncioTestCase):
    def test_desktop_rate_limits_normalize_codex_rollout_shape(self) -> None:
        normalized = compact_desktop_rate_limits(
            {
                "primary": {"used_percent": 27.0, "window_minutes": 300, "resets_at": 1781109888},
                "secondary": {"used_percent": 39.0, "window_minutes": 10080, "resets_at": 1781140478},
            }
        )

        self.assertIsNotNone(normalized)
        assert normalized is not None
        self.assertEqual("5h", normalized["primary"]["label"])
        self.assertEqual(73, normalized["primary"]["remaining_percent"])
        self.assertEqual("7d", normalized["secondary"]["label"])
        self.assertEqual(61, normalized["secondary"]["remaining_percent"])

    def test_latest_user_rollout_skips_newer_subagent_rollout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            user_rollout = root / "2026/06/09/rollout-user-thread.jsonl"
            subagent_rollout = root / "2026/06/10/rollout-subagent-thread.jsonl"
            write_rollout(user_rollout, thread_id="user-thread", thread_source="user")
            write_rollout(subagent_rollout, thread_id="subagent-thread", thread_source="subagent")
            os.utime(user_rollout, (1000, 1000))
            os.utime(subagent_rollout, (2000, 2000))

            self.assertEqual(user_rollout, latest_user_rollout(root))

    def test_latest_account_usage_reads_recent_token_count_across_rollouts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            idle_rollout = root / "2026/06/10/rollout-idle-thread.jsonl"
            usage_rollout = root / "2026/06/09/rollout-usage-thread.jsonl"
            write_rollout(idle_rollout, thread_id="idle-thread", thread_source="user")
            write_rollout(usage_rollout, thread_id="usage-thread", thread_source="user")
            with usage_rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": "2026-06-10T14:21:02.000Z",
                            "type": "event_msg",
                            "payload": {
                                "type": "token_count",
                                "info": {"total_token_usage": {"total_tokens": 5678}},
                                "rate_limits": {
                                    "primary": {"used_percent": 12.0, "window_minutes": 300},
                                    "secondary": {"used_percent": 25.0, "window_minutes": 10080},
                                },
                            },
                        }
                    )
                )
            os.utime(idle_rollout, (2000, 2000))
            os.utime(usage_rollout, (1000, 1000))

            usage = latest_account_usage(root)

            self.assertIsNotNone(usage)
            assert usage is not None
            self.assertEqual(5678, usage.tokens_total)
            self.assertEqual(88, usage.rate_limits["primary"]["remaining_percent"])
            self.assertEqual(75, usage.rate_limits["secondary"]["remaining_percent"])

    def test_latest_account_usage_cache_sees_appended_reset_event(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rollout = root / "2026/06/14/rollout-current-thread.jsonl"
            write_rollout(rollout, thread_id="current-thread", thread_source="user")
            with rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": "2026-06-14T05:00:00.000Z",
                            "type": "event_msg",
                            "payload": {
                                "type": "token_count",
                                "info": {"total_token_usage": {"total_tokens": 1000}},
                                "rate_limits": {"primary": {"used_percent": 87.0, "window_minutes": 300}},
                            },
                        }
                    )
                )

            cache = {}
            usage = latest_account_usage(root, cache=cache)

            self.assertIsNotNone(usage)
            assert usage is not None
            self.assertEqual(13, usage.rate_limits["primary"]["remaining_percent"])

            with rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": "2026-06-14T05:25:00.000Z",
                            "type": "event_msg",
                            "payload": {
                                "type": "token_count",
                                "info": {"total_token_usage": {"total_tokens": 1200}},
                                "rate_limits": {"primary": {"used_percent": 0.0, "window_minutes": 300}},
                            },
                        }
                    )
                )

            usage = latest_account_usage(root, cache=cache)

            self.assertIsNotNone(usage)
            assert usage is not None
            self.assertEqual(100, usage.rate_limits["primary"]["remaining_percent"])
            self.assertEqual(1200, usage.tokens_total)

    async def test_poll_once_builds_desktop_snapshot_from_rollout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rollout = Path(tmp) / "rollout-thread-1.jsonl"
            write_rollout(rollout, thread_id="thread-1", thread_source="user")
            with rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "event_msg",
                            "payload": {"type": "task_started", "turn_id": "turn-1"},
                        }
                    )
                )
                handle.write(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "response_item",
                            "payload": {"type": "function_call", "name": "functions.exec_command"},
                        }
                    )
                )
                handle.write(
                    event_line(
                        {
                            "timestamp": "2026-06-10T14:21:02.000Z",
                            "type": "event_msg",
                            "payload": {
                                "type": "token_count",
                                "info": {"total_token_usage": {"total_tokens": 1234}},
                                "rate_limits": {
                                    "primary": {"used_percent": 10.0, "window_minutes": 300},
                                },
                            },
                        }
                    )
                )
                handle.write(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "event_msg",
                            "payload": {
                                "type": "task_complete",
                                "turn_id": "turn-1",
                                "last_agent_message": "Finished observer update",
                            },
                        }
                    )
                )

            device = FakeStickS3Device()
            bridge = DesktopObserverBridge(device=device, sessions_dir=Path(tmp), rollout_path=rollout)

            self.assertTrue(await bridge.poll_once())
            wire = bridge.state.snapshot().to_wire()
            self.assertEqual(1, wire["total"])
            self.assertEqual(0, wire["running"])
            self.assertEqual("Turn completed", wire["msg"])
            self.assertEqual(1234, wire["tokens"])
            self.assertEqual("5h", wire["rate_limits"]["primary"]["label"])
            self.assertEqual({"speaker": "Codex", "kind": "completed", "text": "Turn completed"}, wire["status"])
            self.assertEqual("Codex", wire["activity"][-1]["speaker"])
            self.assertEqual("completed", wire["activity"][-1]["kind"])
            self.assertIn("Finished observer update", wire["activity"][-1]["text"])
            self.assertTrue(any(item["speaker"] == "Tool" and item["text"] == "exec_command" for item in wire["activity"]))
            self.assertIn("Finished observer update", wire["entries"][0])

    async def test_status_file_is_written_for_menu_bar_helpers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rollout = root / "rollout-thread-2.jsonl"
            status_file = root / "runtime" / "bridge-status.json"
            write_rollout(rollout, thread_id="thread-2", thread_source="user")
            with rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "event_msg",
                            "payload": {"type": "task_started", "turn_id": "turn-2"},
                        }
                    )
                )

            device = FakeStickS3Device()
            bridge = DesktopObserverBridge(device=device, sessions_dir=root, rollout_path=rollout, status_file=status_file)

            self.assertTrue(await bridge.poll_once())
            await bridge.send_snapshot()

            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual("connected", status["state"])
            self.assertEqual("running", status["supervisor_state"])
            self.assertEqual(status["pid"], status["supervisor_pid"])
            self.assertEqual("work", status["mode"])
            self.assertEqual("work", status["codex_state"])
            self.assertEqual("connected", status["device_state"])
            self.assertEqual("thread-2", status["thread_id"])
            self.assertTrue(status["active"])
            self.assertEqual({"speaker": "Codex", "kind": "working", "text": "Working"}, status["status"])
            self.assertEqual("Working", status["detail"])

    async def test_status_file_updates_without_connected_device(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            status_file = Path(tmp) / "runtime" / "bridge-status.json"
            device = FakeStickS3Device()
            device.always_connected = False
            bridge = DesktopObserverBridge(device=device, status_file=status_file)
            bridge._device_connected = False
            bridge._device_state = "disconnected"
            bridge.state.thread_id = "thread-off-device"
            bridge.state.active = True
            bridge.set_status("Codex", "working", "Working")

            await bridge.send_snapshot()

            self.assertEqual([], device.snapshots)
            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual("running", status["bridge_state"])
            self.assertEqual("work", status["codex_state"])
            self.assertEqual("work", status["mode"])
            self.assertEqual("disconnected", status["device_state"])
            self.assertEqual("disconnected", status["state"])
            self.assertTrue(status["active"])
            self.assertEqual("thread-off-device", status["thread_id"])

    async def test_poll_once_seeds_usage_from_recent_rollout_when_current_thread_has_none(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            current_rollout = root / "2026/06/10/rollout-current-thread.jsonl"
            usage_rollout = root / "2026/06/09/rollout-usage-thread.jsonl"
            write_rollout(current_rollout, thread_id="current-thread", thread_source="user")
            write_rollout(usage_rollout, thread_id="usage-thread", thread_source="user")
            with usage_rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": "2026-06-10T14:21:02.000Z",
                            "type": "event_msg",
                            "payload": {
                                "type": "token_count",
                                "info": {"total_token_usage": {"total_tokens": 4321}},
                                "rate_limits": {
                                    "primary": {"used_percent": 40.0, "window_minutes": 300},
                                },
                            },
                        }
                    )
                )

            device = FakeStickS3Device()
            bridge = DesktopObserverBridge(device=device, sessions_dir=root, rollout_path=current_rollout)

            self.assertTrue(await bridge.poll_once())
            wire = bridge.build_snapshot().to_wire()
            self.assertEqual(4321, wire["tokens"])
            self.assertEqual(60, wire["rate_limits"]["primary"]["remaining_percent"])

    def test_older_usage_snapshot_does_not_overwrite_newer_usage(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)

        self.assertTrue(
            bridge.apply_usage(
                DesktopUsageSnapshot(
                    tokens_total=1200,
                    rate_limits={"primary": {"label": "5h", "remaining_percent": 100, "used_percent": 0}},
                    event_ts=200.0,
                )
            )
        )
        self.assertFalse(
            bridge.apply_usage(
                DesktopUsageSnapshot(
                    tokens_total=1000,
                    rate_limits={"primary": {"label": "5h", "remaining_percent": 13, "used_percent": 87}},
                    event_ts=100.0,
                )
            )
        )

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(1200, wire["tokens"])
        self.assertEqual(100, wire["rate_limits"]["primary"]["remaining_percent"])

    async def test_send_snapshot_only_sends_new_activity_after_first_snapshot(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-delta"
        bridge.add_activity("User", "message", "First message")
        bridge.add_activity("Codex", "message", "Second message")

        await bridge.send_snapshot()
        first_wire = device.snapshots[-1].to_wire()
        self.assertEqual(["First message", "Second message"], [item["text"] for item in first_wire["activity"]])

        await bridge.send_snapshot()
        second_wire = device.snapshots[-1].to_wire()
        self.assertNotIn("activity", second_wire)
        self.assertEqual({"speaker": "System", "kind": "connected", "text": "Desktop observer connected"}, second_wire["status"])

        bridge.add_activity("Codex", "message", "Third message")
        await bridge.send_snapshot()
        third_wire = device.snapshots[-1].to_wire()
        self.assertEqual(["Third message"], [item["text"] for item in third_wire["activity"]])

    async def test_connected_sync_sends_compact_snapshot_before_full_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rollout = root / "2026/06/14/rollout-thread-sync.jsonl"
            write_rollout(rollout, thread_id="thread-sync", thread_source="user")
            with rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "event_msg",
                            "payload": {"type": "task_started", "turn_id": "turn-sync"},
                        }
                    )
                )
                handle.write(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "event_msg",
                            "payload": {
                                "type": "agent_message",
                                "message": "Fresh reply status.",
                            },
                        }
                    )
                )
                handle.write(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "event_msg",
                            "payload": {
                                "type": "token_count",
                                "info": {"total_token_usage": {"total_tokens": 2468}},
                                "rate_limits": {"primary": {"used_percent": 30.0, "window_minutes": 300}},
                            },
                        }
                    )
                )

            device = FakeStickS3Device()
            bridge = DesktopObserverBridge(device=device, sessions_dir=root, rollout_path=rollout)
            bridge._device_connected = True
            bridge._device_state = "connected"

            await bridge.sync_connected_device()

            self.assertGreaterEqual(len(device.snapshots), 2)
            initial_wire = device.snapshots[0].to_wire()
            full_wire = device.snapshots[1].to_wire()
            self.assertEqual(1, initial_wire["running"])
            self.assertEqual(0, initial_wire["waiting"])
            self.assertEqual(2468, initial_wire["tokens"])
            self.assertEqual(70, initial_wire["rate_limits"]["primary"]["remaining_percent"])
            self.assertEqual({"speaker": "Codex", "kind": "message", "text": "Fresh reply status."}, initial_wire["status"])
            self.assertNotIn("msg", initial_wire)
            self.assertNotIn("entries", initial_wire)
            self.assertNotIn("activity", initial_wire)
            self.assertEqual(1, full_wire["running"])
            self.assertIn("activity", full_wire)
            self.assertTrue(any(item["text"] == "Fresh reply status." for item in full_wire["activity"]))

    async def test_first_sync_uses_fresh_work_state_after_connect(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            rollout = root / "2026/06/14/rollout-thread-fresh-work.jsonl"
            write_rollout(rollout, thread_id="thread-fresh-work", thread_source="user")
            device = FakeStickS3Device()
            bridge = DesktopObserverBridge(device=device, sessions_dir=root, rollout_path=rollout)

            self.assertTrue(await bridge.poll_once())
            self.assertEqual(0, bridge.build_snapshot().to_wire()["running"])

            with rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "response_item",
                            "payload": {"type": "reasoning", "summary": []},
                        }
                    )
                )

            bridge._device_connected = True
            bridge._device_state = "connected"
            await bridge.sync_connected_device()

            initial_wire = device.snapshots[0].to_wire()
            self.assertEqual(1, initial_wire["running"])
            self.assertEqual({"speaker": "Codex", "kind": "thinking", "text": "Thinking"}, initial_wire["status"])
            self.assertEqual("work", bridge.codex_state())

    async def test_request_status_failure_after_delivery_keeps_snapshot_sent(self) -> None:
        device = StatusRequestFailingDevice()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-status-request-fail"
        bridge.set_status("Codex", "idle", "Idle")

        await bridge.send_snapshot(force=True)

        self.assertEqual(1, len(device.snapshots))
        self.assertTrue(bridge._device_connected)
        self.assertEqual("connected", bridge._device_state)
        self.assertIsNotNone(bridge._last_wire_payload)
        self.assertEqual(device.snapshots[0].to_wire(), bridge._last_wire_payload)

    async def test_status_detail_suppresses_body_text_but_keeps_generic_status(self) -> None:
        device = FakeStickS3Device()
        device.last_status_ack = {"settings": {"detail": 1}}
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-status"
        bridge.set_status("Codex", "message", "Sensitive answer text")
        bridge.add_activity("Codex", "message", "Sensitive answer text")
        bridge.state.tokens_total = 999

        await bridge.send_snapshot()

        wire = device.snapshots[-1].to_wire()
        encoded = json.dumps(wire)
        self.assertNotIn("Sensitive answer text", encoded)
        self.assertNotIn("msg", wire)
        self.assertNotIn("entries", wire)
        self.assertNotIn("activity", wire)
        self.assertEqual({"speaker": "Codex", "kind": "message", "text": "Codex message"}, wire["status"])
        self.assertEqual(999, wire["tokens"])

    async def test_usage_detail_suppresses_specific_status_and_body_text(self) -> None:
        device = FakeStickS3Device()
        device.last_status_ack = {"settings": {"detail": 2}}
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-usage"
        bridge.state.active = True
        bridge.set_status("Tool", "started", "exec_command")
        bridge.add_activity("Codex", "message", "Sensitive answer text")
        bridge.state.tokens_total = 1000
        bridge.state.rate_limits = {"primary": {"label": "5h", "remaining_percent": 80}}

        await bridge.send_snapshot()

        wire = device.snapshots[-1].to_wire()
        encoded = json.dumps(wire)
        self.assertNotIn("Sensitive answer text", encoded)
        self.assertNotIn("exec_command", encoded)
        self.assertNotIn("msg", wire)
        self.assertNotIn("entries", wire)
        self.assertNotIn("activity", wire)
        self.assertEqual({"speaker": "Codex", "kind": "working", "text": "Working"}, wire["status"])
        self.assertEqual(1000, wire["tokens"])
        self.assertEqual(80, wire["rate_limits"]["primary"]["remaining_percent"])

    async def test_usage_detail_idle_status_is_not_usage_message(self) -> None:
        device = FakeStickS3Device()
        device.last_status_ack = {"settings": {"detail": 2}}
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-usage-idle"
        bridge.set_status("Tool", "output", "output ready")
        bridge.state.tokens_total = 1000
        bridge.state.rate_limits = {"primary": {"label": "5h", "remaining_percent": 80}}

        await bridge.send_snapshot()

        wire = device.snapshots[-1].to_wire()
        self.assertNotIn("msg", wire)
        self.assertNotIn("entries", wire)
        self.assertNotIn("activity", wire)
        self.assertEqual({"speaker": "Codex", "kind": "idle", "text": "Idle"}, wire["status"])

    async def test_detail_mode_skips_duplicate_sanitized_snapshots(self) -> None:
        device = FakeStickS3Device()
        device.last_status_ack = {"settings": {"detail": 2}}
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-skip"
        bridge.state.active = True
        bridge.set_status("Codex", "message", "First private message")
        bridge.add_activity("Codex", "message", "First private message")

        await bridge.send_snapshot()
        bridge.set_status("Codex", "message", "Second private message")
        bridge.add_activity("Codex", "message", "Second private message")
        await bridge.send_snapshot()

        self.assertEqual(1, len(device.snapshots))

    async def test_fleet_honors_detail_mode_per_device(self) -> None:
        full = FakeFleetTarget("full", detail=0)
        usage = FakeFleetTarget("usage", detail=2)
        bridge = DesktopObserverBridge(device=FakeFleetDevice([full, usage]))
        bridge.state.thread_id = "thread-fleet-detail"
        bridge.state.active = True
        bridge.set_status("Tool", "started", "exec_command")
        bridge.add_activity("Codex", "message", "Sensitive answer text")
        bridge.state.tokens_total = 1000

        await bridge.send_snapshot()

        full_wire = full.snapshots[-1].to_wire()
        usage_wire = usage.snapshots[-1].to_wire()
        self.assertIn("activity", full_wire)
        self.assertIn("Sensitive answer text", json.dumps(full_wire))
        self.assertNotIn("Sensitive answer text", json.dumps(usage_wire))
        self.assertNotIn("exec_command", json.dumps(usage_wire))
        self.assertNotIn("activity", usage_wire)

    async def test_fleet_disconnected_device_does_not_block_connected_device(self) -> None:
        connected = FakeFleetTarget("connected", detail=0)
        disconnected = FakeFleetTarget("disconnected", detail=0, connected=False)
        bridge = DesktopObserverBridge(device=FakeFleetDevice([connected, disconnected]))
        bridge.state.thread_id = "thread-fleet-connected"
        bridge.set_status("Codex", "idle", "Idle")

        await bridge.send_snapshot(force=True)

        self.assertEqual(1, len(connected.snapshots))
        self.assertEqual(0, len(disconnected.snapshots))

    async def test_reconnected_fleet_device_gets_activity_from_its_own_cursor(self) -> None:
        first = FakeFleetTarget("first", detail=0)
        second = FakeFleetTarget("second", detail=0, connected=False)
        bridge = DesktopObserverBridge(device=FakeFleetDevice([first, second]))
        bridge.state.thread_id = "thread-fleet-reconnect"
        bridge.set_status("Codex", "message", "First")
        bridge.add_activity("Codex", "message", "First activity")

        await bridge.send_snapshot(force=True)
        self.assertEqual(1, first.last_activity_seq_sent)
        self.assertEqual(0, second.last_activity_seq_sent)

        second.state = "connected"
        second.initial_sync_needed = True
        bridge.set_status("Codex", "message", "Second")
        bridge.add_activity("Codex", "message", "Second activity")

        await bridge.send_snapshot()

        second_wire = second.snapshots[-1].to_wire()
        texts = [item["text"] for item in second_wire["activity"]]
        self.assertEqual(["First activity", "Second activity"], texts)

    def test_work_like_activity_sets_running_without_task_started(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-work"

        self.assertTrue(
            bridge.apply_line(
                event_line(
                    {
                        "type": "response_item",
                        "payload": {"type": "function_call", "name": "functions.exec_command"},
                    }
                )
            )
        )

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(1, wire["running"])
        self.assertEqual({"speaker": "Tool", "kind": "started", "text": "exec_command"}, wire["status"])
        self.assertEqual("work", bridge.codex_state())

    def test_reasoning_response_item_sets_working_early(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-thinking"

        self.assertTrue(
            bridge.apply_line(
                event_line(
                    {
                        "timestamp": datetime_now_iso_for_test(),
                        "type": "response_item",
                        "payload": {"type": "reasoning", "summary": []},
                    }
                )
            )
        )

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(1, wire["running"])
        self.assertEqual({"speaker": "Codex", "kind": "thinking", "text": "Thinking"}, wire["status"])

    def test_reasoning_stays_working_past_old_short_grace(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-thinking"

        self.assertTrue(
            bridge.apply_line(
                event_line(
                    {
                        "timestamp": datetime_now_iso_for_test(),
                        "type": "response_item",
                        "payload": {"type": "reasoning", "summary": []},
                    }
                )
            )
        )

        self.assertTrue(bridge.is_working(time.monotonic() + 7.0))
        self.assertEqual(1, bridge.build_snapshot().to_wire()["running"])
        self.assertEqual("work", bridge.codex_state())

    def test_work_like_activity_expires_only_after_watchdog(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-work"
        bridge.mark_active_work()

        self.assertTrue(bridge.is_working())
        bridge._active_work_deadline = time.monotonic() - 0.1

        self.assertFalse(bridge.is_working())
        self.assertEqual(0, bridge.build_snapshot().to_wire()["running"])

    def test_abort_like_event_clears_active_work(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-abort"
        bridge.state.active = True

        self.assertTrue(
            bridge.apply_line(
                event_line(
                    {
                        "type": "event_msg",
                        "payload": {"type": "turn_aborted"},
                    }
                )
            )
        )

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(0, wire["running"])
        self.assertEqual({"speaker": "Codex", "kind": "stopped", "text": "Turn stopped"}, wire["status"])

    def test_task_complete_clears_recent_work_grace(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-complete"

        self.assertTrue(
            bridge.apply_line(
                event_line(
                    {
                        "type": "response_item",
                        "payload": {"type": "function_call", "name": "functions.exec_command"},
                    }
                )
            )
        )
        self.assertEqual(1, bridge.build_snapshot().to_wire()["running"])

        self.assertTrue(
            bridge.apply_line(
                event_line(
                    {
                        "type": "event_msg",
                        "payload": {"type": "task_complete", "last_agent_message": "Done"},
                    }
                )
            )
        )

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(0, wire["running"])
        self.assertEqual({"speaker": "Codex", "kind": "completed", "text": "Turn completed"}, wire["status"])

    def test_old_rollout_activity_does_not_replay_as_working_on_startup(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-old"

        self.assertFalse(
            bridge.apply_line(
                event_line(
                    {
                        "timestamp": "2026-01-01T00:00:00.000Z",
                        "type": "response_item",
                        "payload": {"type": "function_call", "name": "functions.exec_command"},
                    }
                )
            )
        )

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(0, wire["running"])

    def test_old_task_started_does_not_replay_as_working_on_startup(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-old-start"

        self.assertFalse(
            bridge.apply_line(
                event_line(
                    {
                        "timestamp": "2026-01-01T00:00:00.000Z",
                        "type": "event_msg",
                        "payload": {"type": "task_started", "turn_id": "old-turn"},
                    }
                )
            )
        )

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(0, wire["running"])
        self.assertFalse(bridge.state.active)

    def test_expired_work_status_resets_to_idle(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-expired-status"
        bridge.mark_active_work()
        bridge.set_status("Tool", "started", "exec_command")
        bridge._active_work_deadline = time.monotonic() - 0.1

        self.assertTrue(bridge.expire_transient_work_status())

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(0, wire["running"])
        self.assertEqual({"speaker": "Codex", "kind": "idle", "text": "Idle"}, wire["status"])

    def test_status_file_does_not_publish_work_status_with_idle_mode(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            status_file = Path(tmp) / "runtime" / "bridge-status.json"
            device = FakeStickS3Device()
            bridge = DesktopObserverBridge(device=device, status_file=status_file)
            bridge.state.thread_id = "thread-status-sync"

            self.assertTrue(
                bridge.apply_line(
                    event_line(
                        {
                            "timestamp": datetime_now_iso_for_test(),
                            "type": "response_item",
                            "payload": {"type": "function_call", "name": "functions.exec_command"},
                        }
                    )
                )
            )
            bridge.write_status("connected", detail=bridge.state.last_message)

            status = json.loads(status_file.read_text(encoding="utf-8"))
            self.assertEqual("work", status["codex_state"])
            self.assertEqual("work", status["menu_mode"])
            self.assertEqual("work", status["mode"])
            self.assertTrue(status["active"])
            self.assertTrue(status["task_active"])
            self.assertEqual({"speaker": "Tool", "kind": "started", "text": "exec_command"}, status["status"])

    def test_token_count_updates_usage_without_changing_work_status(self) -> None:
        device = FakeStickS3Device()
        bridge = DesktopObserverBridge(device=device)
        bridge.state.thread_id = "thread-token-count"

        self.assertTrue(
            bridge.apply_line(
                event_line(
                    {
                        "timestamp": datetime_now_iso_for_test(),
                        "type": "response_item",
                        "payload": {"type": "function_call", "name": "functions.exec_command"},
                    }
                )
            )
        )
        self.assertTrue(
            bridge.apply_line(
                event_line(
                    {
                        "timestamp": datetime_now_iso_for_test(),
                        "type": "event_msg",
                        "payload": {
                            "type": "token_count",
                            "info": {"total_token_usage": {"total_tokens": 1234}},
                            "rate_limits": {"primary": {"used_percent": 25.0, "window_minutes": 300}},
                        },
                    }
                )
            )
        )

        wire = bridge.build_snapshot().to_wire()
        self.assertEqual(1, wire["running"])
        self.assertEqual(1234, wire["tokens"])
        self.assertEqual({"speaker": "Tool", "kind": "started", "text": "exec_command"}, wire["status"])
        self.assertEqual("work", bridge.codex_state())


if __name__ == "__main__":
    unittest.main()
