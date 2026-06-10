# Protocol

This firmware implements a small bridge protocol for Agent Blob, the StickS3 Codex companion e-pet.

The BLE side is intentionally device-local and simple. It is not an official OpenAI BLE protocol. The Mac bridge maps between this protocol and the documented Codex App Server JSON-RPC approval/event protocol.

## BLE

Advertise a name starting with `Codex-` over Nordic UART Service.

The current firmware implements this with NimBLE-Arduino to keep the app binary small enough for the StickS3 workflow. That is an implementation detail; the on-wire protocol remains Nordic UART Service JSONL.

| Direction | UUID |
| --- | --- |
| Service | `6e400001-b5a3-f393-e0a9-e50e24dcca9e` |
| RX desktop to device | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` |
| TX device to desktop | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` |

Messages are UTF-8 JSON objects, one per line, terminated with `\n`.

BLE notifications and writes are chunked. Use 20-byte chunks unless the host and device negotiate and test a larger MTU.

The validated physical device currently advertises as `Codex-S3-0470`, but host software should only depend on the `Codex-` prefix and NUS service UUID.

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
  "rate_limits": {
    "primary": {
      "label": "5h",
      "used_percent": 8,
      "remaining_percent": 92,
      "window_mins": 300,
      "resets_at": 1781034181
    },
    "secondary": {
      "label": "7d",
      "used_percent": 31,
      "remaining_percent": 69,
      "window_mins": 10080,
      "resets_at": 1781140479
    }
  },
  "plan": {
    "available": true,
    "step": "Patch the firmware",
    "status": "inProgress",
    "completed": 1,
    "total": 3
  },
  "goal": {
    "available": true,
    "objective": "Implement the StickS3 Codex companion",
    "status": "active",
    "time_used_sec": 3661,
    "tokens_used": 12345,
    "token_budget": 20000
  },
  "interaction": {
    "id": "req_abc123",
    "kind": "approval",
    "title": "Command",
    "body": "rm -rf /tmp/foo",
    "options": [
      {"id": "once", "label": "Once"},
      {"id": "session", "label": "Session"},
      {"id": "deny", "label": "Deny"},
      {"id": "cancel", "label": "Cancel"}
    ],
    "selected": 0,
    "multi": false,
    "handoff": false
  }
}
```

`tokens` and `tokens_today` are optional. When they are absent, the firmware displays `Tok: n/a` rather than treating the missing value as zero.

Legacy usage fields accepted by this firmware:

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

Legacy firmware/host pairs may use `prompt` and `permission`:

```json
{"cmd":"permission","id":"req_abc123","decision":"once"}
```

or:

```json
{"cmd":"permission","id":"req_abc123","decision":"deny"}
```

The current Agent Blob firmware uses the normalized interaction response:

```json
{"cmd":"interaction","id":"req_abc123","action":"submit","value":"once"}
```

Approval values:

- `once` -> Codex App Server `accept`
- `session` -> Codex App Server `acceptForSession`
- `deny` -> Codex App Server `decline`
- `cancel` -> Codex App Server `cancel`

The bridge still accepts legacy `permission` packets and normalizes them internally.

## Choice Response

For simple Codex option-list prompts, the bridge sends:

```json
{
  "interaction": {
    "id": "choice_1",
    "kind": "choice",
    "title": "Mode",
    "body": "Pick one",
    "question_id": "mode",
    "options": [
      {"id": "Fast", "label": "Fast"},
      {"id": "Careful", "label": "Careful"}
    ],
    "selected": 0,
    "multi": false,
    "handoff": false
  }
}
```

The device replies:

```json
{"cmd":"interaction","id":"choice_1","action":"submit","value":"Careful"}
```

The bridge maps this back to App Server `item/tool/requestUserInput` answers. Free-form, secret, multi-question, or complex form prompts become handoff interactions and should be completed on the Mac.

## Control Command

The device can send host controls:

```json
{"cmd":"control","action":"interrupt"}
```

The bridge maps this to App Server `turn/interrupt` when it knows the active `threadId` and `turnId`.

## Rate Limits

Codex App Server exposes rolling Codex rate-limit windows through `account/rateLimits/read` and `account/rateLimits/updated`. The current observed Codex bucket uses:

- primary window: 300 minutes, displayed as `5h`
- secondary window: 10080 minutes, displayed as `7d`

Agent Blob displays those windows as bars, not percentage text, on the normal UI. App Server sends `usedPercent`; the bridge converts it to `remaining_percent`.

## Plan and Goal

The bridge forwards App Server `turn/plan/updated` as a compact `plan` object. It selects the in-progress step first, then the next pending step, then the latest completed step.

The bridge forwards App Server `thread/goal/updated` as a compact `goal` object. When App Server sends `thread/goal/cleared`, the bridge sends:

```json
{"goal":{"available":false}}
```

The firmware keeps old plan/goal state only when those fields are absent. An explicit `available:false` clears the corresponding page.

Goal token fields are optional. If `tokens_used` is missing, the goal page displays `Tok: n/a`; if `token_budget` is missing, the page displays only the used token count.

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

## Codex App Server Mapping

The Mac bridge consumes these app-server requests:

- `item/commandExecution/requestApproval`
- `item/fileChange/requestApproval`
- `item/tool/requestUserInput`
- `mcpServer/elicitation/request`

It renders the request as an Agent Blob `interaction`, waits for a device response, and replies to the app-server request with the documented payload. Command/file approvals also include a legacy `prompt` object for compatibility.

The bridge also listens for status and item notifications and renders them as snapshots:

- `account/rateLimits/updated`
- `thread/status/changed`
- `turn/plan/updated`
- `thread/goal/updated`
- `thread/goal/cleared`
- `item/started`
- `item/completed`
- `thread/tokenUsage/updated`
- `serverRequest/resolved`
- `turn/completed`
- `turn/started`

`thread/tokenUsage/updated` is best-effort. The bridge accepts common total-token shapes such as `tokenUsage.total.totalTokens`, `usage.total.totalTokens`, and top-level `totalTokens`, but App Server sessions do not always emit token updates before real work occurs in that session.
