from __future__ import annotations

import abc
import asyncio
import json
import logging
import shlex
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Any

from .device import StickS3Device
from .protocol import (
    SUPPORTED_INTERACTION_METHODS,
    Snapshot,
    codex_activity,
    interaction_response,
    item_summary,
    legacy_status_from_activity,
    latest_entries,
    normalize_goal,
    normalize_plan_update,
    normalize_rate_limits,
    request_interaction,
    TOOL_REQUEST_USER_INPUT_METHOD,
)

LOGGER = logging.getLogger(__name__)


class AppServerTransport(abc.ABC):
    @abc.abstractmethod
    async def send(self, message: dict[str, Any]) -> None:
        raise NotImplementedError

    @abc.abstractmethod
    async def receive(self) -> dict[str, Any]:
        raise NotImplementedError

    @abc.abstractmethod
    async def close(self) -> None:
        raise NotImplementedError


class StdioAppServerTransport(AppServerTransport):
    def __init__(self, command: str) -> None:
        self.command = command
        self._process: asyncio.subprocess.Process | None = None
        self._stderr_tail: deque[str] = deque(maxlen=8)

    async def start(self) -> None:
        argv = shlex.split(self.command)
        self._process = await asyncio.create_subprocess_exec(
            *argv,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
        )
        asyncio.create_task(self._log_stderr())

    async def send(self, message: dict[str, Any]) -> None:
        if self._process is None or self._process.stdin is None:
            raise RuntimeError("app-server stdio process is not running")
        self._process.stdin.write(json.dumps(message, separators=(",", ":")).encode("utf-8") + b"\n")
        await self._process.stdin.drain()

    async def receive(self) -> dict[str, Any]:
        if self._process is None or self._process.stdout is None:
            raise RuntimeError("app-server stdio process is not running")
        line = await self._process.stdout.readline()
        if not line:
            detail = "\n".join(self._stderr_tail)
            if detail:
                raise EOFError(f"app-server stdio closed:\n{detail}")
            raise EOFError("app-server stdio closed")
        return json.loads(line.decode("utf-8"))

    async def close(self) -> None:
        if self._process is None:
            return
        if self._process.returncode is not None:
            return
        try:
            self._process.terminate()
        except ProcessLookupError:
            return
        try:
            await asyncio.wait_for(self._process.wait(), timeout=5)
        except TimeoutError:
            self._process.kill()
            await self._process.wait()

    async def _log_stderr(self) -> None:
        if self._process is None or self._process.stderr is None:
            return
        while line := await self._process.stderr.readline():
            text = line.decode("utf-8", errors="replace").rstrip()
            self._stderr_tail.append(text)
            LOGGER.debug("app-server stderr: %s", text)


class WebSocketAppServerTransport(AppServerTransport):
    def __init__(self, url: str) -> None:
        self.url = url
        self._socket: Any | None = None

    async def start(self) -> None:
        try:
            import websockets
        except ImportError as exc:
            raise RuntimeError("Install bridge dependencies with `python -m pip install -e .`") from exc

        if self.url.startswith("unix://"):
            path = self.url.removeprefix("unix://")
            self._socket = await websockets.unix_connect(path, uri="ws://localhost")
        else:
            self._socket = await websockets.connect(self.url)

    async def send(self, message: dict[str, Any]) -> None:
        if self._socket is None:
            raise RuntimeError("app-server websocket is not connected")
        await self._socket.send(json.dumps(message, separators=(",", ":")))

    async def receive(self) -> dict[str, Any]:
        if self._socket is None:
            raise RuntimeError("app-server websocket is not connected")
        raw = await self._socket.recv()
        if isinstance(raw, bytes):
            raw = raw.decode("utf-8")
        return json.loads(raw)

    async def close(self) -> None:
        if self._socket is not None:
            await self._socket.close()
            self._socket = None


@dataclass
class AppState:
    statuses: dict[str, str] = field(default_factory=dict)
    waiting_threads: set[str] = field(default_factory=set)
    entries: deque[str] = field(default_factory=lambda: deque(maxlen=3))
    tokens_total: int | None = None
    rate_limits: dict[str, dict[str, Any]] | None = None
    raw_rate_limit_snapshot: dict[str, Any] | None = None
    plan: dict[str, Any] | None = None
    goal: dict[str, Any] | None = None
    last_message: str = "Connected to Codex app"
    last_activity_status: str = "idle"
    last_error: str | None = None
    activity_updated_at: float = field(default_factory=time.time)
    active_thread_id: str | None = None
    active_turn_id: str | None = None

    def snapshot(
        self,
        prompt: dict[str, str] | None = None,
        interaction: dict[str, Any] | None = None,
        waiting_kind: str | None = None,
    ) -> Snapshot:
        running = sum(1 for status in self.statuses.values() if status == "active")
        waiting = len(self.waiting_threads) + (1 if interaction or prompt else 0)
        current_activity = self.codex_activity(
            running=running,
            waiting=waiting,
            waiting_kind=waiting_kind,
        )
        return Snapshot(
            total=len(self.statuses),
            running=running,
            waiting=waiting,
            msg=self.last_message,
            entries=latest_entries(self.entries),
            status=legacy_status_from_activity(current_activity),
            codex_activity=current_activity,
            tokens=self.tokens_total,
            rate_limits=self.rate_limits,
            plan=self.plan,
            goal=self.goal,
            prompt=prompt,
            interaction=interaction,
        )

    def codex_activity(self, *, running: int, waiting: int, waiting_kind: str | None = None) -> Any:
        if waiting > 0:
            status = "waiting"
            subtitle = self.last_message or "Waiting for input"
        elif self.last_error:
            status = "failed"
            subtitle = self.last_error
        elif running > 0 or self.active_turn_id:
            status = "running"
            subtitle = self.last_message or "Working"
        elif self.last_activity_status in {"review", "failed"}:
            status = self.last_activity_status
            subtitle = self.last_message or ("Ready for review" if status == "review" else "Codex error")
        else:
            status = "idle"
            subtitle = self.last_message or "Idle"

        thread_label = self.active_thread_id[:8] if self.active_thread_id else None
        return codex_activity(
            status,
            title=thread_label or "Codex",
            subtitle=subtitle,
            waiting_kind=waiting_kind,
            updated_at=self.activity_updated_at,
            thread_label=thread_label,
        )


def nested_dict_value(data: dict[str, Any], path: tuple[str, ...]) -> Any:
    current: Any = data
    for key in path:
        if not isinstance(current, dict):
            return None
        current = current.get(key)
    return current


def extract_total_tokens(params: dict[str, Any]) -> int | None:
    paths = (
        ("tokenUsage", "total", "totalTokens"),
        ("tokenUsage", "totalTokens"),
        ("usage", "total", "totalTokens"),
        ("usage", "totalTokens"),
        ("total", "totalTokens"),
        ("totalTokens",),
    )
    for path in paths:
        value = nested_dict_value(params, path)
        if isinstance(value, int):
            return max(0, value)
    return None


def waiting_kind_for_request(method: str, params: dict[str, Any], interaction: dict[str, Any]) -> str:
    if method == TOOL_REQUEST_USER_INPUT_METHOD:
        return "question"
    if method == "mcpServer/elicitation/request":
        return "tool"
    if method == "item/fileChange/requestApproval":
        return "patch"
    if method == "item/commandExecution/requestApproval":
        if isinstance(params.get("networkApprovalContext"), dict):
            return "network"
        title = str(interaction.get("title") or "").lower()
        return "exec" if "command" in title else "permission"
    return "permission"


class CodexAppServerBridge:
    def __init__(
        self,
        *,
        transport: AppServerTransport,
        device: StickS3Device,
        approval_timeout: float = 300.0,
        heartbeat_interval: float = 10.0,
    ) -> None:
        self.transport = transport
        self.device = device
        self.approval_timeout = approval_timeout
        self.heartbeat_interval = heartbeat_interval
        self.state = AppState()
        self._next_request_id = 1
        self._snapshot_lock = asyncio.Lock()

    async def run(self) -> None:
        await self.device.connect()
        await self.initialize()
        await self.refresh_rate_limits()
        await self.send_snapshot(self.state.snapshot())
        asyncio.create_task(self.handle_device_controls())
        asyncio.create_task(self.send_heartbeats())

        while True:
            message = await self.transport.receive()
            LOGGER.debug("app-server -> bridge: %s", message)
            if "id" in message and "method" in message:
                await self.handle_server_request(message)
            elif "method" in message:
                await self.handle_notification(message)

    async def send_snapshot(self, snapshot: Snapshot) -> None:
        async with self._snapshot_lock:
            await self.device.send_snapshot(snapshot)

    async def send_heartbeats(self) -> None:
        while True:
            await asyncio.sleep(self.heartbeat_interval)
            await self.send_snapshot(self.state.snapshot())

    async def initialize(self) -> None:
        request_id = self._request_id()
        await self.transport.send(
            {
                "id": request_id,
                "method": "initialize",
                "params": {
                    "clientInfo": {
                        "name": "sticks3-codex-companion",
                        "title": "StickS3 Codex Companion",
                        "version": "0.1.0",
                    },
                    "capabilities": {"experimentalApi": True},
                },
            }
        )

        while True:
            message = await self.transport.receive()
            if message.get("id") == request_id:
                if "error" in message:
                    raise RuntimeError(f"app-server initialize failed: {message['error']}")
                LOGGER.info("Initialized app-server: %s", message.get("result"))
                break
            LOGGER.debug("Ignoring pre-initialize message: %s", message)

        await self.transport.send({"method": "initialized"})

    async def refresh_rate_limits(self) -> None:
        request_id = self._request_id()
        await self.transport.send({"id": request_id, "method": "account/rateLimits/read", "params": None})

        while True:
            message = await self.transport.receive()
            if message.get("id") == request_id:
                if "error" in message:
                    LOGGER.warning("Could not read account rate limits: %s", message["error"])
                    return
                self.apply_rate_limit_response(message.get("result") or {})
                return
            if "method" in message and "id" not in message:
                await self.handle_notification(message)
            else:
                LOGGER.debug("Ignoring message while waiting for rate limits: %s", message)

    def apply_rate_limit_response(self, result: dict[str, Any]) -> None:
        snapshot = self.select_rate_limit_snapshot(result)
        if not snapshot:
            return
        self.state.raw_rate_limit_snapshot = snapshot
        self.state.rate_limits = normalize_rate_limits(snapshot)

    def select_rate_limit_snapshot(self, result: dict[str, Any]) -> dict[str, Any] | None:
        by_limit_id = result.get("rateLimitsByLimitId")
        if isinstance(by_limit_id, dict):
            codex = by_limit_id.get("codex")
            if isinstance(codex, dict):
                return codex
        rate_limits = result.get("rateLimits")
        return rate_limits if isinstance(rate_limits, dict) else None

    def apply_rate_limit_update(self, snapshot: dict[str, Any]) -> None:
        if self.state.raw_rate_limit_snapshot is None:
            self.state.raw_rate_limit_snapshot = snapshot
        else:
            for key, value in snapshot.items():
                if value is not None:
                    self.state.raw_rate_limit_snapshot[key] = value
        self.state.rate_limits = normalize_rate_limits(self.state.raw_rate_limit_snapshot)

    async def handle_server_request(self, message: dict[str, Any]) -> None:
        method = message.get("method")
        request_id = message["id"]

        if method not in SUPPORTED_INTERACTION_METHODS:
            await self.transport.send(
                {
                    "id": request_id,
                    "error": {
                        "code": -32601,
                        "message": f"sticks3 bridge does not handle {method}",
                    },
                }
            )
            return

        params = message.get("params") or {}
        self.update_active_context(params)
        interaction = request_interaction(str(method), request_id, params)
        prompt = None
        if interaction.get("kind") == "approval":
            prompt = {
                "id": str(interaction["id"]),
                "tool": str(interaction.get("title") or "Codex"),
                "hint": str(interaction.get("body") or "Approval requested"),
            }
        self.state.last_message = str(interaction.get("body") or interaction.get("title") or "Codex request")
        self.state.activity_updated_at = time.time()
        await self.send_snapshot(
            self.state.snapshot(
                prompt=prompt,
                interaction=interaction,
                waiting_kind=waiting_kind_for_request(str(method), params, interaction),
            )
        )

        try:
            response = await self.device.wait_for_interaction(str(interaction["id"]), timeout=self.approval_timeout)
            result = interaction_response(str(method), interaction, response)
        except TimeoutError:
            LOGGER.warning("Timed out waiting for StickS3 interaction for request %s", request_id)
            response = {"action": "cancel", "value": "cancel"}
            result = interaction_response(str(method), interaction, response)

        await self.transport.send({"id": request_id, "result": result})
        self.state.last_message = self.response_summary(str(method), response)
        self.state.activity_updated_at = time.time()
        await self.send_snapshot(self.state.snapshot())

    def response_summary(self, method: str, response: dict[str, Any]) -> str:
        action = str(response.get("action") or "submit")
        value = str(response.get("value") or "")
        if action == "handoff":
            return "Skipped on device"
        if action == "cancel" or value == "cancel":
            return "Cancelled"
        if method == TOOL_REQUEST_USER_INPUT_METHOD:
            return "Choice sent"
        if value == "session":
            return "Approved session"
        if value == "once":
            return "Approved once"
        if value == "deny":
            return "Declined"
        return "Response sent"

    def update_active_context(self, params: dict[str, Any]) -> None:
        thread_id = params.get("threadId")
        turn_id = params.get("turnId")
        if thread_id:
            self.state.active_thread_id = str(thread_id)
        if turn_id:
            self.state.active_turn_id = str(turn_id)

    async def handle_device_controls(self) -> None:
        while True:
            message = await self.device.wait_for_control()
            action = message.get("action")
            if action == "interrupt":
                await self.interrupt_active_turn()

    async def interrupt_active_turn(self) -> None:
        if not self.state.active_thread_id or not self.state.active_turn_id:
            LOGGER.info("Ignoring StickS3 interrupt; no active turn id is known")
            return
        request_id = self._request_id()
        await self.transport.send(
            {
                "id": request_id,
                "method": "turn/interrupt",
                "params": {
                    "threadId": self.state.active_thread_id,
                    "turnId": self.state.active_turn_id,
                },
            }
        )
        self.state.last_message = "Interrupt sent"
        self.state.activity_updated_at = time.time()
        await self.send_snapshot(self.state.snapshot())

    async def handle_notification(self, message: dict[str, Any]) -> None:
        method = message.get("method")
        params = message.get("params") or {}
        if isinstance(params, dict):
            self.update_active_context(params)
        previous_activity = (self.state.last_message, self.state.last_activity_status, self.state.last_error, self.state.active_turn_id)

        if method == "thread/status/changed":
            thread_id = params.get("threadId")
            status = params.get("status") or {}
            status_type = status.get("type")
            if thread_id:
                self.state.statuses[str(thread_id)] = str(status_type)
                flags = set(status.get("activeFlags") or [])
                if "waitingOnApproval" in flags or "waitingOnUserInput" in flags:
                    self.state.waiting_threads.add(str(thread_id))
                else:
                    self.state.waiting_threads.discard(str(thread_id))
                if status_type == "active":
                    self.state.last_activity_status = "running"
                    self.state.last_error = None
                elif status_type in {"failed", "error"}:
                    self.state.last_activity_status = "failed"
                self.state.last_message = f"Thread {status_type}"

        elif method == "item/started":
            item = params.get("item") or {}
            summary = item_summary(item)
            if summary:
                self.state.entries.appendleft(summary)
                self.state.last_message = summary
                self.state.last_activity_status = "running"
                self.state.last_error = None

        elif method == "item/completed":
            item = params.get("item") or {}
            summary = item_summary(item)
            if summary:
                self.state.entries.appendleft(summary)
                self.state.last_message = summary

        elif method == "thread/tokenUsage/updated":
            total_tokens = extract_total_tokens(params)
            if total_tokens is not None:
                self.state.tokens_total = total_tokens

        elif method == "turn/plan/updated":
            summary = normalize_plan_update(params)
            if summary:
                self.state.plan = summary
                if summary.get("available"):
                    self.state.last_message = f"Plan: {summary.get('step')}"
                else:
                    self.state.last_message = "Plan cleared"

        elif method == "thread/goal/updated":
            self.state.goal = normalize_goal(params.get("goal"))
            if self.state.goal.get("available"):
                self.state.last_message = f"Goal {self.state.goal.get('status')}"
            else:
                self.state.last_message = "Goal unavailable"

        elif method == "thread/goal/cleared":
            self.state.goal = {"available": False}
            self.state.last_message = "Goal cleared"

        elif method == "account/rateLimits/updated":
            snapshot = params.get("rateLimits")
            if isinstance(snapshot, dict):
                self.apply_rate_limit_update(snapshot)
                self.state.last_message = "Rate limits updated"

        elif method == "serverRequest/resolved":
            self.state.last_message = "Request resolved"

        elif method == "error":
            error = params.get("error") or {}
            self.state.last_error = str(error.get("message") or "Codex error")
            self.state.last_activity_status = "failed"
            self.state.last_message = self.state.last_error

        elif method == "turn/started":
            turn = params.get("turn") if isinstance(params, dict) else None
            if isinstance(turn, dict):
                turn_id = turn.get("id")
                if turn_id:
                    self.state.active_turn_id = str(turn_id)
            self.state.last_message = "Turn started"
            self.state.last_activity_status = "running"
            self.state.last_error = None

        elif method == "turn/completed":
            self.state.last_message = "Turn completed"
            self.state.active_turn_id = None
            self.state.last_activity_status = "review"

        current_activity = (self.state.last_message, self.state.last_activity_status, self.state.last_error, self.state.active_turn_id)
        if current_activity != previous_activity:
            self.state.activity_updated_at = time.time()
        await self.send_snapshot(self.state.snapshot())

    def _request_id(self) -> int:
        request_id = self._next_request_id
        self._next_request_id += 1
        return request_id


async def build_transport(kind: str, target: str | None, stdio_command: str) -> AppServerTransport:
    if kind == "stdio":
        transport = StdioAppServerTransport(stdio_command)
        await transport.start()
        return transport
    if kind == "ws":
        if not target:
            raise ValueError("--target is required for ws transport")
        transport = WebSocketAppServerTransport(target)
        await transport.start()
        return transport
    raise ValueError(f"unsupported transport: {kind}")
