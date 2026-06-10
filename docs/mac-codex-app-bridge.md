# Mac Codex App Bridge

This repo targets the Codex app on Mac through a local bridge.

Public OpenAI docs describe Codex App Server as the protocol used for rich clients. It carries JSON-RPC messages over stdio, Unix socket, or WebSocket transports and includes command/file approval requests. The docs do not currently describe native BLE pairing from the desktop app to third-party hardware, so this repo treats BLE as the StickS3 device protocol and App Server as the Codex-side integration.

## Integration Modes

There are two bridge modes:

- `desktop-observer`: read-only status mirroring for the actual Codex Desktop app. It follows local rollout JSONL files under `~/.codex/sessions`, prefers non-subagent Desktop threads, and forwards active/idle state, speaker-labeled activity, token totals, and rate limits when those events are present.
- `app-server`: JSON-RPC validation path for an App Server endpoint. It can still exercise approval and choice mapping in the Python bridge, but the current dashboard firmware is read-only.

True control of an already-open Codex Desktop app thread is currently blocked by product surface, not by StickS3 firmware. On Simon's Mac, Codex Desktop is running private `stdio://` app-server processes and the default app-server control socket `/Users/simon/.codex/app-server-control/app-server-control.sock` is not present. Public docs describe how to expose an App Server transport, but not how third-party hardware can attach to an existing Desktop thread.

## Flow

```text
Codex Desktop rollout logs OR App Server endpoint
  <-> Mac bridge
  <-> BLE Nordic UART Service JSONL
  <-> StickS3 minimal dashboard
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

Observer mode is intentionally read-only. It mirrors status, activity, usage bars, and token totals. It cannot approve prompts, answer prompts, or interrupt turns in the Codex Desktop UI.

The bridge can attach to a running App Server endpoint:

```bash
sticks3-bridge app-server --transport ws --target ws://127.0.0.1:4567
```

It can also use a Unix-socket WebSocket endpoint:

```bash
sticks3-bridge app-server --transport ws --target unix:///tmp/codex-app-server.sock
```

For local validation without the desktop app or BLE hardware:

```bash
sticks3-bridge desktop-observer --fake-device
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

## Dashboard Mapping

The bridge sends:

- `status`: pinned newest action with `speaker`, `kind`, and `text`.
- `activity`: scrollable body entries with stable `seq` IDs for dedupe.
- `tokens`: optional compact token total.
- `rate_limits`: primary `5h` and secondary `7d` usage windows.

Desktop observer event handling:

- `task_started` -> current status `Codex: Working`
- `task_complete` -> current status `Codex: Turn completed`, with the final assistant message in body text
- `agent_message` -> `Codex` body text
- `user_message` -> `User` body text
- function/custom tool calls -> `Tool` body text with a cleaned tool name
- function output -> concise `Tool: output ready`
- `token_count` -> usage fields only; it does not overwrite the current action line
- `patch_apply_end` -> `Tool: Patch applied` or `Tool: Patch failed`

## Device Controls

Main dashboard:

- Button A: newer/down through body text.
- Button B: older/up through body text.
- Long A: open settings.
- Long B: jump to newest when reading older text and `NEW` is shown.
- A+B: no-op in the current dashboard firmware.

Settings menu:

- Button B: next option.
- Button A: rotate/toggle selected value.
- Long A: close settings.
- Auto-closes after 12 seconds of no input.

## Usage Mapping

The bridge calls App Server `account/rateLimits/read` after initialization and listens for `account/rateLimits/updated`. It forwards the Codex bucket's primary and secondary rolling windows to the StickS3.

The current observed App Server payload uses:

- `primary.windowDurationMins = 300`, displayed as `5h`
- `secondary.windowDurationMins = 10080`, displayed as `7d`

App Server reports `usedPercent`; the bridge sends both used and remaining percentages, and the device displays remaining percentages with compact bars.

Token totals are optional. The bridge listens for `thread/tokenUsage/updated` and forwards totals when the App Server emits them, but the device displays `TOK n/a` until that event is available.

In `desktop-observer` mode, the bridge reads `event_msg` `token_count` events from rollout JSONL. Those events use `used_percent` and `window_minutes`; the observer normalizes them to the same device payload used by App Server mode.

## Current Validation Status

Validated on 2026-06-10:

- Python tests cover protocol serialization, approval mapping in the bridge, rate-limit normalization, plan summaries, goal summaries, token usage forwarding, cleared goals, Desktop observer rollout selection, and structured snapshot parsing.
- Firmware builds with PlatformIO.
- Simon's physical StickS3 advertises as `Codex-S3-0470` over BLE.
- Direct USB flash works.
- The bridge can connect to the physical StickS3 over BLE.
- `desktop-observer` can mirror the current Codex Desktop rollout into dashboard snapshots.

Still to validate after the dashboard redesign:

- Flash the redesigned firmware to the physical StickS3.
- Confirm the one-screen layout, wrapped body text, settings menu, `NEW` behavior, and soft sounds on hardware.

## Security Notes

BLE is prototype-open in v1. `desktop-observer` forwards short activity snippets from local Codex rollout logs, so do not use it while secrets or sensitive transcript text are being displayed. Keep any App Server endpoint bound to localhost or a local Unix socket.

## Known Limitation

The public App Server protocol gives a client stream for the endpoint it is connected to. It does not document a passive control interface for every already-open Codex desktop-app panel. `desktop-observer` is a local-log workaround for status display only.
