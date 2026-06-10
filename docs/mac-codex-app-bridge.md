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

## Menu Bar Supervisor

The macOS helper wraps `desktop-observer` with a PID file, status JSON, and log
file so it can be controlled from a menu bar app:

```bash
scripts/sticks3-macos-bridge start
scripts/sticks3-macos-bridge status
scripts/sticks3-macos-bridge restart
scripts/sticks3-macos-bridge stop
```

Runtime files live in `runtime/` and are ignored by Git:

- `bridge.pid`: the process started by the supervisor
- `bridge-status.json`: lifecycle, current status, thread, tokens, rate limits, and latest device telemetry when the StickS3 replies to `status`
- `bridge.log`: bridge stdout/stderr from background starts
- `StickS3Bridge.app`: generated macOS app wrapper for Bluetooth permission

The helper does not search for and kill unrelated `sticks3-bridge` processes. If
you started the observer manually in a terminal, stop that terminal process before
switching to the supervisor so BLE is not claimed by two processes.

SwiftBar must not launch Homebrew Python directly for BLE. New macOS TCC checks
can kill that Python process because its app bundle lacks
`NSBluetoothAlwaysUsageDescription`. The supervisor now generates and opens
`runtime/StickS3Bridge.app`, a local app wrapper with the Bluetooth usage string,
and runs the bridge inside that app process. If macOS prompts for Bluetooth
access to `StickS3 Codex Bridge`, allow it.

For SwiftBar/xbar, use the included plugin folder:

```text
/Users/simon/Documents/workspace/repos/sticks3-codex-companion/macos/swiftbar
```

The plugin calls:

```bash
scripts/sticks3-macos-bridge swiftbar
```

The menu title is intentionally compact:

- `S3 Off`: no supervised bridge process
- `S3 Link`: starting or scanning
- `S3 Idle`: connected but not actively working
- `S3 Work`: Codex is active or the latest status is work-like
- `S3 Err`: bridge error

The Start/Stop/Restart actions in the menu call the same supervisor script.

For launch-at-login, copy the template:

```bash
cp macos/LaunchAgents/com.simon.sticks3-codex-companion.plist ~/Library/LaunchAgents/
launchctl bootstrap "gui/$(id -u)" ~/Library/LaunchAgents/com.simon.sticks3-codex-companion.plist
```

Unload it later with:

```bash
launchctl bootout "gui/$(id -u)" ~/Library/LaunchAgents/com.simon.sticks3-codex-companion.plist
```

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
- function/custom tool calls -> pinned `Tool` status with a cleaned tool name; not added to body scrollback
- function output -> concise `Tool: output ready`
- `token_count` -> usage fields only; it does not overwrite the current action line
- `patch_apply_end` -> `Tool: Patch applied` or `Tool: Patch failed`

## Device Controls

Main dashboard:

- Button A: newer/down through body text.
- Button B: older/up through body text.
- Long A: open settings.
- Long B: jump to newest when reading older text and the unread dot is shown; otherwise enter display sleep.
- A+B: no-op in the current dashboard firmware.

Settings menu:

- Button B: next option.
- Button A: rotate/toggle selected value.
- Long A: close settings.
- Auto-closes after 12 seconds of no input.
- Settings options are brightness, power profile, sound, text navigation, and auto-newest.
- The display auto-sleeps after about 10 seconds in normal modes when there is no unread marker, settings are closed, and you are not reading older text. There is no pre-sleep dimming step. Shake wake is armed 2.5 seconds after sleep starts, while button input and new Codex BLE activity can wake immediately.
- Power profiles tune brightness behavior, animation cadence, loop delay, BLE TX/connection parameters, and optional long-idle deep sleep in `Max`.
- `Travel` is an aggressive battery profile. After display sleep and a short idle window on battery, it requests M5PM1 PMIC shutdown. BLE cannot receive new Codex messages while the device is in that state.

## Usage Mapping

The bridge calls App Server `account/rateLimits/read` after initialization and listens for `account/rateLimits/updated`. It forwards the Codex bucket's primary and secondary rolling windows to the StickS3.

The current observed App Server payload uses:

- `primary.windowDurationMins = 300`, displayed as `5h`
- `secondary.windowDurationMins = 10080`, displayed as `7d`

App Server reports `usedPercent`; the bridge sends both used and remaining percentages, and the device displays remaining percentages with compact bars.

Token totals are optional. The bridge listens for `thread/tokenUsage/updated` and forwards totals when the App Server emits them, but the device displays `TOK n/a` until that event is available.

In `desktop-observer` mode, the bridge reads `event_msg` `token_count` events from rollout JSONL. Those events use `used_percent` and `window_minutes`; the observer normalizes them to the same device payload used by App Server mode.

## Battery Behavior

Battery-saving behavior is mostly firmware-local. The Mac bridge does not need to know when the display is asleep; BLE stays connected in normal display sleep and incoming snapshots can wake the screen. `Travel` mode is different because it powers the device down and drops BLE until the StickS3 is woken/restarted.

The device now:

- caches battery telemetry and samples it roughly every 30 seconds,
- reports battery percent, voltage/current when supported, and CPU MHz in `status` acks,
- lowers LCD brightness levels and defaults to lower settings on battery when no saved preference exists,
- temporarily forces effective `Max` behavior at 20% battery or lower while preserving the saved profile,
- keeps hardware-validated soft speaker cues enabled through M5Unified's StickS3 speaker path without manually toggling the audio enable path from the unused-rail saver,
- explicitly keeps PMIC LEDs and external 5V/boost off unless needed,
- stops redraws for unchanged heartbeat snapshots,
- slows animations and the main loop when idle/asleep, including during `WORK`,
- lowers BLE transmit power and relaxes advertising/connection intervals according to the selected power profile,
- enters display sleep after about 10 seconds in all modes when settings are closed, no unread marker is present, and you are not reading older text,
- optionally enters ESP32 deep sleep in `Max` mode after extended idle, which disconnects BLE until the device is woken and reboots,
- requests M5PM1 PMIC shutdown in `Travel` mode after idle display sleep for longer standby at the cost of BLE wake.

The Desktop observer also reduces battery impact by sending only new activity records after the first snapshot and by using a slower idle heartbeat. Active Codex turns still send immediately when the rollout changes.

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
- Confirm the one-screen layout, wrapped body text, settings menu, and unread-dot behavior on hardware. Soft sounds have been validated on the physical StickS3 after removing manual audio-enable toggling.

## Security Notes

BLE is prototype-open in v1. `desktop-observer` forwards short activity snippets from local Codex rollout logs, so do not use it while secrets or sensitive transcript text are being displayed. Keep any App Server endpoint bound to localhost or a local Unix socket.

## Known Limitation

The public App Server protocol gives a client stream for the endpoint it is connected to. It does not document a passive control interface for every already-open Codex desktop-app panel. `desktop-observer` is a local-log workaround for status display only.
