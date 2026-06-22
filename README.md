# StickS3 / Cardputer Codex Companion

Custom M5Stack firmware for a compact Codex companion activity display.

The StickS3 and Cardputer ADV targets are BLE displays for Codex activity on Simon's Mac. A local bridge connects the device either to Codex Desktop rollout logs in read-only observer mode or to a Codex App Server endpoint for protocol validation. Public Codex docs do not currently document native desktop-app BLE pairing, so this repo uses a small JSONL-over-BLE device protocol whose primary payload is a Codex companion activity object.

## Current Scope

- One dense terminal-style dashboard screen.
- Pinned top/status section:
  - muted mode color: `SYNC`, `WORK`, `IDLE`, `WAIT`, `STALE`, `ERR`, or `OFF`
  - fixed 3x3 status matrix with mode-specific animation
  - small amber unread marker when newer body text arrives while reading older text
  - BLE, USB, and battery indicators
- Canonical Codex activity state via `codex_activity`: `idle`, `running`, `waiting`, `failed`, or `review`, with compact title/subtitle text and optional waiting kind.
- Usage section:
  - `5h` and `7d` remaining percentages with compact bars
  - compact token totals such as `842`, `12.4K`, `57.6M`, or `1.2B`
- Pinned current action line derived from the Codex activity title/subtitle, with legacy speaker labels still accepted during migration.
- Scrollable wrapped body text backed by a fixed ring buffer in `Detail Full`, rendered as compact message blocks with colored speaker headers and blank separators.
- Orientation-aware portrait and landscape dashboard layouts with wrapped body reflow. StickS3 uses IMU-driven autorotate; Cardputer defaults to fixed landscape.
- Settings menu for brightness, power profile, detail level, sound, text navigation, auto-newest behavior, and rotation mode.
- Battery-saving display behavior: `Always` stays awake; `Auto` and `Low` sleep the display after about 10 seconds when settings are closed and you are not reading older text; no pre-sleep dimming.
- Soft buzzer cues for activity, connected, completed, and disconnected/error events.
- BLE `status`, `owner`, `name`, and `unpair` command handling.

Current firmware is read-only for the Codex Desktop workflow. It does not expose approval or choice controls on-device.

## Bridge Modes

- `desktop-observer`: read-only status mirroring for the actual Codex Desktop app. It follows local rollout JSONL files under `~/.codex/sessions`, prefers the newest active non-subagent Desktop thread, and forwards canonical activity state, optional scrollback activity, token totals, and rate limits. Usage is seeded from the freshest recent `token_count` event across rollout files so the account-level limits can show before the current thread starts work.
- `app-server`: JSON-RPC validation path for a Codex App Server endpoint. The Python bridge maps App Server events and interaction requests into the same canonical activity state, but the current firmware is focused on read-only display.

True control of an already-open Codex Desktop app thread is blocked until Codex exposes a documented local attach/control endpoint.

## Implementation Status

As of 2026-06-14:

- StickS3 and Cardputer ADV firmware targets build successfully with PlatformIO on Simon's Mac.
- Firmware binaries are about 1.10 MB or smaller, well within the direct-flash app size.
- Physical StickS3 USB flashing has been validated.
- The device advertises over BLE as `Codex-S3-0470` on Simon's StickS3.
- Cardputer ADV firmware advertises as `Codex-CP-XXXX` and builds, but still needs physical hardware validation.
- The bridge connects over BLE and can mirror the current Codex Desktop thread with `desktop-observer`.
- The bridge uses acknowledged BLE writes for JSONL chunks so partial JSON lines are less likely on the device.
- BLE scans match named `Codex-S3-*` or `Codex-CP-*` devices for discovery, but `desktop-observer` syncs only to explicitly paired devices.
- The Desktop observer can fan out to multiple paired companions, with independent reconnect and detail/privacy settings per device.
- The Desktop observer seeds token/rate-limit rows from recent rollout logs, not only from the active thread's next task.
- The native `StickS3 Companion.app` is the supported Mac menu bar controller; the old SwiftBar plugin workflow has been removed.
- The generated BLE helper app now lives under Application Support, so only the native menu bar app is opened by the user.
- Soft speaker cues work on hardware through M5Unified's StickS3 speaker path after removing manual audio-enable toggling.
- Python bridge tests pass.
- M5Launcher WebUI upload was unreliable on StickS3 without SD; direct USB flash is the current working install path.

## Build And Flash

```bash
git clone https://github.com/SimonBear03/sticks3-codex-companion.git
cd sticks3-codex-companion

python3 -m venv .venv
source .venv/bin/activate
python -m pip install -U pip platformio
pio run -e sticks3
pio run -e cardputer_adv
pio run -e sticks3 --target upload --upload-port /dev/cu.usbmodem101
pio run -e cardputer_adv --target upload --upload-port /dev/cu.usbmodem101
```

If the serial port differs, replace `/dev/cu.usbmodem101` with the port shown by:

```bash
pio device list
```

If StickS3 upload cannot connect, put StickS3 into download mode by holding the side reset/power button until the internal green LED blinks, then retry upload. For Cardputer, power the device off, hold `G0`, power it on, then release `G0` after it enters download mode.

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

Before a real device can receive Codex data, pair it once:

```bash
sticks3-bridge pair-device --label "Desk StickS3"
sticks3-bridge pair-device --label "Cardputer"
sticks3-bridge list-devices
```

The device shows a `PAIR 123456` screen during pairing. Confirm on-device with Button A on StickS3 or GO/G0 on Cardputer. Pairing data is stored on macOS at:

```text
~/Library/Application Support/StickS3 Codex Companion/paired-devices.json
```

Remove a local pairing later with:

```bash
sticks3-bridge unpair-device DEVICE_ID
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

## Mac Menu Bar Helper

The repo includes a native macOS menu bar app, `StickS3 Companion.app`, plus a
small Python supervisor for the Desktop observer. The menu bar app is the
user-facing controller. The supervisor owns only the bridge process that it
starts, and writes runtime state under `runtime/`:

- `runtime/bridge.pid`
- `runtime/bridge-status.json`
- `runtime/bridge.log`

Start and inspect it from the repo root:

```bash
python3 scripts/sticks3-macos-bridge start
python3 scripts/sticks3-macos-bridge status
python3 scripts/sticks3-macos-bridge stop
```

The bridge retries BLE scan/connect/write failures by default, so normal
disconnects should not require manual restarts.

Build or update the native menu bar app from the repo root:

```bash
/bin/zsh scripts/build-macos-companion --launch
```

That builds and opens a locally signed app at:

```text
dist/StickS3 Companion.app
```

Opening `StickS3 Companion.app` is enough to queue `scripts/sticks3-macos-bridge
ensure` when the bridge is stopped. The menu title shows aggregate BLE state as
`BLE connected/paired`, and the popover lists companions with board/name,
connected/scanning/disconnected state, detail mode, last-seen time, and errors.
Single-device and multi-device states use the same layout. The app also shows
current Codex status, token count when present, 5h/7d limits, thread info, and
Start/Stop/Restart actions. No paid Apple Developer account is needed for local
use; the app is ad-hoc signed by the build script.

On macOS, the supervisor starts the BLE-touching bridge through one stable
generated app bundle at:

```text
~/Library/Application Support/StickS3 Codex Companion/StickS3Bridge.app
```

That wrapper exists so macOS can see an `NSBluetoothAlwaysUsageDescription`
string for Bluetooth permission. The wrapper launches the repo's bridge venv
Python through a stable hidden app bundle, generated locally under Application
Support and reused across menu refreshes. It is rebuilt only when the generated
wrapper contents change.

To start it automatically at login and avoid manual bridge starts:

```bash
python3 scripts/sticks3-macos-bridge install-agent --scan-timeout 60
python3 scripts/sticks3-macos-bridge agent-status
```

This writes `~/Library/LaunchAgents/com.simon.sticks3-codex-companion.bridge.plist`
and installs a generated helper app under
`~/Library/Application Support/StickS3 Codex Companion/StickS3Bridge.app`.
Recent macOS builds may still require allowing the helper under System Settings
-> General -> Login Items & Extensions. Opening `StickS3 Companion.app` remains
the practical fallback when launchd refuses the background item.
Because the repo is under `~/Documents`, macOS may also ask once for folder
access when `StickS3 Codex Bridge` reads the bridge code and local Codex session
logs. Repeated folder or Bluetooth prompts usually mean the app was rebuilt,
moved, denied earlier, or launched from a different bundle path.
Newer macOS builds can still block unsigned local background items before the
bridge starts. If `agent-status` says the LaunchAgent is installed/loaded but
the bridge supervisor is stopped, check System Settings -> General -> Login
Items & Extensions and allow the StickS3/zsh background item. The `start`
command falls back to a direct app-wrapper launch if the LaunchAgent kickstart
does not produce a bridge PID. Remove the agent later with:

```bash
python3 scripts/sticks3-macos-bridge uninstall-agent
```

## Controls

StickS3 main dashboard:

- Button A: newer/down through body text.
- Button B: older/up through body text.
- Long A: open settings.
- Long B: jump to newest when reading older text and the unread dot is shown; otherwise enter display sleep.
- A+B: no-op in the dashboard firmware.

Cardputer main dashboard:

- Up key, or `;` without Fn: older/up one line through body text.
- Down key, or `.` without Fn: newer/down one line through body text.
- Left key, or `,` without Fn: older/up one page through body text.
- Right key, or `/` without Fn: newer/down one page through body text.
- Enter: open settings.
- Backspace, Esc, or `` ` `` without Fn: jump to newest when reading older text.
- GO/G0 short: jump to newest when reading older text.
- GO/G0 long: enter display sleep.

StickS3 settings menu:

- Button B: next option.
- Button A: rotate/toggle selected value.
- Long A: close settings.
- Auto-closes after 12 seconds of no input.

Cardputer settings menu:

- Up/Down, or `;`/`.` without Fn: previous/next option.
- Left/Right, or `,`/`/` without Fn: previous/next value.
- Enter or GO/G0 short: rotate/toggle selected value forward.
- Backspace, Esc, or `` ` `` without Fn: close settings.
- GO/G0 long: enter display sleep.
- Auto-closes after 12 seconds of no input.

Settings, in order:

- Brightness: `Low`, `Med`, `High`
- Power: `Always`, `Auto`, `Low`
- Detail: `Full`, `Status`, `Usage`
- Sound: `Off`, `Soft`, `Alerts`
- Text nav: `Page`, `Line`
- Auto newest: `On`, `Off`
- Rotation: `Auto`, `Lock`, `P`, `L`, `P180`, `L180`

On StickS3, `Auto` rotates among all four physical directions after the device is held steady. `Lock` keeps the current direction. `P`, `L`, `P180`, and `L180` force fixed portrait/landscape directions. On Cardputer, `Auto` is skipped because there is no validated IMU orientation path; the default is fixed landscape.

`Detail Full` sends and displays current status plus scrollable message activity. `Detail Status` suppresses body text and sends generic message labels while still allowing tool/status summaries. `Detail Usage` sends only generic state, usage rows, tokens, and device status; it avoids transmitting message bodies, tool names, and legacy `msg`/`entries` text to the device.

The top bar shows `CHG` when board telemetry reports active battery charging, `USB` when external power is present but charging is complete/paused/unknown, and neither label when running on battery. StickS3 has the validated PMIC telemetry path; Cardputer telemetry is best-effort until hardware validation.

`Always` does not automatically turn the display off, so it is the desk profile. `Auto` and `Low` automatically sleep the display after about 10 seconds when there is no unread marker, settings are closed, and you are not reading older text. BLE remains active in display sleep, so new Codex status/activity wakes the screen. Button input also wakes it. StickS3 shake wake is ignored for the first 2.5 seconds after entering sleep so the device can be set down without immediately waking. If battery drops to 20% or below while saved power is `Auto`, the firmware temporarily behaves like `Low` without overwriting the saved profile. `Always` is not downgraded by the low-battery policy. The firmware no longer enters ESP32 deep sleep or PMIC shutdown from a power profile, because those states break the live monitor connection.

Battery optimizations in the firmware:

- Lower LCD brightness levels tuned for battery use.
- Soft speaker cues use M5Unified/M5Cardputer speaker paths; the battery saver does not touch the audio enable path.
- Reduced redraws: unchanged heartbeat snapshots no longer redraw the screen.
- Slower status animation and adaptive loop delays, including during `WORK`.
- Conservative BLE TX power, slower advertising intervals, relaxed BLE connection parameters, and a disconnected advertising watchdog.
- BLE RX resync: partial receive data is cleared on connect/disconnect and malformed transport fragments are ignored instead of putting the dashboard into `ERR`.
- StickS3 PMIC indicator LED suppression without disabling speaker or power rails.
- Low-battery auto policy: 20% or lower on battery forces effective `Low` behavior only from `Auto`.
- `Always` leaves automatic display sleep disabled for desk use.
- `Auto` and `Low` auto-sleep the display after about 10 seconds when idle, not reading old text, and with no unread marker.
- All power profiles keep BLE reachable; use the physical power button when you want to turn the StickS3 off.
- Desktop observer sends a compact first sync packet immediately after BLE connect, then the full snapshot; after that it sends only new activity records and slows idle heartbeat traffic.
- Status acknowledgements include battery percent, voltage/current when available, CPU clock, and power timing.

## Repository Shape

```text
.
├── docs/
│   ├── mac-codex-app-bridge.md
│   ├── cardputer-references.md
│   └── protocol.md
├── bridge/
│   └── sticks3_bridge/
├── macos/
│   └── StickS3Companion/
├── scripts/
├── src/
│   └── main.cpp
├── tests/
└── platformio.ini
```

## Build Targets

Devices:

- M5Stack StickS3: `pio run -e sticks3`, advertises as `Codex-S3-XXXX`.
- M5Stack Cardputer ADV: `pio run -e cardputer_adv`, advertises as `Codex-CP-XXXX`.

Framework: Arduino on ESP32-S3

Primary libraries:

- M5Unified
- M5GFX, pulled through M5Unified dependencies
- M5PM1, for StickS3 PMIC support
- M5Cardputer, for Cardputer ADV keyboard/display integration
- ArduinoJson
- NimBLE-Arduino

The PlatformIO setup uses `esp32-s3-devkitc-1` for both targets. StickS3 keeps StickS3-relevant PSRAM/flash flags; Cardputer ADV uses the no-PSRAM 8 MB ESP32-S3 shape from M5Stack's Cardputer PlatformIO guidance.

## M5Launcher Notes

- WebUI app install was tested on StickS3 without SD and did not complete reliably.
- Direct USB flash is the working install path for now.
- For a durable Launcher workflow, prefer an online/GitHub-release `.bin` that Launcher can pull through OTA, or use SD-capable storage when available.

## References

- M5Stack StickS3 Arduino guide: https://docs.m5stack.com/en/arduino/m5sticks3/program
- StickS3 product docs: https://docs.m5stack.com/en/core/StickS3
- M5Stack Cardputer docs: https://docs.m5stack.com/en/core/Cardputer
- M5Cardputer library: https://github.com/m5stack/M5Cardputer
- Codex app-server docs: https://developers.openai.com/codex/app-server
- ESP32 GATT server docs: https://docs.espressif.com/projects/esp-idf/en/release-v5.1/esp32/api-reference/bluetooth/esp_gatts.html
