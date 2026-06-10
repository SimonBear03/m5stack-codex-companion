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
- Show token usage only when Codex emits it; otherwise show `Tok: n/a` rather than a misleading zero.
- Show the current Codex plan step when the App Server sends plan updates.
- Show the current Codex thread goal when the App Server sends goal updates.

The Mac bridge reads App Server `account/rateLimits/read`, where the current Codex bucket exposes primary and secondary rolling windows. The observed durations are 300 minutes and 10080 minutes, displayed on-device as `5h` and `7d`.

The Mac bridge also listens for App Server `turn/plan/updated`, `thread/goal/updated`, and `thread/goal/cleared` notifications. It forwards compact plan and goal summaries to the StickS3.

Token totals are best-effort. The bridge listens for `thread/tokenUsage/updated`, but the `stdio` App Server validation path may not emit token usage unless the active work happens in that same App Server session. The firmware therefore renders missing token data as `Tok: n/a`.

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
  - NimBLE-based NUS implementation.
  - NUS RX/TX characteristics.
  - JSON line parser.
  - snapshot renderer.
  - status command ack.
  - owner/name/unpair command ack.
  - approval/deny responses.
  - six display pages: limits, status, plan, goal, recent entries, system.
  - redraw-on-change rendering to avoid constant display flicker.

## Current Build State

This repo has completed successful firmware builds on Simon's Mac with repo-local PlatformIO and has been flashed to the physical StickS3 over USB.

What happened:

- Homebrew `brew install platformio` failed because Homebrew does not support the current pre-release macOS 27 environment.
- A repo-local Python venv was created at `.venv/`.
- PlatformIO was installed into `.venv/`.
- `.venv/bin/pio run` succeeds on Simon's Mac.
- `python3 -m unittest discover -s tests` succeeds with 12 tests.
- A fake-device App Server smoke test initialized `codex app-server --stdio` and received `5h` / `7d` rate-limit windows from `account/rateLimits/read`.
- USB flashing to `/dev/cu.usbmodem101` succeeds with `pio run --target upload --upload-port /dev/cu.usbmodem101`.
- BLE status validation succeeds; the device replies with `name = Codex-S3-0470`, battery, heap, and approval counters.
- A short bridge run initializes Codex App Server over `stdio` and sends display snapshots to the StickS3.
- Firmware binary is currently about 756 KB after switching from ESP32 BLE Arduino to NimBLE.
- The screen flicker bug was fixed by removing the forced 500 ms full-screen redraw loop.
- Token display now shows `Tok: n/a` until token usage is actually supplied by the host.

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
pio run --target upload --upload-port /dev/cu.usbmodem101
```

If upload cannot connect, put StickS3 into download mode by holding the side reset/power button until the internal green LED blinks, then retry upload.

Bridge setup requires Python 3.11 or newer. If `.venv/` is Python 3.9 for PlatformIO, create a separate bridge venv:

```bash
python3.11 -m venv .bridge-venv
source .bridge-venv/bin/activate
python -m pip install -U pip
python -m pip install -e .
sticks3-bridge --log-level INFO app-server --transport stdio --scan-timeout 15
```

## Next Validation

- Decide whether to keep using direct USB flash or build a GitHub-release/Launcher OTA install path.
- Keep testing the Python bridge on Simon's Mac.
- Identify or expose a Codex Desktop app WebSocket/socket endpoint if passive desktop-thread mirroring is still desired.
- Trigger a command or file approval in Codex and confirm Button A accepts and Button B declines.
- Confirm token usage updates only when real `thread/tokenUsage/updated` events arrive; otherwise expect `Tok: n/a`.

## Validation Plan

1. Compile firmware without errors.
2. Upload to StickS3 over USB.
3. Confirm screen shows `Advertise Codex-S3-XXXX`.
4. Scan BLE from Mac/iPhone and verify the device advertises as `Codex-S3-XXXX`.
5. Install bridge dependencies on Mac with Python 3.11+ and `python -m pip install -e .`.
6. Run `sticks3-bridge --log-level INFO app-server --transport stdio --scan-timeout 15`.
7. Confirm the bridge sends snapshots to the StickS3.
8. Trigger command/file approvals through the Codex app/app-server endpoint and confirm Button A accepts and Button B declines.
9. If a desktop app endpoint is found, run `sticks3-bridge app-server --transport ws --target <mac-codex-app-server-endpoint>` and compare event coverage against `stdio`.

## Known Risks

- StickS3 PlatformIO board support may require tuning. Current config uses M5Stack's documented `esp32-s3-devkitc-1` PlatformIO shape plus StickS3-relevant flags.
- M5Unified APIs may differ from assumptions in `src/main.cpp`, especially button names, battery APIs, or display setup.
- The public Codex App Server protocol does not guarantee passive observation of every already-open desktop-app panel.
- Exact account usage remaining depends on the App Server endpoint exposing `account/rateLimits/read` and `account/rateLimits/updated`.
- Token totals are session/event dependent and may remain unavailable in `stdio` mode.
- M5Launcher WebUI install is not currently reliable for this no-SD StickS3 workflow.

## Immediate Next Engineering Task

Validate Button A/B approval round-trip against a real Codex approval prompt, then decide whether to invest next in Codex Desktop endpoint discovery or a Launcher-friendly GitHub-release OTA install path.
