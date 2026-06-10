# Current State

Created: 2026-06-09

## Goal

Build Simon's first custom M5Stack StickS3 program: a Codex companion display that can connect over BLE, show what Codex is doing, show completed/recent activity, and eventually surface useful usage information.

The target experience is Codex app on Mac first, not Codex CLI compatibility.

## Product Intent

The StickS3 should act as a tiny external status surface:

- Show connected / idle / running / waiting / stale states.
- Show the latest Codex activity message.
- Show recent completed actions.
- Show approval prompts if the host sends them.
- Use Button A for approve/next.
- Use Button B for deny/next.
- Put Codex 5-hour and 7-day remaining usage percentages on the first page.
- Show the current Codex plan step when the App Server sends plan updates.
- Show the current Codex thread goal when the App Server sends goal updates.

The Mac bridge reads App Server `account/rateLimits/read`, where the current Codex bucket exposes primary and secondary rolling windows. The observed durations are 300 minutes and 10080 minutes, displayed on-device as `5h` and `7d`.

The Mac bridge also listens for App Server `turn/plan/updated`, `thread/goal/updated`, and `thread/goal/cleared` notifications. It forwards compact plan and goal summaries to the StickS3.

## Architecture Decision

Use a bridge-first architecture.

Public OpenAI docs document Codex App Server as the rich-client protocol for Codex and document command/file approval requests over JSON-RPC. They do not document native Codex desktop BLE hardware pairing. The StickS3 therefore speaks a small JSONL-over-BLE device protocol, while the Mac bridge maps that protocol to a Codex App-compatible App Server endpoint.

The StickS3 advertises a BLE name starting with `Codex-` and exposes Nordic UART Service:

- Service: `6e400001-b5a3-f393-e0a9-e50e24dcca9e`
- RX desktop to device: `6e400002-b5a3-f393-e0a9-e50e24dcca9e`
- TX device to desktop: `6e400003-b5a3-f393-e0a9-e50e24dcca9e`

Payloads are newline-delimited JSON objects.

## Files Created

- `README.md` - project overview, build path, controls, pairing path.
- `docs/protocol.md` - BLE UUIDs and JSON message format.
- `docs/mac-codex-app-bridge.md` - Mac Codex app bridge flow and limitations.
- `docs/cardputer-references.md` - Cardputer references and design takeaways for small-screen UX.
- `bridge/sticks3_bridge/` - Python bridge for App Server JSON-RPC to StickS3 BLE.
- `tests/` - Python protocol tests.
- `platformio.ini` - PlatformIO config targeting ESP32-S3 Arduino with M5Unified and ArduinoJson.
- `src/main.cpp` - initial firmware implementation:
  - BLE advertising as `Codex-S3-XXXX`.
  - NUS RX/TX characteristics.
  - JSON line parser.
  - snapshot renderer.
  - status command ack.
  - owner/name/unpair command ack.
  - approval/deny responses.
  - six display pages: limits, status, plan, goal, recent entries, system.

## Current Build State

This repo has completed a successful firmware build in the VPS workspace with repo-local PlatformIO. Simon's Mac should recreate the local toolchain using the setup steps below.

What happened:

- Homebrew `brew install platformio` failed because Homebrew does not support the current pre-release macOS 27 environment.
- A repo-local Python venv was created at `.venv/`.
- PlatformIO was installed into `.venv/` on the VPS.
- `.venv/bin/pio run` succeeded on the VPS.
- `.venv/bin/python -m unittest discover -s tests` succeeded with 11 tests.
- A fake-device App Server smoke test initialized `codex app-server --stdio` and received `5h` / `7d` rate-limit windows from `account/rateLimits/read`.

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

## Next Validation

- Flash the firmware to the physical StickS3.
- Run the Python bridge on Simon's Mac.
- Confirm BLE scanning finds the `Codex-S3-XXXX` device.
- Point the bridge at the Mac Codex app/App Server endpoint.
- Trigger a command or file approval in Codex and confirm Button A accepts and Button B declines.
- Confirm the first device page shows `5h` and `7d` remaining values from the host.

## Validation Plan

1. Compile firmware without errors.
2. Upload to StickS3.
3. Confirm screen shows `Advertise Codex-S3-XXXX`.
4. Scan BLE from Mac/iPhone and verify the device advertises as `Codex-S3-XXXX`.
5. Install bridge dependencies on Mac with `python -m pip install -e .`.
6. Run `sticks3-bridge app-server --transport ws --target <mac-codex-app-server-endpoint>`.
7. Confirm the bridge sends snapshots to the StickS3.
8. Trigger command/file approvals through the Codex app/app-server endpoint and confirm Button A accepts and Button B declines.

## Known Risks

- StickS3 PlatformIO board support may require tuning. Current config uses M5Stack's documented `esp32-s3-devkitc-1` PlatformIO shape plus StickS3-relevant flags.
- M5Unified APIs may differ from assumptions in `src/main.cpp`, especially button names, battery APIs, or display setup.
- The public Codex App Server protocol does not guarantee passive observation of every already-open desktop-app panel.
- Exact account usage remaining depends on the App Server endpoint exposing `account/rateLimits/read` and `account/rateLimits/updated`.

## Immediate Next Engineering Task

Run `pio run` on a machine with a working PlatformIO setup and fix compile errors in `src/main.cpp` until the firmware builds. Then validate the bridge against the Mac Codex app/app-server endpoint.
