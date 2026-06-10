import unittest
from typing import Any

from sticks3_bridge.app_server import AppServerTransport, CodexAppServerBridge
from sticks3_bridge.device import FakeStickS3Device


class DummyTransport(AppServerTransport):
    async def send(self, message: dict[str, Any]) -> None:
        return None

    async def receive(self) -> dict[str, Any]:
        raise EOFError("dummy transport has no messages")

    async def close(self) -> None:
        return None


class AppServerBridgeTests(unittest.IsolatedAsyncioTestCase):
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

    async def test_goal_cleared_reaches_snapshot(self) -> None:
        device = FakeStickS3Device()
        bridge = CodexAppServerBridge(transport=DummyTransport(), device=device)

        await bridge.handle_notification({"method": "thread/goal/cleared", "params": {"threadId": "thread-1"}})

        self.assertEqual({"available": False}, device.snapshots[-1].to_wire()["goal"])


if __name__ == "__main__":
    unittest.main()
