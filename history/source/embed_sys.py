#!/usr/bin/env python3
"""Convert a .sys binary file to a C header with embedded byte array.

Usage: python3 embed_sys.py <input.sys> <output.h> <varname>
Example: python3 embed_sys.py sym_hi.sys SYM_HI_EMBEDDED.H sym_hi_embedded
"""
import sys

def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <input.sys> <output.h> <varname>")
        sys.exit(1)

    infile, outfile, varname = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(infile, 'rb') as f:
        data = f.read()

    with open(outfile, 'w') as f:
        f.write(f"/* {infile} - embedded binary */\n")
        f.write(f"/* Size: {len(data)} bytes */\n")
        f.write(f"static const UCHAR {varname}_data[{len(data)}] = {{\n")
        for i in range(0, len(data), 16):
            chunk = data[i:i+16]
            hex_vals = ', '.join(f'0x{b:02X}' for b in chunk)
            f.write(f"    {hex_vals},\n")
        f.write("};\n")

    print(f"Wrote {outfile}: {len(data)} bytes as {varname}_data")

if __name__ == '__main__':
    main()
