#!/usr/bin/env python3
"""
Create a FAT16 test HDD image with proper CHS geometry for Win98 QEMU testing.

Produces a 16MB raw image with:
  - MBR with one FAT16 partition entry (type 0x06, CHS-correct)
  - FAT16 filesystem with TESTFILE.TXT containing known content
  - CHS geometry matching what QEMU reports for a 16MB raw image

Usage: python3 create_test_hdd.py [output_path]
Default output: /tmp/win98vm/test_hd.img
"""
import os, struct, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/win98vm/test_hd.img"
os.makedirs(os.path.dirname(OUT) or ".", exist_ok=True)

HEADS = 16
SPT = 63
SECTOR = 512
IMG_MB = 16
TOTAL_SECTORS = IMG_MB * 1024 * 1024 // SECTOR
CYLINDERS = TOTAL_SECTORS // (HEADS * SPT)

PART_START_LBA = SPT  # partition starts at track 1 (skip MBR track)
PART_SECTORS = TOTAL_SECTORS - PART_START_LBA

def lba_to_chs(lba):
    c = lba // (HEADS * SPT)
    rem = lba % (HEADS * SPT)
    h = rem // SPT
    s = (rem % SPT) + 1
    if c > 1023:
        c, h, s = 1023, 254, 63
    return c, h, s

def pack_chs(c, h, s):
    return bytes([h, ((c >> 2) & 0xC0) | (s & 0x3F), c & 0xFF])

# Build MBR
mbr = bytearray(512)
# Partition entry 1 at offset 446
pe = bytearray(16)
pe[0] = 0x80  # bootable
start_c, start_h, start_s = lba_to_chs(PART_START_LBA)
pe[1:4] = pack_chs(start_c, start_h, start_s)
pe[4] = 0x06  # FAT16 > 32MB (also used for <32MB with LBA)
end_lba = PART_START_LBA + PART_SECTORS - 1
end_c, end_h, end_s = lba_to_chs(end_lba)
pe[5:8] = pack_chs(end_c, end_h, end_s)
struct.pack_into("<I", pe, 8, PART_START_LBA)
struct.pack_into("<I", pe, 12, PART_SECTORS)
mbr[446:462] = pe
mbr[510] = 0x55
mbr[511] = 0xAA

# Build FAT16 boot sector (BPB)
CLUSTER_SIZE = 4096  # 8 sectors per cluster
RESERVED = 1
NUM_FATS = 2
ROOT_ENTRIES = 512
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR
SECTORS_PER_FAT = ((PART_SECTORS - RESERVED - ROOT_SECTORS) // (CLUSTER_SIZE // SECTOR) * 2 + SECTOR - 1) // SECTOR
# Recalculate properly
data_sectors = PART_SECTORS - RESERVED - NUM_FATS * SECTORS_PER_FAT - ROOT_SECTORS
total_clusters = data_sectors // (CLUSTER_SIZE // SECTOR)
# For FAT16: 2 bytes per entry
SECTORS_PER_FAT = (total_clusters * 2 + SECTOR - 1) // SECTOR
# Iterate to converge
for _ in range(5):
    data_sectors = PART_SECTORS - RESERVED - NUM_FATS * SECTORS_PER_FAT - ROOT_SECTORS
    total_clusters = data_sectors // (CLUSTER_SIZE // SECTOR)
    new_spf = (total_clusters * 2 + SECTOR - 1) // SECTOR
    if new_spf == SECTORS_PER_FAT:
        break
    SECTORS_PER_FAT = new_spf

bpb = bytearray(512)
bpb[0:3] = b'\xEB\x3C\x90'  # jmp short + nop
bpb[3:11] = b'MSDOS5.0'
struct.pack_into("<H", bpb, 11, SECTOR)  # bytes per sector
bpb[13] = CLUSTER_SIZE // SECTOR  # sectors per cluster
struct.pack_into("<H", bpb, 14, RESERVED)  # reserved sectors
bpb[16] = NUM_FATS
struct.pack_into("<H", bpb, 17, ROOT_ENTRIES)  # root entries
if PART_SECTORS < 0x10000:
    struct.pack_into("<H", bpb, 19, PART_SECTORS)
else:
    struct.pack_into("<H", bpb, 19, 0)
    struct.pack_into("<I", bpb, 32, PART_SECTORS)
bpb[21] = 0xF8  # media descriptor (hard disk)
struct.pack_into("<H", bpb, 22, SECTORS_PER_FAT)
struct.pack_into("<H", bpb, 24, SPT)  # sectors per track
struct.pack_into("<H", bpb, 26, HEADS)  # number of heads
struct.pack_into("<I", bpb, 28, PART_START_LBA)  # hidden sectors
bpb[36] = 0x80  # drive number
bpb[38] = 0x29  # extended boot sig
struct.pack_into("<I", bpb, 39, 0x12345678)  # serial
bpb[43:54] = b'NTMINI_TEST'  # volume label (11 chars)
bpb[54:62] = b'FAT16   '  # filesystem type
bpb[510] = 0x55
bpb[511] = 0xAA

# Build FAT
fat = bytearray(SECTORS_PER_FAT * SECTOR)
fat[0] = 0xF8  # media byte
fat[1] = 0xFF
fat[2] = 0xFF  # end-of-chain marker
fat[3] = 0xFF
# Cluster 2 = TESTFILE.TXT (single cluster, end-of-chain)
fat[4] = 0xFF
fat[5] = 0xFF
# Cluster 3 = SECOND.TXT (single cluster, end-of-chain)
fat[6] = 0xFF
fat[7] = 0xFF

# Build root directory
rootdir = bytearray(ROOT_SECTORS * SECTOR)
# Volume label entry
entry = bytearray(32)
entry[0:11] = b'NTMINI_TEST'
entry[11] = 0x08  # volume label attribute
rootdir[0:32] = entry
# TESTFILE.TXT entry
test_content = b"NTMINI HDD TEST DATA\r\nThis file is on the test ATA hard disk.\r\nIf you can read this, ATA HDD I/O works.\r\n"
entry2 = bytearray(32)
entry2[0:11] = b'TESTFILETXT'  # 8.3 name
entry2[11] = 0x20  # archive attribute
struct.pack_into("<H", entry2, 26, 2)  # start cluster = 2
struct.pack_into("<I", entry2, 28, len(test_content))  # file size
rootdir[32:64] = entry2
# SECOND.TXT entry
second_content = (
    b"HDD_SECOND_TXT: This file was read from the FAT16 hard disk image.\r\n"
    b"NTMINI HDD I/O PATH ACTIVE\r\n"
    b"Sector size: 512 bytes\r\n"
    b"Device type: ATA disk (0x00)\r\n"
)
entry3 = bytearray(32)
entry3[0:11] = b'SECOND  TXT'  # 8.3 name (padded with spaces)
entry3[11] = 0x20  # archive attribute
struct.pack_into("<H", entry3, 26, 3)  # start cluster = 3
struct.pack_into("<I", entry3, 28, len(second_content))  # file size
rootdir[64:96] = entry3

# Assemble image
img = bytearray(TOTAL_SECTORS * SECTOR)
# MBR at LBA 0
img[0:512] = mbr
# Partition starts at PART_START_LBA
part_off = PART_START_LBA * SECTOR
# BPB
img[part_off:part_off+512] = bpb
# FAT1
fat1_off = part_off + RESERVED * SECTOR
img[fat1_off:fat1_off+len(fat)] = fat
# FAT2
fat2_off = fat1_off + SECTORS_PER_FAT * SECTOR
img[fat2_off:fat2_off+len(fat)] = fat
# Root directory
root_off = fat2_off + SECTORS_PER_FAT * SECTOR
img[root_off:root_off+len(rootdir)] = rootdir
# Data area (cluster 2 = first data cluster)
data_off = root_off + ROOT_SECTORS * SECTOR
img[data_off:data_off+len(test_content)] = test_content
# Cluster 3 data (one cluster after cluster 2)
cluster3_off = data_off + CLUSTER_SIZE
img[cluster3_off:cluster3_off+len(second_content)] = second_content

with open(OUT, "wb") as f:
    f.write(img)

print(f"Created {OUT}")
print(f"  Size: {len(img)} bytes ({IMG_MB} MB)")
print(f"  CHS: {CYLINDERS}/{HEADS}/{SPT}")
print(f"  Partition: LBA {PART_START_LBA}-{PART_START_LBA+PART_SECTORS-1} (type 0x06 FAT16)")
print(f"  FAT16: {total_clusters} clusters, {CLUSTER_SIZE}-byte clusters")
print(f"  TESTFILE.TXT: {len(test_content)} bytes at cluster 2")
print(f"  SECOND.TXT:  {len(second_content)} bytes at cluster 3")
