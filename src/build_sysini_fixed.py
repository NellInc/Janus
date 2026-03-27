#!/usr/bin/env python3
"""
build_sysini_fixed.py - Build VxD with corrected LE structure for main VMM loader.

Fixes discovered by comparing against ESDI_506.PDR:
1. data_pages_off must be FILE-ABSOLUTE, not LE-relative
2. Table layout: all loader tables first, then fixup tables (no interleaving)
3. loader_section_size must be actual size of loader section
4. import_module_table_off and import_proc_table_off should point to valid offsets

Layout (matching ESDI_506.PDR structure):
  MZ header (0x80 bytes)
  LE header (0xC4 bytes)
  [LOADER SECTION]
    Object table
    Object page map
    Resident names table
    Entry table
  [FIXUP SECTION]
    Fixup page table
    Fixup record table
  [padding to alignment]
  [PAGE DATA]

Usage: python3 build_sysini_fixed.py [max_fixups] [output_name]
  Default: all fixups, V5FIXED.VXD
"""
import struct
import os
import sys

MAX_FIXUPS = int(sys.argv[1]) if len(sys.argv) > 1 else 99999
OUT_NAME = sys.argv[2] if len(sys.argv) > 2 else 'V5FIXED4.VXD'

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
MZ_SIZE = 0x80  # MZ header = LE file offset

# --- Read V5SMALL ---
data = bytearray(open(os.path.join(SCRIPT_DIR, 'V5SMALL.VXD'), 'rb').read())
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

print(f"V5SMALL: {num_obj} objects, {num_pages} pages")

# --- Read objects ---
objects = []
obj_offsets = []
cum = 0
for i in range(num_obj):
    e = obj_tbl + i * 24
    vsize = struct.unpack_from('<I', data, e)[0]
    reloc = struct.unpack_from('<I', data, e + 4)[0]
    pm_idx = struct.unpack_from('<I', data, e + 12)[0]
    pm_cnt = struct.unpack_from('<I', data, e + 16)[0]
    objects.append({'vsize': vsize, 'reloc': reloc, 'pm_idx': pm_idx, 'pm_cnt': pm_cnt})
    obj_offsets.append(cum)
    cum += pm_cnt * page_size
    print(f"  Obj{i+1}: vsize=0x{vsize:X} reloc=0x{reloc:X} pages={pm_cnt}")

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
ddb_obj = struct.unpack_from('<H', data, et_abs + 2)[0]
ddb_off_in_obj = struct.unpack_from('<I', data, et_abs + 5)[0]
merged_ddb_off = obj_offsets[ddb_obj - 1] + ddb_off_in_obj
print(f"DDB: obj{ddb_obj} off 0x{ddb_off_in_obj:X} -> merged 0x{merged_ddb_off:X}")

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
            obj_num = data[ap + 4]
            if has32:
                target = struct.unpack_from('<I', data, ap + 5)[0]
                pos += 9
            else:
                target = struct.unpack_from('<H', data, ap + 5)[0]
                pos += 7

            new_target = obj_offsets[obj_num - 1] + target

            page_fixups.append(0x07)   # src_type: 32-bit offset
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

# --- Build LE with ESDI-style layout ---
NP = num_pages
LE_HDR_SIZE = 0xC4

# LOADER SECTION (contiguous, matching ESDI order)
obj_off = LE_HDR_SIZE                    # object table
pm_off = obj_off + 24                    # page map (1 object * 24 bytes)
rnt_off = pm_off + NP * 4               # resident names (right after page map)

# Resident names table
rn = bytearray()
name = b'NTMINI_DDB'
rn.append(len(name))
rn.extend(name)
rn.extend(b'\x00\x01')  # ordinal
rn.append(0)  # end marker
rnt_size = len(rn)

et_off = rnt_off + rnt_size              # entry table (after resident names)

# Entry table
entry = bytearray(10)
entry[0] = 1; entry[1] = 3              # 1 entry, type 3 (32-bit)
struct.pack_into('<H', entry, 2, 1)      # object 1
entry[4] = 0x03                          # flags: exported + shared
struct.pack_into('<I', entry, 5, merged_ddb_off)
entry[9] = 0                             # end marker
et_size = len(entry)

loader_section_end = et_off + et_size
loader_section_size = loader_section_end - obj_off

# FIXUP SECTION (contiguous, after loader section)
fpt_off = loader_section_end
frt_off = fpt_off + (NP + 1) * 4
fixup_section_end = frt_off + len(new_fixup_data)

# import_module_table and import_proc_table point to end of fixup data
# (valid offset, 0 entries - matching ESDI pattern)
import_off = fixup_section_end
fixup_section_size = (NP + 1) * 4 + len(new_fixup_data)

# PAGE DATA - aligned to 512-byte boundary from file start (matching ESDI pattern)
# data_pages_off is FILE-ABSOLUTE (not LE-relative!)
file_offset_after_fixups = MZ_SIZE + fixup_section_end
# Align to next 0x200 boundary for cleanliness (ESDI uses 0x800)
data_pages_file = (file_offset_after_fixups + 0x1FF) & ~0x1FF
data_off_le_rel = data_pages_file - MZ_SIZE  # LE-relative for padding calc

# NONRESIDENT NAMES TABLE (after page data, file-absolute offset)
# ALL Microsoft VxDs have this - VMM may require it
nrn = bytearray()
nrn_name = b'NTMINI_DDB'
nrn.append(len(nrn_name))
nrn.extend(nrn_name)
nrn.extend(b'\x00\x01')  # ordinal = 1
nrn.append(0)  # end marker
nrn_file_off = data_pages_file + NP * page_size  # right after page data
nrn_len = len(nrn)

print(f"\n--- Layout ---")
print(f"  MZ header:        0x000 - 0x07F ({MZ_SIZE} bytes)")
print(f"  LE header:        0x080 - 0x143 ({LE_HDR_SIZE} bytes)")
print(f"  LOADER SECTION (LE-relative offsets):")
print(f"    Object table:   0x{obj_off:03X}")
print(f"    Page map:       0x{pm_off:03X}")
print(f"    Resident names: 0x{rnt_off:03X} ({rnt_size} bytes)")
print(f"    Entry table:    0x{et_off:03X} ({et_size} bytes)")
print(f"    Loader size:    0x{loader_section_size:03X} ({loader_section_size} bytes)")
print(f"  FIXUP SECTION:")
print(f"    FPT:            0x{fpt_off:03X} ({(NP+1)*4} bytes)")
print(f"    FRT:            0x{frt_off:03X} ({len(new_fixup_data)} bytes)")
print(f"    Import tables:  0x{import_off:03X}")
print(f"    Fixup size:     0x{fixup_section_size:03X} ({fixup_section_size} bytes)")
print(f"  PAGE DATA:")
print(f"    File-absolute:  0x{data_pages_file:03X}")
print(f"    LE-relative:    0x{data_off_le_rel:03X}")

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
struct.pack_into('<I', le_hdr, 0x2C, 0)        # last_page_bytes (0 = full page)

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
struct.pack_into('<I', le_hdr, 0x84, NP)                   # preload all pages

# Nonresident names - FILE ABSOLUTE (all Microsoft VxDs have this)
struct.pack_into('<I', le_hdr, 0x88, nrn_file_off)         # nonresident_names_off
struct.pack_into('<I', le_hdr, 0x8C, nrn_len)              # nonresident_names_len

# Extended VLE fields (LE+0xB0 to LE+0xC0) - ALL Microsoft VxDs have these
# LE+0xB8 = end of module data (file-absolute) = nrn_off + nrn_len
# LE+0xBC = varies (~500, purpose unclear but always present)
# LE+0xC0 = OS version info (high byte = 0x04 = Win386)
nrn_end = nrn_file_off + nrn_len
struct.pack_into('<I', le_hdr, 0xB8, nrn_end)              # end of module data
struct.pack_into('<I', le_hdr, 0xBC, 0x000001F4)           # common value from MS VxDs
struct.pack_into('<I', le_hdr, 0xC0, 0x04000000)           # Win386 OS type

# --- Build object table entry ---
obj_e = bytearray(24)
struct.pack_into('<I', obj_e, 0, merged_vsize)
struct.pack_into('<I', obj_e, 4, 0)       # reloc base = 0
struct.pack_into('<I', obj_e, 8, 0x2045)  # flags: readable, writable, executable, preload, 32-bit
struct.pack_into('<I', obj_e, 12, 1)      # page map idx (1-based)
struct.pack_into('<I', obj_e, 16, NP)     # page count
struct.pack_into('<I', obj_e, 20, 0x444F434C)  # reserved = "LCOD" (all MS VxDs have this)

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

out = bytearray()
out.extend(mz)           # MZ header
out.extend(le_hdr)        # LE header
out.extend(obj_e)         # Object table
out.extend(pm)            # Page map
out.extend(rn)            # Resident names
out.extend(entry)         # Entry table
out.extend(fpt_data)      # FPT
out.extend(new_fixup_data) # FRT

# Pad to data_pages_file offset
while len(out) < data_pages_file:
    out.append(0)

# Page data
out.extend(all_pages[:NP * page_size])

# Nonresident names table (immediately after page data)
out.extend(nrn)

outpath = os.path.join(SCRIPT_DIR, OUT_NAME)
open(outpath, 'wb').write(out)
print(f'\n{OUT_NAME}: {len(out)} bytes')
print(f'  1 object, {NP} pages, vsize=0x{merged_vsize:X}')
print(f'  {fixup_count} fixup records')
print(f'  DDB at merged offset 0x{merged_ddb_off:X}')
print(f'  data_pages_off (file-absolute): 0x{data_pages_file:X}')
print(f'  nonresident_names_off (file-absolute): 0x{nrn_file_off:X} ({nrn_len} bytes)')
print(f'  object reserved field: 0x444F434C (LCOD)')

# Verify the IOS loader dp formula still works
ios_dp = (et_off + 0x1F) & ~0x1F
print(f'\n--- IOS loader compatibility ---')
print(f'  IOS dp = align_up(et_off=0x{et_off:X}, 32) = 0x{ios_dp:X} (LE-relative)')
print(f'  Actual page data LE-relative: 0x{data_off_le_rel:X}')
if ios_dp == data_off_le_rel:
    print(f'  IOS dp matches page data: YES (works via both loaders)')
else:
    print(f'  IOS dp does NOT match page data (only works via SYSTEM.INI)')
    print(f'  To make IOS-compatible too: place pages at LE+0x{ios_dp:X} (file offset 0x{MZ_SIZE + ios_dp:X})')
