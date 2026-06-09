# Current State

Created: 2026-06-09

## Goal

Build Simon's first custom M5Stack StickS3 program: a Codex companion display that can connect over BLE, show what Codex is doing, show completed/recent activity, and eventually surface useful usage information.

The target experience is Codex app compatibility first, not only Codex CLI compatibility.

## Product Intent

The StickS3 should act as a tiny external status surface:

- Show connected / idle / running / waiting / stale states.
- Show the latest Codex activity message.
- Show recent completed actions.
- Show approval prompts if the host sends them.
- Use Button A for approve/next.
- Use Button B for deny/next.
- Show `tokens_today`, total tokens, and remaining usage percentage if the host sends those fields.

Exact account-wide usage remaining is not yet a guaranteed data source. The firmware accepts remaining-usage fields, but we still need to prove whether Codex app developer-mode BLE or another host bridge can send the real value.

## Architecture Decision

Use the CodeBuddy/Codex hardware-buddy BLE protocol shape directly.

The StickS3 advertises a BLE name starting with `Codex-` and exposes Nordic UART Service:

- Service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX desktop to device: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- TX device to desktop: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

Payloads are newline-delimited JSON objects.

## Files Created

- `README.md` - project overview, build path, controls, pairing path.
- `docs/protocol.md` - BLE UUIDs and JSON message format.
- `platformio.ini` - PlatformIO config targeting ESP32-S3 Arduino with M5Unified and ArduinoJson.
- `src/main.cpp` - initial firmware implementation:
  - BLE advertising as `Codex-S3-XXXX`.
  - NUS RX/TX characteristics.
  - JSON line parser.
  - snapshot renderer.
  - status command ack.
  - owner/name/unpair command ack.
  - approval/deny responses.
  - four display pages: dashboard, recent entries, usage, system.

## Current Build State

This repo has not yet completed a successful firmware build on Simon's Mac.

What happened:

- Homebrew `brew install platformio` failed because Homebrew does not support the current pre-release macOS 27 environment.
- A repo-local Python venv was created at `.venv/`.
- PlatformIO was installed into `.venv/`.
- First `pio run` started downloading ESP32 PlatformIO packages into `.platformio/`, but the build was interrupted before completion.

Generated local folders are intentionally ignored:

- `.venv/`
- `.pio/`
- `.platformio/`

They should be recreated on the next machine.

## Next Computer Setup

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
pio run --target upload
```

If upload cannot connect, put StickS3 into download mode by holding the side reset/power button until the internal green LED blinks, then retry upload.

## Validation Plan

1. Compile firmware without errors.
2. Upload to StickS3.
3. Confirm screen shows `Advertise Codex-S3-XXXX`.
4. Scan BLE from Mac/iPhone and verify the device advertises as `Codex-S3-XXXX`.
5. Enable Codex app developer mode:
   - Help -> Troubleshooting -> Enable Developer Mode.
6. Try to pair the Codex app hardware-buddy BLE path to the StickS3.
7. If direct app pairing works:
   - Confirm state snapshots render.
   - Confirm recent activity renders.
   - Confirm approval prompts and Button A/B responses work.
8. If direct app pairing does not work:
   - Build a small Mac bridge using `codex app-server`.
   - Have the bridge send the same JSON snapshots to the StickS3 over BLE.

## Known Risks

- StickS3 PlatformIO board support may require tuning. Current config uses `esp32-s3-devkitc-1` plus StickS3-relevant flags.
- M5Unified APIs may differ from assumptions in `src/main.cpp`, especially button names, battery APIs, or display setup.
- Codex app hardware-buddy BLE support is developer-mode and not guaranteed to be stable.
- Exact account usage remaining may not be available through a stable personal API; firmware can only display it if the host sends it.

## Immediate Next Engineering Task

Run `pio run` on a machine with a working PlatformIO setup and fix compile errors in `src/main.cpp` until the firmware builds.
