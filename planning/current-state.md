# Current State

Created: 2026-06-09
Updated: 2026-06-11

## Goal

Build Simon's first custom M5Stack StickS3 program: a serious minimal Codex dashboard that connects over BLE, mirrors what Codex Desktop is doing, and surfaces usage information.

The target experience is Codex app on Mac first, not Codex CLI compatibility.

## Product Intent

The StickS3 should act as a compact terminal-style status device:

- One main dashboard screen, not a multi-page app.
- Pinned top/status section with muted mode color, flat 3x3 status animation matrix, small unread dot, BLE/USB/battery state, `5h` and `7d` remaining bars, compact tokens, and the newest current action.
- Scrollable wrapped body text for recent conversation activity, rendered as compact message blocks with colored speaker headers and blank separators.
- Speaker labels in current status and body text: `User`, `Codex`, `System`, and subagent names if exposed later. Tool events stay in the pinned current status and do not enter scrollback.
- Button A moves newer/down through body text.
- Button B moves older/up through body text.
- Long A opens/closes settings.
- Long B jumps to newest when reading older text and the unread dot is visible; otherwise it enters display sleep.
- Settings include brightness, power profile, sound, text navigation, and auto-newest.
- Display auto-sleeps after about 10 seconds in all modes when no unread marker is present, settings are closed, and the user is not reading older text; there is no pre-sleep dimming.
- Normal display sleep keeps BLE reachable; `Travel` power mode requests PMIC shutdown after idle display sleep and trades away BLE wake for longer standby.
- 20% battery or lower on battery temporarily forces effective `Max` behavior while preserving the saved profile.
- Soft, office-safe sound cues for activity and important state changes.
- Firmware attempts to turn off the PMIC/internal green board status LED at boot.
- Read-only operation for the current Codex Desktop workflow.

## Architecture Decision

Use a bridge-first architecture.

Public OpenAI docs document Codex App Server as the rich-client protocol for Codex, but they do not document native Codex desktop BLE hardware pairing or third-party attachment to an already-open Desktop thread. The StickS3 therefore speaks a small JSONL-over-BLE protocol, while the Mac bridge maps Codex Desktop rollout logs or App Server events into dashboard snapshots.

The StickS3 advertises a BLE name starting with `Codex-` and exposes Nordic UART Service:

- Service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX desktop to device: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- TX device to desktop: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

Payloads are newline-delimited JSON objects.

## Integration Modes

- `desktop-observer` reads local Codex Desktop rollout JSONL under `~/.codex/sessions` and mirrors active/idle state, speaker-labeled activity, token totals, and rate-limit windows from the actual Desktop thread.
- `app-server` connects to a Codex App Server endpoint and remains useful for bridge protocol validation. The Python bridge still contains approval/choice mapping logic, but the current firmware is read-only.

The current product path is status mirroring through `desktop-observer`. True control of an already-open Codex Desktop app thread remains blocked until Codex exposes a documented local attach/control endpoint.

## Files

- `README.md` - project overview, build path, controls, and pairing path.
- `docs/protocol.md` - BLE UUIDs and JSON dashboard snapshot format.
- `docs/mac-codex-app-bridge.md` - Mac Codex bridge flow and limitations.
- `docs/cardputer-references.md` - Cardputer references and design takeaways for small-screen UX.
- `bridge/sticks3_bridge/` - Python bridge for Desktop observer and App Server JSON-RPC.
- `scripts/sticks3-macos-bridge` - macOS supervisor for starting, stopping, and reporting Desktop observer state.
- `macos/swiftbar/sticks3-codex.5s.sh` - SwiftBar/xbar menu plugin that displays bridge state and exposes Start/Stop/Restart.
- `macos/LaunchAgents/com.simon.sticks3-codex-companion.plist` - launch-at-login template for the supervisor.
- `runtime/StickS3Bridge.app` - generated local app wrapper, ignored by Git, used so macOS can grant Bluetooth permission to the menu-bar-started bridge.
- `tests/` - Python protocol and bridge tests.
- `platformio.ini` - PlatformIO config targeting ESP32-S3 Arduino with M5Unified, M5PM1, ArduinoJson, and NimBLE-Arduino.
- `src/main.cpp` - current firmware implementation:
  - BLE advertising as `Codex-S3-XXXX`.
  - NimBLE-based NUS implementation.
  - JSON line parser.
  - one-screen dashboard renderer.
  - structured `status` and `activity` snapshot handling.
  - raw activity message cache plus wrapped activity ring buffer with page/line navigation, colored message headers, and blank message separators.
  - compact text wraps from raw message text using rendered pixel width.
  - 1000-character desktop observer activity cap with 4 recent activity records on first send, then delta activity snapshots.
  - 8192-byte firmware JSON line receive buffer.
  - smart punctuation normalization before device display.
  - tool activity filtering so tool events update pinned status without filling scrollback.
  - settings menu and persisted dashboard settings.
  - display sleep with long-B shortcut, delayed shake wake, BLE wake, and work/new-activity wake.
  - battery-saving power profiles: Balanced, Saver, Max, Travel.
  - cached battery telemetry with voltage/current status ack fields.
  - lower brightness levels, low-battery Max override, PMIC LED/external boost shutdown, reduced redraws, adaptive loop delays, BLE TX/interval tuning, optional long-idle deep sleep in Max mode, and PMIC shutdown in Travel mode.
  - soft buzzer cues.
  - status command ack.
  - owner/name/unpair command ack.

## Current Build State

This repo has completed successful firmware builds on Simon's Mac with repo-local PlatformIO and has been flashed to the physical StickS3 over USB.

Current validation:

- `.venv/bin/pio run` succeeds on Simon's Mac.
- `.bridge-venv/bin/python -m unittest discover -s tests` succeeds with 26 tests.
- Firmware binary is about 1.10 MB.
- USB flashing to `/dev/cu.usbmodem101` and `/dev/cu.usbmodem2101` has succeeded.
- BLE status validation has succeeded; Simon's device replies as `Codex-S3-0470`.
- `desktop-observer` parses local Codex Desktop rollouts, skips newer subagent rollouts by default, normalizes token/rate-limit events, and emits structured `status`/`activity` snapshots.
- `desktop-observer` can write a status JSON file for the macOS menu bar helper.
- `scripts/sticks3-macos-bridge` provides supervised start/stop/restart/status and SwiftBar/xbar output without killing manually started bridge processes.
- `scripts/sticks3-macos-bridge start` now launches the bridge through a generated `StickS3Bridge.app` wrapper with an `NSBluetoothAlwaysUsageDescription`, avoiding SwiftBar-launched Homebrew Python TCC crashes on macOS 27.
- The old flicker issue was addressed by drawing to an `M5Canvas` sprite and pushing only on redraw.
- The battery pass builds and adds 10-second all-mode display sleep, power profiles, telemetry, redraw throttling, adaptive loop delay, BLE power tuning, low-battery Max override, explicit PMIC LED/boost shutdown, and Travel-mode PMIC shutdown.
- Manual audio-enable toggling was removed after hardware noise appeared; sound cues now work on hardware through M5Unified's StickS3 speaker path.
- The Desktop observer now slows idle heartbeat traffic and sends only new activity records after the first snapshot instead of repeating scrollback on every heartbeat.

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
scripts/sticks3-macos-bridge start
scripts/sticks3-macos-bridge status
```

SwiftBar/xbar can use this plugin folder:

```text
/Users/simon/Documents/workspace/repos/sticks3-codex-companion/macos/swiftbar
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
13. Confirm display auto-sleeps after about 10 seconds in WORK, IDLE, WAIT, STALE, ERR, and OFF modes when not reading old text, with no pre-sleep dimming.
14. Confirm sound modes are quiet and not repetitive.
15. Confirm no old pet wording, screens, stats, or animations remain.
16. Confirm `scripts/sticks3-macos-bridge start/status/stop` controls the bridge when no manual observer is already running.
17. Confirm the SwiftBar/xbar plugin shows `S3 Link`, `S3 Idle`, `S3 Work`, and `S3 Err` states correctly.
18. Confirm new BLE activity and shake wake the display after auto sleep.
19. Confirm speaker cues play as tones, not only pops, after removing manual audio-enable toggling. Done on 2026-06-11.
20. Confirm battery telemetry appears in status ack and settings footer.
21. Confirm low-battery effective Max behavior when battery is 20% or lower.
22. Confirm `Travel` mode enters PMIC shutdown after display sleep on battery and document the actual wake behavior on hardware.
23. Confirm idle observer heartbeats no longer duplicate body activity.

## Known Risks

- StickS3 PlatformIO board support may require tuning. Current config uses M5Stack's documented `esp32-s3-devkitc-1` PlatformIO shape plus StickS3-relevant flags.
- M5Unified APIs may differ across versions, especially power, speaker, display, or button APIs.
- The public Codex App Server protocol does not guarantee passive observation of every already-open desktop-app panel.
- `desktop-observer` reads local rollout logs and is status-only; it cannot approve, answer prompts, or interrupt the active Desktop UI.
- Token totals are session/event dependent and may remain unavailable until Codex Desktop writes token events.
- Chinese display is in test mode: UTF-8 travels through BLE/JSON, dashboard text uses `efontCN_14` for both ASCII and non-ASCII body/current-status text, and wrapping is UTF-8/pixel-width aware. Broader CJK typography and mixed-language polish still need hardware validation.
- The internal green LED off path uses the M5PM1 PMIC API and still needs hardware validation; boot/download indicator behavior may be hardware-controlled.
- Travel mode uses M5PM1 PMIC shutdown and best-effort PMIC GPIO4 wake-source arming. BLE cannot wake the device from this state; actual shake wake depends on the board IMU interrupt chain and still needs hardware validation.
- M5Launcher WebUI install is not currently reliable for this no-SD StickS3 workflow.

## Immediate Next Engineering Task

Validate the remaining dashboard, battery, and Mac helper behavior on hardware: PMIC green LED off attempt, unread marker, 14px CJK-capable dashboard text, new activity/shake wake behavior, battery telemetry, low-battery effective Max behavior, Travel-mode shutdown/wake behavior, idle observer heartbeat dedupe, and the SwiftBar/xbar bridge supervisor workflow.
