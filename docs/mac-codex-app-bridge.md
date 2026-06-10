# Mac Codex App Bridge

This repo targets the Codex app on Mac through a local bridge.

Public OpenAI docs describe Codex App Server as the protocol used for rich clients. It carries JSON-RPC messages over stdio, Unix socket, or WebSocket transports and includes command/file approval requests. The docs do not currently describe native BLE pairing from the desktop app to third-party hardware, so this repo treats BLE as the StickS3 device protocol and App Server as the Codex-side integration.

## Integration Modes

There are two bridge modes:

- `desktop-observer`: read-only status mirroring for the actual Codex Desktop app. It follows local rollout JSONL files under `~/.codex/sessions`, prefers non-subagent Desktop threads, and forwards active/idle state, recent activity, token totals, and rate limits when those events are present.
- `app-server`: JSON-RPC control path for an App Server endpoint. It can handle approvals, option-list prompts, interrupts, rate limits, plan updates, and goals, but only for the App Server session it is connected to.

True control of an already-open Codex Desktop app thread is currently blocked by product surface, not by StickS3 firmware. On Simon's Mac, Codex Desktop is running private `stdio://` app-server processes and the default app-server control socket `/Users/simon/.codex/app-server-control/app-server-control.sock` is not present. Public docs describe how to expose an App Server transport, but not how third-party hardware can attach to an existing Desktop thread.

## Flow

```text
Codex Desktop rollout logs OR App Server endpoint
  <-> Mac bridge
  <-> BLE Nordic UART Service JSONL
  <-> Agent Blob display, overlays, and buttons
```

For the Desktop app experience Simon asked for, run:

```bash
sticks3-bridge --log-level INFO desktop-observer --scan-timeout 15
```

Observer mode auto-selects the freshest non-subagent rollout under `~/.codex/sessions`. To pin a specific thread or file:

```bash
sticks3-bridge desktop-observer --thread-id <thread-id>
sticks3-bridge desktop-observer --rollout ~/.codex/sessions/.../rollout-....jsonl
```

Observer mode is intentionally read-only. Button approvals, choice answers, and interrupts require `app-server` mode.

The bridge can attach to a running App Server endpoint:

```bash
sticks3-bridge app-server --transport ws --target ws://127.0.0.1:4567
```

It can also use a Unix-socket WebSocket endpoint:

```bash
sticks3-bridge app-server --transport ws --target unix:///tmp/codex-app-server.sock
```

For local validation without the desktop app or BLE hardware, spawn the public app-server implementation over stdio and use a fake device:

```bash
sticks3-bridge app-server --transport stdio --fake-device --auto-decision deny
```

The stdio path is a validation harness for App Server control. It starts its own App Server session and should not be described as "connected to this Desktop chat."

## Mac Setup

Use a Python 3.11+ environment for the bridge:

```bash
python3.11 -m venv .bridge-venv
source .bridge-venv/bin/activate
python -m pip install -U pip
python -m pip install -e .
```

Keep this separate from the PlatformIO `.venv/` if that environment was created with Python 3.9.

## Current Validation Status

Validated on 2026-06-10:

- The bridge initializes `codex app-server --stdio` with a fake StickS3 device.
- The bridge reads `account/rateLimits/read`.
- The fake device receives `rate_limits.primary.label = 5h` and `rate_limits.secondary.label = 7d`.
- Python tests cover approval mapping, rate-limit normalization, plan summaries, goal summaries, token usage forwarding, and cleared goals.
- Python tests cover Desktop observer rollout selection and snapshot parsing.
- Simon's physical StickS3 advertises as `Codex-S3-0470` over BLE.
- A direct BLE status request receives a valid `{"ack":"status","ok":true}` response with battery, heap, and approval counters.
- The bridge connects to the physical StickS3 over BLE and initializes a Codex App Server `stdio` session.

Still to validate:

- Run `desktop-observer` against the physical StickS3 while this Codex Desktop app thread is active.
- Button A/B approval decisions round-trip into a live App Server approval prompt.
- Whether future Codex Desktop builds expose a supported attach/control endpoint for third-party local clients.

The `stdio` path starts its own App Server process. It is useful for development and validates the hardware control path, but it should not be treated as a passive observer of every existing Codex Desktop conversation.

## Approval Mapping

- App Server `item/commandExecution/requestApproval` -> StickS3 prompt with tool `Command` or `Network`.
- App Server `item/fileChange/requestApproval` -> StickS3 prompt with tool `Files`.
- Button A sends interaction value `once` -> App Server `{"decision":"accept"}`.
- Long Button A sends interaction value `session` -> App Server `{"decision":"acceptForSession"}`.
- Button B sends interaction value `deny` -> App Server `{"decision":"decline"}`.
- Long Button B sends interaction value `cancel` -> App Server `{"decision":"cancel"}`.

The bridge does not yet expose exec-policy amendments or network-policy amendments from the hardware buttons.

## Choice Mapping

The bridge handles App Server `item/tool/requestUserInput` when the request is a single option-list question. The StickS3 shows it as an Agent Blob choice overlay:

- Button B cycles options.
- Button A marks the highlighted option.
- Double/long Button A submits.
- Long Button B cancels.
- The bridge sends at most eight options, matching the current firmware capacity.

Free-form answers, secret answers, multi-question prompts, and complex MCP forms are shown as handoff interactions. The bridge does not submit placeholder text for those cases; handoff/cancel returns an empty/no-device-answer payload so the request fails closed instead of inventing user input. `mcpServer/elicitation/request` handling is defensive until it is observed in a real App Server session.

## Control Mapping

Agent Blob can send `{"cmd":"control","action":"interrupt"}`. The bridge maps that to App Server `turn/interrupt` when it has observed an active `threadId` and `turnId`.

## Usage Mapping

The bridge calls App Server `account/rateLimits/read` after initialization and listens for `account/rateLimits/updated`. It forwards the Codex bucket's primary and secondary rolling windows to the StickS3.

The current observed App Server payload uses:

- `primary.windowDurationMins = 300`, displayed as `5h`
- `secondary.windowDurationMins = 10080`, displayed as `7d`

App Server reports `usedPercent`; the bridge sends both used and remaining percentages, and Agent Blob displays the remaining windows as bars rather than raw percentages.

Token totals are optional. The bridge listens for `thread/tokenUsage/updated` and forwards totals when the App Server emits them, but the device displays `Tok: n/a` until that event is available.

In `desktop-observer` mode, the bridge reads `event_msg` `token_count` events from rollout JSONL. Those events use `used_percent` and `window_minutes`; the observer normalizes them to the same device payload used by App Server mode.

## Plan and Goal Mapping

The bridge listens for:

- `turn/plan/updated`
- `thread/goal/updated`
- `thread/goal/cleared`

Plan updates are summarized to one current step for the StickS3 page: in-progress first, then pending, then the latest completed step. Goal updates include objective, status, elapsed seconds, used tokens, and optional token budget.

The bridge intentionally ignores `item/plan/delta` for the device UI because the App Server schema marks those deltas experimental and says concatenated deltas are not authoritative.

## Device Pages

The firmware cycles through five screens:

1. Blob home
2. Codex detail
3. Limits
4. Care
5. System

Approval and choice requests appear as overlays above the current screen.

## Security Notes

BLE is prototype-open in v1. `desktop-observer` forwards short activity snippets from local Codex rollout logs, so do not use it while secrets or sensitive transcript text are being displayed. Keep any App Server endpoint bound to localhost or a local Unix socket.

## Known Limitation

The public App Server protocol gives a client stream for the endpoint it is connected to. It does not document a passive control interface for every already-open Codex desktop-app panel. `desktop-observer` is a local-log workaround for status display only.
