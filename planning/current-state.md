# Current State

Created: 2026-06-09
Updated: 2026-06-10

## Goal

Build Simon's first custom M5Stack StickS3 program: a serious minimal Codex dashboard that connects over BLE, mirrors what Codex Desktop is doing, and surfaces usage information.

The target experience is Codex app on Mac first, not Codex CLI compatibility.

## Product Intent

The StickS3 should act as a compact terminal-style status device:

- One main dashboard screen, not a multi-page app.
- Pinned top/status section with muted mode color, `NEW`, BLE/USB/battery state, `5h` and `7d` remaining bars, compact tokens, and the newest current action.
- Scrollable wrapped body text for recent Codex activity, rendered as compact message blocks instead of a continuous text stream.
- Speaker labels in current status and body text: `User`, `Codex`, `Tool`, `System`, and subagent names if exposed later.
- Button A moves newer/down through body text.
- Button B moves older/up through body text.
- Long A opens/closes settings.
- Long B jumps to newest when reading older text and `NEW` is available.
- Settings include brightness, sound, text navigation, text size, and auto-newest.
- Soft, office-safe sound cues for activity and important state changes.
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
- `tests/` - Python protocol and bridge tests.
- `platformio.ini` - PlatformIO config targeting ESP32-S3 Arduino with M5Unified, M5PM1, ArduinoJson, and NimBLE-Arduino.
- `src/main.cpp` - current firmware implementation:
  - BLE advertising as `Codex-S3-XXXX`.
  - NimBLE-based NUS implementation.
  - JSON line parser.
  - one-screen dashboard renderer.
  - structured `status` and `activity` snapshot handling.
  - wrapped activity ring buffer with page/line navigation and message-block rendering.
  - settings menu and persisted dashboard settings.
  - soft buzzer cues.
  - status command ack.
  - owner/name/unpair command ack.

## Current Build State

This repo has completed successful firmware builds on Simon's Mac with repo-local PlatformIO and has been flashed to the physical StickS3 over USB.

Current validation:

- `.venv/bin/pio run` succeeds on Simon's Mac.
- `PYTHONPATH=bridge python3 -m unittest discover -s tests` succeeds with 23 tests.
- Firmware binary is about 790 KB.
- USB flashing to `/dev/cu.usbmodem101` and `/dev/cu.usbmodem2101` has succeeded previously.
- BLE status validation has succeeded; Simon's device replies as `Codex-S3-0470`.
- `desktop-observer` parses local Codex Desktop rollouts, skips newer subagent rollouts by default, normalizes token/rate-limit events, and emits structured `status`/`activity` snapshots.
- The old flicker issue was addressed by drawing to an `M5Canvas` sprite and pushing only on redraw.

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
11. Confirm long B jumps to newest when `NEW` is shown.
12. Confirm sound modes are quiet and not repetitive.
13. Confirm no old pet wording, screens, stats, or animations remain.

## Known Risks

- StickS3 PlatformIO board support may require tuning. Current config uses M5Stack's documented `esp32-s3-devkitc-1` PlatformIO shape plus StickS3-relevant flags.
- M5Unified APIs may differ across versions, especially power, speaker, display, or button APIs.
- The public Codex App Server protocol does not guarantee passive observation of every already-open desktop-app panel.
- `desktop-observer` reads local rollout logs and is status-only; it cannot approve, answer prompts, or interrupt the active Desktop UI.
- Token totals are session/event dependent and may remain unavailable until Codex Desktop writes token events.
- M5Launcher WebUI install is not currently reliable for this no-SD StickS3 workflow.

## Immediate Next Engineering Task

Flash the redesigned dashboard firmware to the physical StickS3, start `desktop-observer`, and validate layout, scrolling, settings, `NEW`, and sound behavior on hardware.
