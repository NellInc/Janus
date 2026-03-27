#!/usr/bin/env python3
"""
verify_and_launch.py - Verify Win98 disk image state and launch QEMU
                       with all debug output channels enabled.

Checks:
  1. MSDOS.SYS has BootLog=1
  2. SYSTEM.INI has device=ntmini.vxd in [386Enh]
  3. VxD is deployed at known clusters
  4. VxD starts with valid LE structure

Then provides QEMU launch command with:
  -debugcon file:/tmp/vxd_debug.log  (captures port 0xE9 output)
  -serial file:/tmp/serial.log       (captures COM1 output)
"""

import struct
import sys
import os
import subprocess

DISK = os.environ.get("WIN98_IMG", "/tmp/win98vm/win98.img")
CLUSTER_SIZE = 4096
SECTORS_PER_CLUSTER = 8
BYTES_PER_SECTOR = 512

# Known VxD deployment coordinates from contprompt
VXD_CLUSTERS = [160851, 160852, 160853, 160859]
VXD_DIR_ENTRY = 0x12208D40  # Directory entry offset

# FAT32 partition info (need to determine these)
# Standard archive.org Win98 SE image layout:
# Partition starts at sector 63 (typical), FAT32
PARTITION_START_SECTOR = 63
PARTITION_START = PARTITION_START_SECTOR * BYTES_PER_SECTOR

def cluster_to_offset(cluster_num):
    """Convert FAT32 cluster number to file offset in disk image.

    FAT32 data area starts after reserved sectors + FATs.
    For this specific image, we use the known working formula.
    Cluster 2 is the first data cluster.
    """
    # Read BPB to get exact layout
    with open(DISK, 'rb') as f:
        f.seek(PARTITION_START)
        bpb = f.read(90)

    bytes_per_sector = struct.unpack_from('<H', bpb, 11)[0]
    sectors_per_cluster = bpb[13]
    reserved_sectors = struct.unpack_from('<H', bpb, 14)[0]
    num_fats = bpb[16]
    fat_size_32 = struct.unpack_from('<I', bpb, 36)[0]

    data_start_sector = PARTITION_START_SECTOR + reserved_sectors + (num_fats * fat_size_32)
    data_start_byte = data_start_sector * bytes_per_sector

    # Cluster 2 is at data_start_byte
    offset = data_start_byte + (cluster_num - 2) * sectors_per_cluster * bytes_per_sector
    return offset


def check_msdos_sys():
    """Search for MSDOS.SYS and check BootLog setting."""
    print("\n=== Checking MSDOS.SYS ===")

    # MSDOS.SYS is typically in the root directory
    # Read root directory cluster from BPB
    with open(DISK, 'rb') as f:
        f.seek(PARTITION_START)
        bpb = f.read(90)
        root_cluster = struct.unpack_from('<I', bpb, 44)[0]

    root_offset = cluster_to_offset(root_cluster)

    with open(DISK, 'rb') as f:
        f.seek(root_offset)
        root_data = f.read(CLUSTER_SIZE)

    # Search directory entries for MSDOS.SYS (8.3: "MSDOS   SYS")
    found = False
    for i in range(0, len(root_data), 32):
        entry = root_data[i:i+32]
        if len(entry) < 32:
            break
        name = entry[0:11]
        if name == b'MSDOS   SYS':
            found = True
            first_cluster_hi = struct.unpack_from('<H', entry, 20)[0]
            first_cluster_lo = struct.unpack_from('<H', entry, 26)[0]
            first_cluster = (first_cluster_hi << 16) | first_cluster_lo
            file_size = struct.unpack_from('<I', entry, 28)[0]

            print(f"  Found MSDOS.SYS: cluster={first_cluster}, size={file_size}")

            # Read the file content
            file_offset = cluster_to_offset(first_cluster)
            f.seek(file_offset)
            content = f.read(min(file_size, CLUSTER_SIZE))

            try:
                text = content.decode('ascii', errors='replace')
                print(f"  Content preview (first 500 chars):")
                print("  " + text[:500].replace('\n', '\n  '))

                if 'BootLog=1' in text:
                    print("  [OK] BootLog=1 is present")
                elif 'BootLog=0' in text:
                    print("  [WARN] BootLog=0 found - boot logging is DISABLED")
                elif 'BootLog' in text:
                    print("  [WARN] BootLog found but value unclear")
                else:
                    print("  [WARN] No BootLog setting found")
            except:
                print("  [ERROR] Could not decode MSDOS.SYS content")
            break

    if not found:
        print("  [WARN] MSDOS.SYS not found in root directory")


def check_system_ini():
    """Search for SYSTEM.INI and verify ntmini.vxd entry."""
    print("\n=== Checking SYSTEM.INI ===")

    # SYSTEM.INI is in C:\WINDOWS\SYSTEM.INI
    # We need to traverse the directory structure to find it
    # For simplicity, search the disk for the string "device=ntmini"

    with open(DISK, 'rb') as f:
        # Search in a reasonable range around the Windows directory
        # SYSTEM.INI is typically in the first GB of the disk
        chunk_size = 4 * 1024 * 1024  # 4MB chunks

        for base in range(0, 512 * 1024 * 1024, chunk_size):
            f.seek(base)
            data = f.read(chunk_size)

            # Search for our device entry
            patterns = [b'device=ntmini.vxd', b'DEVICE=NTMINI.VXD',
                       b'device=NTMINI.VXD', b'Device=ntmini.vxd']
            for pat in patterns:
                idx = data.find(pat)
                if idx >= 0:
                    # Found it! Show context
                    context_start = max(0, idx - 100)
                    context_end = min(len(data), idx + 200)
                    context = data[context_start:context_end]

                    try:
                        text = context.decode('ascii', errors='replace')
                        abs_offset = base + idx
                        print(f"  Found at disk offset 0x{abs_offset:X}:")
                        print(f"  ...{text}...")
                        print("  [OK] SYSTEM.INI has ntmini.vxd device entry")
                    except:
                        print(f"  Found at offset 0x{base+idx:X} (could not decode)")
                    return True

    print("  [WARN] Could not find 'device=ntmini.vxd' in SYSTEM.INI")
    return False


def check_vxd_deployment():
    """Verify VxD is deployed at known clusters."""
    print("\n=== Checking VxD Deployment ===")

    with open(DISK, 'rb') as f:
        # Read the directory entry to get file size
        f.seek(VXD_DIR_ENTRY)
        dir_entry = f.read(32)

        if len(dir_entry) < 32:
            print("  [ERROR] Could not read directory entry")
            return False

        name = dir_entry[0:11]
        file_size = struct.unpack_from('<I', dir_entry, 28)[0]

        print(f"  Dir entry name: {name}")
        print(f"  Dir entry size: {file_size} bytes")

        if name[:6] != b'NTMINI':
            print("  [WARN] Directory entry doesn't look like NTMINI")

        # Read VxD from clusters
        vxd_data = bytearray()
        for cluster in VXD_CLUSTERS:
            offset = cluster_to_offset(cluster)
            f.seek(offset)
            vxd_data.extend(f.read(CLUSTER_SIZE))

        vxd_data = bytes(vxd_data[:file_size])

        # Check MZ header
        if len(vxd_data) < 64:
            print("  [ERROR] VxD data too small")
            return False

        if vxd_data[0:2] == b'MZ':
            print("  [OK] MZ header present")

            le_offset = struct.unpack_from('<I', vxd_data, 0x3C)[0]
            print(f"  LE header offset: 0x{le_offset:X}")

            if le_offset + 2 <= len(vxd_data):
                le_sig = vxd_data[le_offset:le_offset+2]
                if le_sig == b'LE':
                    print("  [OK] LE signature present")

                    # Read module flags
                    flags = struct.unpack_from('<I', vxd_data, le_offset + 0x10)[0]
                    print(f"  Module flags: 0x{flags:08X}")
                    if flags & 0x20000:
                        print("  [OK] DEVICE_DRIVER flag set")
                    else:
                        print("  [WARN] DEVICE_DRIVER flag NOT set")

                    # Read num pages
                    num_pages = struct.unpack_from('<I', vxd_data, le_offset + 0x14)[0]
                    print(f"  Number of pages: {num_pages}")

                    # Check for our debug markers in the code
                    if b'NTMINI-V3-A' in vxd_data:
                        print("  [OK] V3 debug strings found (this is the V3 build)")
                    elif b'NTMINI' in vxd_data[le_offset:]:
                        print("  [OK] NTMINI DDB name found")

                    # Check for port 0xE9 writes (E6 E9 = out 0xE9, al)
                    e9_count = vxd_data.count(b'\xE6\xE9')
                    print(f"  Port 0xE9 write instructions: {e9_count}")

                    # Check for Out_Debug_String calls (int 0x20; dd 0x0001001D)
                    ods_marker = struct.pack('<I', 0x0001001D)
                    ods_count = vxd_data.count(b'\xCD\x20' + ods_marker)
                    print(f"  Out_Debug_String calls: {ods_count}")

                else:
                    print(f"  [ERROR] Expected 'LE' signature, got {le_sig}")
        else:
            print(f"  [ERROR] No MZ header (got {vxd_data[0:2]})")

        return True


def write_qemu_script():
    """Write a QEMU launch script with all debug channels."""
    print("\n=== QEMU Launch Script ===")

    script = """#!/bin/bash
# Launch Win98 VM with all debug output channels enabled
#
# Debug channels:
#   1. Port 0xE9 -> /tmp/vxd_debug.log (VxD writes to debugcon)
#   2. COM1     -> /tmp/serial.log     (serial port output)
#   3. QEMU monitor -> telnet localhost:55556
#
# After boot, check:
#   cat /tmp/vxd_debug.log   (should show 'V' chars + debug strings)
#   cat /tmp/serial.log      (if VxD has COM1 output)

# Clean previous logs
rm -f /tmp/vxd_debug.log /tmp/serial.log

qemu-system-i386 \\
  -m 128 \\
  -M pc \\
  -cpu pentium \\
  -drive file=${{WIN98_IMG:-/tmp/win98vm/win98.img}},format=raw \\
  -boot c \\
  -vga std \\
  -rtc base=localtime \\
  -display cocoa \\
  -monitor telnet:127.0.0.1:55556,server,nowait \\
  -debugcon file:/tmp/vxd_debug.log \\
  -serial file:/tmp/serial.log

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
"""

    script_path = os.path.join(os.path.dirname(__file__), "launch_debug.sh")
    with open(script_path, 'w') as f:
        f.write(script)
    os.chmod(script_path, 0o755)
    print(f"  Written: {script_path}")
    print()
    print("  Run:  ./launch_debug.sh")
    print()
    print("  After Win98 boots, close QEMU and check:")
    print("    cat /tmp/vxd_debug.log    # port 0xE9 output")
    print("    cat /tmp/serial.log       # COM1 output")


def main():
    print("=" * 60)
    print("Win98 VxD Debug Verification")
    print("=" * 60)

    if not os.path.exists(DISK):
        print(f"\n[ERROR] Disk image not found: {DISK}")
        sys.exit(1)

    disk_size = os.path.getsize(DISK)
    print(f"\nDisk image: {DISK} ({disk_size / (1024*1024*1024):.1f} GB)")

    try:
        check_msdos_sys()
    except Exception as e:
        print(f"  [ERROR] {e}")

    try:
        check_system_ini()
    except Exception as e:
        print(f"  [ERROR] {e}")

    try:
        check_vxd_deployment()
    except Exception as e:
        print(f"  [ERROR] {e}")

    write_qemu_script()

    print()
    print("=" * 60)
    print("THEORY: Ring-0 I/O port writes from VxDs go directly to")
    print("QEMU hardware. VMM only traps V86/ring-3 I/O via GPF.")
    print("The existing VxD's port 0xE9 writes should reach debugcon")
    print("if QEMU is started with -debugcon flag.")
    print()
    print("If debugcon captures nothing, the fallback is COM1 serial")
    print("output (requires rebuilding VxD - run build_serial_vxd.py)")
    print("=" * 60)


if __name__ == '__main__':
    main()
