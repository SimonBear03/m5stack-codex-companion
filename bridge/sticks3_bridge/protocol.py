from __future__ import annotations

import json
from collections.abc import Iterable
from dataclasses import dataclass
from typing import Any

NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

DEFAULT_BLE_CHUNK_SIZE = 20
MAX_INTERACTION_OPTIONS = 8

COMMAND_APPROVAL_METHOD = "item/commandExecution/requestApproval"
FILE_APPROVAL_METHOD = "item/fileChange/requestApproval"
TOOL_REQUEST_USER_INPUT_METHOD = "item/tool/requestUserInput"
MCP_ELICITATION_REQUEST_METHOD = "mcpServer/elicitation/request"
SUPPORTED_APPROVAL_METHODS = {COMMAND_APPROVAL_METHOD, FILE_APPROVAL_METHOD}
SUPPORTED_INTERACTION_METHODS = {
    COMMAND_APPROVAL_METHOD,
    FILE_APPROVAL_METHOD,
    TOOL_REQUEST_USER_INPUT_METHOD,
    MCP_ELICITATION_REQUEST_METHOD,
}


def compact_json(data: dict[str, Any]) -> str:
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


def encode_json_line(data: dict[str, Any]) -> bytes:
    return (compact_json(data) + "\n").encode("utf-8")


def chunk_bytes(data: bytes, size: int = DEFAULT_BLE_CHUNK_SIZE) -> list[bytes]:
    if size <= 0:
        raise ValueError("chunk size must be positive")
    return [data[index : index + size] for index in range(0, len(data), size)]


class JsonLineDecoder:
    def __init__(self) -> None:
        self._buffer = bytearray()

    def feed(self, data: bytes) -> list[dict[str, Any]]:
        messages: list[dict[str, Any]] = []
        self._buffer.extend(data)

        while True:
            try:
                newline = self._buffer.index(ord("\n"))
            except ValueError:
                break

            line = bytes(self._buffer[:newline]).strip()
            del self._buffer[: newline + 1]
            if not line:
                continue
            messages.append(json.loads(line.decode("utf-8")))

        return messages


@dataclass(frozen=True)
class Snapshot:
    total: int = 0
    running: int = 0
    waiting: int = 0
    msg: str = "Connected to Codex app"
    entries: tuple[str, ...] = ()
    status: dict[str, Any] | None = None
    activity: tuple[dict[str, Any], ...] = ()
    tokens: int | None = None
    tokens_today: int | None = None
    remaining_pct: int | None = None
    rate_limits: dict[str, dict[str, Any]] | None = None
    plan: dict[str, Any] | None = None
    goal: dict[str, Any] | None = None
    prompt: dict[str, str] | None = None
    interaction: dict[str, Any] | None = None

    def to_wire(self) -> dict[str, Any]:
        data: dict[str, Any] = {
            "total": self.total,
            "running": self.running,
            "waiting": self.waiting,
            "msg": self.msg,
            "entries": list(self.entries[:3]),
        }
        if self.status:
            data["status"] = self.status
        if self.activity:
            data["activity"] = list(self.activity)
        if self.tokens is not None:
            data["tokens"] = self.tokens
        if self.tokens_today is not None:
            data["tokens_today"] = self.tokens_today
        if self.remaining_pct is not None:
            data["remaining_pct"] = max(0, min(100, self.remaining_pct))
        if self.rate_limits:
            data["rate_limits"] = self.rate_limits
        if self.plan:
            data["plan"] = self.plan
        if self.goal:
            data["goal"] = self.goal
        if self.prompt:
            data["prompt"] = self.prompt
        if self.interaction:
            data["interaction"] = self.interaction
        return data


def short_text(value: Any, limit: int = 80) -> str:
    text = "" if value is None else str(value)
    text = " ".join(text.split())
    if len(text) <= limit:
        return text
    return text[: max(0, limit - 3)] + "..."


def command_prompt_hint(params: dict[str, Any]) -> str:
    network = params.get("networkApprovalContext")
    if isinstance(network, dict):
        host = network.get("host") or "network"
        protocol = network.get("protocol")
        port = network.get("port")
        target = ":".join(str(part) for part in (protocol, host, port) if part)
        return short_text(params.get("reason") or f"Network access: {target}")

    command = params.get("command")
    cwd = params.get("cwd")
    reason = params.get("reason")
    if reason and command:
        return short_text(f"{reason}: {command}")
    if command:
        return short_text(command)
    if cwd:
        return short_text(f"Command in {cwd}")
    return "Command approval requested"


def approval_prompt(method: str, request_id: Any, params: dict[str, Any]) -> dict[str, str]:
    prompt_id = str(request_id)
    if method == COMMAND_APPROVAL_METHOD:
        tool = "Network" if isinstance(params.get("networkApprovalContext"), dict) else "Command"
        return {"id": prompt_id, "tool": tool, "hint": command_prompt_hint(params)}

    if method == FILE_APPROVAL_METHOD:
        reason = params.get("reason")
        grant_root = params.get("grantRoot")
        hint = reason or (f"File change under {grant_root}" if grant_root else "File change approval requested")
        return {"id": prompt_id, "tool": "Files", "hint": short_text(hint)}

    return {"id": prompt_id, "tool": "Codex", "hint": "Approval requested"}


def approval_interaction(method: str, request_id: Any, params: dict[str, Any]) -> dict[str, Any]:
    prompt = approval_prompt(method, request_id, params)
    return {
        "id": prompt["id"],
        "kind": "approval",
        "title": prompt["tool"],
        "body": prompt["hint"],
        "options": [
            {"id": "once", "label": "Once"},
            {"id": "session", "label": "Session"},
            {"id": "deny", "label": "Deny"},
            {"id": "cancel", "label": "Cancel"},
        ],
        "selected": 0,
        "multi": False,
        "handoff": False,
    }


def handoff_interaction(request_id: Any, title: str, body: Any, question_id: Any | None = None) -> dict[str, Any]:
    interaction = {
        "id": str(request_id),
        "kind": "handoff",
        "title": short_text(title, 18),
        "body": short_text(body, 80),
        "options": [{"id": "handoff", "label": "Open Mac"}],
        "selected": 0,
        "multi": False,
        "handoff": True,
    }
    if question_id is not None:
        interaction["question_id"] = str(question_id)
    return interaction


def tool_user_input_interaction(request_id: Any, params: dict[str, Any]) -> dict[str, Any]:
    questions = params.get("questions")
    if not isinstance(questions, list) or len(questions) != 1:
        return handoff_interaction(request_id, "Codex Choice", "Open on Mac")

    question = questions[0]
    if not isinstance(question, dict):
        return handoff_interaction(request_id, "Codex Choice", "Open on Mac")
    if question.get("isSecret") or question.get("isOther"):
        return handoff_interaction(
            request_id,
            question.get("header") or "Codex Choice",
            question.get("question"),
            question.get("id") or "answer",
        )

    options = question.get("options")
    if not isinstance(options, list) or not options:
        return handoff_interaction(
            request_id,
            question.get("header") or "Codex Choice",
            question.get("question"),
            question.get("id") or "answer",
        )

    normalized_options: list[dict[str, str]] = []
    for index, option in enumerate(options[:MAX_INTERACTION_OPTIONS]):
        if not isinstance(option, dict):
            continue
        label = short_text(option.get("label"), 18)
        if label:
            normalized_options.append({"id": label, "label": label})

    if not normalized_options:
        return handoff_interaction(
            request_id,
            question.get("header") or "Codex Choice",
            question.get("question"),
            question.get("id") or "answer",
        )

    return {
        "id": str(request_id),
        "kind": "choice",
        "title": short_text(question.get("header") or "Codex Choice", 18),
        "body": short_text(question.get("question"), 80),
        "question_id": str(question.get("id") or "answer"),
        "options": normalized_options,
        "selected": 0,
        "multi": False,
        "handoff": False,
    }


def mcp_elicitation_interaction(request_id: Any, params: dict[str, Any]) -> dict[str, Any]:
    mode = params.get("mode")
    if mode != "form":
        return handoff_interaction(request_id, "MCP Request", params.get("message") or "Open on Mac")

    schema = params.get("requestedSchema")
    properties = schema.get("properties") if isinstance(schema, dict) else None
    required = schema.get("required") if isinstance(schema, dict) else None
    if not isinstance(properties, dict) or len(properties) != 1:
        return handoff_interaction(request_id, "MCP Request", params.get("message") or "Open on Mac")

    field_id, field_schema = next(iter(properties.items()))
    if not isinstance(field_schema, dict):
        return handoff_interaction(request_id, "MCP Request", params.get("message") or "Open on Mac")

    enum_values = field_schema.get("enum")
    enum_names = field_schema.get("enumNames")
    if not isinstance(enum_values, list) or not enum_values:
        return handoff_interaction(request_id, field_schema.get("title") or "MCP Request", params.get("message"))

    normalized_options: list[dict[str, str]] = []
    for index, value in enumerate(enum_values[:MAX_INTERACTION_OPTIONS]):
        label = enum_names[index] if isinstance(enum_names, list) and index < len(enum_names) else value
        normalized_options.append({"id": str(value), "label": short_text(label, 18)})

    return {
        "id": str(request_id),
        "kind": "mcp_choice",
        "title": short_text(field_schema.get("title") or "MCP Request", 18),
        "body": short_text(params.get("message"), 80),
        "question_id": str(field_id),
        "options": normalized_options,
        "selected": 0,
        "multi": False,
        "handoff": bool(isinstance(required, list) and len(required) > 1),
    }


def request_interaction(method: str, request_id: Any, params: dict[str, Any]) -> dict[str, Any]:
    if method in SUPPORTED_APPROVAL_METHODS:
        return approval_interaction(method, request_id, params)
    if method == TOOL_REQUEST_USER_INPUT_METHOD:
        return tool_user_input_interaction(request_id, params)
    if method == MCP_ELICITATION_REQUEST_METHOD:
        return mcp_elicitation_interaction(request_id, params)
    return handoff_interaction(request_id, "Codex", "Open on Mac")


def approval_response(method: str, decision: str) -> dict[str, Any]:
    decisions = {
        "once": "accept",
        "session": "acceptForSession",
        "deny": "decline",
        "cancel": "cancel",
    }
    mapped = decisions.get(decision)
    if mapped is None:
        raise ValueError(f"unsupported approval decision: {decision}")
    if method == COMMAND_APPROVAL_METHOD:
        return {"decision": mapped}
    if method == FILE_APPROVAL_METHOD:
        return {"decision": mapped}
    raise ValueError(f"unsupported approval method: {method}")


def tool_user_input_response(interaction: dict[str, Any], value: Any) -> dict[str, Any]:
    question_id = str(interaction.get("question_id") or "answer")
    if isinstance(value, list):
        answers = [str(item) for item in value]
    else:
        answers = [str(value)]
    return {"answers": {question_id: {"answers": answers}}}


def empty_tool_user_input_response(interaction: dict[str, Any]) -> dict[str, Any]:
    question_id = interaction.get("question_id")
    if question_id is None:
        return {"answers": {}}
    return {"answers": {str(question_id): {"answers": []}}}


def mcp_elicitation_response(interaction: dict[str, Any], value: Any) -> dict[str, Any]:
    question_id = str(interaction.get("question_id") or "answer")
    return {"action": "accept", "content": {question_id: value}}


def interaction_response(method: str, interaction: dict[str, Any], message: dict[str, Any]) -> dict[str, Any]:
    action = str(message.get("action") or "submit")
    if action in {"cancel", "handoff"}:
        if method == MCP_ELICITATION_REQUEST_METHOD:
            return {"action": "cancel"}
        if method in SUPPORTED_APPROVAL_METHODS:
            return approval_response(method, "cancel")
        return empty_tool_user_input_response(interaction)

    value = message.get("value")
    if method in SUPPORTED_APPROVAL_METHODS:
        return approval_response(method, str(value or "once"))
    if method == TOOL_REQUEST_USER_INPUT_METHOD:
        return tool_user_input_response(interaction, value)
    if method == MCP_ELICITATION_REQUEST_METHOD:
        return mcp_elicitation_response(interaction, value)
    raise ValueError(f"unsupported interaction method: {method}")


def item_summary(item: dict[str, Any]) -> str | None:
    item_type = item.get("type")
    if item_type == "commandExecution":
        command = item.get("command")
        status = item.get("status")
        if command:
            return short_text(f"{status or 'cmd'}: {command}", 48)
    if item_type == "fileChange":
        status = item.get("status")
        return short_text(f"{status or 'files'}: file change", 48)
    if item_type == "agentMessage":
        text = item.get("text") or item.get("message")
        if text:
            return short_text(text, 48)
    return short_text(item_type, 48) if item_type else None


def latest_entries(entries: Iterable[str], limit: int = 3) -> tuple[str, ...]:
    cleaned = [short_text(entry, 48) for entry in entries if entry]
    return tuple(cleaned[:limit])


def normalize_plan_update(params: dict[str, Any] | None) -> dict[str, Any] | None:
    if not isinstance(params, dict):
        return None

    steps = params.get("plan")
    if not isinstance(steps, list):
        return None

    valid_steps = [step for step in steps if isinstance(step, dict) and isinstance(step.get("step"), str)]
    if not valid_steps:
        return {"available": False}

    def status_of(step: dict[str, Any]) -> str:
        return str(step.get("status") or "pending")

    selected = next((step for step in valid_steps if status_of(step) in {"inProgress", "in_progress"}), None)
    if selected is None:
        selected = next((step for step in valid_steps if status_of(step) == "pending"), None)
    if selected is None:
        selected = valid_steps[-1]

    completed = sum(1 for step in valid_steps if status_of(step) == "completed")
    return {
        "available": True,
        "step": short_text(selected.get("step"), 80),
        "status": status_of(selected),
        "completed": completed,
        "total": len(valid_steps),
    }


def normalize_goal(goal: dict[str, Any] | None) -> dict[str, Any]:
    if not isinstance(goal, dict):
        return {"available": False}

    objective = goal.get("objective")
    if not isinstance(objective, str) or not objective.strip():
        return {"available": False}

    normalized: dict[str, Any] = {
        "available": True,
        "objective": short_text(objective, 96),
        "status": str(goal.get("status") or "active"),
    }

    time_used = goal.get("timeUsedSeconds", goal.get("time_used_sec"))
    if isinstance(time_used, int):
        normalized["time_used_sec"] = max(0, time_used)

    tokens_used = goal.get("tokensUsed", goal.get("tokens_used"))
    if isinstance(tokens_used, int):
        normalized["tokens_used"] = max(0, tokens_used)

    token_budget = goal.get("tokenBudget", goal.get("token_budget"))
    if isinstance(token_budget, int):
        normalized["token_budget"] = max(0, token_budget)

    return normalized


def window_label(window_mins: int | None) -> str:
    if not window_mins:
        return "limit"
    if window_mins % 1440 == 0:
        return f"{window_mins // 1440}d"
    if window_mins % 60 == 0:
        return f"{window_mins // 60}h"
    return f"{window_mins}m"


def normalize_rate_limit_window(window: dict[str, Any] | None) -> dict[str, Any] | None:
    if not isinstance(window, dict):
        return None

    used_percent = window.get("usedPercent", window.get("used_percent"))
    if not isinstance(used_percent, int):
        return None

    window_mins = window.get("windowDurationMins", window.get("window_mins"))
    if not isinstance(window_mins, int):
        window_mins = None

    remaining_percent = max(0, min(100, 100 - used_percent))
    normalized: dict[str, Any] = {
        "label": window_label(window_mins),
        "used_percent": max(0, min(100, used_percent)),
        "remaining_percent": remaining_percent,
    }
    if window_mins is not None:
        normalized["window_mins"] = window_mins
    resets_at = window.get("resetsAt", window.get("resets_at"))
    if isinstance(resets_at, int):
        normalized["resets_at"] = resets_at
    return normalized


def normalize_rate_limits(snapshot: dict[str, Any] | None) -> dict[str, dict[str, Any]] | None:
    if not isinstance(snapshot, dict):
        return None

    primary = normalize_rate_limit_window(snapshot.get("primary"))
    secondary = normalize_rate_limit_window(snapshot.get("secondary"))
    normalized: dict[str, dict[str, Any]] = {}
    if primary:
        normalized["primary"] = primary
    if secondary:
        normalized["secondary"] = secondary
    return normalized or None
