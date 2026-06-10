# Protocol

This firmware implements a small bridge protocol for the StickS3 Codex dashboard.

The BLE side is intentionally device-local and simple. It is not an official OpenAI BLE protocol. The Mac bridge maps Codex Desktop rollout events or Codex App Server events into compact dashboard snapshots.

## BLE

Advertise a name starting with `Codex-` over Nordic UART Service.

The current firmware implements this with NimBLE-Arduino to keep the app binary small enough for the StickS3 workflow. The on-wire protocol remains Nordic UART Service JSONL.

| Direction | UUID |
| --- | --- |
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX desktop to device | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| TX device to desktop | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

Messages are UTF-8 JSON objects, one per line, terminated with `\n`.

BLE notifications and writes are chunked. Use 20-byte chunks unless the host and device negotiate and test a larger MTU.

The validated physical device currently advertises as `Codex-S3-0470`, but host software should only depend on the `Codex-` prefix and NUS service UUID.

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

The desktop observer caps each activity text at 1000 characters and keeps the latest 4 activity records for the first snapshot after connect or rollout switch. After that, it sends only new activity records. BLE still sends JSON as 20-byte chunks, but that is only a transport chunk size. The firmware accepts JSON lines up to 8192 bytes, keeps the latest raw activity messages, and rebuilds wrapped body lines for the selected text mode. The rendered body uses a fixed 190-line ring buffer.

The bridge normalizes common smart punctuation such as curly apostrophes, curly quotes, long dashes, and ellipses to terminal-safe ASCII before sending text to the device. Chinese display is currently a test path: the BLE/JSON path carries UTF-8, dashboard text uses M5GFX `efontCN_14` for both ASCII and non-ASCII text, and body wrapping measures rendered pixel width instead of raw byte count. Mixed-language typography still needs hardware validation.

The firmware still accepts legacy `msg` and `entries`. When structured `activity` is absent, it uses those fields as a fallback body source.

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
    "name": "Codex-S3-0470",
    "sec": false,
    "bat": {
      "pct": 85,
      "mv": 3890,
      "ma": -72,
      "usb": true
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
      "low_battery_max": false,
      "sound": 1,
      "nav": 0,
      "auto_newest": true,
      "auto_dim_ms": 0,
      "auto_sleep_ms": 10000,
      "deep_sleep_ms": 0,
      "travel_shutdown_ms": 0
    }
  }
}
```

Battery telemetry fields are best-effort. `bat.mv` and `bat.ma` are omitted when the board API cannot provide a plausible reading. `settings.power` maps the saved profile: `0=Balanced`, `1=Saver`, `2=Max`, `3=Travel`. `settings.effective_power` can report `2=Max` when the low-battery auto policy is active even if the saved profile is lower.

The firmware also accepts `owner`, `name`, and `unpair` commands for compatibility with existing bridge tooling.

## Rate Limits

Codex App Server exposes rolling Codex rate-limit windows through `account/rateLimits/read` and `account/rateLimits/updated`. Codex Desktop rollout logs can also include the same window shape in `token_count` events.

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
- `token_count` -> token total and rate-limit windows without replacing the current action line
- `patch_apply_end` -> `Tool: Patch applied` or `Tool: Patch failed`

The observer sends active changes immediately. Idle heartbeat defaults to 45 seconds and does not repeat old `activity` items; this keeps the BLE radio and firmware JSON parser quieter while preserving the pinned status and usage rows.

## App Server Compatibility

The Python bridge still contains App Server mappings for command/file approvals, simple option-list prompts, interrupts, plan updates, goals, and token usage. The current dashboard firmware is read-only and does not expose approval or choice controls, so App Server interaction payloads are not part of the active device UX.

The public App Server protocol gives a client stream for the endpoint it is connected to. It does not document a passive control interface for every already-open Codex desktop-app panel. `desktop-observer` is the current workaround for status display.
