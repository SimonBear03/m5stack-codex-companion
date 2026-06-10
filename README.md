# StickS3 Codex Companion

Custom M5Stack StickS3 firmware for a serious minimal Codex status dashboard.

The StickS3 is a BLE display for Codex activity on Simon's Mac. A local bridge connects the device either to Codex Desktop rollout logs in read-only observer mode or to a Codex App Server endpoint for protocol validation. Public Codex docs do not currently document native desktop-app BLE pairing, so this repo uses a small JSONL-over-BLE device protocol.

## Current Scope

- One dense terminal-style dashboard screen.
- Pinned top/status section:
  - muted mode color: `RUN`, `IDLE`, `WAIT`, `STALE`, or `OFF`
  - `NEW` marker when newer body text arrives while reading older text
  - BLE, USB, and battery indicators
- Usage section:
  - `5h` and `7d` remaining percentages with compact bars
  - compact token totals such as `842`, `12.4K`, `57.6M`, or `1.2B`
- Pinned current action line with speaker labels such as `Codex`, `User`, `Tool`, and `System`.
- Scrollable wrapped body text backed by a fixed ring buffer.
- Settings menu for brightness, sound, text navigation, text size, and auto-newest behavior.
- Soft buzzer cues for activity, connected, completed, and disconnected/error events.
- BLE `status`, `owner`, `name`, and `unpair` command handling.

Current firmware is read-only for the Codex Desktop workflow. It does not expose approval or choice controls on-device.

## Bridge Modes

- `desktop-observer`: read-only status mirroring for the actual Codex Desktop app. It follows local rollout JSONL files under `~/.codex/sessions`, prefers non-subagent Desktop threads, and forwards active/idle state, speaker-labeled activity, token totals, and rate limits when those events are present.
- `app-server`: JSON-RPC validation path for a Codex App Server endpoint. The Python bridge still contains approval/choice mapping logic, but the current dashboard firmware is focused on read-only display.

True control of an already-open Codex Desktop app thread is blocked until Codex exposes a documented local attach/control endpoint.

## Implementation Status

As of 2026-06-10:

- Firmware builds successfully with PlatformIO on Simon's Mac.
- Firmware binary is about 790 KB, well within the direct-flash app size.
- Physical StickS3 USB flashing has been validated.
- The device advertises over BLE as `Codex-S3-0470` on Simon's StickS3.
- The bridge connects over BLE and can mirror the current Codex Desktop thread with `desktop-observer`.
- Python bridge tests pass.
- M5Launcher WebUI upload was unreliable on StickS3 without SD; direct USB flash is the current working install path.

## Build And Flash

```bash
git clone https://github.com/SimonBear03/sticks3-codex-companion.git
cd sticks3-codex-companion

python3 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip platformio
pio run
pio run --target upload --upload-port /dev/cu.usbmodem101
```

If the serial port differs, replace `/dev/cu.usbmodem101` with the port shown by:

```bash
pio device list
```

If upload cannot connect, put StickS3 into download mode by holding the side reset/power button until the internal green LED blinks, then retry upload.

Direct USB upload writes the firmware app to flash and is the validated path right now. It can bypass the current M5Launcher boot flow. Restore M5Launcher later through M5Burner if you want to return to the launcher environment.

## Bridge Setup

Use Python 3.11 or newer for the bridge. Keep this separate from the PlatformIO `.venv/` if that environment uses Python 3.9.

```bash
python3.11 -m venv .bridge-venv
source .bridge-venv/bin/activate
python -m pip install -U pip
python -m pip install -e .
```

Run read-only against local Codex Desktop rollout logs:

```bash
sticks3-bridge --log-level INFO desktop-observer --scan-timeout 15
```

By default this follows the freshest non-subagent rollout under `~/.codex/sessions`. You can pin it to a known thread or file:

```bash
sticks3-bridge desktop-observer --thread-id 019eaa0e-8e80-7821-aac1-a7c63bd09ad1
sticks3-bridge desktop-observer --rollout ~/.codex/sessions/2026/06/09/rollout-example.jsonl
```

For bridge validation without hardware:

```bash
sticks3-bridge desktop-observer --fake-device
sticks3-bridge app-server --transport stdio --fake-device --auto-decision deny
```

## Controls

Main dashboard:

- Button A: newer/down through body text.
- Button B: older/up through body text.
- Long A: open settings.
- Long B: jump to newest when reading older text and `NEW` is shown.
- A+B: no-op in the dashboard firmware.

Settings menu:

- Button B: next option.
- Button A: rotate/toggle selected value.
- Long A: close settings.
- Auto-closes after 12 seconds of no input.

Settings, in order:

- Brightness: `Low`, `Med`, `High`
- Sound: `Off`, `Soft`, `Alerts`
- Text nav: `Page`, `Line`
- Text size: `Compact`, `Readable`
- Auto newest: `On`, `Off`

## Repository Shape

```text
.
├── docs/
│   ├── mac-codex-app-bridge.md
│   ├── cardputer-references.md
│   └── protocol.md
├── bridge/
│   └── sticks3_bridge/
├── src/
│   └── main.cpp
├── tests/
└── platformio.ini
```

## Build Target

Device: M5Stack StickS3

Framework: Arduino on ESP32-S3

Primary libraries:

- M5Unified
- M5GFX, pulled through M5Unified dependencies
- M5PM1, for StickS3 PMIC support
- ArduinoJson
- NimBLE-Arduino

The PlatformIO setup uses `esp32-s3-devkitc-1` with StickS3-relevant build flags because StickS3 board support in PlatformIO may lag behind Arduino board manager support.

## M5Launcher Notes

- WebUI app install was tested on StickS3 without SD and did not complete reliably.
- Direct USB flash is the working install path for now.
- For a durable Launcher workflow, prefer an online/GitHub-release `.bin` that Launcher can pull through OTA, or use SD-capable storage when available.

## References

- M5Stack StickS3 Arduino guide: https://docs.m5stack.com/en/arduino/m5sticks3/program
- StickS3 product docs: https://docs.m5stack.com/en/core/StickS3
- Codex app-server docs: https://developers.openai.com/codex/app-server
- ESP32 GATT server docs: https://docs.espressif.com/projects/esp-idf/en/release-v5.1/esp32/api-reference/bluetooth/esp_gatts.html
