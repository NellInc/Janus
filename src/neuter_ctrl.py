#!/usr/bin/env python3
"""Replace NTMINI_Control with CLC;RET to test if LE+fixups load correctly."""
import struct
import sys
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def neuter(inp, out):
    data = bytearray(open(inp, 'rb').read())
    le = struct.unpack_from('<I', data, 0x3C)[0]
    dp_off = struct.unpack_from('<I', data, le + 0x80)[0]
    et_off = struct.unpack_from('<I', data, le + 0x5C)[0]
    et = le + et_off
    ddb_offset = struct.unpack_from('<I', data, et + 5)[0]

    # Read ctrl_proc offset from DDB+0x18
    # For raw Watcom: data_pages_off is file-absolute
    # For repacked: data_pages_off is also file-absolute (Fix #1)
    # Try both interpretations and see which gives a valid DDB
    for label, base in [("LE-relative", le + dp_off), ("file-absolute", dp_off)]:
        ddb_abs = base + ddb_offset
        if ddb_abs + 0x44 > len(data):
            continue
        name = data[ddb_abs + 0x0C:ddb_abs + 0x14]
        if name == b'NTMINI  ':
            ctrl_raw = struct.unpack_from('<I', data, ddb_abs + 0x18)[0]
            print(f"DDB found ({label}) at file 0x{ddb_abs:X}")
            print(f"  ctrl_proc raw value: 0x{ctrl_raw:X}")

            # ctrl_proc is the offset of NTMINI_Control within the object
            # Replace the code at that offset with CLC; RET
            ctrl_file = base + ctrl_raw
            print(f"  NTMINI_Control at file 0x{ctrl_file:X}")
            print(f"  Current bytes: {data[ctrl_file:ctrl_file+16].hex()}")

            # Write CLC (0xF8) + RET (0xC3)
            data[ctrl_file] = 0xF8  # clc
            data[ctrl_file + 1] = 0xC3  # ret
            print(f"  Replaced with: F8 C3 (clc; ret)")

            open(out, 'wb').write(data)
            print(f"Written {out}")
            return True

    print("ERROR: DDB not found")
    return False

inp = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SCRIPT_DIR, 'V5SMALL_PATCHED.VXD')
out = sys.argv[2] if len(sys.argv) > 2 else inp.replace('.VXD', '_NEUTER.VXD')
neuter(inp, out)
