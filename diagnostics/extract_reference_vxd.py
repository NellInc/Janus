#!/usr/bin/env python3
"""
extract_reference_vxd.py - Extract a known-working VxD from the Win98 disk image.

Extracts ESDI_506.PDR (or another VxD) from IOSUBSYS for comparison
against our hand-built VxD.

Usage: python3 extract_reference_vxd.py [filename_83]
  Default: ESDI_506PDR
"""
import struct
import sys
import os

DISK = os.environ.get("WIN98_IMG", "/tmp/win98vm/win98.img")
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

sys.path.insert(0, SCRIPT_DIR)
import deploy_to_iosubsys as fat

def extract_file_from_dir(f, dir_cluster, name_83):
    """Extract a file from a directory, return its bytes."""
    entry, offset = fat.find_entry_in_dir(f, dir_cluster, name_83)
    if entry is None:
        return None
    cluster = fat.parse_entry_cluster(entry)
    size = fat.parse_entry_size(entry)
    print(f"  Found {name_83.decode().strip()} at cluster {cluster}, size {size}")

    chain = fat.get_cluster_chain(f, cluster)
    content = bytearray()
    for c in chain:
        offset = fat.cluster_to_offset(c)
        f.seek(offset)
        content.extend(f.read(fat.CLUSTER_SIZE))
    return bytes(content[:size])

def list_dir(f, dir_cluster):
    """List all entries in a directory."""
    chain = fat.get_cluster_chain(f, dir_cluster)
    entries = []
    for c in chain:
        offset = fat.cluster_to_offset(c)
        f.seek(offset)
        for i in range(fat.CLUSTER_SIZE // 32):
            entry = f.read(32)
            if entry[0] == 0x00:
                return entries
            if entry[0] == 0xE5:
                continue
            if entry[11] == 0x0F:
                continue  # LFN
            if entry[11] & 0x08:
                continue  # volume label
            name = entry[0:11].decode('ascii', errors='replace')
            size = struct.unpack_from('<I', entry, 28)[0]
            flags = entry[11]
            is_dir = bool(flags & 0x10)
            entries.append((name, size, is_dir, flags))
    return entries

def main():
    target_83 = sys.argv[1].encode() if len(sys.argv) > 1 else b'ESDI_506PDR'
    # Pad to 11 chars
    while len(target_83) < 11:
        target_83 += b' '

    with open(DISK, 'rb') as f:
        fat.detect_fat32_geometry(f)

        # Navigate to IOSUBSYS
        print("\n--- Navigating to IOSUBSYS ---")
        win_entry, _ = fat.find_entry_in_dir(f, fat.ROOT_CLUSTER, b'WINDOWS    ')
        if win_entry is None:
            print("ERROR: WINDOWS not found")
            sys.exit(1)
        win_cluster = fat.parse_entry_cluster(win_entry)

        sys_entry, _ = fat.find_entry_in_dir(f, win_cluster, b'SYSTEM     ')
        if sys_entry is None:
            print("ERROR: SYSTEM not found")
            sys.exit(1)
        sys_cluster = fat.parse_entry_cluster(sys_entry)

        ios_entry, _ = fat.find_entry_in_dir(f, sys_cluster, b'IOSUBSYS   ')
        if ios_entry is None:
            print("ERROR: IOSUBSYS not found")
            sys.exit(1)
        ios_cluster = fat.parse_entry_cluster(ios_entry)

        print(f"  IOSUBSYS at cluster {ios_cluster}")

        # List all files in IOSUBSYS
        print("\n--- Files in IOSUBSYS ---")
        entries = list_dir(f, ios_cluster)
        vxd_files = []
        for name, size, is_dir, flags in entries:
            if is_dir:
                continue
            ext = name[8:11].strip()
            if ext in ('VXD', 'PDR', 'MPD'):
                vxd_files.append((name, size))
                print(f"  {name.strip():20s} {size:8d} bytes")

        # Also check SYSTEM dir for VxDs
        print("\n--- VxD/PDR files in SYSTEM ---")
        sys_entries = list_dir(f, sys_cluster)
        for name, size, is_dir, flags in sys_entries:
            if is_dir:
                continue
            ext = name[8:11].strip()
            if ext in ('VXD', 'PDR') and size > 0:
                print(f"  {name.strip():20s} {size:8d} bytes")

        # Extract target
        print(f"\n--- Extracting {target_83.decode().strip()} ---")
        data = extract_file_from_dir(f, ios_cluster, target_83)
        if data is None:
            # Try SYSTEM directory
            print(f"  Not in IOSUBSYS, trying SYSTEM...")
            data = extract_file_from_dir(f, sys_cluster, target_83)

        if data is None:
            print(f"  ERROR: {target_83.decode().strip()} not found")
            print("\n  Available VxD/PDR files listed above.")
            sys.exit(1)

        outpath = os.path.join(SCRIPT_DIR, target_83.decode().strip().replace(' ', '') + '.extracted')
        open(outpath, 'wb').write(data)
        print(f"\n  Saved to {outpath} ({len(data)} bytes)")

        # Quick LE header check
        if len(data) > 0x80:
            le_off = struct.unpack_from('<I', data, 0x3C)[0]
            sig = data[le_off:le_off+2]
            print(f"  LE offset: 0x{le_off:X}, signature: {sig}")
            if sig == b'LE':
                flags = struct.unpack_from('<I', data, le_off + 0x10)[0]
                num_pages = struct.unpack_from('<I', data, le_off + 0x14)[0]
                num_obj = struct.unpack_from('<I', data, le_off + 0x44)[0]
                dp_off = struct.unpack_from('<I', data, le_off + 0x80)[0]
                print(f"  module_flags: 0x{flags:08X}")
                print(f"  num_pages: {num_pages}")
                print(f"  num_objects: {num_obj}")
                print(f"  data_pages_off: 0x{dp_off:X}")

if __name__ == '__main__':
    main()
