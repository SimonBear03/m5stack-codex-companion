# Protocol

This firmware implements a small bridge protocol for the M5Stack Codex dashboard targets.

The BLE side is intentionally device-local and simple. It is not an official OpenAI BLE protocol. The Mac bridge maps Codex Desktop rollout events or Codex App Server events into compact dashboard snapshots.

## BLE

Advertise a name starting with a supported Codex companion prefix over Nordic UART Service:

- StickS3: `Codex-S3-`
- Cardputer ADV: `Codex-CP-`

The current firmware implements this with NimBLE-Arduino to keep the app binary small enough for the device workflows. The on-wire protocol remains Nordic UART Service JSONL.

| Direction | UUID |
| --- | --- |
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX desktop to device | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| TX device to desktop | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

Messages are UTF-8 JSON objects, one per line, terminated with `\n`.

BLE notifications and writes are chunked. Use 20-byte chunks unless the host and device negotiate and test a larger MTU.

The firmware clears any partial receive buffer on BLE connect/disconnect and
ignores malformed JSON fragments as transport noise instead of turning the
dashboard mode into `ERR`. A clean full snapshot after reconnect restores the
visible state.

The validated physical StickS3 currently advertises as `Codex-S3-0470`. Host scans use `Codex-S3-*` and `Codex-CP-*` names for discovery, but the Desktop observer sends private Codex data only to paired devices that authenticate with their stored secret.

## Pairing And Auth

Unauthenticated hosts may send:

```json
{"cmd":"hello","nonce":"host-discovery-nonce"}
```

The device replies with safe metadata:

```json
{
  "ack": "hello",
  "ok": true,
  "data": {
    "device_id": "sticks3-0470-a1b2c3d4",
    "board": "sticks3",
    "name": "Codex-S3-0470",
    "paired": true,
    "nonce": "device-session-nonce"
  }
}
```

Pairing is explicit. The bridge sends `pair_begin` with a generated host id and 32-byte secret, the device displays a six-digit code, and the user confirms on-device. The bridge then retries `pair_commit` until the device has been physically confirmed or pairing times out after 60 seconds.

For normal sync, the bridge authenticates each BLE connection with:

```json
{"cmd":"auth","host_id":"...","nonce":"host-session-nonce","mac":"hex-hmac-sha256"}
```

The HMAC message is `auth:v1:<device_id>:<device_nonce>:<host_nonce>:<host_id>`, keyed by the stored per-device secret. When a device is paired, snapshots, `status`, `owner`, `name`, and `unpair` are private and are rejected until `auth` succeeds for the current BLE connection.

## Dashboard Snapshot

The device accepts snapshots like:

```json
{
  "total": 1,
  "running": 1,
  "waiting": 0,
  "msg": "Codex working",
  "entries": ["Codex: Editing firmware", "Tool: pio run"],
  "status": {
    "speaker": "Codex",
    "kind": "message",
    "text": "Editing firmware"
  },
  "activity": [
    {
      "seq": "d42",
      "speaker": "Tool",
      "kind": "started",
      "text": "pio run"
    },
    {
      "seq": "d41",
      "speaker": "Codex",
      "kind": "message",
      "text": "Editing firmware"
    }
  ],
  "tokens": 57560861,
  "rate_limits": {
    "primary": {
      "label": "5h",
      "used_percent": 32,
      "remaining_percent": 68,
      "window_mins": 300,
      "resets_at": 1781109888
    },
    "secondary": {
      "label": "7d",
      "used_percent": 40,
      "remaining_percent": 60,
      "window_mins": 10080,
      "resets_at": 1781140478
    }
  }
}
```

`status` is the pinned newest action line. `activity` is the scrollable body input and should be ordered oldest-to-newest so the newest message lands at the bottom of the terminal window. Each activity item should include a stable `seq`; the firmware dedupes recently seen sequence IDs so heartbeat snapshots do not duplicate body text. Bridges may omit `activity` on heartbeat snapshots when no new body text exists.

The firmware renders conversation activity as compact message blocks, not a raw line stream: a colored header such as `[Codex]` or `[User]`, followed by flush-left wrapped body lines and a blank separator line before the next message. Tool activity is treated as pinned current status only and is not added to scrollback, so command noise does not displace the readable conversation.

The device reports a `settings.detail` preference in status acks: `0=Full`, `1=Status`, `2=Usage`. Bridges should honor it before sending snapshots. `Full` may include `activity` and message text. `Status` should omit body `activity` and replace full user/Codex message text with generic labels. `Usage` should omit body `activity`, omit tool names/message text, omit legacy `msg`/`entries`, and send only generic state plus usage/token fields.

The desktop observer caps each activity text at 1000 characters and keeps the latest 4 activity records for the first snapshot after connect or rollout switch. After that, it sends only new activity records. BLE sends JSON as 20-byte chunks using acknowledged writes, but that is only a transport chunk size. The firmware accepts JSON lines up to 8192 bytes, keeps the latest raw activity messages, and rebuilds wrapped body lines for the selected text mode. The rendered body uses a fixed 190-line ring buffer.

The bridge normalizes common smart punctuation such as curly apostrophes, curly quotes, long dashes, and ellipses to terminal-safe ASCII before sending text to the device. Chinese display is currently a test path: the BLE/JSON path carries UTF-8, dashboard text uses M5GFX `efontCN_14` for both ASCII and non-ASCII text, and body wrapping measures rendered pixel width instead of raw byte count. Mixed-language typography still needs hardware validation.

The firmware still accepts legacy `msg` and `entries`. When structured `activity` is absent and legacy text fields are present, it uses those fields as a fallback body source. Counter-only snapshots that carry only usage/rate-limit fields update the pinned widgets without creating body text.

`tokens` is optional. When absent, the firmware displays `TOK n/a` rather than treating missing data as zero. Token totals are shown with compact units on-device.

Legacy usage fields accepted by this firmware:

```json
{"rate_limit_remaining_percent": 72}
```

or:

```json
{"remaining_pct": 72}
```

or:

```json
{"remaining": 72}
```

## Status Command

When host sends:

```json
{"cmd":"status"}
```

The device replies:

```json
{
  "ack": "status",
  "ok": true,
  "data": {
    "device_id": "sticks3-0470-a1b2c3d4",
    "board": "sticks3",
    "name": "Codex-S3-0470",
    "sec": true,
    "auth": true,
    "bat": {
      "pct": 85,
      "mv": 3890,
      "ma": -72,
      "usb": true,
      "charging": true
    },
    "sys": {
      "up": 123,
      "heap": 120000,
      "cpu_mhz": 120
    },
    "stats": {
      "appr": 0,
      "deny": 0
    },
    "settings": {
      "brightness": 1,
      "power": 1,
      "effective_power": 1,
      "low_battery_low": false,
      "low_battery_max": false,
      "sound": 1,
      "detail": 0,
      "nav": 0,
      "auto_newest": true,
      "rotation_mode": 0,
      "display_rotation": 1,
      "auto_dim_ms": 0,
      "auto_sleep_ms": 10000,
      "deep_sleep_ms": 0,
      "travel_shutdown_ms": 0
    }
  }
}
```

Battery telemetry fields are best-effort. `bat.mv` and `bat.ma` are omitted when the board API cannot provide a plausible reading. `bat.usb` means external power is present. `bat.charging` is emitted only when the board API reports a known charge state; when true, the top bar shows `CHG`, otherwise USB power shows as `USB`. `settings.power` maps the saved profile: `0=Always`, `1=Auto`, `2=Low`. `settings.effective_power` can report `2=Low` when the low-battery auto policy downgrades saved `Auto`; saved `Always` remains no-auto-sleep. `settings.detail` maps `0=Full`, `1=Status`, `2=Usage`. `settings.low_battery_max` is kept as a compatibility alias for `settings.low_battery_low`. `settings.auto_sleep_ms` is `0` when automatic display sleep is disabled for the current effective profile. `settings.deep_sleep_ms` and `settings.travel_shutdown_ms` are kept for compatibility and currently report `0` because dashboard power profiles keep BLE reachable. `settings.rotation_mode` maps `0=Auto`, `1=Lock`, `2=P`, `3=L`, `4=P180`, `5=L180`; `settings.display_rotation` is the active M5GFX rotation value `0..3`.

The firmware also accepts authenticated `owner`, `name`, and `unpair` commands for compatibility with existing bridge tooling.

## Rate Limits

Codex App Server exposes rolling Codex rate-limit windows through `account/rateLimits/read` and `account/rateLimits/updated`. Codex Desktop rollout logs can also include the same window shape in `token_count` events. `desktop-observer` seeds the `5h` and `7d` account usage rows from the freshest recent `token_count` event across rollout files, then keeps following the selected/current thread for activity text.

The `tokens` field intentionally comes from `info.total_token_usage.total_tokens` in the latest observed `token_count` event. The bridge does not compute a local daily or weekly token sum.

The current observed Codex bucket uses:

- primary window: 300 minutes, displayed as `5h`
- secondary window: 10080 minutes, displayed as `7d`

The device displays remaining percentages beside compact bars, for example `5h 68%`.

## Desktop Observer Mapping

`desktop-observer` reads local Codex rollout JSONL and emits structured dashboard snapshots:

- `task_started` -> `Codex: Working`
- `task_complete` -> `Codex: Turn completed` plus the last agent message in activity
- `agent_message` -> `Codex` activity
- `user_message` -> `User` activity
- `response_item` function calls -> `Tool` activity with a cleaned tool name
- `token_count` -> token total and account rate-limit windows without replacing the current action line
- `patch_apply_end` -> `Tool: Patch applied` or `Tool: Patch failed`

The observer treats fresh tool calls, tool output, patches, task starts, and Codex messages as durable work until a terminal event arrives, with a long watchdog fallback so the top bar can show `WORK` through longer thinking/tool runs. It refreshes usage from recent rollout `token_count` events about every 5 seconds with a file-change cache, so rate-limit rows can populate while the current observed thread is idle and reset/refill events replace stale low-limit values quickly. It also strips or suppresses message payloads according to the device-reported detail mode, and skips duplicate sanitized snapshots except for heartbeat sends. Idle heartbeat defaults to 45 seconds and does not repeat old `activity` items; this keeps the BLE radio and firmware JSON parser quieter while preserving the pinned status and usage rows.

On BLE reconnect, the Desktop observer re-polls Codex state, sends a compact
first sync packet, then sends the full dashboard snapshot before requesting the
device status ack, so the StickS3 gets current usage and Codex state immediately
after the link is established. The firmware shows `SYNC` between BLE connect and
the first valid snapshot. The bridge also checks BLE client liveness while idle
and keeps reconnect backoff short so display wake and reconnect do not wait for a
long idle heartbeat.

## App Server Compatibility

The Python bridge still contains App Server mappings for command/file approvals, simple option-list prompts, interrupts, plan updates, goals, and token usage. The current dashboard firmware is read-only and does not expose approval or choice controls, so App Server interaction payloads are not part of the active device UX.

The public App Server protocol gives a client stream for the endpoint it is connected to. It does not document a passive control interface for every already-open Codex desktop-app panel. `desktop-observer` is the current workaround for status display.
