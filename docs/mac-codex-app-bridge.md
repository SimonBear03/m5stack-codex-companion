# Mac Codex App Bridge

This repo targets the Codex app on Mac through a local bridge.

Public OpenAI docs describe Codex App Server as the protocol used for rich clients. It carries JSON-RPC messages over stdio, Unix socket, or WebSocket transports and includes command/file approval requests. The docs do not currently describe native BLE pairing from the desktop app to third-party hardware, so this repo treats BLE as the StickS3 device protocol and App Server as the Codex-side integration.

## Flow

```text
Codex app / App Server endpoint
  <-> JSON-RPC app-server bridge
  <-> BLE Nordic UART Service JSONL
  <-> Agent Blob display, overlays, and buttons
```

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

The stdio path is a validation harness. The intended product path is a Mac Codex app/app-server endpoint.

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
- Simon's physical StickS3 advertises as `Codex-S3-0470` over BLE.
- A direct BLE status request receives a valid `{"ack":"status","ok":true}` response with battery, heap, and approval counters.
- The bridge connects to the physical StickS3 over BLE and initializes a Codex App Server `stdio` session.

Still to validate:

- The Codex app exposes or can be paired with an App Server endpoint suitable for this bridge.
- Button A/B approval decisions round-trip into a live Codex app approval prompt.
- Whether a WebSocket/socket endpoint can passively mirror already-open Codex Desktop app threads.

The `stdio` path starts its own App Server process. It is useful for development and validates the hardware bridge path, but it should not be treated as a passive observer of every existing Codex Desktop conversation.

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

Free-form answers, secret answers, multi-question prompts, and complex MCP forms are shown as handoff interactions and should be answered on the Mac.

## Control Mapping

Agent Blob can send `{"cmd":"control","action":"interrupt"}`. The bridge maps that to App Server `turn/interrupt` when it has observed an active `threadId` and `turnId`.

## Usage Mapping

The bridge calls App Server `account/rateLimits/read` after initialization and listens for `account/rateLimits/updated`. It forwards the Codex bucket's primary and secondary rolling windows to the StickS3.

The current observed App Server payload uses:

- `primary.windowDurationMins = 300`, displayed as `5h`
- `secondary.windowDurationMins = 10080`, displayed as `7d`

App Server reports `usedPercent`; the bridge sends both used and remaining percentages, and Agent Blob displays the remaining windows as bars rather than raw percentages.

Token totals are optional. The bridge listens for `thread/tokenUsage/updated` and forwards totals when the App Server emits them, but the device displays `Tok: n/a` until that event is available.

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

BLE is prototype-open in v1. Do not send sensitive transcript snippets, secrets, or long command payloads to the StickS3. Keep the App Server endpoint bound to localhost or a local Unix socket.

## Known Limitation

The public App Server protocol gives a client stream for the endpoint it is connected to. It does not document a passive observer interface for every already-open Codex desktop-app panel. If the Mac app exposes an App Server endpoint, point the bridge at that endpoint and validate which conversations/events it emits.
