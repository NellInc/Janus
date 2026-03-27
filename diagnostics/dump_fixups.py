#!/usr/bin/env python3
"""Dump all fixup records from V5REPACKED_NEW, flag anomalies."""
import struct
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
data = bytearray(open(os.path.join(SCRIPT_DIR, 'V5REPACKED_NEW.VXD'), 'rb').read())
le = struct.unpack_from('<I', data, 0x3C)[0]

np = struct.unpack_from('<I', data, le + 0x14)[0]
fpt_off = le + struct.unpack_from('<I', data, le + 0x68)[0]
frt_off = le + struct.unpack_from('<I', data, le + 0x6C)[0]

fpt = [struct.unpack_from('<I', data, fpt_off + i * 4)[0] for i in range(np + 1)]

print(f"Pages: {np}, FPT: {fpt}")
print()

for pg in range(np):
    start = fpt[pg]
    end = fpt[pg + 1]
    if start == end:
        continue
    count = (end - start) // 5
    print(f"=== Page {pg}: {count} fixups (bytes {start}-{end}) ===")
    anomalies = 0
    for i in range(count):
        pos = frt_off + start + i * 5
        src_type = data[pos]
        tgt_flags = data[pos + 1]
        src_offset = struct.unpack_from('<H', data, pos + 2)[0]
        obj_num = data[pos + 4]

        flags = []
        if src_type != 0x07:
            flags.append(f"BAD_TYPE=0x{src_type:02X}")
        if tgt_flags != 0x00:
            flags.append(f"BAD_FLAGS=0x{tgt_flags:02X}")
        if obj_num != 0x01:
            flags.append(f"BAD_OBJ={obj_num}")
        if src_offset > 0x0FFC:
            flags.append(f"HIGH_OFF")

        # Read the target value written to page data
        pg_data_start = le + struct.unpack_from('<I', data, le + 0x80)[0] + pg * 4096
        signed_off = src_offset if src_offset < 0x8000 else src_offset - 0x10000
        abs_off = pg * 4096 + signed_off
        if 0 <= abs_off and abs_off + 4 <= np * 4096:
            target = struct.unpack_from('<I', data, pg_data_start + signed_off)[0]
        else:
            target = None
            flags.append("CROSS_BOUNDARY")

        flag_str = f"  *** {', '.join(flags)}" if flags else ""
        if flags or i < 5 or i >= count - 2:  # show first 5, last 2, and all anomalies
            tgt_str = f"tgt=0x{target:08X}" if target is not None else "tgt=OOBOUNDS"
            print(f"  [{i:3d}] type=0x{src_type:02X} flags=0x{tgt_flags:02X} off=0x{src_offset:04X} obj={obj_num} {tgt_str}{flag_str}")
            anomalies += len(flags)
        elif i == 5:
            print(f"  ... ({count - 7} more records) ...")

    if anomalies == 0:
        print(f"  (no anomalies in {count} records)")
    print()
