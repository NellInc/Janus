#!/usr/bin/env python3
"""Compare LE headers of two VxD files field by field."""
import struct
import sys

LE_FIELDS = [
    (0x00, '2s', 'signature'),
    (0x02, 'B', 'byte_order'),
    (0x03, 'B', 'word_order'),
    (0x04, '<I', 'format_level'),
    (0x08, '<H', 'cpu_type'),
    (0x0A, '<H', 'os_type'),
    (0x0C, '<I', 'module_version'),
    (0x10, '<I', 'module_flags'),
    (0x14, '<I', 'num_pages'),
    (0x18, '<I', 'eip_object'),
    (0x1C, '<I', 'eip_offset'),
    (0x20, '<I', 'esp_object'),
    (0x24, '<I', 'esp_offset'),
    (0x28, '<I', 'page_size'),
    (0x2C, '<I', 'last_page_bytes'),
    (0x30, '<I', 'fixup_section_size'),
    (0x34, '<I', 'fixup_section_checksum'),
    (0x38, '<I', 'loader_section_size'),
    (0x3C, '<I', 'loader_section_checksum'),
    (0x40, '<I', 'object_table_off'),
    (0x44, '<I', 'num_objects'),
    (0x48, '<I', 'object_page_table_off'),
    (0x4C, '<I', 'object_iter_pages_off'),
    (0x50, '<I', 'resource_table_off'),
    (0x54, '<I', 'num_resource_entries'),
    (0x58, '<I', 'resident_names_off'),
    (0x5C, '<I', 'entry_table_off'),
    (0x60, '<I', 'module_directives_off'),
    (0x64, '<I', 'num_module_directives'),
    (0x68, '<I', 'fixup_page_table_off'),
    (0x6C, '<I', 'fixup_record_table_off'),
    (0x70, '<I', 'import_module_table_off'),
    (0x74, '<I', 'import_module_entries'),
    (0x78, '<I', 'import_proc_table_off'),
    (0x7C, '<I', 'per_page_checksum_off'),
    (0x80, '<I', 'data_pages_off'),
    (0x84, '<I', 'num_preload_pages'),
    (0x88, '<I', 'nonresident_names_off'),
    (0x8C, '<I', 'nonresident_names_len'),
    (0x90, '<I', 'nonresident_names_checksum'),
    (0x94, '<I', 'auto_ds_object'),
    (0x98, '<I', 'debug_info_off'),
    (0x9C, '<I', 'debug_info_len'),
    (0xA0, '<I', 'instance_preload'),
    (0xA4, '<I', 'instance_demand'),
    (0xA8, '<I', 'heap_size'),
    (0xAC, '<I', 'stack_size'),
]

def read_le(path):
    data = open(path, 'rb').read()
    le_off = struct.unpack_from('<I', data, 0x3C)[0]
    return data, le_off

def compare(path1, path2):
    d1, le1 = read_le(path1)
    d2, le2 = read_le(path2)

    print(f"File 1: {path1} ({len(d1)} bytes, LE at 0x{le1:X})")
    print(f"File 2: {path2} ({len(d2)} bytes, LE at 0x{le2:X})")
    print()

    diffs = []
    for off, fmt, name in LE_FIELDS:
        v1 = struct.unpack_from(fmt, d1, le1 + off)
        v2 = struct.unpack_from(fmt, d2, le2 + off)
        if fmt == '2s':
            s1, s2 = v1[0], v2[0]
        else:
            s1, s2 = v1[0], v2[0]

        match = "  " if s1 == s2 else "**"
        if isinstance(s1, bytes):
            print(f"{match} LE+0x{off:02X} {name:30s}: {s1!r:>14s}  vs  {s2!r:<14s}")
        else:
            print(f"{match} LE+0x{off:02X} {name:30s}: 0x{s1:08X}  vs  0x{s2:08X}")
        if s1 != s2:
            diffs.append((off, name, s1, s2))

    print(f"\n{'='*70}")
    print(f"Differences: {len(diffs)}")
    for off, name, v1, v2 in diffs:
        if isinstance(v1, bytes):
            print(f"  LE+0x{off:02X} {name}: {v1!r} vs {v2!r}")
        else:
            print(f"  LE+0x{off:02X} {name}: 0x{v1:08X} vs 0x{v2:08X}")

    # Compare object table entries
    print(f"\n{'='*70}")
    print("Object table comparison:")
    for i in range(max(struct.unpack_from('<I', d1, le1+0x44)[0],
                       struct.unpack_from('<I', d2, le2+0x44)[0])):
        ot1 = le1 + struct.unpack_from('<I', d1, le1+0x40)[0] + i * 24
        ot2 = le2 + struct.unpack_from('<I', d2, le2+0x40)[0] + i * 24
        if ot1 + 24 <= len(d1):
            vs1 = struct.unpack_from('<I', d1, ot1)[0]
            rl1 = struct.unpack_from('<I', d1, ot1+4)[0]
            fl1 = struct.unpack_from('<I', d1, ot1+8)[0]
            pi1 = struct.unpack_from('<I', d1, ot1+12)[0]
            pc1 = struct.unpack_from('<I', d1, ot1+16)[0]
            r1 = struct.unpack_from('<I', d1, ot1+20)[0]
            print(f"  F1 Obj{i+1}: vsize=0x{vs1:X} reloc=0x{rl1:X} flags=0x{fl1:X} pmidx={pi1} pages={pc1} rsv=0x{r1:X}")
        if ot2 + 24 <= len(d2):
            vs2 = struct.unpack_from('<I', d2, ot2)[0]
            rl2 = struct.unpack_from('<I', d2, ot2+4)[0]
            fl2 = struct.unpack_from('<I', d2, ot2+8)[0]
            pi2 = struct.unpack_from('<I', d2, ot2+12)[0]
            pc2 = struct.unpack_from('<I', d2, ot2+16)[0]
            r2 = struct.unpack_from('<I', d2, ot2+20)[0]
            print(f"  F2 Obj{i+1}: vsize=0x{vs2:X} reloc=0x{rl2:X} flags=0x{fl2:X} pmidx={pi2} pages={pc2} rsv=0x{r2:X}")

    # Compare page map entries
    print(f"\n{'='*70}")
    print("Page map comparison:")
    np1 = struct.unpack_from('<I', d1, le1+0x14)[0]
    np2 = struct.unpack_from('<I', d2, le2+0x14)[0]
    pm1 = le1 + struct.unpack_from('<I', d1, le1+0x48)[0]
    pm2 = le2 + struct.unpack_from('<I', d2, le2+0x48)[0]
    for i in range(max(np1, np2)):
        if i < np1:
            e1 = d1[pm1+i*4:pm1+i*4+4]
            pn1 = (e1[0]<<16)|(e1[1]<<8)|e1[2]
            pt1 = e1[3]
        else:
            pn1, pt1 = '-', '-'
        if i < np2:
            e2 = d2[pm2+i*4:pm2+i*4+4]
            pn2 = (e2[0]<<16)|(e2[1]<<8)|e2[2]
            pt2 = e2[3]
        else:
            pn2, pt2 = '-', '-'
        match = "  " if (pn1 == pn2 and pt1 == pt2) else "**"
        print(f"{match}  Page {i}: num={pn1} type={pt1}  vs  num={pn2} type={pt2}")

    # Compare entry table
    print(f"\n{'='*70}")
    print("Entry table comparison:")
    et1 = le1 + struct.unpack_from('<I', d1, le1+0x5C)[0]
    et2 = le2 + struct.unpack_from('<I', d2, le2+0x5C)[0]
    print(f"  F1: bytes={d1[et1:et1+16].hex()}")
    print(f"  F2: bytes={d2[et2:et2+16].hex()}")
    # Parse
    for label, d, et in [("F1", d1, et1), ("F2", d2, et2)]:
        cnt = d[et]
        typ = d[et+1]
        obj = struct.unpack_from('<H', d, et+2)[0]
        flg = d[et+4]
        off = struct.unpack_from('<I', d, et+5)[0]
        print(f"  {label}: count={cnt} type={typ} obj={obj} flags=0x{flg:02X} offset=0x{off:X}")

    # Compare resident names
    print(f"\n{'='*70}")
    print("Resident names:")
    rn1 = le1 + struct.unpack_from('<I', d1, le1+0x58)[0]
    rn2 = le2 + struct.unpack_from('<I', d2, le2+0x58)[0]
    nlen1 = d1[rn1]
    nlen2 = d2[rn2]
    name1 = d1[rn1+1:rn1+1+nlen1]
    name2 = d2[rn2+1:rn2+1+nlen2]
    print(f"  F1: len={nlen1} name={name1}")
    print(f"  F2: len={nlen2} name={name2}")


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} FILE1.VXD FILE2.VXD")
        sys.exit(1)
    compare(sys.argv[1], sys.argv[2])
