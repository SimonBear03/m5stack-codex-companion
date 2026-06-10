# Mac Codex App Bridge

This repo targets the Codex app on Mac through a local bridge.

Public OpenAI docs describe Codex App Server as the protocol used for rich clients. It carries JSON-RPC messages over stdio, Unix socket, or WebSocket transports and includes command/file approval requests. The docs do not currently describe native BLE pairing from the desktop app to third-party hardware, so this repo treats BLE as the StickS3 device protocol and App Server as the Codex-side integration.

## Flow

```text
Codex app / App Server endpoint
  <-> JSON-RPC app-server bridge
  <-> BLE Nordic UART Service JSONL
  <-> StickS3 display and A/B buttons
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

## Current Validation Status

Validated on 2026-06-10 in the VPS workspace:

- The bridge initializes `codex app-server --stdio` with a fake StickS3 device.
- The bridge reads `account/rateLimits/read`.
- The fake device receives `rate_limits.primary.label = 5h` and `rate_limits.secondary.label = 7d`.
- Python tests cover approval mapping, rate-limit normalization, plan summaries, goal summaries, and cleared goals.

Still to validate on Simon's Mac:

- The Codex app exposes or can be paired with an App Server endpoint suitable for this bridge.
- BLE scanning connects to the flashed StickS3.
- Button A/B approval decisions round-trip into a live Codex app approval prompt.

## Approval Mapping

- App Server `item/commandExecution/requestApproval` -> StickS3 prompt with tool `Command` or `Network`.
- App Server `item/fileChange/requestApproval` -> StickS3 prompt with tool `Files`.
- Button A sends `{"cmd":"permission","decision":"once"}` -> App Server `{"decision":"accept"}`.
- Button B sends `{"cmd":"permission","decision":"deny"}` -> App Server `{"decision":"decline"}`.

V1 does not implement persistent approval choices such as `acceptForSession`, exec-policy amendments, or network-policy amendments from the hardware buttons.

## Usage Mapping

The bridge calls App Server `account/rateLimits/read` after initialization and listens for `account/rateLimits/updated`. It forwards the Codex bucket's primary and secondary rolling windows to the StickS3.

The current observed App Server payload uses:

- `primary.windowDurationMins = 300`, displayed as `5h`
- `secondary.windowDurationMins = 10080`, displayed as `7d`

App Server reports `usedPercent`; the bridge sends both used and remaining percentages, and the StickS3 limits page displays remaining percentage.

## Plan and Goal Mapping

The bridge listens for:

- `turn/plan/updated`
- `thread/goal/updated`
- `thread/goal/cleared`

Plan updates are summarized to one current step for the StickS3 page: in-progress first, then pending, then the latest completed step. Goal updates include objective, status, elapsed seconds, used tokens, and optional token budget.

The bridge intentionally ignores `item/plan/delta` for the device UI because the App Server schema marks those deltas experimental and says concatenated deltas are not authoritative.

## Device Pages

The firmware cycles through six pages:

1. Limits
2. Status
3. Plan
4. Goal
5. Recent
6. System

## Security Notes

BLE is prototype-open in v1. Do not send sensitive transcript snippets, secrets, or long command payloads to the StickS3. Keep the App Server endpoint bound to localhost or a local Unix socket.

## Known Limitation

The public App Server protocol gives a client stream for the endpoint it is connected to. It does not document a passive observer interface for every already-open Codex desktop-app panel. If the Mac app exposes an App Server endpoint, point the bridge at that endpoint and validate which conversations/events it emits.
