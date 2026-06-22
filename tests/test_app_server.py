import unittest
from typing import Any

from sticks3_bridge.app_server import AppServerTransport, CodexAppServerBridge
from sticks3_bridge.device import FakeStickS3Device
from sticks3_bridge.protocol import COMMAND_APPROVAL_METHOD, TOOL_REQUEST_USER_INPUT_METHOD


class DummyTransport(AppServerTransport):
    async def send(self, message: dict[str, Any]) -> None:
        return None

    async def receive(self) -> dict[str, Any]:
        raise EOFError("dummy transport has no messages")

    async def close(self) -> None:
        return None


class RecordingTransport(DummyTransport):
    def __init__(self) -> None:
        self.sent: list[dict[str, Any]] = []

    async def send(self, message: dict[str, Any]) -> None:
        self.sent.append(message)


class AppServerBridgeTests(unittest.IsolatedAsyncioTestCase):
    async def test_command_approval_request_uses_interaction_snapshot(self) -> None:
        device = FakeStickS3Device(auto_decision="session")
        transport = RecordingTransport()
        bridge = CodexAppServerBridge(transport=transport, device=device)

        await bridge.handle_server_request(
            {
                "id": "req-1",
                "method": COMMAND_APPROVAL_METHOD,
                "params": {"command": "git status", "threadId": "thread-1", "turnId": "turn-1"},
            }
        )

        prompt_wire = device.snapshots[0].to_wire()
        self.assertEqual("approval", prompt_wire["interaction"]["kind"])
        self.assertEqual("waiting", prompt_wire["codex_activity"]["status"])
        self.assertEqual("exec", prompt_wire["codex_activity"]["waiting_kind"])
        self.assertEqual(["once", "session", "deny", "cancel"], [option["id"] for option in prompt_wire["interaction"]["options"]])
        self.assertEqual({"decision": "acceptForSession"}, transport.sent[-1]["result"])

    async def test_tool_user_input_request_uses_choice_interaction(self) -> None:
        device = FakeStickS3Device(auto_decision="once")
        transport = RecordingTransport()
        bridge = CodexAppServerBridge(transport=transport, device=device)

        await bridge.handle_server_request(
            {
                "id": "choice-1",
                "method": TOOL_REQUEST_USER_INPUT_METHOD,
                "params": {
                    "threadId": "thread-1",
                    "turnId": "turn-1",
                    "itemId": "item-1",
                    "questions": [
                        {
                            "header": "Mode",
                            "id": "mode",
                            "question": "Pick one",
                            "options": [
                                {"label": "Fast", "description": "Move quickly"},
                                {"label": "Careful", "description": "Move carefully"},
                            ],
                        }
                    ],
                },
            }
        )

        prompt_wire = device.snapshots[0].to_wire()
        self.assertEqual("choice", prompt_wire["interaction"]["kind"])
        self.assertEqual("mode", prompt_wire["interaction"]["question_id"])
        self.assertEqual("waiting", prompt_wire["codex_activity"]["status"])
        self.assertEqual("question", prompt_wire["codex_activity"]["waiting_kind"])
        self.assertEqual({"answers": {"mode": {"answers": ["once"]}}}, transport.sent[-1]["result"])

    async def test_plan_and_goal_notifications_reach_snapshot(self) -> None:
        device = FakeStickS3Device()
        bridge = CodexAppServerBridge(transport=DummyTransport(), device=device)

        await bridge.handle_notification(
            {
                "method": "turn/plan/updated",
                "params": {
                    "threadId": "thread-1",
                    "turnId": "turn-1",
                    "plan": [
                        {"status": "completed", "step": "Inspect repo"},
                        {"status": "inProgress", "step": "Implement plan page"},
                    ],
                },
            }
        )

        await bridge.handle_notification(
            {
                "method": "thread/goal/updated",
                "params": {
                    "threadId": "thread-1",
                    "goal": {
                        "threadId": "thread-1",
                        "objective": "Ship the StickS3 companion",
                        "status": "active",
                        "timeUsedSeconds": 120,
                        "tokensUsed": 500,
                        "updatedAt": 0,
                        "createdAt": 0,
                    },
                },
            }
        )

        wire = device.snapshots[-1].to_wire()
        self.assertEqual("Implement plan page", wire["plan"]["step"])
        self.assertEqual("Ship the StickS3 companion", wire["goal"]["objective"])

    async def test_turn_lifecycle_maps_to_codex_activity(self) -> None:
        device = FakeStickS3Device()
        bridge = CodexAppServerBridge(transport=DummyTransport(), device=device)

        await bridge.handle_notification(
            {
                "method": "turn/started",
                "params": {"threadId": "thread-1", "turn": {"id": "turn-1"}},
            }
        )
        self.assertEqual("running", device.snapshots[-1].to_wire()["codex_activity"]["status"])

        await bridge.handle_notification(
            {
                "method": "turn/completed",
                "params": {"threadId": "thread-1", "turnId": "turn-1"},
            }
        )
        self.assertEqual("review", device.snapshots[-1].to_wire()["codex_activity"]["status"])

    async def test_error_notification_maps_to_failed_codex_activity(self) -> None:
        device = FakeStickS3Device()
        bridge = CodexAppServerBridge(transport=DummyTransport(), device=device)

        await bridge.handle_notification(
            {
                "method": "error",
                "params": {"error": {"message": "Codex failed"}},
            }
        )

        wire = device.snapshots[-1].to_wire()
        self.assertEqual("failed", wire["codex_activity"]["status"])
        self.assertEqual("danger", wire["codex_activity"]["level"])
        self.assertEqual({"speaker": "Codex", "kind": "error", "text": "Codex failed"}, wire["status"])

    async def test_goal_cleared_reaches_snapshot(self) -> None:
        device = FakeStickS3Device()
        bridge = CodexAppServerBridge(transport=DummyTransport(), device=device)

        await bridge.handle_notification({"method": "thread/goal/cleared", "params": {"threadId": "thread-1"}})

        self.assertEqual({"available": False}, device.snapshots[-1].to_wire()["goal"])

    async def test_token_usage_notification_reaches_snapshot(self) -> None:
        device = FakeStickS3Device()
        bridge = CodexAppServerBridge(transport=DummyTransport(), device=device)

        await bridge.handle_notification(
            {
                "method": "thread/tokenUsage/updated",
                "params": {
                    "threadId": "thread-1",
                    "usage": {"total": {"totalTokens": 1234}},
                },
            }
        )

        self.assertEqual(1234, device.snapshots[-1].to_wire()["tokens"])


if __name__ == "__main__":
    unittest.main()
