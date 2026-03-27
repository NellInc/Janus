#!/usr/bin/env python3
"""
deploy_to_iosubsys.py - Deploy a VxD to WINDOWS\SYSTEM\IOSUBSYS on a FAT32 disk image.

Auto-detects FAT32 geometry from the disk's BPB. Traverses the FAT32 directory
tree from root -> WINDOWS -> SYSTEM -> IOSUBSYS, then creates or updates an
NTMINI.PDR entry, allocating free clusters from the FAT.

Also provides a function to remove 'device=ntmini.vxd' from SYSTEM.INI if present.

Usage:
    python3 deploy_to_iosubsys.py NTMINI_V5.VXD [--disk /path/to/disk.img]
    python3 deploy_to_iosubsys.py NTMINI_V5.VXD --remove-sysini
    python3 deploy_to_iosubsys.py --remove-sysini   # Only remove SYSTEM.INI line
"""

import struct
import sys
import os

# ── Disk geometry (auto-detected from BPB) ────────────────────────────────
DISK = os.environ.get("WIN98_IMG", "/tmp/win98vm/win98.img")
# These are set by detect_fat32_geometry()
PARTITION_LBA = 0
BPS = 512
SPC = 0
RESERVED = 0
NFATS = 0
FAT32SZ = 0
ROOT_CLUSTER = 0
CLUSTER_SIZE = 0
DATA_START_SEC = 0
DATA_START = 0
FAT_OFFSET = 0
VXD_CLUSTERS = []  # Dynamically allocated


def detect_fat32_geometry(f):
    """Auto-detect FAT32 parameters from MBR partition table and BPB.

    Reads the MBR to find the first FAT32 partition, then reads its BPB
    to extract all geometry parameters. Sets module-level globals.
    """
    global PARTITION_LBA, BPS, SPC, RESERVED, NFATS, FAT32SZ, ROOT_CLUSTER
    global CLUSTER_SIZE, DATA_START_SEC, DATA_START, FAT_OFFSET

    # Read MBR
    f.seek(0)
    mbr = f.read(512)

    # Check MBR signature
    if mbr[510:512] != b'\x55\xAA':
        raise RuntimeError("No MBR signature found (not 55 AA)")

    # Find first FAT32 partition (type 0x0B or 0x0C)
    part_found = False
    for i in range(4):
        entry_off = 446 + i * 16
        ptype = mbr[entry_off + 4]
        if ptype in (0x0B, 0x0C, 0x0E):  # FAT32 types
            PARTITION_LBA = struct.unpack_from('<I', mbr, entry_off + 8)[0]
            part_found = True
            print(f"  Found FAT32 partition {i+1}: type=0x{ptype:02X}, LBA={PARTITION_LBA}")
            break

    if not part_found:
        # Try treating entire disk as FAT32 (no MBR, just a BPB at sector 0)
        PARTITION_LBA = 0
        print("  No FAT32 partition in MBR, trying offset 0 (floppy/superfloppy)")

    # Read BPB from partition start
    part_byte = PARTITION_LBA * 512
    f.seek(part_byte)
    bpb = f.read(512)

    # Validate BPB - check for jump instruction
    if bpb[0] not in (0xEB, 0xE9):
        raise RuntimeError(f"No jump instruction at partition start (got 0x{bpb[0]:02X})")

    # Parse BPB fields
    BPS = struct.unpack_from('<H', bpb, 11)[0]
    SPC = bpb[13]
    RESERVED = struct.unpack_from('<H', bpb, 14)[0]
    NFATS = bpb[16]
    total_sectors_16 = struct.unpack_from('<H', bpb, 19)[0]
    total_sectors_32 = struct.unpack_from('<I', bpb, 32)[0]
    FAT32SZ = struct.unpack_from('<I', bpb, 36)[0]
    ROOT_CLUSTER = struct.unpack_from('<I', bpb, 44)[0]

    total_sectors = total_sectors_32 if total_sectors_16 == 0 else total_sectors_16

    CLUSTER_SIZE = BPS * SPC
    DATA_START_SEC = PARTITION_LBA + RESERVED + NFATS * FAT32SZ
    DATA_START = DATA_START_SEC * BPS
    FAT_OFFSET = (PARTITION_LBA + RESERVED) * BPS

    print(f"  BPB: BPS={BPS} SPC={SPC} Reserved={RESERVED} FATs={NFATS}")
    print(f"  FAT32: sectors_per_fat={FAT32SZ} root_cluster={ROOT_CLUSTER}")
    print(f"  Total sectors: {total_sectors} ({total_sectors * BPS // 1048576} MB)")
    print(f"  Cluster size: {CLUSTER_SIZE} bytes")
    print(f"  Data start: sector {DATA_START_SEC} (byte 0x{DATA_START:X})")


def allocate_clusters(f, count):
    """Find 'count' free clusters in the FAT and chain them together.

    Returns list of allocated cluster numbers. Updates FAT entries to form a chain.
    """
    global VXD_CLUSTERS
    free = []
    # Start searching from cluster 2 (first data cluster)
    max_cluster = FAT32SZ * BPS // 4  # max entries in one FAT
    c = 2
    while len(free) < count and c < max_cluster:
        val = read_fat_entry_raw(f, c)
        if val == 0:  # free cluster
            free.append(c)
        c += 1

    if len(free) < count:
        raise RuntimeError(f"Only found {len(free)} free clusters, need {count}")

    # Write the FAT chain: each cluster points to the next, last = EOC
    for i, cluster in enumerate(free):
        if i < len(free) - 1:
            next_val = free[i + 1]
        else:
            next_val = 0x0FFFFFF8  # end of chain
        write_fat_entry(f, cluster, next_val)

    VXD_CLUSTERS = free
    print(f"  Allocated {count} clusters: {free[0]}..{free[-1]}")
    return free


def read_fat_entry_raw(f, cluster):
    """Read a raw FAT32 entry without masking (for free cluster detection)."""
    off = FAT_OFFSET + cluster * 4
    f.seek(off)
    raw = f.read(4)
    return struct.unpack('<I', raw)[0] & 0x0FFFFFFF


def write_fat_entry(f, cluster, value):
    """Write a FAT32 entry for the given cluster in ALL FAT copies."""
    for fat_num in range(NFATS):
        fat_base = (PARTITION_LBA + RESERVED + fat_num * FAT32SZ) * BPS
        off = fat_base + cluster * 4
        # Preserve upper 4 bits
        f.seek(off)
        old = struct.unpack('<I', f.read(4))[0]
        new_val = (old & 0xF0000000) | (value & 0x0FFFFFFF)
        f.seek(off)
        f.write(struct.pack('<I', new_val))

# Target 8.3 name - deploy as .PDR to reuse existing dir entry
TARGET_83NAME = b'NTMINI  PDR'  # 8 chars name + 3 chars extension, space-padded

# Directory path to traverse
DIR_PATH = ["WINDOWS ", "SYSTEM  ", "IOSUBSYS"]
DIR_PATH_83 = [
    b'WINDOWS    ',   # "WINDOWS" + 4 spaces + no ext (3 spaces)
    b'SYSTEM     ',   # "SYSTEM" + 2 spaces + no ext (3 spaces)
    b'IOSUBSYS   ',   # "IOSUBSYS" + 0 spaces + no ext (3 spaces)
]


def cluster_to_offset(cluster):
    """Convert FAT32 cluster number to disk byte offset."""
    return DATA_START + (cluster - 2) * CLUSTER_SIZE


def fat_entry_offset(cluster):
    """Return the byte offset in the disk image for a FAT entry."""
    return FAT_OFFSET + cluster * 4


def read_fat_entry(f, cluster):
    """Read a single FAT32 entry for the given cluster."""
    f.seek(fat_entry_offset(cluster))
    raw = f.read(4)
    return struct.unpack('<I', raw)[0] & 0x0FFFFFFF


def get_cluster_chain(f, start_cluster):
    """Follow the FAT chain from start_cluster, return list of clusters."""
    chain = []
    c = start_cluster
    while c >= 2 and c < 0x0FFFFFF8:
        chain.append(c)
        c = read_fat_entry(f, c)
        if len(chain) > 100000:
            raise RuntimeError("FAT chain exceeds 100000 clusters, likely corrupt")
    return chain


def read_directory(f, start_cluster):
    """Read all 32-byte directory entries from a directory starting at start_cluster.

    Returns list of (entry_bytes, disk_offset) tuples for each entry.
    Follows the FAT chain for multi-cluster directories.
    """
    chain = get_cluster_chain(f, start_cluster)
    entries = []
    for c in chain:
        offset = cluster_to_offset(c)
        f.seek(offset)
        data = f.read(CLUSTER_SIZE)
        for i in range(0, CLUSTER_SIZE, 32):
            entry = data[i:i + 32]
            if len(entry) < 32:
                break
            entries.append((entry, offset + i))
            # First byte 0x00 means no more entries
            if entry[0] == 0x00:
                return entries
    return entries


def parse_entry_name(entry):
    """Extract the 11-byte 8.3 name from a directory entry."""
    return entry[0:11]


def parse_entry_cluster(entry):
    """Extract the starting cluster from a directory entry."""
    hi = struct.unpack_from('<H', entry, 20)[0]
    lo = struct.unpack_from('<H', entry, 26)[0]
    return (hi << 16) | lo


def parse_entry_size(entry):
    """Extract the file size from a directory entry."""
    return struct.unpack_from('<I', entry, 28)[0]


def is_lfn_entry(entry):
    """Check if this is a Long File Name entry."""
    return (entry[11] & 0x0F) == 0x0F


def is_deleted(entry):
    """Check if this entry is deleted."""
    return entry[0] == 0xE5


def is_end(entry):
    """Check if this is the end-of-directory marker."""
    return entry[0] == 0x00


def find_entry_in_dir(f, dir_cluster, target_name_83):
    """Search a directory for an 8.3 name match.

    Returns (entry_bytes, disk_offset) or (None, None).
    """
    entries = read_directory(f, dir_cluster)
    for entry, offset in entries:
        if is_end(entry):
            break
        if is_deleted(entry) or is_lfn_entry(entry):
            continue
        name = parse_entry_name(entry)
        if name == target_name_83:
            return entry, offset
    return None, None


def traverse_to_iosubsys(f):
    """Traverse root -> WINDOWS -> SYSTEM -> IOSUBSYS, return the IOSUBSYS cluster.

    Raises RuntimeError if any directory in the path is not found.
    """
    current_cluster = ROOT_CLUSTER
    path_so_far = "root"

    for target_name in DIR_PATH_83:
        entry, offset = find_entry_in_dir(f, current_cluster, target_name)
        if entry is None:
            readable = target_name.decode('ascii', errors='replace').strip()
            raise RuntimeError(
                f"Directory '{readable}' not found in {path_so_far}"
            )
        readable = target_name.decode('ascii', errors='replace').strip()
        cluster = parse_entry_cluster(entry)
        print(f"  Found '{readable}' at cluster {cluster} (dir entry @ 0x{offset:X})")
        current_cluster = cluster
        path_so_far += f"/{readable}"

    return current_cluster


def find_free_entry(f, dir_cluster):
    """Find a free directory entry slot (first byte 0x00 or 0xE5).

    Returns (disk_offset, is_end_marker) where is_end_marker indicates
    whether we need to write a new end-of-directory marker after.
    """
    entries = read_directory(f, dir_cluster)
    for entry, offset in entries:
        if is_end(entry):
            return offset, True
        if is_deleted(entry):
            return offset, False
    raise RuntimeError("No free directory entry found (directory full)")


def build_dir_entry(name_83, first_cluster, file_size, attr=0x20):
    """Build a 32-byte FAT32 directory entry.

    attr=0x20 is the Archive attribute.
    """
    entry = bytearray(32)
    # Name (11 bytes)
    entry[0:11] = name_83
    # Attributes
    entry[11] = attr
    # Reserved / NT case flags
    entry[12] = 0
    # Creation time tenths
    entry[13] = 0
    # Creation time (00:00:00)
    struct.pack_into('<H', entry, 14, 0)
    # Creation date (2026-03-22 = ((2026-1980)<<9 | 3<<5 | 22) = (46<<9 | 3<<5 | 22))
    date_val = (46 << 9) | (3 << 5) | 22
    struct.pack_into('<H', entry, 16, date_val)
    # Last access date
    struct.pack_into('<H', entry, 18, date_val)
    # First cluster high
    struct.pack_into('<H', entry, 20, (first_cluster >> 16) & 0xFFFF)
    # Last write time (00:00:00)
    struct.pack_into('<H', entry, 22, 0)
    # Last write date
    struct.pack_into('<H', entry, 24, date_val)
    # First cluster low
    struct.pack_into('<H', entry, 26, first_cluster & 0xFFFF)
    # File size
    struct.pack_into('<I', entry, 28, file_size)
    return bytes(entry)


def deploy_vxd(vxd_path):
    """Deploy a VxD file to WINDOWS\\SYSTEM\\IOSUBSYS on the disk image."""
    print("=" * 60)
    print("Deploy VxD to IOSUBSYS")
    print("=" * 60)

    # Read VxD file
    if not os.path.exists(vxd_path):
        print(f"ERROR: VxD file not found: {vxd_path}")
        return False

    with open(vxd_path, 'rb') as f:
        vxd_data = f.read()

    file_size = len(vxd_data)
    print(f"  VxD file: {vxd_path} ({file_size} bytes)")

    if file_size == 0:
        print("ERROR: VxD file is empty")
        return False

    # Verify MZ header
    if vxd_data[:2] != b'MZ':
        print(f"WARNING: VxD does not start with MZ header (got {vxd_data[:2].hex()})")
        print("  Proceeding anyway...")

    if not os.path.exists(DISK):
        print(f"ERROR: Disk image not found: {DISK}")
        return False

    with open(DISK, 'r+b') as f:
        # ── Step 0: Auto-detect FAT32 geometry ──
        print("\n--- Detecting FAT32 geometry ---")
        detect_fat32_geometry(f)

        clusters_needed = (file_size + CLUSTER_SIZE - 1) // CLUSTER_SIZE
        print(f"  Need {clusters_needed} clusters for {file_size} bytes")

        # ── Step 1: Traverse to IOSUBSYS ──
        print("\n--- Traversing directory tree ---")
        try:
            iosubsys_cluster = traverse_to_iosubsys(f)
        except RuntimeError as e:
            print(f"ERROR: {e}")
            return False
        print(f"  IOSUBSYS directory at cluster {iosubsys_cluster}")

        # ── Step 2: Check for existing entry ──
        print("\n--- Checking for existing NTMINI.PDR entry ---")
        existing_entry, existing_offset = find_entry_in_dir(
            f, iosubsys_cluster, TARGET_83NAME
        )

        if existing_entry is not None:
            old_cluster = parse_entry_cluster(existing_entry)
            old_size = parse_entry_size(existing_entry)
            print(f"  Found existing entry at 0x{existing_offset:X}")
            print(f"    Old cluster: {old_cluster}, old size: {old_size}")
            entry_offset = existing_offset
            # Free old cluster chain
            if old_cluster >= 2:
                print("  Freeing old cluster chain...")
                c = old_cluster
                freed = 0
                while c >= 2 and c < 0x0FFFFFF8:
                    next_c = read_fat_entry(f, c)
                    write_fat_entry(f, c, 0)  # mark free
                    freed += 1
                    c = next_c
                print(f"  Freed {freed} clusters")
        else:
            print("  No existing entry found, creating new one")
            entry_offset, is_end = find_free_entry(f, iosubsys_cluster)
            print(f"  Using free slot at 0x{entry_offset:X}")

            # If we consumed the end-of-directory marker, write a new one after
            if is_end:
                next_offset = entry_offset + 32
                f.seek(next_offset)
                existing_byte = f.read(1)
                # Only write end marker if next slot isn't already 0x00
                if existing_byte and existing_byte[0] != 0x00:
                    f.seek(next_offset)
                    f.write(b'\x00' * 32)
                    print(f"  Wrote end-of-directory marker at 0x{next_offset:X}")

        # ── Step 3: Allocate clusters and write VxD data ──
        print("\n--- Allocating clusters ---")
        allocate_clusters(f, clusters_needed)
        print("\n--- Writing VxD data to clusters ---")
        for i, c in enumerate(VXD_CLUSTERS):
            chunk_start = i * CLUSTER_SIZE
            chunk_end = min((i + 1) * CLUSTER_SIZE, file_size)

            if chunk_start >= file_size:
                # Past end of file, write zeros to fill remaining cluster
                chunk = b'\x00' * CLUSTER_SIZE
            else:
                chunk = vxd_data[chunk_start:chunk_end]
                # Pad to cluster size if needed
                if len(chunk) < CLUSTER_SIZE:
                    chunk = chunk + b'\x00' * (CLUSTER_SIZE - len(chunk))

            offset = cluster_to_offset(c)
            f.seek(offset)
            f.write(chunk)
            if chunk_start < file_size:
                written = min(CLUSTER_SIZE, file_size - chunk_start)
                print(f"  Cluster {c} @ 0x{offset:X}: {written} bytes data"
                      + (" + padding" if written < CLUSTER_SIZE else ""))
            else:
                print(f"  Cluster {c} @ 0x{offset:X}: zeroed (past EOF)")

        # ── Step 4: Write directory entry ──
        print("\n--- Writing directory entry ---")
        new_entry = build_dir_entry(TARGET_83NAME, VXD_CLUSTERS[0], file_size)
        f.seek(entry_offset)
        f.write(new_entry)
        print(f"  Entry at 0x{entry_offset:X}:")
        print(f"    Name: {TARGET_83NAME.decode('ascii')}")
        print(f"    Attr: 0x20 (Archive)")
        print(f"    First cluster: {VXD_CLUSTERS[0]} (hi=0x{VXD_CLUSTERS[0]>>16:04X} lo=0x{VXD_CLUSTERS[0]&0xFFFF:04X})")
        print(f"    File size: {file_size}")

        # ── Step 5: Clear FAT dirty flags ──
        print("\n--- Clearing FAT dirty flags ---")
        for fat_num in range(NFATS):
            fat_base = (PARTITION_LBA + RESERVED + fat_num * FAT32SZ) * BPS
            # Cluster 1 entry is at offset 4 (cluster 0 is at offset 0)
            clus1_offset = fat_base + 4
            f.seek(clus1_offset)
            raw = f.read(4)
            val = struct.unpack('<I', raw)[0]
            # Set bits 27 and 28 to mark as clean
            clean_val = val | (1 << 27) | (1 << 28)
            if clean_val != val:
                f.seek(clus1_offset)
                f.write(struct.pack('<I', clean_val))
                print(f"  FAT{fat_num + 1} cluster 1: 0x{val:08X} -> 0x{clean_val:08X} (dirty flags cleared)")
            else:
                print(f"  FAT{fat_num + 1} cluster 1: 0x{val:08X} (already clean)")

        # ── Step 6: Verify ──
        print("\n--- Verification ---")
        f.seek(entry_offset)
        verify_entry = f.read(32)
        v_name = verify_entry[0:11]
        v_attr = verify_entry[11]
        v_hi = struct.unpack_from('<H', verify_entry, 20)[0]
        v_lo = struct.unpack_from('<H', verify_entry, 26)[0]
        v_cluster = (v_hi << 16) | v_lo
        v_size = struct.unpack_from('<I', verify_entry, 28)[0]
        print(f"  Entry name: '{v_name.decode('ascii', errors='replace')}'")
        print(f"  Attr: 0x{v_attr:02X}")
        print(f"  First cluster: {v_cluster}")
        print(f"  Size: {v_size}")

        # Verify first bytes of written data
        f.seek(cluster_to_offset(VXD_CLUSTERS[0]))
        header = f.read(4)
        print(f"  First 4 bytes on disk: {header.hex()}")
        print(f"  First 4 bytes of VxD:  {vxd_data[:4].hex()}")
        if header == vxd_data[:4]:
            print("  [OK] Data matches")
        else:
            print("  [ERROR] Data mismatch!")
            return False

    print("\n" + "=" * 60)
    print("Deployment complete.")
    print(f"  NTMINI.PDR deployed to WINDOWS\\SYSTEM\\IOSUBSYS")
    print(f"  {file_size} bytes across {len(VXD_CLUSTERS)} clusters")
    print("=" * 60)
    return True


def remove_device_from_sysini():
    """Remove 'device=ntmini.vxd' line from SYSTEM.INI on the disk image.

    Searches the disk for the string and replaces it with spaces to preserve
    file size and FAT cluster layout.
    """
    print("\n" + "=" * 60)
    print("Remove device=ntmini.vxd from SYSTEM.INI")
    print("=" * 60)

    if not os.path.exists(DISK):
        print(f"ERROR: Disk image not found: {DISK}")
        return False

    # Patterns to search for (case variations)
    patterns = [
        b'device=ntmini.vxd',
        b'DEVICE=NTMINI.VXD',
        b'Device=ntmini.vxd',
        b'device=NTMINI.VXD',
    ]

    found_count = 0
    disk_size = os.path.getsize(DISK)
    chunk_size = 4 * 1024 * 1024  # 4 MB chunks

    with open(DISK, 'r+b') as f:
        offset = 0
        while offset < disk_size:
            f.seek(offset)
            data = f.read(chunk_size + 64)  # overlap to catch boundary matches
            if not data:
                break

            for pattern in patterns:
                search_start = 0
                while True:
                    pos = data.find(pattern, search_start)
                    if pos == -1:
                        break

                    abs_offset = offset + pos
                    # Replace with spaces (same length preserves file size)
                    replacement = b' ' * len(pattern)
                    f.seek(abs_offset)
                    f.write(replacement)
                    found_count += 1
                    print(f"  Replaced '{pattern.decode()}' at offset 0x{abs_offset:X} with spaces")
                    search_start = pos + len(pattern)

            offset += chunk_size

    if found_count == 0:
        print("  No 'device=ntmini.vxd' entries found (already clean or not present)")
    else:
        print(f"  Replaced {found_count} occurrence(s)")

    return True


def main():
    global DISK
    remove_sysini = '--remove-sysini' in sys.argv

    # Parse --disk argument
    argv = sys.argv[1:]
    if '--disk' in argv:
        idx = argv.index('--disk')
        if idx + 1 < len(argv):
            DISK = argv[idx + 1]
            argv = argv[:idx] + argv[idx + 2:]
        else:
            print("ERROR: --disk requires a path argument")
            sys.exit(1)

    args = [a for a in argv if not a.startswith('--')]

    if not args and not remove_sysini:
        print("Usage: python3 deploy_to_iosubsys.py <VXD_FILE> [--disk /path/to/img] [--remove-sysini]")
        print("       python3 deploy_to_iosubsys.py --remove-sysini")
        sys.exit(1)

    if args:
        vxd_path = args[0]
        # If not an absolute path, look relative to script directory
        if not os.path.isabs(vxd_path):
            vxd_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), vxd_path)

        if not deploy_vxd(vxd_path):
            sys.exit(1)

    if remove_sysini:
        if not remove_device_from_sysini():
            sys.exit(1)

    print("\nDone.")


if __name__ == '__main__':
    main()
