#!/usr/bin/env python3
"""
verify_and_launch.py - Verify Win98 disk image state and launch QEMU
                       with all debug output channels enabled.

Checks:
  1. FAT32 geometry is readable from the disk image
  2. MSDOS.SYS has BootLog=1
  3. SYSTEM.INI has device=ntmini.vxd in [386Enh]
  4. NTMINI is deployed via SYSTEM or IOSUBSYS and starts with a valid LE structure

Then provides QEMU launch command with:
  -debugcon file:/tmp/vxd_debug.log  (captures port 0xE9 output)
  -serial file:/tmp/serial.log       (captures COM1 output)
"""

import struct
import sys
import os
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)
import deploy_to_iosubsys as fat

PERSISTENT_VM_DIR = os.path.expanduser("~/Documents/VMs/win98vm")
DEFAULT_DISK = "/tmp/win98vm/win98.img"
FALLBACK_DISKS = [
    DEFAULT_DISK,
    "/tmp/win98vm/win98_nogeom.img",
    os.path.join(PERSISTENT_VM_DIR, "win98.img"),
    os.path.join(PERSISTENT_VM_DIR, "win98_nogeom.img"),
]
DISK = os.environ.get("WIN98_IMG", DEFAULT_DISK)

MSDOS_SYS_83 = b'MSDOS   SYS'
WINDOWS_DIR_83 = b'WINDOWS    '
SYSTEM_DIR_83 = b'SYSTEM     '
IOSUBSYS_DIR_83 = b'IOSUBSYS   '
SYSTEM_INI_83 = b'SYSTEM  INI'
NTMINI_VXD_83 = b'NTMINI  VXD'
NTMINI_PDR_83 = b'NTMINI  PDR'


def init_fat_geometry():
    """Populate FAT32 geometry globals from the selected disk image."""
    print("\n=== Detecting FAT32 Geometry ===")
    with open(DISK, 'rb') as f:
        fat.detect_fat32_geometry(f)


def read_file_from_entry(f, entry):
    """Read a file payload by following its FAT chain."""
    start_cluster = fat.parse_entry_cluster(entry)
    file_size = fat.parse_entry_size(entry)
    data = bytearray()

    if start_cluster >= 2 and file_size > 0:
        for cluster in fat.get_cluster_chain(f, start_cluster):
            f.seek(fat.cluster_to_offset(cluster))
            data.extend(f.read(fat.CLUSTER_SIZE))

    return bytes(data[:file_size]), start_cluster, file_size


def find_entry_in_path(f, dir_names_83, target_name_83):
    """Traverse a directory path and return a target entry within it."""
    current_cluster = fat.ROOT_CLUSTER

    for dir_name in dir_names_83:
        entry, _ = fat.find_entry_in_dir(f, current_cluster, dir_name)
        if entry is None:
            return None, None
        current_cluster = fat.parse_entry_cluster(entry)

    return fat.find_entry_in_dir(f, current_cluster, target_name_83)


def check_msdos_sys():
    """Search for MSDOS.SYS and check BootLog setting."""
    print("\n=== Checking MSDOS.SYS ===")

    with open(DISK, 'rb') as f:
        entry, _ = fat.find_entry_in_dir(f, fat.ROOT_CLUSTER, MSDOS_SYS_83)
        if entry is None:
            print("  [WARN] MSDOS.SYS not found in root directory")
            return False

        content, start_cluster, file_size = read_file_from_entry(f, entry)

    print(f"  Found MSDOS.SYS: cluster={start_cluster}, size={file_size}")

    try:
        text = content.decode('ascii', errors='replace')
        print("  Content preview (first 500 chars):")
        print("  " + text[:500].replace('\n', '\n  '))

        if 'BootLog=1' in text:
            print("  [OK] BootLog=1 is present")
        elif 'BootLog=0' in text:
            print("  [WARN] BootLog=0 found, boot logging is disabled")
        elif 'BootLog' in text:
            print("  [WARN] BootLog found but value unclear")
        else:
            print("  [WARN] No BootLog setting found")
    except Exception as exc:
        print(f"  [ERROR] Could not decode MSDOS.SYS content: {exc}")
        return False

    return True


def check_system_ini():
    """Search for SYSTEM.INI and verify ntmini.vxd entry."""
    print("\n=== Checking SYSTEM.INI ===")

    with open(DISK, 'rb') as f:
        entry, offset = find_entry_in_path(f, [WINDOWS_DIR_83], SYSTEM_INI_83)
        if entry is None:
            print("  [WARN] SYSTEM.INI not found in WINDOWS directory")
            return False

        content, start_cluster, file_size = read_file_from_entry(f, entry)

    print(f"  Found SYSTEM.INI: dir entry @ 0x{offset:X}, cluster={start_cluster}, size={file_size}")

    text = content.decode('ascii', errors='replace')
    section = None

    for line in text.replace('\r\n', '\n').replace('\r', '\n').split('\n'):
        stripped = line.strip()
        lower = stripped.lower()

        if stripped.startswith('[') and stripped.endswith(']'):
            section = lower
            continue

        if lower.startswith('device=') and 'ntmini.vxd' in lower:
            print(f"  ...{stripped}...")
            if section == '[386enh]':
                print("  [OK] SYSTEM.INI has ntmini.vxd device entry in [386Enh]")
                return True
            print(f"  [WARN] NTMINI device entry found outside [386Enh]: {section}")
            return False

    print("  [WARN] Could not find 'device=ntmini.vxd' in SYSTEM.INI")
    return False


def check_vxd_deployment():
    """Verify NTMINI is deployed via SYSTEM or IOSUBSYS."""
    print("\n=== Checking VxD Deployment ===")

    with open(DISK, 'rb') as f:
        candidates = [
            ("WINDOWS\\SYSTEM\\NTMINI.VXD", [WINDOWS_DIR_83, SYSTEM_DIR_83], NTMINI_VXD_83),
            ("WINDOWS\\SYSTEM\\IOSUBSYS\\NTMINI.PDR", [WINDOWS_DIR_83, SYSTEM_DIR_83, IOSUBSYS_DIR_83], NTMINI_PDR_83),
        ]

        for label, dir_names, target_name in candidates:
            entry, offset = find_entry_in_path(f, dir_names, target_name)
            if entry is None:
                continue

            vxd_data, start_cluster, file_size = read_file_from_entry(f, entry)
            print(f"  Found {label}: dir entry @ 0x{offset:X}, cluster={start_cluster}, size={file_size} bytes")

            if len(vxd_data) < 64:
                print("  [ERROR] VxD data too small")
                return False

            if vxd_data[0:2] != b'MZ':
                print(f"  [ERROR] No MZ header (got {vxd_data[0:2]})")
                return False

            print("  [OK] MZ header present")
            le_offset = struct.unpack_from('<I', vxd_data, 0x3C)[0]
            print(f"  LE header offset: 0x{le_offset:X}")

            if le_offset + 2 > len(vxd_data):
                print(f"  [ERROR] LE offset 0x{le_offset:X} beyond file size")
                return False

            le_sig = vxd_data[le_offset:le_offset+2]
            if le_sig != b'LE':
                print(f"  [ERROR] Expected 'LE' signature, got {le_sig}")
                return False

            print("  [OK] LE signature present")
            flags = struct.unpack_from('<I', vxd_data, le_offset + 0x10)[0]
            print(f"  Module flags: 0x{flags:08X}")
            if flags & 0x20000:
                print("  [OK] DEVICE_DRIVER flag set")
            else:
                print("  [WARN] DEVICE_DRIVER flag not set")

            num_pages = struct.unpack_from('<I', vxd_data, le_offset + 0x14)[0]
            print(f"  Number of pages: {num_pages}")

            if b'NTMINI-V3-A' in vxd_data:
                print("  [OK] V3 debug strings found")
            elif b'NTMINI' in vxd_data[le_offset:]:
                print("  [OK] NTMINI DDB name found")

            e9_count = vxd_data.count(b'\xE6\xE9')
            print(f"  Port 0xE9 write instructions: {e9_count}")

            ods_marker = struct.pack('<I', 0x0001001D)
            ods_count = vxd_data.count(b'\xCD\x20' + ods_marker)
            print(f"  Out_Debug_String calls: {ods_count}")
            return True

    print("  [WARN] NTMINI is not deployed in WINDOWS\\SYSTEM or IOSUBSYS")
    return False


def write_qemu_script():
    """Write a QEMU launch script with all debug channels."""
    print("\n=== QEMU Launch Script ===")

    script = f"""#!/bin/bash
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

WIN98_IMG="${{WIN98_IMG:-{DISK}}}"
TEST_CD_ISO="${{TEST_CD_ISO:-/tmp/win98vm/test_cd.iso}}"
QEMU_DISPLAY="${{QEMU_DISPLAY:-cocoa}}"
QEMU_EXTRA_ARGS="${{QEMU_EXTRA_ARGS:-}}"

# Clean previous logs
rm -f /tmp/vxd_debug.log /tmp/serial.log

CDROM_ARGS=()
if [ -f "$TEST_CD_ISO" ]; then
    CDROM_ARGS=(-cdrom "$TEST_CD_ISO")
fi

qemu-system-i386 \\
  -m 128 \\
  -M pc \\
  -cpu pentium \\
  -drive file="$WIN98_IMG",format=raw \\
  -boot c \\
  -vga std \\
  -rtc base=localtime \\
  -display "$QEMU_DISPLAY" \\
  -monitor telnet:127.0.0.1:55556,server,nowait \\
  -debugcon file:/tmp/vxd_debug.log \\
  -serial file:/tmp/serial.log \\
  "${{CDROM_ARGS[@]}}" \\
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
"""

    script_path = os.path.join(os.path.dirname(__file__), "launch_debug.sh")
    with open(script_path, 'w') as f:
        f.write(script)
    os.chmod(script_path, 0o755)
    print(f"  Written: {script_path}")
    print()
    print("  Run:  ./launch_debug.sh")
    print("  Or headless: QEMU_DISPLAY=none ./launch_debug.sh")
    print()
    print("  After Win98 boots, close QEMU and check:")
    print("    cat /tmp/vxd_debug.log    # port 0xE9 output")
    print("    cat /tmp/serial.log       # COM1 output")


def resolve_disk_path():
    """Return the first existing Win98 disk image path plus the attempted list."""
    candidates = []

    if DISK:
        candidates.append(DISK)

    for path in FALLBACK_DISKS:
        if path not in candidates:
            candidates.append(path)

    for path in candidates:
        if os.path.exists(path):
            return path, candidates

    return None, candidates


def main():
    global DISK

    print("=" * 60)
    print("Win98 VxD Debug Verification")
    print("=" * 60)

    DISK, attempted = resolve_disk_path()
    if DISK is None:
        print("\n[ERROR] Disk image not found.")
        print("  Tried:")
        for path in attempted:
            print(f"    {path}")
        print("  Set WIN98_IMG to the correct raw Win98 image path.")
        sys.exit(1)

    print(f"\nUsing disk image: {DISK}")

    disk_size = os.path.getsize(DISK)
    print(f"\nDisk image: {DISK} ({disk_size / (1024*1024*1024):.1f} GB)")
    init_fat_geometry()

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
