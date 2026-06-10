import unittest

from sticks3_bridge.protocol import (
    COMMAND_APPROVAL_METHOD,
    FILE_APPROVAL_METHOD,
    JsonLineDecoder,
    Snapshot,
    approval_prompt,
    approval_response,
    chunk_bytes,
    encode_json_line,
    normalize_goal,
    normalize_plan_update,
    normalize_rate_limits,
)


class ProtocolTests(unittest.TestCase):
    def test_chunk_bytes_uses_ble_safe_default(self) -> None:
        chunks = chunk_bytes(b"x" * 41)
        self.assertEqual([20, 20, 1], [len(chunk) for chunk in chunks])

    def test_line_decoder_handles_split_messages(self) -> None:
        decoder = JsonLineDecoder()
        payload = encode_json_line({"cmd": "permission", "id": "1", "decision": "once"})
        self.assertEqual([], decoder.feed(payload[:8]))
        messages = decoder.feed(payload[8:])
        self.assertEqual([{"cmd": "permission", "id": "1", "decision": "once"}], messages)

    def test_command_approval_prompt_and_response(self) -> None:
        prompt = approval_prompt(COMMAND_APPROVAL_METHOD, 7, {"command": "git push", "cwd": "/tmp/repo"})
        self.assertEqual("7", prompt["id"])
        self.assertEqual("Command", prompt["tool"])
        self.assertIn("git push", prompt["hint"])
        self.assertEqual({"decision": "accept"}, approval_response(COMMAND_APPROVAL_METHOD, "once"))
        self.assertEqual({"decision": "decline"}, approval_response(COMMAND_APPROVAL_METHOD, "deny"))

    def test_file_approval_prompt_and_response(self) -> None:
        prompt = approval_prompt(FILE_APPROVAL_METHOD, "abc", {"grantRoot": "/tmp/repo"})
        self.assertEqual("abc", prompt["id"])
        self.assertEqual("Files", prompt["tool"])
        self.assertIn("/tmp/repo", prompt["hint"])
        self.assertEqual({"decision": "accept"}, approval_response(FILE_APPROVAL_METHOD, "once"))

    def test_rate_limits_normalize_5h_and_7d_windows(self) -> None:
        normalized = normalize_rate_limits(
            {
                "primary": {"usedPercent": 8, "windowDurationMins": 300, "resetsAt": 1781034181},
                "secondary": {"usedPercent": 31, "windowDurationMins": 10080, "resetsAt": 1781140479},
            }
        )
        self.assertEqual("5h", normalized["primary"]["label"])
        self.assertEqual(92, normalized["primary"]["remaining_percent"])
        self.assertEqual("7d", normalized["secondary"]["label"])
        self.assertEqual(69, normalized["secondary"]["remaining_percent"])

    def test_plan_update_selects_in_progress_step(self) -> None:
        normalized = normalize_plan_update(
            {
                "plan": [
                    {"status": "completed", "step": "Read the docs"},
                    {"status": "inProgress", "step": "Patch the firmware"},
                    {"status": "pending", "step": "Run tests"},
                ]
            }
        )
        self.assertTrue(normalized["available"])
        self.assertEqual("Patch the firmware", normalized["step"])
        self.assertEqual("inProgress", normalized["status"])
        self.assertEqual(1, normalized["completed"])
        self.assertEqual(3, normalized["total"])

    def test_plan_update_falls_back_to_pending_then_completed(self) -> None:
        pending = normalize_plan_update(
            {
                "plan": [
                    {"status": "completed", "step": "Inspect schema"},
                    {"status": "pending", "step": "Document the UI"},
                ]
            }
        )
        self.assertEqual("Document the UI", pending["step"])

        completed = normalize_plan_update(
            {
                "plan": [
                    {"status": "completed", "step": "Inspect schema"},
                    {"status": "completed", "step": "Document the UI"},
                ]
            }
        )
        self.assertEqual("Document the UI", completed["step"])

    def test_goal_normalization_and_clear_shape(self) -> None:
        normalized = normalize_goal(
            {
                "objective": "Implement the StickS3 Codex app companion",
                "status": "active",
                "timeUsedSeconds": 3661,
                "tokensUsed": 12345,
                "tokenBudget": 20000,
            }
        )
        self.assertTrue(normalized["available"])
        self.assertEqual("active", normalized["status"])
        self.assertEqual(3661, normalized["time_used_sec"])
        self.assertEqual(12345, normalized["tokens_used"])
        self.assertEqual(20000, normalized["token_budget"])

        self.assertEqual({"available": False}, normalize_goal(None))

    def test_snapshot_serializes_clear_goal_and_plan(self) -> None:
        wire = Snapshot(plan={"available": False}, goal={"available": False}).to_wire()
        self.assertEqual({"available": False}, wire["plan"])
        self.assertEqual({"available": False}, wire["goal"])


if __name__ == "__main__":
    unittest.main()
