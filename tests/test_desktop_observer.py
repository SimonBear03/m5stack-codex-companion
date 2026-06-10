import json
import os
import tempfile
import unittest
from pathlib import Path

from sticks3_bridge.desktop_observer import (
    DesktopObserverBridge,
    compact_desktop_rate_limits,
    latest_user_rollout,
)
from sticks3_bridge.device import FakeStickS3Device


def event_line(event: dict) -> str:
    return json.dumps(event, separators=(",", ":")) + "\n"


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

    async def test_poll_once_builds_desktop_snapshot_from_rollout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            rollout = Path(tmp) / "rollout-thread-1.jsonl"
            write_rollout(rollout, thread_id="thread-1", thread_source="user")
            with rollout.open("a", encoding="utf-8") as handle:
                handle.write(
                    event_line(
                        {
                            "timestamp": "2026-06-10T14:21:00.000Z",
                            "type": "event_msg",
                            "payload": {"type": "task_started", "turn_id": "turn-1"},
                        }
                    )
                )
                handle.write(
                    event_line(
                        {
                            "timestamp": "2026-06-10T14:21:01.000Z",
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
                            "timestamp": "2026-06-10T14:21:02.000Z",
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
            bridge = DesktopObserverBridge(device=device, rollout_path=rollout)

            self.assertTrue(await bridge.poll_once())
            wire = bridge.state.snapshot().to_wire()
            self.assertEqual(1, wire["total"])
            self.assertEqual(0, wire["running"])
            self.assertEqual("Turn completed", wire["msg"])
            self.assertEqual(1234, wire["tokens"])
            self.assertEqual("5h", wire["rate_limits"]["primary"]["label"])
            self.assertIn("Finished observer update", wire["entries"][0])


if __name__ == "__main__":
    unittest.main()
