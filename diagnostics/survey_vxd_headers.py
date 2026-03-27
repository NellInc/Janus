#!/usr/bin/env python3
"""Survey LE header fields across multiple VxDs in IOSUBSYS to find patterns."""
import struct
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import deploy_to_iosubsys as fat

DISK = os.environ.get("WIN98_IMG", "/tmp/win98vm/win98.img")

def extract_file(f, dir_cluster, name_83):
    entry, offset = fat.find_entry_in_dir(f, dir_cluster, name_83)
    if entry is None:
        return None
    cluster = fat.parse_entry_cluster(entry)
    size = fat.parse_entry_size(entry)
    chain = fat.get_cluster_chain(f, cluster)
    content = bytearray()
    for c in chain:
        off = fat.cluster_to_offset(c)
        f.seek(off)
        content.extend(f.read(fat.CLUSTER_SIZE))
    return bytes(content[:size])

def analyze_le(data, name):
    if len(data) < 0x80:
        return None
    le = struct.unpack_from('<I', data, 0x3C)[0]
    if le + 0xAC > len(data):
        return None
    sig = data[le:le+2]
    if sig != b'LE':
        return None

    flags = struct.unpack_from('<I', data, le + 0x10)[0]
    num_pages = struct.unpack_from('<I', data, le + 0x14)[0]
    num_obj = struct.unpack_from('<I', data, le + 0x44)[0]
    last_page = struct.unpack_from('<I', data, le + 0x2C)[0]
    dp_off = struct.unpack_from('<I', data, le + 0x80)[0]
    nrn_off = struct.unpack_from('<I', data, le + 0x88)[0]
    nrn_len = struct.unpack_from('<I', data, le + 0x8C)[0]
    imp_mod = struct.unpack_from('<I', data, le + 0x70)[0]
    imp_proc = struct.unpack_from('<I', data, le + 0x78)[0]
    imp_ent = struct.unpack_from('<I', data, le + 0x74)[0]
    ldr_size = struct.unpack_from('<I', data, le + 0x38)[0]
    fix_size = struct.unpack_from('<I', data, le + 0x30)[0]
    preload = struct.unpack_from('<I', data, le + 0x84)[0]

    # Check obj table reserved field
    obj_tbl = le + struct.unpack_from('<I', data, le + 0x40)[0]
    rsv = struct.unpack_from('<I', data, obj_tbl + 20)[0] if obj_tbl + 24 <= len(data) else 0

    print(f"  {name:20s} obj={num_obj} pg={num_pages} lp={last_page:4X} dp=0x{dp_off:X} "
          f"nrn={nrn_off:X}/{nrn_len} imp={imp_mod:X}/{imp_ent} rsv=0x{rsv:08X} "
          f"ldr=0x{ldr_size:X} fix=0x{fix_size:X} pre={preload}")
    return True

with open(DISK, 'rb') as f:
    fat.detect_fat32_geometry(f)

    # Navigate to IOSUBSYS
    win_entry, _ = fat.find_entry_in_dir(f, fat.ROOT_CLUSTER, b'WINDOWS    ')
    win_cluster = fat.parse_entry_cluster(win_entry)
    sys_entry, _ = fat.find_entry_in_dir(f, win_cluster, b'SYSTEM     ')
    sys_cluster = fat.parse_entry_cluster(sys_entry)
    ios_entry, _ = fat.find_entry_in_dir(f, sys_cluster, b'IOSUBSYS   ')
    ios_cluster = fat.parse_entry_cluster(ios_entry)

    # List all VxD/PDR files
    print("--- IOSUBSYS VxDs ---")
    chain = fat.get_cluster_chain(f, ios_cluster)
    for c in chain:
        offset = fat.cluster_to_offset(c)
        f.seek(offset)
        for i in range(fat.CLUSTER_SIZE // 32):
            entry = f.read(32)
            if entry[0] == 0x00:
                break
            if entry[0] == 0xE5 or entry[11] == 0x0F or entry[11] & 0x08:
                continue
            if entry[11] & 0x10:  # directory
                continue
            name = entry[0:11].decode('ascii', errors='replace')
            size = struct.unpack_from('<I', entry, 28)[0]
            ext = name[8:11].strip()
            if ext in ('VXD', 'PDR'):
                data = extract_file(f, ios_cluster, entry[0:11])
                if data:
                    analyze_le(data, name.strip())

    # Also check a few SYSTEM VxDs
    print("\n--- Select SYSTEM VxDs ---")
    for name_83 in [b'VMM32   VXD', b'PCI     VXD', b'NDIS    VXD', b'VPOWERD VXD']:
        data = extract_file(f, sys_cluster, name_83)
        if data:
            analyze_le(data, name_83.decode().strip())
