from __future__ import annotations

import json
from collections.abc import Iterable
from dataclasses import dataclass
from typing import Any

NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

DEFAULT_BLE_CHUNK_SIZE = 20

COMMAND_APPROVAL_METHOD = "item/commandExecution/requestApproval"
FILE_APPROVAL_METHOD = "item/fileChange/requestApproval"
SUPPORTED_APPROVAL_METHODS = {COMMAND_APPROVAL_METHOD, FILE_APPROVAL_METHOD}


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
    tokens: int | None = None
    tokens_today: int | None = None
    remaining_pct: int | None = None
    rate_limits: dict[str, dict[str, Any]] | None = None
    plan: dict[str, Any] | None = None
    goal: dict[str, Any] | None = None
    prompt: dict[str, str] | None = None

    def to_wire(self) -> dict[str, Any]:
        data: dict[str, Any] = {
            "total": self.total,
            "running": self.running,
            "waiting": self.waiting,
            "msg": self.msg,
            "entries": list(self.entries[:3]),
        }
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


def approval_response(method: str, decision: str) -> dict[str, Any]:
    approved = decision == "once"
    if method == COMMAND_APPROVAL_METHOD:
        return {"decision": "accept" if approved else "decline"}
    if method == FILE_APPROVAL_METHOD:
        return {"decision": "accept" if approved else "decline"}
    raise ValueError(f"unsupported approval method: {method}")


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
