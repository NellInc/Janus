#!/bin/bash
# Launch Win98 VM with all debug output channels enabled
#
# Debug channels:
#   1. Port 0xE9 -> /tmp/vxd_debug.log (VxD writes to debugcon)
#   2. COM1     -> /tmp/serial.log     (serial port output)
#   3. QEMU monitor -> telnet localhost:55556
#
# Environment overrides:
#   WIN98_IMG     Raw Win98 disk image path
#   TEST_CD_ISO   Optional ISO to attach as CD-ROM
#   QEMU_DISPLAY  Display backend, e.g. cocoa or none
#   QEMU_EXTRA_ARGS Additional QEMU arguments

WIN98_IMG="${WIN98_IMG:-/Users/nellwatson/Documents/VMs/win98vm/win98.img}"
TEST_CD_ISO="${TEST_CD_ISO:-/tmp/win98vm/test_cd.iso}"
QEMU_DISPLAY="${QEMU_DISPLAY:-cocoa}"
QEMU_EXTRA_ARGS="${QEMU_EXTRA_ARGS:-}"

# Clean previous logs
rm -f /tmp/vxd_debug.log /tmp/serial.log

CDROM_ARGS=()
if [ -f "$TEST_CD_ISO" ]; then
    CDROM_ARGS=(-cdrom "$TEST_CD_ISO")
fi

qemu-system-i386 \
  -m 128 \
  -M pc \
  -cpu pentium \
  -drive file="$WIN98_IMG",format=raw \
  -boot c \
  -vga std \
  -rtc base=localtime \
  -display "$QEMU_DISPLAY" \
  -monitor telnet:127.0.0.1:55556,server,nowait \
  -debugcon file:/tmp/vxd_debug.log \
  -serial file:/tmp/serial.log \
  "${CDROM_ARGS[@]}" \
  $QEMU_EXTRA_ARGS

echo ""
echo "=== Debug output ==="
echo "Port 0xE9 (debugcon):"
if [ -f /tmp/vxd_debug.log ]; then
    xxd /tmp/vxd_debug.log | head -20
    echo "Size: $(wc -c < /tmp/vxd_debug.log) bytes"
else
    echo "(no output)"
fi
echo ""
echo "COM1 (serial):"
if [ -f /tmp/serial.log ]; then
    cat /tmp/serial.log | head -20
    echo "Size: $(wc -c < /tmp/serial.log) bytes"
else
    echo "(no output)"
fi
