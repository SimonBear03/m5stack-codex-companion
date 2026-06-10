#!/bin/zsh
REPO="${STICKS3_REPO:-/Users/simon/Documents/workspace/repos/sticks3-codex-companion}"
SUPERVISOR="$REPO/scripts/sticks3-macos-bridge"

if [[ ! -x "$SUPERVISOR" ]]; then
  echo "S3 Setup | color=#d65a4a"
  echo "---"
  echo "Supervisor is not executable"
  echo "$SUPERVISOR"
  exit 0
fi

exec "$SUPERVISOR" swiftbar
