# StickS3 Codex Companion

Custom M5Stack StickS3 firmware for Agent Blob, an offline slime e-pet that also acts as a Codex companion device.

The target is the Codex app on Mac. The StickS3 acts as a small BLE display/approval/control device, and a local Mac bridge connects it to a Codex App-compatible app-server endpoint. Public Codex docs do not currently document native desktop-app BLE pairing, so the bridge is the supported path.

## Current Scope

- Boot into Agent Blob, an offline-first slime e-pet that works without Codex.
- Persist Agent Blob care stats in ESP32 NVS:
  - mood
  - energy
  - hunger
  - cleanliness
  - bond
  - focus
- Show Codex status as a small HUD: offline, idle, working, waiting, stale.
- Show latest status message and recent activity entries.
- Show pending approval and choice prompts as Agent Blob overlays.
- Send approval decisions: approve once, approve for session, deny, or cancel.
- Send simple option-list answers for Codex user-input prompts.
- Show a five-screen device UI:
  - Blob: Agent Blob home with Codex HUD and 5h/7d limit bars.
  - Codex: status, current plan step, goal state, and recent activity.
  - Limits: large 5-hour and 7-day remaining bars.
  - Care: Agent Blob care stat bars and care actions.
  - System: BLE, battery, heap, and device status.
- Show Codex rolling usage windows when sent by the host:
  - 5-hour remaining bar
  - 7-day remaining bar
  - optional total tokens, shown as `Tok: n/a` when the host has not emitted token usage
- Reply to host `status`, `owner`, `name`, and `unpair` commands.

The Mac bridge reads Codex App Server `account/rateLimits/read` and forwards the primary and secondary rolling windows to the StickS3. In the current App Server payload, those windows are 300 minutes and 10080 minutes, displayed as `5h` and `7d` bars. The bridge also listens for App Server plan and goal notifications and renders compact summaries on the Codex screen.

## Implementation Status

As of 2026-06-10:

- Firmware builds successfully with PlatformIO on Simon's Mac.
- Physical StickS3 USB flashing has been validated with `/dev/cu.usbmodem101`.
- The flashed firmware advertises over BLE as `Codex-S3-0470` and responds to `{"cmd":"status"}`.
- The Mac bridge connects over BLE and initializes a Codex App Server `stdio` session.
- Python bridge protocol tests pass.
- Fake-device App Server smoke test initializes and receives Codex `5h` / `7d` rate-limit data.
- The firmware now uses NimBLE instead of ESP32 BLE Arduino; the Agent Blob build is about 767 KB.
- The display no longer does a full-screen redraw every 500 ms; redraws happen on state/page/status changes.
- M5Launcher WebUI upload was unreliable on StickS3 without SD; direct USB flash is the current working install path.
- Agent Blob home, care stats, approval overlays, choice overlays, and bar-based limits are implemented in firmware.
- Live approval round-trip against a real Codex Desktop prompt is still pending.

## Next Machine Handoff

On the machine with the physical StickS3:

```bash
git clone https://github.com/SimonBear03/sticks3-codex-companion.git
cd sticks3-codex-companion
```

Build and upload the firmware:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip platformio
pio run
pio run --target upload --upload-port /dev/cu.usbmodem101
```

If the serial port differs, replace `/dev/cu.usbmodem101` with the port shown by `pio device list`. If upload cannot connect, put StickS3 into download mode by holding the side reset/power button until the internal green LED blinks, then retry upload.

Set up the bridge separately with Python 3.11 or newer:

```bash
python3.11 -m venv .bridge-venv
source .bridge-venv/bin/activate
python -m pip install -U pip
python -m pip install -e .
sticks3-bridge --log-level INFO app-server --transport stdio --scan-timeout 15
```

First hardware checks after flashing:

- Device boots into **Agent Blob**.
- Home screen shows Agent Blob plus `5h` and `7d` bars, not raw percentages.
- Button A pets Blob, Button B changes screens, long A opens Care, long B sleeps/wakes Blob.
- BLE advertises as `Codex-S3-XXXX`.
- Bridge connects and sends rate-limit snapshots.
- A real Codex approval prompt shows the approval overlay:
  - A: approve once
  - long A: approve for session
  - B: decline
  - long B: cancel

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

Direct USB upload writes the firmware app to flash and is the validated path right now. It can bypass the current M5Launcher boot flow. Restore M5Launcher later through M5Burner if you want to return to the launcher environment.

M5Launcher notes:

- WebUI app install was tested on StickS3 without SD and did not complete reliably.
- The first failure was binary size; NimBLE reduced the binary below the Launcher app-slot size check.
- After the size fix, WebUI still stalled during upload/write with no device-side error.
- For a durable Launcher workflow, prefer an online/GitHub-release `.bin` that Launcher can pull through OTA, or use SD-capable storage when available.

## Mac Codex App Bridge

Install the Python bridge on the Mac:

```bash
python3.11 -m venv .bridge-venv
source .bridge-venv/bin/activate
python -m pip install -U pip
python -m pip install -e .
```

The bridge package requires Python 3.11 or newer. The repo-local PlatformIO `.venv/` on Simon's Mac uses Python 3.9, so keep the bridge environment separate.

Run against a running Codex App-compatible app-server WebSocket endpoint:

```bash
sticks3-bridge app-server --transport ws --target ws://127.0.0.1:4567
```

For bridge validation without hardware:

```bash
sticks3-bridge app-server --transport stdio --fake-device --auto-decision deny
```

For the validated local hardware path:

```bash
sticks3-bridge --log-level INFO app-server --transport stdio --scan-timeout 15
```

This should initialize the App Server and log a snapshot containing `rate_limits.primary.label = 5h` and `rate_limits.secondary.label = 7d`. A command wrapped in `timeout` exits with code `124` when the long-running bridge is intentionally stopped.

The `stdio` path starts its own App Server session. It is the reliable development path today, but it does not guarantee passive mirroring of every already-open Codex Desktop app thread.

See `docs/mac-codex-app-bridge.md` for the Mac-side flow and limitations.

## Controls

Agent Blob routes input by priority: safety combo, active Codex overlay, then the current screen.

Default Pet Home controls:

- Button A: pet Agent Blob.
- Button B: next screen.
- Double A: play.
- Double B: previous screen.
- Long A: open Care.
- Long B: sleep/wake Agent Blob.
- Hold A+B: send Codex interrupt when connected.

Approval overlay controls:

- Button A: approve once.
- Long A: approve for session.
- Button B: deny.
- Long B: cancel.
- Double A/B: toggle details.
- Hold A+B: cancel and interrupt.

Choice overlay controls:

- Button A: select highlighted option.
- Double A or long A: submit selected option.
- Button B: next option.
- Double B: previous option.
- Long B: cancel.

## References

- M5Stack StickS3 Arduino guide: https://docs.m5stack.com/en/arduino/m5sticks3/program
- StickS3 product docs: https://docs.m5stack.com/en/core/StickS3
- Codex app-server docs: https://developers.openai.com/codex/app-server
- ESP32 GATT server docs: https://docs.espressif.com/projects/esp-idf/en/release-v5.1/esp32/api-reference/bluetooth/esp_gatts.html
