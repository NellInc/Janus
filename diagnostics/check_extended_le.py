#!/usr/bin/env python3
"""Check extended LE header fields (LE+0xB0 to LE+0xC3) across multiple VxDs."""
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

def check_extended(data, name):
    if len(data) < 0x80:
        return
    le = struct.unpack_from('<I', data, 0x3C)[0]
    if le + 0xC4 > len(data):
        return
    sig = data[le:le+2]
    if sig != b'LE':
        return

    num_pages = struct.unpack_from('<I', data, le + 0x14)[0]
    num_obj = struct.unpack_from('<I', data, le + 0x44)[0]
    dp_off = struct.unpack_from('<I', data, le + 0x80)[0]
    nrn_off = struct.unpack_from('<I', data, le + 0x88)[0]
    nrn_len = struct.unpack_from('<I', data, le + 0x8C)[0]
    last_page = struct.unpack_from('<I', data, le + 0x2C)[0]

    # Extended fields
    ext = []
    for off in range(0xB0, 0xC4, 4):
        val = struct.unpack_from('<I', data, le + off)[0]
        ext.append(val)

    # Compute expected end of page data
    if last_page == 0:
        page_data_end = dp_off + num_pages * 4096
    else:
        page_data_end = dp_off + (num_pages - 1) * 4096 + last_page

    nrn_end = nrn_off + nrn_len if nrn_off > 0 else 0
    file_size = len(data)

    print(f"  {name:20s} obj={num_obj} pg={num_pages} dp=0x{dp_off:X} nrn_end=0x{nrn_end:X} filesz=0x{file_size:X}")
    print(f"    LE+0xB0: 0x{ext[0]:08X}  LE+0xB4: 0x{ext[1]:08X}")
    print(f"    LE+0xB8: 0x{ext[2]:08X}  LE+0xBC: 0x{ext[3]:08X}")
    print(f"    LE+0xC0: 0x{ext[4]:08X}")
    if ext[2] > 0:
        print(f"    LE+0xB8 == nrn_end? {ext[2] == nrn_end} (nrn_end=0x{nrn_end:X})")
    print()

with open(DISK, 'rb') as f:
    fat.detect_fat32_geometry(f)
    win_entry, _ = fat.find_entry_in_dir(f, fat.ROOT_CLUSTER, b'WINDOWS    ')
    win_cluster = fat.parse_entry_cluster(win_entry)
    sys_entry, _ = fat.find_entry_in_dir(f, win_cluster, b'SYSTEM     ')
    sys_cluster = fat.parse_entry_cluster(sys_entry)
    ios_entry, _ = fat.find_entry_in_dir(f, sys_cluster, b'IOSUBSYS   ')
    ios_cluster = fat.parse_entry_cluster(ios_entry)

    for name_83 in [b'ESDI_506PDR', b'HSFLOP  PDR', b'RMM     PDR', b'SCSIPORTPDR',
                     b'APIX    VXD', b'CDFS    VXD', b'DISKTSD VXD']:
        data = extract_file(f, ios_cluster, name_83)
        if data:
            check_extended(data, name_83.decode().strip())

    for name_83 in [b'PCI     VXD', b'NDIS    VXD', b'VPOWERD VXD', b'ISAPNP  VXD']:
        data = extract_file(f, sys_cluster, name_83)
        if data:
            check_extended(data, name_83.decode().strip())
