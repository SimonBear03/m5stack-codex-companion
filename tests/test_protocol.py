import unittest

from sticks3_bridge.protocol import (
    COMMAND_APPROVAL_METHOD,
    CodexActivity,
    FILE_APPROVAL_METHOD,
    MAX_INTERACTION_OPTIONS,
    TOOL_REQUEST_USER_INPUT_METHOD,
    JsonLineDecoder,
    Snapshot,
    approval_prompt,
    approval_response,
    chunk_bytes,
    codex_activity,
    codex_state_from_activity,
    encode_json_line,
    interaction_response,
    normalize_goal,
    normalize_plan_update,
    normalize_rate_limits,
    request_interaction,
    short_text,
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

    def test_short_text_normalizes_terminal_punctuation(self) -> None:
        self.assertEqual('I\'m "ready" - done...', short_text("I\u2019m \u201cready\u201d \u2014 done\u2026"))

    def test_command_approval_prompt_and_response(self) -> None:
        prompt = approval_prompt(COMMAND_APPROVAL_METHOD, 7, {"command": "git push", "cwd": "/tmp/repo"})
        self.assertEqual("7", prompt["id"])
        self.assertEqual("Command", prompt["tool"])
        self.assertIn("git push", prompt["hint"])
        self.assertEqual({"decision": "accept"}, approval_response(COMMAND_APPROVAL_METHOD, "once"))
        self.assertEqual({"decision": "acceptForSession"}, approval_response(COMMAND_APPROVAL_METHOD, "session"))
        self.assertEqual({"decision": "decline"}, approval_response(COMMAND_APPROVAL_METHOD, "deny"))
        self.assertEqual({"decision": "cancel"}, approval_response(COMMAND_APPROVAL_METHOD, "cancel"))

    def test_file_approval_prompt_and_response(self) -> None:
        prompt = approval_prompt(FILE_APPROVAL_METHOD, "abc", {"grantRoot": "/tmp/repo"})
        self.assertEqual("abc", prompt["id"])
        self.assertEqual("Files", prompt["tool"])
        self.assertIn("/tmp/repo", prompt["hint"])
        self.assertEqual({"decision": "accept"}, approval_response(FILE_APPROVAL_METHOD, "once"))
        self.assertEqual({"decision": "acceptForSession"}, approval_response(FILE_APPROVAL_METHOD, "session"))

    def test_approval_interaction_normalizes_options(self) -> None:
        interaction = request_interaction(COMMAND_APPROVAL_METHOD, 8, {"command": "git status"})
        self.assertEqual("8", interaction["id"])
        self.assertEqual("approval", interaction["kind"])
        self.assertEqual(["once", "session", "deny", "cancel"], [option["id"] for option in interaction["options"]])

    def test_tool_user_input_choice_interaction_and_response(self) -> None:
        interaction = request_interaction(
            TOOL_REQUEST_USER_INPUT_METHOD,
            "choice-1",
            {
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
        )
        self.assertEqual("choice", interaction["kind"])
        self.assertFalse(interaction["handoff"])
        self.assertEqual("mode", interaction["question_id"])
        self.assertEqual(["Fast", "Careful"], [option["label"] for option in interaction["options"]])
        self.assertEqual(
            {"answers": {"mode": {"answers": ["Careful"]}}},
            interaction_response(
                TOOL_REQUEST_USER_INPUT_METHOD,
                interaction,
                {"cmd": "interaction", "id": "choice-1", "action": "submit", "value": "Careful"},
            ),
        )

    def test_tool_user_input_handoff_for_freeform_or_secret(self) -> None:
        interaction = request_interaction(
            TOOL_REQUEST_USER_INPUT_METHOD,
            "secret-1",
            {
                "questions": [
                    {
                        "header": "Token",
                        "id": "token",
                        "isSecret": True,
                        "question": "Enter token",
                        "options": None,
                    }
                ]
            },
        )
        self.assertTrue(interaction["handoff"])
        self.assertEqual("handoff", interaction["kind"])
        self.assertEqual("token", interaction["question_id"])
        self.assertEqual(
            {"answers": {"token": {"answers": []}}},
            interaction_response(
                TOOL_REQUEST_USER_INPUT_METHOD,
                interaction,
                {"cmd": "interaction", "id": "secret-1", "action": "handoff", "value": "handoff"},
            ),
        )

    def test_tool_user_input_options_match_device_capacity(self) -> None:
        interaction = request_interaction(
            TOOL_REQUEST_USER_INPUT_METHOD,
            "many-options",
            {
                "questions": [
                    {
                        "header": "Pick",
                        "id": "choice",
                        "question": "Pick one",
                        "options": [{"label": f"Option {index}"} for index in range(MAX_INTERACTION_OPTIONS + 3)],
                    }
                ]
            },
        )
        self.assertFalse(interaction["handoff"])
        self.assertEqual(MAX_INTERACTION_OPTIONS, len(interaction["options"]))
        self.assertEqual(f"Option {MAX_INTERACTION_OPTIONS - 1}", interaction["options"][-1]["label"])

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

    def test_snapshot_serializes_dashboard_status_and_activity(self) -> None:
        status = {"speaker": "Codex", "kind": "message", "text": "Running tests"}
        activity = ({"seq": "d1", "speaker": "Tool", "kind": "started", "text": "exec_command"},)
        wire = Snapshot(status=status, activity=activity).to_wire()
        self.assertEqual(status, wire["status"])
        self.assertEqual(list(activity), wire["activity"])
        self.assertIn("msg", wire)
        self.assertIn("entries", wire)

    def test_snapshot_serializes_codex_activity(self) -> None:
        activity = codex_activity(
            "waiting",
            title="thread-1",
            subtitle="Approve command",
            waiting_kind="exec",
            updated_at=1000,
            thread_label="thread-1",
            project_label="repo",
        )
        wire = Snapshot(codex_activity=activity).to_wire()

        self.assertEqual("codex_activity/v1", wire["codex_activity"]["schema"])
        self.assertEqual("waiting", wire["codex_activity"]["status"])
        self.assertEqual("warning", wire["codex_activity"]["level"])
        self.assertEqual("exec", wire["codex_activity"]["waiting_kind"])
        self.assertEqual(1000 + 86400, wire["codex_activity"]["expires_at"])
        self.assertEqual(0, wire["codex_activity"]["priority"])
        self.assertEqual("wait", codex_state_from_activity(activity))

    def test_codex_activity_normalizes_unknown_status(self) -> None:
        wire = CodexActivity(status="unknown", updated_at=10).to_wire()
        self.assertEqual("idle", wire["status"])
        self.assertNotIn("expires_at", wire)

    def test_snapshot_can_omit_legacy_text_fields(self) -> None:
        wire = Snapshot(status={"speaker": "Codex", "kind": "idle", "text": "Idle"}, legacy_text=False).to_wire()
        self.assertEqual({"speaker": "Codex", "kind": "idle", "text": "Idle"}, wire["status"])
        self.assertNotIn("msg", wire)
        self.assertNotIn("entries", wire)

    def test_snapshot_serializes_interaction(self) -> None:
        interaction = {"id": "req_1", "kind": "approval", "title": "Command"}
        self.assertEqual(interaction, Snapshot(interaction=interaction).to_wire()["interaction"])


if __name__ == "__main__":
    unittest.main()
