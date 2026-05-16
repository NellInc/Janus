#!/bin/bash
# Run all HDD I/O tests through the IOS calldown chain.
# Prerequisites: post-first-boot win98.img, Docker for rebuilds.
# Usage: bash scripts/run_hdd_tests.sh [timeout_per_test]
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DISK="$HOME/Documents/VMs/win98vm/win98.img"
ART="$REPO/artifacts_search_ctx_instrument_20260514_143819"
TIMEOUT="${1:-300}"
PASS=0
FAIL=0
RESULTS=""

cleanup() {
    pkill -f "qemu-system-i386" 2>/dev/null || true
    sleep 1
}

run_test() {
    local TEST_NAME="$1"
    local TEST_EXE="$2"
    local OUTPUT_FILE="$3"
    local EXPECTED_PATTERN="$4"

    echo "=== TEST: $TEST_NAME ==="
    cleanup

    # Fresh test HDD
    python3 "$REPO/src/create_test_hdd.py" /tmp/win98vm/test_hd.img >/dev/null 2>&1

    # Deploy
    python3 "$REPO/src/deploy_sysini.py" "$REPO/binaries/NT5FIXED.VXD" >/dev/null 2>&1
    python3 "$ART/disk_ops.py" write windows "$TEST_EXE" "$ART/harness/$TEST_EXE" >/dev/null 2>&1
    python3 "$ART/disk_ops.py" winini_run "C:\\WINDOWS\\$TEST_EXE" >/dev/null 2>&1

    # Boot
    rm -f /tmp/nt5dbg_test.log /tmp/qemu-test.sock
    qemu-system-i386 -m 256 -M pc -cpu pentium2 \
        -drive "file=$DISK,format=raw" -boot c -vga std \
        -rtc base=localtime -display none \
        -debugcon file:/tmp/nt5dbg_test.log \
        -drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw" \
        -monitor unix:/tmp/qemu-test.sock,server,nowait &
    local QPID=$!

    sleep 130

    # Dismiss wizard
    python3 -c "
import socket,time
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
s.connect('/tmp/qemu-test.sock'); s.settimeout(2)
try: s.recv(4096)
except: pass
for _ in range(6):
    s.sendall(b'sendkey esc\n'); time.sleep(1)
    s.sendall(b'sendkey ret\n'); time.sleep(2)
s.close()
" 2>/dev/null

    # Wait for test to complete
    sleep "$((TIMEOUT - 130))"

    # Retrieve result
    kill $QPID 2>/dev/null
    sleep 2

    local RESULT_FILE="/tmp/${TEST_NAME}_result.txt"
    python3 "$ART/disk_ops.py" read windows "$OUTPUT_FILE" "$RESULT_FILE" >/dev/null 2>&1 || true

    if [ -f "$RESULT_FILE" ] && grep -q "$EXPECTED_PATTERN" "$RESULT_FILE" 2>/dev/null; then
        echo "  PASS: found '$EXPECTED_PATTERN'"
        PASS=$((PASS + 1))
        RESULTS="$RESULTS\n  PASS  $TEST_NAME"
    else
        echo "  FAIL: '$EXPECTED_PATTERN' not found"
        [ -f "$RESULT_FILE" ] && head -5 "$RESULT_FILE"
        FAIL=$((FAIL + 1))
        RESULTS="$RESULTS\n  FAIL  $TEST_NAME"
    fi
}

echo "NT5-9x HDD I/O Test Suite"
echo "========================="
echo ""

# Test 1: SREAD — read D:\SECOND.TXT (150 bytes)
run_test "SREAD" "SREAD.EXE" "SREAD.TXT" "READ_OK=0x00000001"

# Test 2: HREAD — multi-drive probe for SECOND.TXT
run_test "HREAD" "HREAD.EXE" "HREAD.TXT" "READ_OK=0x00000001"

# Test 3: SWRITE — write + readback D:\WRITETEST.TXT
run_test "SWRITE" "SWRITE.EXE" "SWRITE.TXT" "WRITE_OK=0x00000001"

cleanup

echo ""
echo "========================="
echo "Results: $PASS passed, $FAIL failed"
echo -e "$RESULTS"
