#!/usr/bin/env python3
"""Dump and compare DDB contents between V5REPACKED and TEST_8PAGE."""
import struct
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

DDB_FIELDS = [
    (0x00, '<I', 'DDB_Next'),
    (0x04, '<H', 'DDB_SDK_Version'),
    (0x06, '<H', 'DDB_Req_Device_Number'),
    (0x08, '<B', 'DDB_Dev_Major_Version'),
    (0x09, '<B', 'DDB_Dev_Minor_Version'),
    (0x0A, '<H', 'DDB_Flags'),
    (0x0C, '8s', 'DDB_Name'),
    (0x14, '<I', 'DDB_Init_Order'),
    (0x18, '<I', 'DDB_Control_Proc'),
    (0x1C, '<I', 'DDB_V86_API_Proc'),
    (0x20, '<I', 'DDB_PM_API_Proc'),
    (0x24, '<I', 'DDB_V86_API_CSIP'),
    (0x28, '<I', 'DDB_PM_API_CSIP'),
    (0x2C, '<I', 'DDB_Reference_Data'),
    (0x30, '<I', 'DDB_Service_Table_Ptr'),
    (0x34, '<I', 'DDB_Service_Table_Size'),
    (0x38, '<I', 'DDB_Win32_Service_Table'),
    (0x3C, '<I', 'DDB_Prev'),
    (0x40, '<I', 'DDB_Size'),
]

def dump_ddb(name, data, ddb_file_offset):
    print(f'\n=== DDB from {name} ===')
    print(f'File offset: 0x{ddb_file_offset:X}')
    print(f'Raw bytes: {data[ddb_file_offset:ddb_file_offset+0x44].hex()}')
    for off, fmt, field in DDB_FIELDS:
        val = struct.unpack_from(fmt, data, ddb_file_offset + off)
        if fmt == '8s':
            print(f'  +0x{off:02X} {field:30s} = {val[0]!r}')
        elif 'H' in fmt or 'B' in fmt:
            print(f'  +0x{off:02X} {field:30s} = 0x{val[0]:04X}')
        else:
            print(f'  +0x{off:02X} {field:30s} = 0x{val[0]:08X}')

# V5REPACKED
v5 = open(os.path.join(SCRIPT_DIR, 'V5REPACKED.VXD'), 'rb').read()
v5_le = struct.unpack_from('<I', v5, 0x3C)[0]
v5_dp = v5_le + struct.unpack_from('<I', v5, v5_le + 0x80)[0]
v5_et = v5_le + struct.unpack_from('<I', v5, v5_le + 0x5C)[0]
v5_ddb_off_in_obj = struct.unpack_from('<I', v5, v5_et + 5)[0]
v5_ddb_file = v5_dp + v5_ddb_off_in_obj
dump_ddb('V5REPACKED', v5, v5_ddb_file)

# TEST_8PAGE
t8 = open(os.path.join(SCRIPT_DIR, 'TEST_8PAGE.VXD'), 'rb').read()
t8_le = struct.unpack_from('<I', t8, 0x3C)[0]
t8_dp = t8_le + struct.unpack_from('<I', t8, t8_le + 0x80)[0]
t8_et = t8_le + struct.unpack_from('<I', t8, t8_le + 0x5C)[0]
t8_ddb_off_in_obj = struct.unpack_from('<I', t8, t8_et + 5)[0]
t8_ddb_file = t8_dp + t8_ddb_off_in_obj
dump_ddb('TEST_8PAGE', t8, t8_ddb_file)

# Also check the original Watcom V5SMALL
v5small = os.path.join(SCRIPT_DIR, 'V5SMALL.VXD')
if os.path.exists(v5small):
    d = open(v5small, 'rb').read()
    le = struct.unpack_from('<I', d, 0x3C)[0]
    et_off = struct.unpack_from('<I', d, le + 0x5C)[0]
    et = le + et_off
    et_count = d[et]
    et_type = d[et + 1]
    et_obj = struct.unpack_from('<H', d, et + 2)[0]
    et_flags = d[et + 4]
    et_offset = struct.unpack_from('<I', d, et + 5)[0]
    dp_off = struct.unpack_from('<I', d, le + 0x80)[0]
    num_obj = struct.unpack_from('<I', d, le + 0x44)[0]
    obj_tbl = le + struct.unpack_from('<I', d, le + 0x40)[0]

    print(f'\n=== V5SMALL entry table ===')
    print(f'  count={et_count} type={et_type} obj={et_obj} flags=0x{et_flags:02X} offset=0x{et_offset:X}')
    print(f'  num_objects={num_obj}')

    # Find DDB in original multi-object layout
    for i in range(num_obj):
        e = obj_tbl + i * 24
        vsize = struct.unpack_from('<I', d, e)[0]
        reloc = struct.unpack_from('<I', d, e + 4)[0]
        flags = struct.unpack_from('<I', d, e + 8)[0]
        pm_idx = struct.unpack_from('<I', d, e + 12)[0]
        pm_cnt = struct.unpack_from('<I', d, e + 16)[0]
        print(f'  Obj{i+1}: vsize=0x{vsize:X} reloc=0x{reloc:X} flags=0x{flags:X} pages={pm_cnt} pm_idx={pm_idx}')

    # Get page data for the DDB's object
    pm_tbl = le + struct.unpack_from('<I', d, le + 0x48)[0]
    # DDB is in object et_obj
    # Find that object's first page
    obj_e = obj_tbl + (et_obj - 1) * 24
    obj_pm_idx = struct.unpack_from('<I', d, obj_e + 12)[0]  # 1-based

    # Read page map entry for first page of DDB's object
    pm_entry = pm_tbl + (obj_pm_idx - 1) * 4
    page_num = (d[pm_entry] << 16) | (d[pm_entry+1] << 8) | d[pm_entry+2]

    page_size = struct.unpack_from('<I', d, le + 0x28)[0]
    # data_pages_off might be LE-relative or file-relative for Watcom
    # Try both
    ddb_page_data_off_le_rel = dp_off + (page_num - 1) * page_size
    ddb_file_off_le_rel = le + ddb_page_data_off_le_rel + et_offset
    ddb_file_off_abs = dp_off + (page_num - 1) * page_size + et_offset

    print(f'  DDB page_num={page_num}, dp_off=0x{dp_off:X}')
    print(f'  Trying LE-relative: file offset 0x{ddb_file_off_le_rel:X}')
    if ddb_file_off_le_rel + 0x44 <= len(d):
        dump_ddb('V5SMALL (LE-relative dp)', d, ddb_file_off_le_rel)
    print(f'  Trying file-absolute: file offset 0x{ddb_file_off_abs:X}')
    if ddb_file_off_abs + 0x44 <= len(d):
        dump_ddb('V5SMALL (file-absolute dp)', d, ddb_file_off_abs)
