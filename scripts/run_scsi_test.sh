#!/bin/bash
# Run SCSI CD-ROM integration test with patched QEMU.
# Prerequisites:
#   - Patched QEMU at /tmp/qemu-patched/qemu-10.2.2/build/qemu-system-i386
#     (build with: bash reference/qemu-patches/build-patched-qemu.sh)
#   - Post-first-boot win98.img at ~/Documents/VMs/win98vm/win98.img
#   - V5SCSI.VXD built and LE-fixed
# Usage: bash scripts/run_scsi_test.sh [timeout]
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DISK="$HOME/Documents/VMs/win98vm/win98.img"
QEMU="${QEMU_BIN:-/tmp/qemu-patched/qemu-10.2.2/build/qemu-system-i386}"
TIMEOUT="${1:-300}"
LOGFILE="/tmp/nt5_scsi_debug.log"

if [ ! -f "$QEMU" ]; then
    echo "ERROR: patched QEMU not found at $QEMU"
    echo "Build it: bash $REPO/reference/qemu-patches/build-patched-qemu.sh"
    exit 1
fi

if [ ! -f "$DISK" ]; then
    echo "ERROR: Win98 disk image not found at $DISK"
    exit 1
fi

cleanup() {
    pkill -f "qemu-system-i386.*scsi_test" 2>/dev/null || true
    sleep 1
}
cleanup

echo "=== SCSI Integration Test ==="
echo "QEMU: $QEMU"
echo "Disk: $DISK"

# Create test ISO for SCSI CD-ROM
mkdir -p /tmp/win98vm/cdroot
echo "SCSI_TEST_DATA" > /tmp/win98vm/cdroot/README.TXT
echo "HELLO FROM SCSI" > /tmp/win98vm/cdroot/SCSI.TXT
rm -f /tmp/win98vm/test_cd.iso
hdiutil makehybrid -iso -joliet -o /tmp/win98vm/test_cd.iso /tmp/win98vm/cdroot 2>/dev/null \
    || mkisofs -J -o /tmp/win98vm/test_cd.iso /tmp/win98vm/cdroot 2>/dev/null

# Deploy V5SCSI.VXD (if available)
if [ -f "$REPO/binaries/V5SCSI_FIXED.VXD" ]; then
    python3 "$REPO/src/deploy_sysini.py" "$REPO/binaries/V5SCSI_FIXED.VXD" 2>/dev/null
    echo "Deployed: V5SCSI_FIXED.VXD"
elif [ -f "$REPO/binaries/NT5FIXED.VXD" ]; then
    echo "WARNING: V5SCSI_FIXED.VXD not found, using NT5FIXED.VXD (IDE mode)"
    python3 "$REPO/src/deploy_sysini.py" "$REPO/binaries/NT5FIXED.VXD" 2>/dev/null
fi

# Create SCSI HDD test image (16MB FAT16)
SCSI_HDD="/tmp/win98vm/scsi_hd.img"
python3 -c "
import struct, os
SIZE = 16 * 1024 * 1024  # 16MB
with open('$SCSI_HDD', 'wb') as f:
    # MBR with single partition
    mbr = bytearray(512)
    mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA
    # Partition entry at 0x1BE: type=0x06 (FAT16), start CHS, end CHS, LBA start=1, size
    pe = mbr[0x1BE:0x1CE]
    pe[0] = 0x80  # active
    pe[4] = 0x06  # FAT16
    struct.pack_into('<I', pe, 8, 1)  # LBA start
    struct.pack_into('<I', pe, 12, SIZE // 512 - 1)  # sectors
    mbr[0x1BE:0x1CE] = pe
    f.write(mbr)
    f.write(b'\x00' * (SIZE - 512))
print('Created SCSI HDD image: $SCSI_HDD')
" 2>/dev/null && echo "SCSI HDD image: $SCSI_HDD"

# Boot with SCSI controller
rm -f "$LOGFILE" /tmp/qemu-scsi-test.sock
echo "Booting Win98 with LSI SCSI controller..."
"$QEMU" -m 256 -M pc -cpu pentium2 \
    -drive "file=$DISK,format=raw" -boot c -vga std \
    -rtc base=localtime -display none \
    -debugcon "file:$LOGFILE" \
    -drive "file=/tmp/win98vm/test_cd.iso,media=cdrom,if=ide,index=2" \
    -device lsi53c810,id=scsi0 \
    -device scsi-cd,drive=scsicd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0 \
    -drive "id=scsicd0,file=/tmp/win98vm/test_cd.iso,if=none,media=cdrom,readonly=on" \
    -device scsi-hd,drive=scsihd0,bus=scsi0.0,channel=0,scsi-id=1,lun=0 \
    -drive "id=scsihd0,file=$SCSI_HDD,if=none,format=raw" \
    -monitor "unix:/tmp/qemu-scsi-test.sock,server,nowait" \
    -name "scsi_test" &
QPID=$!

# Wait for boot
echo "Waiting ${TIMEOUT}s for boot + VxD init..."
sleep "$TIMEOUT"

# Kill QEMU
kill $QPID 2>/dev/null
sleep 2

# Analyze debug log
echo ""
echo "=== Debug Log Analysis ==="
if [ ! -f "$LOGFILE" ] || [ ! -s "$LOGFILE" ]; then
    echo "FAIL: No debug output (VxD not loaded or boot failed)"
    exit 1
fi

LOG_SIZE=$(wc -c < "$LOGFILE")
echo "Log size: $LOG_SIZE bytes"

# Check key markers
check_marker() {
    if grep -q "$1" "$LOGFILE" 2>/dev/null; then
        echo "  PASS: $2"
        return 0
    else
        echo "  FAIL: $2"
        return 1
    fi
}

PASS=0; FAIL=0

check_marker "PCI SCSI" "PCI scan found SCSI controller" && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
check_marker "VID=0x00001000" "Symbios/LSI vendor ID" && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
check_marker "INQUIRY" "SCSI INQUIRY sent" && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
check_marker "SRB_STATUS_SUCCESS\|status=0x00000001" "INQUIRY success" && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
check_marker "SCSI CD-ROM FOUND\|type=0x00000005" "CD-ROM detected" && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
check_marker "type=0x00000000" "HDD detected (type 0x00)" && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
check_marker "D: MOUNTED\|CDROM_Attach\|FSD" "Drive mounted" && PASS=$((PASS+1)) || FAIL=$((FAIL+1))
check_marker "READ OK\|READ_OK\|IFS: READ" "File read succeeded" && PASS=$((PASS+1)) || FAIL=$((FAIL+1))

echo ""
echo "=== Results: $PASS pass, $FAIL fail ==="
if [ $FAIL -eq 0 ]; then
    echo "*** ALL PASS ***"
else
    echo "*** NEEDS ATTENTION ***"
    echo ""
    echo "Relevant log lines:"
    grep -E "SP:|V5:|SCSI|INQUIRY|FOUND|mount|READ|FAIL|ERROR" "$LOGFILE" | tail -40
fi
