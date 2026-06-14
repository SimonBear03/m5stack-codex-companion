# Current State

Created: 2026-06-09
Updated: 2026-06-14

## Goal

Build Simon's first custom M5Stack StickS3 program: a serious minimal Codex dashboard that connects over BLE, mirrors what Codex Desktop is doing, and surfaces usage information.

The target experience is Codex app on Mac first, not Codex CLI compatibility.

## Product Intent

The StickS3 should act as a compact terminal-style status device:

- One main dashboard screen, not a multi-page app.
- Pinned top/status section with muted mode color, flat 3x3 status animation matrix, small unread marker, BLE/USB/battery state, `5h` and `7d` remaining bars, compact tokens, and the newest current action.
- Scrollable wrapped body text for recent conversation activity in `Detail Full`, rendered as compact message blocks with colored speaker headers and blank separators.
- Portrait and landscape layouts are both supported. Auto-rotation uses the StickS3 IMU, allows all four physical directions, and rewraps raw message blocks when the display geometry changes.
- Speaker labels in current status and body text: `User`, `Codex`, `System`, and subagent names if exposed later. Tool events stay in the pinned current status and do not enter scrollback.
- Button A moves newer/down through body text.
- Button B moves older/up through body text.
- Long A opens/closes settings.
- Long B jumps to newest when reading older text and the unread dot is visible; otherwise it enters display sleep.
- Settings include brightness, power profile, detail level, sound, text navigation, auto-newest, and rotation.
- Detail modes are `Full`, `Status`, and `Usage`; lower detail modes suppress message bodies to reduce BLE payloads, redraw work, and privacy exposure.
- `Always` stays awake for desk use; `Auto` and `Low` auto-sleep the display after about 10 seconds when no unread marker is present, settings are closed, and the user is not reading older text; there is no pre-sleep dimming.
- All power profiles keep BLE reachable; the physical power button is used when the StickS3 should be turned off.
- 20% battery or lower on battery temporarily forces effective `Low` behavior while preserving the saved profile.
- Soft, office-safe sound cues for activity and important state changes.
- Firmware attempts to turn off the PMIC/internal green board status LED at boot.
- Read-only operation for the current Codex Desktop workflow.

## Architecture Decision

Use a bridge-first architecture.

Public OpenAI docs document Codex App Server as the rich-client protocol for Codex, but they do not document native Codex desktop BLE hardware pairing or third-party attachment to an already-open Desktop thread. The StickS3 therefore speaks a small JSONL-over-BLE protocol, while the Mac bridge maps Codex Desktop rollout logs or App Server events into dashboard snapshots.

The StickS3 advertises a BLE name starting with `Codex-S3-` and exposes Nordic UART Service:

- Service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX desktop to device: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- TX device to desktop: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

Payloads are newline-delimited JSON objects.

## Integration Modes

- `desktop-observer` reads local Codex Desktop rollout JSONL under `~/.codex/sessions`, mirrors active/idle state and speaker-labeled activity from the actual Desktop thread, and seeds rate-limit windows plus the latest rollout token total from recent `token_count` events.
- `app-server` connects to a Codex App Server endpoint and remains useful for bridge protocol validation. The Python bridge still contains approval/choice mapping logic, but the current firmware is read-only.

The current product path is status mirroring through `desktop-observer`. True control of an already-open Codex Desktop app thread remains blocked until Codex exposes a documented local attach/control endpoint.

## Files

- `README.md` - project overview, build path, controls, and pairing path.
- `docs/protocol.md` - BLE UUIDs and JSON dashboard snapshot format.
- `docs/mac-codex-app-bridge.md` - Mac Codex bridge flow and limitations.
- `docs/cardputer-references.md` - Cardputer references and design takeaways for small-screen UX.
- `bridge/sticks3_bridge/` - Python bridge for Desktop observer and App Server JSON-RPC.
- `scripts/sticks3-macos-bridge` - macOS supervisor for starting, stopping, reporting Desktop observer state, and installing a launch-at-login agent.
- `macos/StickS3Companion/` - native SwiftUI menu bar app that displays bridge state and exposes Start/Stop/Restart actions.
- `scripts/build-macos-companion` - packages `StickS3 Companion.app` into `~/Applications` using the installed Xcode toolchain and ad-hoc signing.
- `~/Applications/StickS3Bridge.app` - generated stable app wrapper used so macOS can grant Bluetooth permission to the menu-bar-started bridge.
- `tests/` - Python protocol and bridge tests.
- `platformio.ini` - PlatformIO config targeting ESP32-S3 Arduino with M5Unified, M5PM1, ArduinoJson, and NimBLE-Arduino.
- `src/main.cpp` - current firmware implementation:
  - BLE advertising as `Codex-S3-XXXX`.
  - `SYNC` top-bar mode while BLE is connected but the first valid dashboard snapshot has not arrived.
  - NimBLE-based NUS implementation.
  - JSON line parser.
  - one-screen dashboard renderer.
  - structured `status` and `activity` snapshot handling.
  - raw activity message cache plus wrapped activity ring buffer with page/line navigation, colored message headers, and blank message separators.
  - detail setting persisted as `Full`, `Status`, or `Usage`.
  - compact text wraps from raw message text using rendered pixel width.
  - 1000-character desktop observer activity cap with 4 recent activity records on first send, then delta activity snapshots.
  - acknowledged BLE writes for host-to-device JSON chunks.
  - 8192-byte firmware JSON line receive buffer.
  - smart punctuation normalization before device display.
  - tool activity filtering so tool events update pinned status without filling scrollback.
  - settings menu and persisted dashboard settings, including rotation mode.
  - autorotation with dedicated portrait/landscape layout metrics and body reflow from raw activity blocks.
  - profile-aware display sleep with long-B shortcut, delayed shake wake, BLE wake, and work/new-activity wake.
  - battery-saving power profiles: Always, Auto, Low.
  - cached battery telemetry with voltage/current status ack fields.
  - lower brightness levels, low-battery Low override, PMIC indicator LED suppression, reduced redraws, adaptive loop delays, and BLE TX/interval tuning while keeping BLE reachable.
  - BLE receive resync on connect/disconnect and malformed transport fragments ignored without entering dashboard `ERR`.
  - soft buzzer cues.
  - status command ack.
  - owner/name/unpair command ack.

## Current Build State

This repo has completed successful firmware builds on Simon's Mac with repo-local PlatformIO and has been flashed to the physical StickS3 over USB.

Current validation:

- `.venv/bin/pio run` succeeds on Simon's Mac.
- `.bridge-venv/bin/python -m unittest discover -s tests` succeeds.
- Firmware binary is about 1.10 MB.
- USB flashing to `/dev/cu.usbmodem101` and `/dev/cu.usbmodem2101` has succeeded.
- BLE status validation has succeeded; Simon's device replies as `Codex-S3-0470`.
- `desktop-observer` parses local Codex Desktop rollouts, skips newer subagent rollouts by default for activity, normalizes account token/rate-limit events from recent rollout files, emits structured `status`/`activity` snapshots, treats fresh tool/message activity as work-like for the dashboard top bar, and strips outgoing message payloads according to the device `Detail` setting.
- `desktop-observer` can write a status JSON file for the macOS menu bar helper.
- `scripts/sticks3-macos-bridge` provides supervised start/stop/restart/status and `install-agent`/`uninstall-agent` login auto-start management without killing unrelated manually started bridge processes.
- `scripts/sticks3-macos-bridge start` now launches the bridge through the stable generated `~/Applications/StickS3Bridge.app` wrapper with an `NSBluetoothAlwaysUsageDescription`. The wrapper executable embeds the bridge venv's Python runtime instead of `exec`ing `Python.app`, so Bluetooth access stays associated with `StickS3 Codex Bridge`. The generated app is reused and only rebuilt when its generated contents change, reducing repeated macOS Bluetooth and Files & Folders prompts.
- `scripts/sticks3-macos-bridge install-agent` installs `~/Library/LaunchAgents/com.simon.sticks3-codex-companion.bridge.plist` and a generated helper app at `~/Applications/StickS3Bridge.app`. On this Mac, launchd can still refuse or immediately stop the background item until macOS allows it under System Settings -> General -> Login Items & Extensions. Opening the native `StickS3 Companion.app` queues `ensure`, so it is the practical fallback controller.
- The old flicker issue was addressed by drawing to an `M5Canvas` sprite and pushing only on redraw.
- The battery pass builds and adds profile-aware display sleep, power profiles, telemetry, redraw throttling, adaptive loop delay, BLE power tuning, low-battery Low override for `Auto`, and PMIC indicator LED suppression without disabling speaker or power rails. Top bar power text shows `CHG` for active charging and `USB` for external power when charging is complete/paused/unknown. `Always` is no-auto-sleep even at low battery. Deep sleep and Travel/PMIC shutdown were removed from the normal profiles because they break live monitoring.
- Manual audio-enable toggling was removed after hardware noise appeared; sound cues now work on hardware through M5Unified's StickS3 speaker path.
- The Desktop observer now retries BLE scan/connect/write failures inside the long-lived observer, keeps Codex observation running while the StickS3 is disconnected, checks BLE liveness while idle, scans only named `Codex-S3-*` devices by default, re-polls Codex/usage state immediately after BLE connect, sends a compact first sync packet before the full forced snapshot, keeps status-ack writes best-effort after snapshot delivery, keeps reconnect backoff short, ignores stale rollout replay as live work, resets transient work status to idle after the grace window expires, scans recent rollout usage about every 5 seconds with a file-change cache, rejects older usage snapshots after seeing newer ones, slows idle heartbeat traffic, sends only new activity records after the first snapshot instead of repeating scrollback on every heartbeat, omits legacy `msg`/`entries` in lower detail modes, sends acknowledged BLE writes to avoid partial JSON lines, seeds rate-limit rows before the current thread emits its own `token_count`, clears work grace immediately on `task_complete`, and skips duplicate sanitized snapshots in lower detail modes. The current `TOK` value intentionally uses the latest `total_token_usage.total_tokens` from rollout `token_count`.

M5Launcher notes:

- WebUI upload/install was unreliable on StickS3 without SD storage.
- The first Launcher failure was `FAIL 365:5`, meaning the app binary exceeded the selected app slot. NimBLE reduced the firmware below that size check.
- After the size fix, WebUI still stalled during upload/write without a device-side error and the WebUI stopped responding to `/ping`.
- Direct USB flash is the working install path for now.
- A durable Launcher path should use an online/GitHub-release `.bin` that Launcher pulls through OTA, or an SD/storage-backed Launcher flow.

Generated local folders are intentionally ignored:

- `.venv/`
- `.bridge-venv/`
- `.pio/`
- `.platformio/`
- `runtime/`

They should be recreated on the next machine.

## Setup

Clone the repo:

```bash
git clone https://github.com/SimonBear03/sticks3-codex-companion.git
cd sticks3-codex-companion
```

Install PlatformIO using whichever path works on that machine.

Preferred if Homebrew works:

```bash
brew install platformio
pio run
```

Fallback with a local Python venv:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip platformio
pio run
```

If the build succeeds, upload:

```bash
pio run --target upload --upload-port /dev/cu.usbmodem101
```

If upload cannot connect, put StickS3 into download mode by holding the side reset/power button until the internal green LED blinks, then retry upload.

Bridge setup requires Python 3.11 or newer. If `.venv/` is Python 3.9 for PlatformIO, create a separate bridge venv:

```bash
python3.11 -m venv .bridge-venv
source .bridge-venv/bin/activate
python -m pip install -U pip
python -m pip install -e .
sticks3-bridge --log-level INFO desktop-observer --scan-timeout 15
```

Menu bar helper setup after the bridge venv exists:

```bash
scripts/sticks3-macos-bridge install-agent --scan-timeout 60
scripts/sticks3-macos-bridge status
```

If `agent-status` reports installed/loaded but `status` reports the supervisor
as stopped, macOS blocked the LaunchAgent before the bridge process started.
Allow the StickS3/zsh background item in System Settings -> General -> Login
Items & Extensions, or open `~/Applications/StickS3 Companion.app` / run
`scripts/sticks3-macos-bridge start` to launch the bridge in the current user
session.

Native menu bar app setup after the bridge venv exists:

```bash
scripts/build-macos-companion
open "$HOME/Applications/StickS3 Companion.app"
```

## Validation Plan

1. Compile firmware without errors.
2. Upload to StickS3 over USB.
3. Confirm screen shows `Advertise Codex-S3-XXXX` or connected dashboard state.
4. Confirm BLE advertises as `Codex-S3-XXXX`.
5. Run `sticks3-bridge --log-level INFO desktop-observer --scan-timeout 15`.
6. Confirm the device shows `5h` and `7d` remaining percentages and compact token totals.
7. Confirm the pinned current action updates with speaker labels.
8. Confirm body text wraps and no longer truncates to one row.
9. Confirm Button A moves newer/down and Button B moves older/up.
10. Confirm long A opens settings, Button B cycles options, Button A rotates values, and long A closes settings.
11. Confirm long B jumps to newest when the unread dot is shown.
12. Confirm long B enters display sleep when not reading old text.
13. Confirm `Always` stays awake, while `Auto` and `Low` auto-sleep after about 10 seconds when not reading old text, with no pre-sleep dimming.
14. Confirm sound modes are quiet and not repetitive.
15. Confirm no old pet wording, screens, stats, or animations remain.
16. Confirm `scripts/sticks3-macos-bridge start/status/stop` controls the bridge when no manual observer is already running.
17. Confirm the native menu bar app shows Codex-first states such as `Codex Wait · S3 Scan`, `Codex Idle · S3 BLE`, `Codex Work · S3 BLE`, and `Codex Err · S3 Err` correctly.
18. Confirm new BLE activity and shake wake the display after auto sleep.
19. Confirm speaker cues play as tones, not only pops, after removing manual audio-enable toggling. Done on 2026-06-11.
20. Confirm battery telemetry appears in status ack and settings footer.
21. Confirm low-battery effective Low behavior when battery is 20% or lower.
22. Confirm all power profiles remain BLE-reachable after display sleep.
23. Confirm idle observer heartbeats no longer duplicate body activity.
24. Confirm tool calls, tool output, patches, and Codex messages show `WORK` briefly even when no separate `task_started` snapshot reaches the device.
25. Confirm autorotate chooses all four directions with stable-device debounce, and that `Lock`, `P`, `L`, `P180`, and `L180` settings behave correctly.
26. Confirm landscape layout keeps top bar, usage, and current status pinned while body text scrolls and rewraps cleanly.
27. Confirm usage rows populate on bridge start from recent account `token_count` events even before the current thread starts a new task.
28. Confirm `Detail Full`, `Detail Status`, and `Detail Usage` change bridge payloads and device body rendering as intended.

## Known Risks

- StickS3 PlatformIO board support may require tuning. Current config uses M5Stack's documented `esp32-s3-devkitc-1` PlatformIO shape plus StickS3-relevant flags.
- M5Unified APIs may differ across versions, especially power, speaker, display, or button APIs.
- The public Codex App Server protocol does not guarantee passive observation of every already-open desktop-app panel.
- `desktop-observer` reads local rollout logs and is status-only; it cannot approve, answer prompts, or interrupt the active Desktop UI.
- Token totals are session/event dependent and may remain unavailable until Codex Desktop writes token events.
- Chinese display is in test mode: UTF-8 travels through BLE/JSON, dashboard text uses `efontCN_14` for both ASCII and non-ASCII body/current-status text, and wrapping is UTF-8/pixel-width aware. Broader CJK typography and mixed-language polish still need hardware validation.
- The internal green LED off path uses the M5PM1 PMIC API and still needs hardware validation; boot/download indicator behavior may be hardware-controlled.
- Deep sleep and PMIC shutdown are intentionally not exposed as power profiles. They are better treated as physical power-button behavior because BLE cannot wake the device from those states.
- M5Launcher WebUI install is not currently reliable for this no-SD StickS3 workflow.

## Immediate Next Engineering Task

Validate the remaining dashboard, battery, and Mac helper behavior on hardware: PMIC green LED off attempt, unread marker, 14px CJK-capable dashboard text, detail modes, usage-only payload silence, new activity/shake wake behavior, battery telemetry, low-battery effective Low behavior in `Auto`, BLE reachability across all power profiles, idle observer heartbeat dedupe, and the native menu bar bridge supervisor workflow.
