# StickS3 Codex Companion

Custom M5Stack StickS3 firmware for showing Codex work status on a physical device.

The first target is direct compatibility with the Codex desktop hardware-buddy BLE protocol: the StickS3 advertises a `Codex-*` BLE device over Nordic UART Service, receives newline-delimited JSON status snapshots, displays current Codex state, and sends approval decisions back when a prompt is pending.

## Current Scope

- Show Codex status: disconnected, idle, running, waiting, stale.
- Show latest status message and recent activity entries.
- Show pending approval prompt when provided by the host.
- Send approval decisions:
  - Button A: approve once when a prompt is active.
  - Button B: deny when a prompt is active.
- Show token counters when sent by the host:
  - `tokens_today`
  - `tokens`
  - optional `rate_limit_remaining_percent`, `remaining_pct`, or `remaining`
- Reply to host `status`, `owner`, `name`, and `unpair` commands.

Exact account-wide Codex usage remaining is not currently a stable public personal API. This firmware displays remaining usage only if the Codex app or a future bridge sends that field.

## Repository Shape

```text
.
├── docs/
│   └── protocol.md
├── include/
├── src/
│   └── main.cpp
└── platformio.ini
```

## Build Target

Device: M5Stack StickS3

Framework: Arduino on ESP32-S3

Primary libraries:

- M5Unified
- M5GFX, pulled through M5Unified dependencies
- ArduinoJson

The M5Stack Arduino guide says to select the `M5StickS3` board in Arduino IDE and install `M5Unified` plus `M5GFX`. The PlatformIO setup here uses `esp32-s3-devkitc-1` with the StickS3-relevant build flags because StickS3 board support in PlatformIO may lag behind Arduino board manager support.

## Build With PlatformIO

Install PlatformIO first if needed:

```bash
brew install platformio
```

Then build:

```bash
cd /Users/simon/Documents/workspace/repos/sticks3-codex-companion
pio run
```

Upload:

```bash
pio run --target upload
```

If upload cannot connect, put StickS3 into download mode: hold the side reset/power button for about 2 seconds until the internal green LED blinks, then release.

## Codex App Pairing Path

1. Flash this firmware.
2. Open Codex desktop app.
3. Enable developer mode: **Help -> Troubleshooting -> Enable Developer Mode**.
4. Look for hardware buddy / BLE pairing support.
5. Pair with the advertised device name, for example `Codex-S3-2411`.

If Codex app direct BLE pairing does not show up, the fallback is a local Mac bridge that reads `codex app-server` events and sends the same JSON snapshots to the StickS3.

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
- CodeBuddy hardware BLE protocol reference: `/Users/simon/Documents/workspace/repos/CodeBuddy/firmware/REFERENCE.md`
