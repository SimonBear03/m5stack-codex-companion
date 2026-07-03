# Mac Codex App Bridge

This repo targets the Codex app on Mac through a local bridge.

Public OpenAI docs describe Codex App Server as the protocol used for rich clients. It carries JSON-RPC messages over stdio, Unix socket, or WebSocket transports and includes command/file approval requests. The docs do not currently describe native BLE pairing from the desktop app to third-party hardware, so this repo treats BLE as the local companion device protocol and App Server as the Codex-side integration.

## Integration Modes

There are two bridge modes:

- `desktop-observer`: read-only status mirroring for the actual Codex Desktop app. It follows local rollout JSONL files under `~/.codex/sessions`, prefers non-subagent Desktop threads for activity, and forwards active/idle state, speaker-labeled activity, token totals, and rate limits. Rate-limit rows are seeded from recent `token_count` events across rollout files.
- `app-server`: JSON-RPC validation path for an App Server endpoint. It can still exercise approval and choice mapping in the Python bridge, but the current dashboard firmware is read-only.

True control of an already-open Codex Desktop app thread is currently blocked by product surface, not by M5Stack firmware. Codex Desktop may run private `stdio://` app-server processes without exposing the default app-server control socket at `~/.codex/app-server-control/app-server-control.sock`. Public docs describe how to expose an App Server transport, but not how third-party hardware can attach to an existing Desktop thread.

## Flow

```text
Codex Desktop rollout logs OR App Server endpoint
  <-> Mac bridge
  <-> BLE Nordic UART Service JSONL
  <-> M5Stack companion dashboard
```

For the local Desktop app observer experience, run:

```bash
sticks3-bridge --log-level INFO desktop-observer --scan-timeout 15
```

Observer mode auto-selects the freshest non-subagent rollout under `~/.codex/sessions`. To pin a specific thread or file:

```bash
sticks3-bridge desktop-observer --thread-id <thread-id>
sticks3-bridge desktop-observer --rollout ~/.codex/sessions/.../rollout-....jsonl
```

Observer mode is intentionally read-only. It mirrors status, activity, usage bars, and token totals. Activity follows the selected/current thread, while the `5h` and `7d` usage bars are account-level and can be seeded from recent rollout `token_count` events before the current thread starts a new task. The device `Detail` setting controls how much text the bridge sends: `Full` sends message activity, `Status` suppresses body text, and `Usage` sends generic state plus usage only, without legacy `msg`/`entries` text. Observer mode cannot approve prompts, answer prompts, or interrupt turns in the Codex Desktop UI.

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

The native `M5Stack Codex Companion.app` wraps `desktop-observer` with a PID file,
status JSON, and log file so it can be controlled from the macOS menu bar:

```bash
python3 scripts/sticks3-macos-bridge start
python3 scripts/sticks3-macos-bridge status
python3 scripts/sticks3-macos-bridge restart
python3 scripts/sticks3-macos-bridge stop
```

Runtime files live in `runtime/` and are ignored by Git:

- `bridge.pid`: the process started by the supervisor
- `bridge-status.json`: lifecycle, canonical `codex_activity`, legacy current status, thread, tokens, rate limits, and latest device telemetry when a companion replies to `status`
- `bridge.log`: bridge stdout/stderr from background starts

The generated Bluetooth-permission wrapper is installed outside the repo at
`~/Library/Application Support/M5Stack Codex Companion/.M5StackCodexBridge.app`.
The support directory is marked out of Spotlight indexing so only the native menu
bar app should appear as a user-openable app.

The bridge retries BLE scan/connect/write failures by default. Normal device
sleep, advertising delays, or transient disconnects should move the helper
between `BLE 0/n` and `BLE n/n` states without a manual restart. BLE liveness is
checked while idle so a closed link is noticed before the next long heartbeat,
and retry backoff is kept short for
faster wake/reconnect. Default BLE scans use named `Codex-S3-*` and
`Codex-CP-*` devices for discovery, not arbitrary Nordic UART Service
peripherals, but Desktop observer sync is paired-only. Pairing records live at
`~/Library/Application Support/M5Stack Codex Companion/paired-devices.json` and
the observer fans out snapshots only after the device id authenticates with its
stored secret. Each paired companion has an independent reconnect loop, status
ack, and detail/privacy setting. After a BLE connect, the bridge re-polls Codex
state, sends a compact first sync packet, sends the latest full snapshot, then
asks the device for settings/status. The firmware top bar shows `SYNC` until
that first valid snapshot is parsed.

The helper does not search for and kill unrelated `sticks3-bridge` processes. If
you started the observer manually in a terminal, stop that terminal process before
switching to the supervisor so BLE is not claimed by two processes.

Build or update the native menu bar app from the repo root:

```bash
/bin/zsh scripts/build-macos-companion --launch
```

That builds and opens a locally signed app at
`/Applications/M5Stack Codex Companion.app`.
Opening it queues `scripts/sticks3-macos-bridge ensure` when the bridge is
stopped. The app reads `runtime/bridge-status.json` every 2 seconds and exposes
Start, Stop, Restart, Open Log, Open Repo, Reveal Helper, and Quit actions. The
popover lists companions with board/name, connected/scanning/disconnected state,
detail mode, last-seen time, and any error. Single-device and multi-device
states use the same layout. No paid Apple Developer account is needed for local
use; the app is ad-hoc signed. The app stores this repo path in its
`Info.plist`, so it can live in Applications while still controlling the local
bridge code and venv. This is a developer install shape, not a public release
artifact; see `docs/distribution.md` for the signed/notarized app and Homebrew
Cask plan.

The supervisor generates a stable local helper wrapper at
`~/Library/Application Support/M5Stack Codex Companion/.M5StackCodexBridge.app` with
the Bluetooth usage string. Its native launcher embeds the repo's bridge venv
Python in the helper app process. The wrapper is reused and only rebuilt when
generated contents change; this avoids showing a second app next to
`M5Stack Codex Companion.app`. If macOS prompts for Bluetooth
access to `M5Stack Codex Bridge`, allow it.
The first launch migrates the old StickS3-branded pairing store when needed, so
already-paired devices keep trusting the same Mac host secret after the app
renames its support directory.

Because this repo lives under `~/Documents`, macOS may also ask once for folder
access when the app reads the bridge code and local Codex rollout logs. That
should be a one-time Files & Folders permission for `M5Stack Codex Bridge`.
Repeated prompts usually mean the helper app was regenerated with a new identity,
launched from a different bundle path, or the previous prompt was denied.

The menu title is intentionally compact:

- `Codex Off · BLE 0/0`: no supervised bridge process
- `Codex Wait · BLE 0/2`: bridge is running with no paired companions connected
- `Codex Idle · BLE 2/2`: Codex observer is current and all paired companions are connected
- `Codex Work · BLE 1/2`: Codex is active while one paired companion is connected
- `Codex Err · BLE 0/2`: bridge or device error

`Codex Work` uses the same recent-work window as the firmware top bar. Tool
calls, tool output, patches, task starts, and Codex messages keep the bridge in
work state briefly even if Codex Desktop batches a later completion event into
the same poll. Old rollout events are not replayed as live work on startup.

The Start/Stop/Restart actions in the menu call the same supervisor script.

For launch-at-login, install the generated LaunchAgent:

```bash
python3 scripts/sticks3-macos-bridge install-agent --scan-timeout 60
python3 scripts/sticks3-macos-bridge agent-status
```

The installer writes
`~/Library/LaunchAgents/com.simon.m5stack-codex-companion.bridge.plist` and
installs the generated helper app under Application Support. The bridge itself
retries BLE failures forever by default, so normal companion display sleep,
advertising delay, and transient disconnects should not require a restart.
Installing unloads and removes the old StickS3-branded LaunchAgent/helper when
present so macOS does not run two bridge processes.

On newer macOS builds, launch-at-login is best-effort until the background item
is accepted by the user. If `agent-status` reports installed/loaded while the
bridge supervisor is still stopped, macOS blocked launchd before the bridge
could start. Check System Settings -> General -> Login Items & Extensions and
allow the M5Stack Codex background item. The native menu app remains the fallback
controller when the background item is blocked. The supervisor's `start` command
falls back to direct app-wrapper launch when LaunchAgent kickstart does not
produce a PID. Unload and remove the LaunchAgent later with:

```bash
python3 scripts/sticks3-macos-bridge uninstall-agent
```

## Dashboard Mapping

The bridge sends:

- `status`: pinned newest action with `speaker`, `kind`, and `text`.
- `activity`: scrollable body entries with stable `seq` IDs for dedupe, sent only in `Detail Full`.
- `tokens`: optional compact token total from the latest observed token event.
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

StickS3 main dashboard:

- Button A: newer/down through body text.
- Button B: older/up through body text.
- Long A: open settings.
- Long B: jump to newest when reading older text and the unread dot is shown; otherwise enter display sleep.
- A+B: no-op in the current dashboard firmware.

Cardputer main dashboard:

- Up key, or `;` without Fn: older/up one line through body text.
- Down key, or `.` without Fn: newer/down one line through body text.
- Left key, or `,` without Fn: older/up one page through body text.
- Right key, or `/` without Fn: newer/down one page through body text.
- Enter: open settings.
- Backspace, Esc, or `` ` `` without Fn: jump to newest when reading older text.
- GO/G0 short: jump to newest when reading older text.
- GO/G0 long: enter display sleep.

StickS3 settings menu:

- Button B: next option.
- Button A: rotate/toggle selected value.
- Long A: close settings.
- Auto-closes after 12 seconds of no input.

Cardputer settings menu:

- Up/Down, or `;`/`.` without Fn: previous/next option.
- Left/Right, or `,`/`/` without Fn: previous/next value.
- Enter or GO/G0 short: rotate/toggle selected value forward.
- Backspace, Esc, or `` ` `` without Fn: close settings.
- GO/G0 long: enter display sleep.
- Auto-closes after 12 seconds of no input.

Both boards:

- Settings options are brightness, power profile, detail level, sound, text navigation, auto-newest, and rotation.
- Detail supports `Full`, `Status`, and `Usage`. `Full` shows scrollable message activity. `Status` keeps generic current status but suppresses body text. `Usage` keeps usage/token/device state and generic work/idle state without transmitting message bodies, tool names, or legacy text fields.
- Rotation supports `Auto`, `Lock`, `P`, `L`, `P180`, and `L180`; `Auto` uses the StickS3 IMU and reflows wrapped body text when the display switches between portrait and landscape.
- `Always` keeps the display awake for desk use and is not downgraded by the low-battery auto policy. `Auto` and `Low` auto-sleep the display after about 10 seconds when there is no unread marker, settings are closed, and you are not reading older text. There is no pre-sleep dimming step. Shake wake is armed 2.5 seconds after sleep starts, while button input and new Codex BLE activity can wake immediately.
- Power profiles tune brightness behavior, automatic display sleep, animation cadence, loop delay, BLE TX/connection parameters, and CPU speed while keeping BLE reachable.

## Usage Mapping

The bridge calls App Server `account/rateLimits/read` after initialization and listens for `account/rateLimits/updated`. It forwards the Codex bucket's primary and secondary rolling windows to the companion.

The current observed App Server payload uses:

- `primary.windowDurationMins = 300`, displayed as `5h`
- `secondary.windowDurationMins = 10080`, displayed as `7d`

App Server reports `usedPercent`; the bridge sends both used and remaining percentages, and the device displays remaining percentages with compact bars.

Token totals are optional. The bridge listens for `thread/tokenUsage/updated` and forwards totals when the App Server emits them, but the device displays `TOK n/a` until that event is available.

In `desktop-observer` mode, the bridge reads `event_msg` `token_count` events from rollout JSONL. Those events use `used_percent` and `window_minutes`; the observer normalizes them to the same device payload used by App Server mode. On startup and about every 5 seconds after that, it scans recently changed rollout files with a small file cache for the freshest `token_count` event, so reset/refill updates replace stale low-limit values quickly even if the current observed thread has not emitted its own usage event yet. The forwarded `tokens` value intentionally comes from `info.total_token_usage.total_tokens` in that event.

## Battery Behavior

Battery-saving behavior is mostly firmware-local. The Mac bridge does not need to know when the display is asleep; BLE stays connected in display sleep and incoming snapshots can wake the screen. Power profiles no longer enter ESP32 deep sleep or PMIC shutdown, because those states break the live monitor connection.

The device now:

- caches battery telemetry and samples it roughly every 5 seconds,
- reports battery percent, voltage/current when supported, and CPU MHz in `status` acks,
- lowers LCD brightness levels and defaults to lower settings on battery when no saved preference exists,
- temporarily forces effective `Low` behavior at 20% battery or lower while preserving the saved profile,
- keeps hardware-validated soft speaker cues enabled through M5Unified's StickS3 speaker path without manually toggling the audio enable path from the unused-rail saver,
- repeatedly turns off PMIC indicator LEDs without disabling speaker or power rails,
- stops redraws for unchanged heartbeat snapshots,
- clears partial BLE receive data on connect/disconnect and ignores malformed transport fragments without putting the dashboard into `ERR`,
- slows animations and the main loop when idle/asleep, including during `WORK`,
- lowers BLE transmit power and relaxes advertising/connection intervals according to the selected power profile,
- leaves automatic display sleep disabled in `Always` for desk use,
- enters display sleep after about 10 seconds in `Auto` and `Low` when settings are closed, no unread marker is present, and you are not reading older text,
- keeps BLE reachable in all power profiles; use the physical power button when you want to turn the StickS3 off.

The Desktop observer also reduces battery impact by sending only new activity records after the first snapshot, omitting legacy text fields in lower detail modes, stripping message payloads in lower detail modes, skipping duplicate sanitized snapshots, and using a slower idle heartbeat. BLE JSONL chunks use acknowledged writes for reliability. Active or recently work-like Codex turns still send immediately when the rollout changes.

## Current Validation Status

Validated on 2026-06-10:

- Python tests cover protocol serialization, approval mapping in the bridge, rate-limit normalization, plan summaries, goal summaries, token usage forwarding, cleared goals, Desktop observer rollout selection, and structured snapshot parsing.
- Firmware builds with PlatformIO.
- A physical StickS3 advertises as `Codex-S3-XXXX` over BLE.
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
