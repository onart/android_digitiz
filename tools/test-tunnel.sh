#!/usr/bin/env bash
# Exercises the ADB reverse tunnel end to end without the guest app.
#
# Sends a real HELLO from the phone through the tunnel using the device's
# netcat, then prints whatever the host replies. Expect HELLO_ACK, HOST_STATE,
# and a PING every second; because netcat never answers a PONG, the host should
# also drop the session after three missed heartbeats.
#
# Usage:  start digitiz_host, then run this.
#
# On Git Bash, MSYS_NO_PATHCONV stops /data/local/tmp being rewritten into a
# Windows path.

set -euo pipefail
export MSYS_NO_PATHCONV=1

ADB="${ADB:-$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe}"
DEVICE_PORT="${DEVICE_PORT:-27183}"
REMOTE="/data/local/tmp/digitiz-hello.bin"
HOLD_SECONDS="${HOLD_SECONDS:-7}"

if [ ! -x "$ADB" ] && ! command -v "$ADB" >/dev/null 2>&1; then
    echo "adb not found at: $ADB" >&2
    echo "set ADB=/path/to/adb and retry" >&2
    exit 1
fi

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# HELLO payload: proto_ver u16, reserved u16, screen_w i32, screen_h i32,
# density f32, device[64].  Header: 'D','I', type, flags, payload_len u32.
python -c "
import struct, sys
device = b'nc-test'.ljust(64, b'\x00')
payload = struct.pack('<HHiif', 1, 0, 720, 1544, 2.25) + device
assert len(payload) == 80, len(payload)
header = struct.pack('<BBBBI', ord('D'), ord('I'), 0x01, 0, len(payload))
open(sys.argv[1], 'wb').write(header + payload)
" "$tmpdir/hello.bin"

echo "== reverse mappings before =="
"$ADB" reverse --list || true

echo
echo "== pushing HELLO =="
"$ADB" push "$tmpdir/hello.bin" "$REMOTE"

echo
echo "== connecting from the device, holding ${HOLD_SECONDS}s =="
"$ADB" shell "(cat $REMOTE; sleep $HOLD_SECONDS) | nc 127.0.0.1 $DEVICE_PORT | xxd"

echo
echo "== done; check the host console =="
