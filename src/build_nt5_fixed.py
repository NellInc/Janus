#!/usr/bin/env python3
"""
build_nt5_fixed.py - Build NT5 VxD with corrected LE structure for main VMM loader.

Adapted from build_sysini_fixed.py for NT5RAW.VXD (3 objects, 11 pages).

Fixes applied (matching ESDI_506.PDR pattern):
1. data_pages_off must be FILE-ABSOLUTE, not LE-relative
2. Table layout: all loader tables first, then fixup tables (no interleaving)
3. loader_section_size must be actual size of loader section
4. import_module_table_off and import_proc_table_off should point to valid offsets
5. Merges all objects into a single flat object
6. Extended LE fields (LE+0xB8, LE+0xBC, LE+0xC0)

IOS-compatible layout (page data at ios_dp = align_up(et_off, 32)):
  MZ header (0x80 bytes)
  LE header (0xC4 bytes)
  [LOADER SECTION]
    Object table
    Object page map
    Resident names table
    Entry table
  [padding to 32-byte alignment]
  [PAGE DATA]           <- at LE-relative ios_dp
  [NONRESIDENT NAMES TABLE]
  [FIXUP SECTION]
    Fixup page table
    Fixup record table

Usage: python3 build_nt5_fixed.py [max_fixups] [output_name]
  Default: all fixups, NT5FIXED.VXD
  Example: python3 build_nt5_fixed.py 99999 NT5PDR.VXD
"""
import struct
import os
import sys

MAX_FIXUPS = int(sys.argv[1]) if len(sys.argv) > 1 else 99999
OUT_NAME = sys.argv[2] if len(sys.argv) > 2 else 'NT5FIXED.VXD'

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, '..'))
BIN_DIR = os.path.join(REPO_DIR, 'binaries')
MZ_SIZE = 0x80  # MZ header = LE file offset

# --- Read NT5RAW ---
raw_path = os.path.join(SCRIPT_DIR, '..', 'history', 'source', 'NT5RAW.VXD')
data = bytearray(open(raw_path, 'rb').read())
le = struct.unpack_from('<I', data, 0x3C)[0]
page_size = struct.unpack_from('<I', data, le + 0x28)[0]
num_pages = struct.unpack_from('<I', data, le + 0x14)[0]
num_obj = struct.unpack_from('<I', data, le + 0x44)[0]
obj_tbl = le + struct.unpack_from('<I', data, le + 0x40)[0]
pm_tbl = le + struct.unpack_from('<I', data, le + 0x48)[0]
fpt_abs = le + struct.unpack_from('<I', data, le + 0x68)[0]
frt_abs = le + struct.unpack_from('<I', data, le + 0x6C)[0]
dp_off = struct.unpack_from('<I', data, le + 0x80)[0]
et_abs = le + struct.unpack_from('<I', data, le + 0x5C)[0]

print(f"NT5RAW: {num_obj} objects, {num_pages} pages, page_size={page_size}")
print(f"  LE offset: 0x{le:X}")
print(f"  data_pages_off (raw): 0x{dp_off:X}")

# --- Read objects ---
objects = []
obj_offsets = []
cum = 0
for i in range(num_obj):
    e = obj_tbl + i * 24
    vsize = struct.unpack_from('<I', data, e)[0]
    reloc = struct.unpack_from('<I', data, e + 4)[0]
    flags = struct.unpack_from('<I', data, e + 8)[0]
    pm_idx = struct.unpack_from('<I', data, e + 12)[0]
    pm_cnt = struct.unpack_from('<I', data, e + 16)[0]
    objects.append({'vsize': vsize, 'reloc': reloc, 'flags': flags, 'pm_idx': pm_idx, 'pm_cnt': pm_cnt})
    obj_offsets.append(cum)
    cum += pm_cnt * page_size
    print(f"  Obj{i+1}: vsize=0x{vsize:X} reloc=0x{reloc:X} flags=0x{flags:X} pages={pm_cnt} pm_idx={pm_idx}")

# --- Extract page data ---
all_pages = bytearray()
for pg in range(num_pages):
    pg_off = dp_off + pg * page_size
    all_pages.extend(data[pg_off:pg_off + page_size])
while len(all_pages) < num_pages * page_size:
    all_pages.extend(b'\x00' * page_size)

# --- Compute merged vsize ---
merged_vsize = max(obj_offsets[i] + objects[i]['vsize'] for i in range(num_obj))
print(f"Merged vsize: 0x{merged_vsize:X}")

# --- Entry table: DDB offset ---
# Parse entry table to find DDB entry
et_byte0 = data[et_abs]
et_byte1 = data[et_abs + 1]
print(f"Entry table at 0x{et_abs:X}: count={et_byte0} type={et_byte1}")

if et_byte1 == 3:
    # 32-bit entry
    ddb_obj = struct.unpack_from('<H', data, et_abs + 2)[0]
    ddb_off_in_obj = struct.unpack_from('<I', data, et_abs + 5)[0]
elif et_byte1 == 0x03:
    ddb_obj = struct.unpack_from('<H', data, et_abs + 2)[0]
    ddb_off_in_obj = struct.unpack_from('<I', data, et_abs + 5)[0]
else:
    # Try alternate entry table format
    ddb_obj = data[et_abs + 4] if len(data) > et_abs + 4 else 1
    ddb_off_in_obj = struct.unpack_from('<I', data, et_abs + 5)[0] if len(data) > et_abs + 8 else 0
    print(f"  WARNING: Unexpected entry table type {et_byte1}, guessing DDB location")

merged_ddb_off = obj_offsets[ddb_obj - 1] + ddb_off_in_obj
print(f"DDB: obj{ddb_obj} off 0x{ddb_off_in_obj:X} -> merged 0x{merged_ddb_off:X}")

# --- Resident name: override to match DDB name (NTMINI) ---
rnt_name = b'NTMINI'
print(f"Resident name: '{rnt_name.decode('ascii')}' (overridden to match DDB)")

# --- Read and translate fixup records ---
fpt = [struct.unpack_from('<I', data, fpt_abs + i * 4)[0] for i in range(num_pages + 1)]

new_fixup_data = bytearray()
new_fpt = [0]
fixup_count = 0
stopped = False

for pg in range(num_pages):
    rs = fpt[pg]
    re = fpt[pg + 1]
    page_fixups = bytearray()
    pos = rs

    while pos < re and not stopped:
        ap = frt_abs + pos
        src_type = data[ap]
        tgt_flags = data[ap + 1]
        src_offset = struct.unpack_from('<H', data, ap + 2)[0]
        tgt_type = tgt_flags & 0x03
        has32 = bool(tgt_flags & 0x10)

        if src_type & 0x10:
            pos = re
            continue

        if tgt_type == 0:
            # Internal fixup
            obj_num = data[ap + 4]
            if has32:
                target = struct.unpack_from('<I', data, ap + 5)[0]
                pos += 9
            else:
                target = struct.unpack_from('<H', data, ap + 5)[0]
                pos += 7

            new_target = obj_offsets[obj_num - 1] + target

            # Preserve original src_type: 0x07 = 32-bit offset,
            # 0x08 = 32-bit self-relative (for CALL/JMP rel32).
            # Using 0x07 for self-relative fixups corrupts all cross-object calls.
            page_fixups.append(src_type & 0x0F)  # preserve src_type, clear flags
            if new_target <= 0xFFFF:
                page_fixups.append(0x00)   # internal, 16-bit target
                page_fixups.extend(struct.pack('<H', src_offset))
                page_fixups.append(0x01)   # object 1
                page_fixups.extend(struct.pack('<H', new_target))
            else:
                page_fixups.append(0x10)   # internal, 32-bit target
                page_fixups.extend(struct.pack('<H', src_offset))
                page_fixups.append(0x01)   # object 1
                page_fixups.extend(struct.pack('<I', new_target))

            fixup_count += 1
            if fixup_count >= MAX_FIXUPS:
                stopped = True
        elif tgt_type == 3:
            # Internal entry via ordinal
            rec_size = 5
            page_fixups.extend(data[ap:ap + rec_size])
            pos += rec_size
            fixup_count += 1
            if fixup_count >= MAX_FIXUPS:
                stopped = True
        else:
            pos = re

    new_fixup_data.extend(page_fixups)
    new_fpt.append(len(new_fixup_data))

print(f"\nFixups: {fixup_count} records, {len(new_fixup_data)} bytes")

# --- Build LE with IOS-compatible layout ---
# Key: page data must start at ios_dp = align_up(et_off, 32) (LE-relative)
# so IOS's formula lands on our pages.
NP = num_pages
LE_HDR_SIZE = 0xC4

# LOADER SECTION (contiguous, matching ESDI order)
obj_off = LE_HDR_SIZE                    # object table
pm_off = obj_off + 24                    # page map (1 object * 24 bytes)
rnt_off = pm_off + NP * 4               # resident names (right after page map)

# Resident names table (use the original module name)
rn = bytearray()
rn.append(len(rnt_name))
rn.extend(rnt_name)
rn.extend(b'\x00\x01')  # ordinal
rn.append(0)  # end marker
rnt_size = len(rn)

et_off = rnt_off + rnt_size              # entry table (after resident names)

# Entry table
entry = bytearray(10)
entry[0] = 1; entry[1] = 3              # 1 entry, type 3 (32-bit)
struct.pack_into('<H', entry, 2, 1)      # object 1
entry[4] = 0x01                          # flags: exported only (0x03 crashes VXDLDR)
struct.pack_into('<I', entry, 5, merged_ddb_off)
entry[9] = 0                             # end marker
et_size = len(entry)

loader_section_end = et_off + et_size
loader_section_size = loader_section_end - obj_off

# --- ESDI_506-compatible layout: fixups BEFORE page data ---
# ESDI_506 layout: loader → fixups → [pad to 0x200] → pages
# VXDLDR expects fixup tables contiguous with loader section.

# FIXUP SECTION (immediately after loader section)
fpt_off = loader_section_end             # LE-relative
frt_off = fpt_off + (NP + 1) * 4
fixup_section_end_le = frt_off + len(new_fixup_data)

# import_module_table and import_proc_table point to end of fixup data
import_off = fixup_section_end_le
fixup_section_size = (NP + 1) * 4 + len(new_fixup_data)

# PAGE DATA: align to 512-byte sector boundary after fixup section (matching ESDI_506)
data_off_le_rel = (fixup_section_end_le + 0x1FF) & ~0x1FF
data_pages_file = MZ_SIZE + data_off_le_rel  # file-absolute

# NONRESIDENT NAMES TABLE (after page data)
nrn = bytearray()
nrn.append(len(rnt_name))
nrn.extend(rnt_name)
nrn.extend(b'\x00\x01')  # ordinal = 1
nrn.append(0)  # end marker
nrn_file_off = data_pages_file + NP * page_size
nrn_len = len(nrn)

print(f"\n--- Layout (ESDI_506-compatible: fixups before pages) ---")
print(f"  MZ header:        0x000 - 0x07F ({MZ_SIZE} bytes)")
print(f"  LE header:        0x080 - 0x143 ({LE_HDR_SIZE} bytes)")
print(f"  LOADER SECTION (LE-relative offsets):")
print(f"    Object table:   0x{obj_off:03X}")
print(f"    Page map:       0x{pm_off:03X}")
print(f"    Resident names: 0x{rnt_off:03X} ({rnt_size} bytes)")
print(f"    Entry table:    0x{et_off:03X} ({et_size} bytes)")
print(f"    Loader size:    0x{loader_section_size:03X} ({loader_section_size} bytes)")
print(f"  FIXUP SECTION (after loader, before pages):")
print(f"    FPT:            0x{fpt_off:03X} (LE-rel)")
print(f"    FRT:            0x{frt_off:04X} (LE-rel)")
print(f"    Import tables:  0x{import_off:04X} (LE-rel)")
print(f"    Fixup size:     0x{fixup_section_size:04X} ({fixup_section_size} bytes)")
print(f"  PAGE DATA (sector-aligned after fixups):")
print(f"    LE-relative:    0x{data_off_le_rel:05X}")
print(f"    File-absolute:  0x{data_pages_file:05X}")
print(f"  NONRESIDENT NAMES:")
print(f"    File-absolute:  0x{nrn_file_off:05X} ({nrn_len} bytes)")

# --- Build FPT ---
fpt_data = bytearray((NP + 1) * 4)
for i, v in enumerate(new_fpt):
    struct.pack_into('<I', fpt_data, i * 4, v)

# --- Build LE header ---
le_hdr = bytearray(LE_HDR_SIZE)
le_hdr[0:2] = b'LE'
struct.pack_into('<H', le_hdr, 0x08, 2)       # CPU 386
struct.pack_into('<H', le_hdr, 0x0A, 4)       # OS Win386
struct.pack_into('<I', le_hdr, 0x10, 0x00038000)  # module flags (VxD)
struct.pack_into('<I', le_hdr, 0x14, NP)       # num pages
struct.pack_into('<I', le_hdr, 0x28, 4096)     # page size
last_page_sz = len(all_pages) % page_size
if last_page_sz == 0:
    last_page_sz = page_size  # full last page
struct.pack_into('<I', le_hdr, 0x2C, last_page_sz)  # last_page_bytes

# Loader section
struct.pack_into('<I', le_hdr, 0x38, loader_section_size)  # CORRECT loader section size
struct.pack_into('<I', le_hdr, 0x40, obj_off)              # object table offset
struct.pack_into('<I', le_hdr, 0x44, 1)                    # 1 object
struct.pack_into('<I', le_hdr, 0x48, pm_off)               # page map offset
struct.pack_into('<I', le_hdr, 0x58, rnt_off)              # resident names offset
struct.pack_into('<I', le_hdr, 0x5C, et_off)               # entry table offset

# Fixup section
struct.pack_into('<I', le_hdr, 0x30, fixup_section_size)   # fixup section size
struct.pack_into('<I', le_hdr, 0x68, fpt_off)              # FPT offset
struct.pack_into('<I', le_hdr, 0x6C, frt_off)              # FRT offset
struct.pack_into('<I', le_hdr, 0x70, import_off)           # import module table (valid offset)
struct.pack_into('<I', le_hdr, 0x78, import_off)           # import proc table (valid offset)

# Page data - FILE ABSOLUTE (key fix!)
struct.pack_into('<I', le_hdr, 0x80, data_pages_file)      # data_pages_off = FILE ABSOLUTE
struct.pack_into('<I', le_hdr, 0x84, NP)                   # preload ALL pages (ESDI preloads 2; we need all for merged single object)

# Nonresident names - FILE ABSOLUTE (all Microsoft VxDs have this)
struct.pack_into('<I', le_hdr, 0x88, nrn_file_off)         # nonresident_names_off
struct.pack_into('<I', le_hdr, 0x8C, nrn_len)              # nonresident_names_len

# Extended VLE fields (LE+0xB0 to LE+0xC0) - ALL Microsoft VxDs have these
# LE+0xB8 = end of module data (file-absolute) = end of fixup section
# LE+0xBC = varies (~500, purpose unclear but always present)
# LE+0xC0 = OS version info (high byte = 0x04 = Win386)
file_end = nrn_file_off + nrn_len
struct.pack_into('<I', le_hdr, 0xB8, file_end)              # end of module data
struct.pack_into('<I', le_hdr, 0xBC, 0x000001F4)           # common value from MS VxDs
struct.pack_into('<I', le_hdr, 0xC0, 0x04000000)           # Win386 OS type

# --- Build object table entry ---
obj_e = bytearray(24)
struct.pack_into('<I', obj_e, 0, merged_vsize)
struct.pack_into('<I', obj_e, 4, 0x00000000)  # reloc base = 0, matching ESDI_506.PDR obj1
struct.pack_into('<I', obj_e, 8, 0x2047)  # flags: readable, writable, executable, preload, 32-bit
struct.pack_into('<I', obj_e, 12, 1)      # page map idx (1-based)
struct.pack_into('<I', obj_e, 16, NP)     # page count
struct.pack_into('<I', obj_e, 20, 0x444F434C)  # reserved = 'LCOD', matching ESDI_506.PDR obj1

# --- Build page map ---
pm = bytearray(NP * 4)
for i in range(NP):
    pm[i * 4 + 2] = i + 1  # page data number (1-based)
    # pm[i * 4 + 3] = 0    # flags: legal/valid page (already zero)

# --- Assemble file ---
# Use a proper MZ header matching ESDI_506.PDR (not just 'MZ' + zeros)
# VMM may validate MZ header fields before looking at LE
mz = bytearray(MZ_SIZE)
mz[0:2] = b'MZ'
struct.pack_into('<H', mz, 0x04, 4)        # e_cp: pages in file
struct.pack_into('<H', mz, 0x08, 4)        # e_cparhdr: header paragraphs (64 bytes)
struct.pack_into('<H', mz, 0x0C, 0xFFFF)   # e_maxalloc
struct.pack_into('<H', mz, 0x10, 0x00B8)   # e_ss
struct.pack_into('<H', mz, 0x18, 0x0040)   # e_lfarlc: relocation table offset
struct.pack_into('<I', mz, 0x3C, MZ_SIZE)  # e_lfanew: LE offset

# DOS stub: "This program cannot be run in DOS mode."
dos_stub = bytes([
    0x0E, 0x1F, 0xBA, 0x0E, 0x00, 0xB4, 0x09, 0xCD,
    0x21, 0xB8, 0x01, 0x4C, 0xCD, 0x21,
]) + b'This program cannot be run in DOS mode.\r\n$'
mz[0x40:0x40+len(dos_stub)] = dos_stub

# File assembly order: MZ -> LE hdr -> loader -> fixups -> pad -> pages -> NRN
out = bytearray()
out.extend(mz)           # MZ header
out.extend(le_hdr)        # LE header
out.extend(obj_e)         # Object table
out.extend(pm)            # Page map
out.extend(rn)            # Resident names
out.extend(entry)         # Entry table

# Fixup tables (immediately after loader, before page data — matching ESDI_506)
out.extend(fpt_data)      # FPT
out.extend(new_fixup_data) # FRT

# Pad to sector boundary for page data
while len(out) < data_pages_file:
    out.append(0)

# Page data (sector-aligned after fixup section)
out.extend(all_pages[:NP * page_size])

# Nonresident names table (after page data)
out.extend(nrn)

# --- Output ---
legacy_outpath = os.path.join(SCRIPT_DIR, OUT_NAME)

if os.path.isabs(OUT_NAME) or os.path.dirname(OUT_NAME):
    outpath = OUT_NAME
else:
    os.makedirs(BIN_DIR, exist_ok=True)
    outpath = os.path.join(BIN_DIR, OUT_NAME)
open(outpath, 'wb').write(out)
if outpath != legacy_outpath:
    open(legacy_outpath, 'wb').write(out)

# Also produce NT5PDR.VXD alongside the primary output
if OUT_NAME != 'NT5PDR.VXD':
    pdr_path = os.path.join(BIN_DIR, 'NT5PDR.VXD')
    open(pdr_path, 'wb').write(out)
    pdr_legacy = os.path.join(SCRIPT_DIR, 'NT5PDR.VXD')
    open(pdr_legacy, 'wb').write(out)

print(f'\n{OUT_NAME}: {len(out)} bytes')
print(f'  Output path: {outpath}')
if outpath != legacy_outpath:
    print(f'  Legacy copy: {legacy_outpath}')
if OUT_NAME != 'NT5PDR.VXD':
    print(f'  PDR copy:    {pdr_path}')
print(f'  1 object (merged from {num_obj}), {NP} pages, vsize=0x{merged_vsize:X}')
print(f'  {fixup_count} fixup records')
print(f'  DDB at merged offset 0x{merged_ddb_off:X}')
print(f'  data_pages_off (file-absolute): 0x{data_pages_file:X}')
print(f'  nonresident_names_off (file-absolute): 0x{nrn_file_off:X} ({nrn_len} bytes)')
print(f'  object reserved field: 0x444F434C (LCOD)')

# Layout verification
print(f'\n--- Layout verification ---')
print(f'  Fixup section: LE+0x{fpt_off:X} to LE+0x{fixup_section_end_le:X}')
print(f'  Page data:     LE+0x{data_off_le_rel:X} (file 0x{data_pages_file:X})')
print(f'  Layout:        loader → fixups → pages (matching ESDI_506.PDR)')
