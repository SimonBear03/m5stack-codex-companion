# StickS3 Codex Companion

Custom M5Stack StickS3 firmware for showing Codex work status on a physical device.

The target is the Codex app on Mac. The StickS3 acts as a small BLE display/approval device, and a local Mac bridge connects it to a Codex App-compatible app-server endpoint. Public Codex docs do not currently document native desktop-app BLE pairing, so the bridge is the supported path.

## Current Scope

- Show Codex status: disconnected, idle, running, waiting, stale.
- Show latest status message and recent activity entries.
- Show pending approval prompt when provided by the host.
- Send approval decisions:
  - Button A: approve once when a prompt is active.
  - Button B: deny when a prompt is active.
- Show a six-page device UI:
  - Limits: 5-hour and 7-day Codex usage remaining.
  - Status: running/waiting threads and active approval prompt.
  - Plan: current Codex plan step.
  - Goal: current Codex thread goal.
  - Recent: latest Codex activity entries.
  - System: BLE, battery, and heap status.
- Show Codex rolling usage windows when sent by the host:
  - 5-hour remaining percentage
  - 7-day remaining percentage
  - optional total tokens
- Reply to host `status`, `owner`, `name`, and `unpair` commands.

The Mac bridge reads Codex App Server `account/rateLimits/read` and forwards the primary and secondary rolling windows to the StickS3. In the current App Server payload, those windows are 300 minutes and 10080 minutes, displayed as `5h` and `7d`. The bridge also listens for App Server plan and goal notifications and renders the current step/goal on their own pages.

## Implementation Status

As of 2026-06-10:

- Firmware builds successfully with PlatformIO in the VPS workspace.
- Python bridge protocol tests pass.
- Fake-device App Server smoke test initializes and receives Codex `5h` / `7d` rate-limit data.
- Physical StickS3 flashing and Mac Codex app endpoint validation are still the next hardware-side checks.

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

The M5Stack Arduino guide says to select the `M5StickS3` board in Arduino IDE and install `M5Unified` plus `M5GFX`. The PlatformIO setup here uses `esp32-s3-devkitc-1` with the StickS3-relevant build flags because StickS3 board support in PlatformIO may lag behind Arduino board manager support.

## Build With PlatformIO

Install PlatformIO first if needed:

```bash
brew install platformio
```

If Homebrew is unavailable, use a repo-local Python environment:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip platformio
```

Then build:

```bash
pio run
```

Upload:

```bash
pio run --target upload
```

If upload cannot connect, put StickS3 into download mode: hold the side reset/power button for about 2 seconds until the internal green LED blinks, then release.

## Mac Codex App Bridge

Install the Python bridge on the Mac:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip
python -m pip install -e .
```

Run against a running Codex App-compatible app-server WebSocket endpoint:

```bash
sticks3-bridge app-server --transport ws --target ws://127.0.0.1:4567
```

For bridge validation without hardware:

```bash
sticks3-bridge app-server --transport stdio --fake-device --auto-decision deny
```

This should initialize the App Server and log a snapshot containing `rate_limits.primary.label = 5h` and `rate_limits.secondary.label = 7d`. A command wrapped in `timeout` exits with code `124` when the long-running bridge is intentionally stopped.

See `docs/mac-codex-app-bridge.md` for the Mac-side flow and limitations.

## Controls

- Button A:
  - If approval prompt is active: approve once.
  - Otherwise: next screen.
- Button B:
  - If approval prompt is active: deny.
  - Otherwise: next screen.

## References

- M5Stack StickS3 Arduino guide: https://docs.m5stack.com/en/arduino/m5sticks3/program
- StickS3 product docs: https://docs.m5stack.com/en/core/StickS3
- Codex app-server docs: https://developers.openai.com/codex/app-server
- ESP32 GATT server docs: https://docs.espressif.com/projects/esp-idf/en/release-v5.1/esp32/api-reference/bluetooth/esp_gatts.html
