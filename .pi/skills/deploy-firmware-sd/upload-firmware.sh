#!/usr/bin/env bash
# Usage: upload-firmware.sh <device-ip> [firmware.bin]
set -euo pipefail

IP="${1:?usage: upload-firmware.sh <device-ip> [firmware.bin]}"
BIN="${2:-.pio/build/default/firmware.bin}"
[ -f "$BIN" ] || { echo "not found: $BIN" >&2; exit 1; }

# /upload rejects existing files, so delete first; 400 "not found" is fine.
curl -sS -X POST "http://$IP/delete" --data-urlencode "path=/firmware.bin" -o /dev/null

curl -fsS -F "file=@$BIN;filename=firmware.bin" "http://$IP/upload?path=/" && echo
