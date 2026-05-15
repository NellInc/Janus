#!/bin/bash
# Launch Win98 VM with both CD-ROM and ATA hard disk for NTMINI testing
#
# IDE layout:
#   Primary master  (index 0): Win98 boot disk
#   Secondary master (index 2): CD-ROM via -cdrom (QEMU default)
#   Secondary slave  (index 3): Test HDD image (32MB, MBR + test data)
#
# The NTMINI VxD initializes on the secondary IDE channel. With both
# a CD-ROM and an ATA disk present, the driver should:
#   - Detect CD-ROM on secondary master (ATAPI, 2048-byte sectors)
#   - Detect ATA disk on secondary slave (ATA, 512-byte sectors)
#
# To test HDD-only (no CD-ROM), omit TEST_CD_ISO or set it to a
# nonexistent path.
#
# Environment overrides:
#   WIN98_IMG      Raw Win98 disk image path
#   TEST_CD_ISO    Optional ISO to attach as CD-ROM on secondary master
#   TEST_HDD_IMG   Raw HDD test image (default: /tmp/win98vm/test_hd.img)
#   QEMU_DISPLAY   Display backend, e.g. cocoa or none

WIN98_IMG="${WIN98_IMG:-/Users/nellwatson/Documents/VMs/win98vm/win98.img}"
TEST_CD_ISO="${TEST_CD_ISO:-/tmp/win98vm/test_cd.iso}"
TEST_HDD_IMG="${TEST_HDD_IMG:-/tmp/win98vm/test_hd.img}"
QEMU_DISPLAY="${QEMU_DISPLAY:-cocoa}"

# Create test HDD image if it doesn't exist
if [ ! -f "$TEST_HDD_IMG" ]; then
    echo "Creating test HDD image at $TEST_HDD_IMG ..."
    mkdir -p "$(dirname "$TEST_HDD_IMG")"
    qemu-img create -f raw "$TEST_HDD_IMG" 32M
    # Write MBR signature (0x55 at offset 510, 0xAA at offset 511)
    printf '\x55\xaa' | dd of="$TEST_HDD_IMG" bs=1 seek=510 conv=notrunc 2>/dev/null
    # Write test pattern at LBA 1 (offset 512)
    printf 'NTMINI HDD TEST DATA\n' | dd of="$TEST_HDD_IMG" bs=1 seek=512 conv=notrunc 2>/dev/null
    echo "Test HDD image created."
fi

# Clean previous logs
rm -f /tmp/vxd_debug.log /tmp/serial.log

CDROM_ARGS=()
if [ -f "$TEST_CD_ISO" ]; then
    CDROM_ARGS=(-cdrom "$TEST_CD_ISO")
fi

HDD_ARGS=()
if [ -f "$TEST_HDD_IMG" ]; then
    # index=3 = secondary slave (secondary master index=2 is used by -cdrom)
    HDD_ARGS=(-drive "file=$TEST_HDD_IMG,if=ide,index=3,format=raw")
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
  "${HDD_ARGS[@]}"

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
