# Protocol

This firmware implements the CodeBuddy/Codex hardware-buddy BLE shape.

## BLE

Advertise a name starting with `Codex-` over Nordic UART Service.

| Direction | UUID |
| --- | --- |
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX desktop to device | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| TX device to desktop | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

Messages are UTF-8 JSON objects, one per line, terminated with `\n`.

## Status Snapshot

The device accepts snapshots like:

```json
{
  "total": 3,
  "running": 1,
  "waiting": 1,
  "msg": "approve: Bash",
  "entries": ["10:42 git push", "10:41 yarn test"],
  "tokens": 184502,
  "tokens_today": 31200,
  "prompt": {
    "id": "req_abc123",
    "tool": "Bash",
    "hint": "rm -rf /tmp/foo"
  }
}
```

Optional usage fields accepted by this firmware:

```json
{
  "rate_limit_remaining_percent": 72
}
```

or:

```json
{
  "remaining_pct": 72
}
```

or:

```json
{
  "remaining": 72
}
```

## Permission Response

When `prompt.id` is present:

```json
{"cmd":"permission","id":"req_abc123","decision":"once"}
```

or:

```json
{"cmd":"permission","id":"req_abc123","decision":"deny"}
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
    "name": "Codex-S3-2411",
    "sec": false,
    "bat": {
      "pct": 85,
      "usb": true
    },
    "sys": {
      "up": 123,
      "heap": 120000
    },
    "stats": {
      "appr": 2,
      "deny": 1
    }
  }
}
```
