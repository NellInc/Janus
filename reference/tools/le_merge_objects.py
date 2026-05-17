#!/usr/bin/env python3
"""
le_merge_objects.py - Merge multiple LE (Linear Executable) objects into one.

Win98's VMM32 loader can only load VxDs with 1 LE object from SYSTEM.INI
device= lines.  Watcom wlink sometimes produces 2 objects (code+data).
This post-link tool merges them into a single object so the VxD can load.

Usage:
    python3 le_merge_objects.py input.vxd output.vxd
"""

import struct
import sys
from dataclasses import dataclass, field
from typing import List


# ---- data structures -------------------------------------------------------

@dataclass
class LEObject:
    vsize: int
    base: int
    flags: int
    page_index: int      # 1-based
    page_count: int
    reserved: int


@dataclass
class FixupRecord:
    """One parsed fixup record (possibly with a source list)."""
    src_type_byte: int    # original byte (includes src_list flag etc.)
    tgt_flags_byte: int   # original byte
    # source
    src_list: bool
    src_offsets: List[int]       # 1 element if not src_list
    # target — only internal (type 0) records get obj/offset rewriting
    tgt_type: int                # 0=internal, 1=import-ord, 2=import-name
    # internal target fields (tgt_type == 0)
    tgt_obj: int
    tgt_offset: int
    tgt_32bit: bool              # True  => 4-byte target offset encoding
    tgt_16bit_obj: bool          # True  => 2-byte object number
    # raw tail bytes for non-internal fixups (import ordinal / name)
    raw_target_tail: bytes = b""


# ---- parsing helpers --------------------------------------------------------

def read_le_header(data: bytes, le_off: int) -> dict:
    """Return a dict of the LE header fields we care about."""
    get4 = lambda off: struct.unpack_from("<I", data, le_off + off)[0]
    return dict(
        le_off=le_off,
        signature=data[le_off:le_off + 2],
        num_pages=get4(0x14),
        page_size=get4(0x28),
        last_page_size=get4(0x2C),
        obj_tbl_off=get4(0x40),          # from LE start
        num_objects=get4(0x44),
        obj_pagemap_off=get4(0x48),      # from LE start
        fixup_page_tbl_off=get4(0x68),   # from LE start
        fixup_record_tbl_off=get4(0x6C), # from LE start
        imp_mod_tbl_off=get4(0x70),      # from LE start
        data_pages_off=get4(0x80),       # from FILE start (NOT LE start)
    )


def read_objects(data: bytes, le_off: int, obj_tbl_off: int,
                 num_objects: int) -> List[LEObject]:
    base = le_off + obj_tbl_off
    objs = []
    for i in range(num_objects):
        off = base + i * 24
        objs.append(LEObject(
            vsize=struct.unpack_from("<I", data, off)[0],
            base=struct.unpack_from("<I", data, off + 4)[0],
            flags=struct.unpack_from("<I", data, off + 8)[0],
            page_index=struct.unpack_from("<I", data, off + 12)[0],
            page_count=struct.unpack_from("<I", data, off + 16)[0],
            reserved=struct.unpack_from("<I", data, off + 20)[0],
        ))
    return objs


def read_fixup_page_table(data: bytes, abs_fpt: int,
                          num_pages: int) -> List[int]:
    """Return num_pages+1 entries (last is sentinel)."""
    return [struct.unpack_from("<I", data, abs_fpt + i * 4)[0]
            for i in range(num_pages + 1)]


def parse_fixup_records(data: bytes, abs_frt: int,
                        start: int, end: int) -> List[FixupRecord]:
    """Parse all fixup records in [start, end) byte range."""
    records: List[FixupRecord] = []
    pos = start
    while pos < end:
        src_type_byte = data[abs_frt + pos]
        tgt_flags_byte = data[abs_frt + pos + 1]

        src_type = src_type_byte & 0x0F
        src_list = bool(src_type_byte & 0x20)

        tgt_type = tgt_flags_byte & 0x03
        tgt_32bit = bool(tgt_flags_byte & 0x10)
        tgt_16bit_obj = bool(tgt_flags_byte & 0x20)
        tgt_8bit_ord = bool(tgt_flags_byte & 0x40)
        tgt_32bit_off_listed = bool(tgt_flags_byte & 0x08)

        pos += 2

        # ---- source offset(s) ----
        if src_list:
            cnt = data[abs_frt + pos]
            pos += 1
            src_offsets = []
            for _ in range(cnt):
                src_offsets.append(struct.unpack_from("<H", data, abs_frt + pos)[0])
                pos += 2
        else:
            src_offsets = [struct.unpack_from("<H", data, abs_frt + pos)[0]]
            pos += 2

        # ---- target ----
        tgt_obj = 0
        tgt_offset = 0
        raw_target_tail = b""

        if tgt_type == 0:  # internal
            if tgt_16bit_obj:
                tgt_obj = struct.unpack_from("<H", data, abs_frt + pos)[0]
                pos += 2
            else:
                tgt_obj = data[abs_frt + pos]
                pos += 1
            if tgt_32bit:
                tgt_offset = struct.unpack_from("<I", data, abs_frt + pos)[0]
                pos += 4
            else:
                tgt_offset = struct.unpack_from("<H", data, abs_frt + pos)[0]
                pos += 2

        elif tgt_type == 1:  # import by ordinal
            tail_start = abs_frt + pos
            if tgt_16bit_obj:
                pos += 2
            else:
                pos += 1
            if tgt_8bit_ord:
                pos += 1
            elif tgt_32bit:
                pos += 4
            else:
                pos += 2
            if tgt_32bit_off_listed:
                pos += 4
            raw_target_tail = data[tail_start:abs_frt + pos]

        elif tgt_type == 2:  # import by name
            tail_start = abs_frt + pos
            if tgt_16bit_obj:
                pos += 2
            else:
                pos += 1
            if tgt_32bit:
                pos += 4
            else:
                pos += 2
            if tgt_32bit_off_listed:
                pos += 4
            raw_target_tail = data[tail_start:abs_frt + pos]

        else:
            raise ValueError(f"Unsupported fixup target type {tgt_type} "
                             f"at record offset 0x{pos:X}")

        records.append(FixupRecord(
            src_type_byte=src_type_byte,
            tgt_flags_byte=tgt_flags_byte,
            src_list=src_list,
            src_offsets=src_offsets,
            tgt_type=tgt_type,
            tgt_obj=tgt_obj,
            tgt_offset=tgt_offset,
            tgt_32bit=tgt_32bit,
            tgt_16bit_obj=tgt_16bit_obj,
            raw_target_tail=raw_target_tail,
        ))
    return records


# ---- serialization ----------------------------------------------------------

def serialize_fixup_record(rec: FixupRecord) -> bytes:
    """Serialize one (possibly rewritten) fixup record to bytes."""
    out = bytearray()

    # src_type byte (unchanged)
    out.append(rec.src_type_byte)

    # tgt_flags byte — may have bit 4 toggled for 32-bit promotion
    tgt_flags = rec.tgt_flags_byte
    if rec.tgt_type == 0:
        if rec.tgt_32bit:
            tgt_flags |= 0x10
        else:
            tgt_flags &= ~0x10
    out.append(tgt_flags)

    # source offset(s)
    if rec.src_list:
        out.append(len(rec.src_offsets))
        for so in rec.src_offsets:
            out += struct.pack("<H", so)
    else:
        out += struct.pack("<H", rec.src_offsets[0])

    # target
    if rec.tgt_type == 0:  # internal
        if rec.tgt_16bit_obj:
            out += struct.pack("<H", rec.tgt_obj)
        else:
            out.append(rec.tgt_obj & 0xFF)
        if rec.tgt_32bit:
            out += struct.pack("<I", rec.tgt_offset)
        else:
            out += struct.pack("<H", rec.tgt_offset)
    else:
        out += rec.raw_target_tail

    return bytes(out)


# ---- main merge logic -------------------------------------------------------

def merge_le_objects(input_path: str, output_path: str) -> None:
    with open(input_path, "rb") as f:
        data = bytearray(f.read())

    # ---- read LE header ----
    le_off_field = struct.unpack_from("<I", data, 0x3C)[0]
    hdr = read_le_header(data, le_off_field)
    le_off = hdr["le_off"]

    if hdr["signature"] != b"LE":
        raise ValueError(f"Not an LE executable (got {hdr['signature']!r})")
    if hdr["num_objects"] < 2:
        print("Already 1 object, nothing to merge.")
        with open(output_path, "wb") as f:
            f.write(data)
        return

    print(f"LE header at file offset 0x{le_off:X}")
    print(f"Objects: {hdr['num_objects']}, Pages: {hdr['num_pages']}, "
          f"Page size: {hdr['page_size']}")

    objs = read_objects(data, le_off, hdr["obj_tbl_off"], hdr["num_objects"])
    for i, o in enumerate(objs):
        print(f"  obj{i+1}: vsize=0x{o.vsize:X} base=0x{o.base:X} "
              f"flags=0x{o.flags:X} pages={o.page_count} "
              f"page_idx={o.page_index}")

    page_size = hdr["page_size"]
    num_pages = hdr["num_pages"]

    # ---- compute cumulative offsets (for remapping obj N -> obj 1) ----
    # cum_offset[i] = virtual byte offset where object i's data starts
    # in the merged single object.
    cum_offset = [0]  # obj1 starts at 0
    for i in range(len(objs) - 1):
        cum_offset.append(cum_offset[-1] + objs[i].page_count * page_size)

    print(f"Cumulative offsets: {['0x%X' % c for c in cum_offset]}")

    # ---- parse all fixup records (per page) ----
    abs_fpt = le_off + hdr["fixup_page_tbl_off"]
    abs_frt = le_off + hdr["fixup_record_tbl_off"]
    fpt = read_fixup_page_table(data, abs_fpt, num_pages)

    all_page_records: List[List[FixupRecord]] = []
    obj2_rewrite_count = 0
    promoted_to_32bit = 0

    for pg in range(num_pages):
        recs = parse_fixup_records(data, abs_frt, fpt[pg], fpt[pg + 1])

        # rewrite internal fixups targeting objects > 1
        for rec in recs:
            if rec.tgt_type == 0 and rec.tgt_obj > 1:
                old_obj = rec.tgt_obj
                old_off = rec.tgt_offset
                rec.tgt_offset += cum_offset[old_obj - 1]
                rec.tgt_obj = 1
                obj2_rewrite_count += 1

                # promote to 32-bit encoding if needed
                if not rec.tgt_32bit and rec.tgt_offset > 0xFFFF:
                    rec.tgt_32bit = True
                    promoted_to_32bit += 1

        all_page_records.append(recs)

    print(f"Fixups rewritten (obj>1 -> obj1): {obj2_rewrite_count}")
    print(f"  promoted 16-bit -> 32-bit: {promoted_to_32bit}")

    # ---- serialize new fixup records and rebuild fixup page table ----
    new_frt = bytearray()
    new_fpt_entries = []
    for pg_recs in all_page_records:
        new_fpt_entries.append(len(new_frt))
        for rec in pg_recs:
            new_frt += serialize_fixup_record(rec)
    new_fpt_entries.append(len(new_frt))  # sentinel

    old_frt_size = fpt[-1]  # sentinel = total size of old fixup records
    new_frt_size = len(new_frt)
    delta = new_frt_size - old_frt_size
    print(f"Fixup records: old size=0x{old_frt_size:X}, "
          f"new size=0x{new_frt_size:X}, delta={delta:+d}")

    # ---- build new fixup page table bytes ----
    new_fpt_bytes = b"".join(struct.pack("<I", e) for e in new_fpt_entries)

    # ---- compute merged object entry ----
    merged_flags = 0
    for o in objs:
        merged_flags |= o.flags

    total_pages = sum(o.page_count for o in objs)
    merged_vsize = cum_offset[-1] + objs[-1].vsize

    merged_obj = LEObject(
        vsize=merged_vsize,
        base=objs[0].base,
        flags=merged_flags,
        page_index=1,
        page_count=total_pages,
        reserved=0,
    )

    print(f"Merged object: vsize=0x{merged_obj.vsize:X} "
          f"flags=0x{merged_obj.flags:X} pages={merged_obj.page_count}")

    # ---- assemble output file ----
    # Layout:
    #   [MZ stub .. LE header .. object table .. page map .. fixup_page_table]
    #   is contiguous up to fixup_record_table start.
    #   Then: new fixup records, then everything from imp_mod_table onward
    #   (which may be empty, followed by data pages).
    #
    # We keep the same structure but:
    #  - overwrite object table (1 entry instead of N, leave rest as padding)
    #  - rewrite fixup page table
    #  - replace fixup record table (possibly different size)
    #  - adjust offsets in LE header

    out = bytearray(data)

    # -- patch LE header: num_objects = 1 --
    struct.pack_into("<I", out, le_off + 0x44, 1)

    # -- write merged object table entry (overwrite first slot) --
    obj_tbl_abs = le_off + hdr["obj_tbl_off"]
    struct.pack_into("<I", out, obj_tbl_abs + 0, merged_obj.vsize)
    struct.pack_into("<I", out, obj_tbl_abs + 4, merged_obj.base)
    struct.pack_into("<I", out, obj_tbl_abs + 8, merged_obj.flags)
    struct.pack_into("<I", out, obj_tbl_abs + 12, merged_obj.page_index)
    struct.pack_into("<I", out, obj_tbl_abs + 16, merged_obj.page_count)
    struct.pack_into("<I", out, obj_tbl_abs + 20, merged_obj.reserved)
    # Zero out old object table entries beyond the first
    for i in range(1, hdr["num_objects"]):
        for j in range(24):
            out[obj_tbl_abs + i * 24 + j] = 0

    # -- write new fixup page table (same location, same # entries) --
    for i, entry in enumerate(new_fpt_entries):
        struct.pack_into("<I", out, abs_fpt + i * 4, entry)

    # -- splice new fixup records into file --
    # Everything between abs_frt and abs_frt+old_frt_size is replaced.
    old_frt_end = abs_frt + old_frt_size
    new_frt_end = abs_frt + new_frt_size

    out_spliced = bytearray()
    out_spliced += out[:abs_frt]
    out_spliced += new_frt
    out_spliced += out[old_frt_end:]

    # -- update LE header offsets that shifted by delta --
    # imp_mod_tbl_off (LE+0x70), and any other offsets between fixup records
    # and data pages. These are offsets from LE start.
    # Also: imp_proc_tbl_off (LE+0x74), fixup_checksum etc.

    # List of LE header fields (offset from LE start) that are "from LE start"
    # and point past the fixup record table:
    le_offsets_to_adjust = [
        0x70,  # import module table offset
        0x74,  # import procedure table offset
    ]
    for field_off in le_offsets_to_adjust:
        old_val = struct.unpack_from("<I", out_spliced, le_off + field_off)[0]
        if old_val > 0:
            struct.pack_into("<I", out_spliced, le_off + field_off,
                             old_val + delta)

    # data_pages_off (LE+0x80) is from FILE start
    old_dp = hdr["data_pages_off"]
    struct.pack_into("<I", out_spliced, le_off + 0x80, old_dp + delta)

    print(f"Data pages offset: 0x{old_dp:X} -> 0x{old_dp + delta:X}")
    print(f"Output file size: {len(out_spliced)} (was {len(data)})")

    # ---- self-check: verify merged structure ----
    print("\n--- Self-check ---")
    check_le_off = struct.unpack_from("<I", out_spliced, 0x3C)[0]
    check_nobj = struct.unpack_from("<I", out_spliced, check_le_off + 0x44)[0]
    print(f"  num_objects = {check_nobj}")
    check_obj_abs = check_le_off + struct.unpack_from("<I", out_spliced, check_le_off + 0x40)[0]
    check_vsize = struct.unpack_from("<I", out_spliced, check_obj_abs)[0]
    check_flags = struct.unpack_from("<I", out_spliced, check_obj_abs + 8)[0]
    check_pgcnt = struct.unpack_from("<I", out_spliced, check_obj_abs + 16)[0]
    print(f"  obj1: vsize=0x{check_vsize:X} flags=0x{check_flags:X} "
          f"pages={check_pgcnt}")
    assert check_nobj == 1, "FAIL: num_objects != 1"
    assert check_pgcnt == total_pages, "FAIL: page count mismatch"
    assert check_vsize == merged_vsize, "FAIL: vsize mismatch"

    # spot-check rewritten fixups: re-parse a few from the output
    check_fpt_off = struct.unpack_from("<I", out_spliced, check_le_off + 0x68)[0]
    check_frt_off = struct.unpack_from("<I", out_spliced, check_le_off + 0x6C)[0]
    check_abs_fpt = check_le_off + check_fpt_off
    check_abs_frt = check_le_off + check_frt_off
    # Read FPT for a page that had obj2 fixups (page 10, index 9)
    pg9_start = struct.unpack_from("<I", out_spliced, check_abs_fpt + 9 * 4)[0]
    pg9_end = struct.unpack_from("<I", out_spliced, check_abs_fpt + 10 * 4)[0]
    pg9_recs = parse_fixup_records(out_spliced, check_abs_frt, pg9_start, pg9_end)
    print(f"  page 10: {len(pg9_recs)} fixup records (spot-check)")
    obj2_in_output = 0
    samples_shown = 0
    for rec in pg9_recs:
        if rec.tgt_type == 0:
            if rec.tgt_obj != 1:
                obj2_in_output += 1
            # show a few that were originally obj2 (offset >= 0x13000)
            if rec.tgt_offset >= cum_offset[1] and samples_shown < 3:
                print(f"    fixup -> obj{rec.tgt_obj} offset=0x{rec.tgt_offset:X} "
                      f"32bit={rec.tgt_32bit}")
                samples_shown += 1
    if obj2_in_output > 0:
        print(f"  WARNING: {obj2_in_output} fixups still target obj>1!")
    else:
        print(f"  OK: all internal fixups target obj1")

    # ---- write output ----
    with open(output_path, "wb") as f:
        f.write(out_spliced)
    print(f"\nWrote {output_path}")


# ---- entry point ------------------------------------------------------------

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} input.vxd output.vxd")
        sys.exit(1)
    merge_le_objects(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
