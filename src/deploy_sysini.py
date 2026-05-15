#!/usr/bin/env python3
"""
deploy_sysini.py - Deploy VxD via SYSTEM.INI or IOSUBSYS.

Default mode: deploys to WINDOWS/SYSTEM/NTMINI.VXD and adds device=NTMINI.VXD
to the [386Enh] section of SYSTEM.INI.

PDR mode (--pdr): deploys to WINDOWS/SYSTEM/IOSUBSYS/NTMINI.PDR instead.
This loads during IOS init, before ESDI_506 claims devices.

Also removes any existing NECATAPI.VXD from IOSUBSYS in both modes.

Usage:
  python3 deploy_sysini.py <VXD_FILE>                  # SYSTEM.INI mode
  python3 deploy_sysini.py <VXD_FILE> --pdr             # IOSUBSYS PDR mode
  python3 deploy_sysini.py <VXD_FILE> --suppress-esdi   # Also remove ESDI_506.PDR
"""
import struct
import sys
import os

PERSISTENT_VM_DIR = os.path.expanduser("~/Documents/VMs/win98vm")
DEFAULT_DISK = "/tmp/win98vm/win98.img"
REPO_DIR = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def resolve_disk_path():
    env_disk = os.environ.get("WIN98_IMG")
    candidates = [
        env_disk,
        DEFAULT_DISK,
        "/tmp/win98vm/win98_nogeom.img",
        os.path.join(PERSISTENT_VM_DIR, "win98.img"),
        os.path.join(PERSISTENT_VM_DIR, "win98_nogeom.img"),
    ]

    for path in candidates:
        if path and os.path.exists(path):
            return path

    return env_disk or DEFAULT_DISK


def resolve_vxd_input_path(vxd_path):
    """Resolve a VxD path from cwd, repo root, or the script directory."""
    if os.path.isabs(vxd_path):
        return vxd_path

    candidates = [
        os.path.abspath(vxd_path),
        os.path.join(REPO_DIR, vxd_path),
        os.path.join(SCRIPT_DIR, vxd_path),
    ]

    for path in candidates:
        if path and os.path.exists(path):
            return path

    return os.path.abspath(vxd_path)


DISK = resolve_disk_path()
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# Import FAT32 functions from deploy_to_iosubsys
sys.path.insert(0, SCRIPT_DIR)
import deploy_to_iosubsys as fat

def read_file_content(f, start_cluster):
    """Read entire file content following FAT chain."""
    chain = fat.get_cluster_chain(f, start_cluster)
    content = bytearray()
    for c in chain:
        offset = fat.cluster_to_offset(c)
        f.seek(offset)
        content.extend(f.read(fat.CLUSTER_SIZE))
    return content

def write_file_content(f, start_cluster, data, max_clusters=None):
    """Write data to existing cluster chain. Returns True if it fit."""
    chain = fat.get_cluster_chain(f, start_cluster)
    if max_clusters:
        chain = chain[:max_clusters]
    total_space = len(chain) * fat.CLUSTER_SIZE
    if len(data) > total_space:
        return False
    pos = 0
    for c in chain:
        offset = fat.cluster_to_offset(c)
        chunk = data[pos:pos + fat.CLUSTER_SIZE]
        if len(chunk) < fat.CLUSTER_SIZE:
            chunk = chunk + b'\x00' * (fat.CLUSTER_SIZE - len(chunk))
        f.seek(offset)
        f.write(chunk)
        pos += fat.CLUSTER_SIZE
    return True

def find_dir(f, parent_cluster, name_83):
    """Find a directory entry and return its cluster."""
    entry, offset = fat.find_entry_in_dir(f, parent_cluster, name_83)
    if entry is None:
        return None, None
    return fat.parse_entry_cluster(entry), offset

def deploy_to_system_dir(f, vxd_data, vxd_name_83=b'NTMINI  VXD'):
    """Deploy VxD to WINDOWS SYSTEM directory."""
    print("\n--- Deploy to WINDOWS\\SYSTEM\\ ---")

    # Navigate to WINDOWS\SYSTEM
    win_cluster, _ = find_dir(f, fat.ROOT_CLUSTER, b'WINDOWS    ')
    if not win_cluster:
        print("ERROR: WINDOWS directory not found")
        return False
    print(f"  WINDOWS at cluster {win_cluster}")

    sys_cluster, _ = find_dir(f, win_cluster, b'SYSTEM     ')
    if not sys_cluster:
        print("ERROR: SYSTEM directory not found")
        return False
    print(f"  SYSTEM at cluster {sys_cluster}")

    file_size = len(vxd_data)
    clusters_needed = (file_size + fat.CLUSTER_SIZE - 1) // fat.CLUSTER_SIZE

    # Check for existing entry
    existing_entry, existing_offset = fat.find_entry_in_dir(f, sys_cluster, vxd_name_83)

    if existing_entry is not None:
        old_cluster = fat.parse_entry_cluster(existing_entry)
        old_size = fat.parse_entry_size(existing_entry)
        print(f"  Found existing {vxd_name_83.decode().strip()} at 0x{existing_offset:X} (cluster {old_cluster}, size {old_size})")
        entry_offset = existing_offset
        # Free old clusters
        if old_cluster >= 2:
            c = old_cluster
            freed = 0
            while c >= 2 and c < 0x0FFFFFF8:
                next_c = fat.read_fat_entry(f, c)
                fat.write_fat_entry(f, c, 0)
                freed += 1
                c = next_c
            print(f"  Freed {freed} old clusters")
    else:
        print(f"  No existing entry, creating new")
        entry_offset, is_end = fat.find_free_entry(f, sys_cluster)
        if is_end:
            next_off = entry_offset + 32
            f.seek(next_off)
            b = f.read(1)
            if b and b[0] != 0x00:
                f.seek(next_off)
                f.write(b'\x00' * 32)

    # Allocate clusters and write data
    clusters = fat.allocate_clusters(f, clusters_needed)
    for i, c in enumerate(clusters):
        chunk_start = i * fat.CLUSTER_SIZE
        chunk = vxd_data[chunk_start:chunk_start + fat.CLUSTER_SIZE]
        if len(chunk) < fat.CLUSTER_SIZE:
            chunk = chunk + b'\x00' * (fat.CLUSTER_SIZE - len(chunk))
        offset = fat.cluster_to_offset(c)
        f.seek(offset)
        f.write(chunk)

    # Write directory entry
    new_entry = fat.build_dir_entry(vxd_name_83, clusters[0], file_size)
    f.seek(entry_offset)
    f.write(new_entry)
    print(f"  Deployed {file_size} bytes as {vxd_name_83.decode().strip()} ({len(clusters)} clusters)")
    return True


def deploy_to_iosubsys(f, vxd_data):
    """Deploy VxD as NTMINI.PDR to WINDOWS\\SYSTEM\\IOSUBSYS directory.

    PDRs in IOSUBSYS load during IOS initialization, before ESDI_506
    claims IDE devices. This solves the timing problem where our driver
    needs to register before the default IDE driver.
    """
    print("\n--- Deploy to WINDOWS\\SYSTEM\\IOSUBSYS\\ as NTMINI.PDR ---")

    # Traverse to IOSUBSYS
    try:
        iosubsys_cluster = fat.traverse_to_iosubsys(f)
    except RuntimeError as e:
        print(f"  ERROR: {e}")
        return False

    pdr_name_83 = b'NTMINI  PDR'
    file_size = len(vxd_data)
    clusters_needed = (file_size + fat.CLUSTER_SIZE - 1) // fat.CLUSTER_SIZE

    # Check for existing NTMINI.PDR entry
    existing_entry, existing_offset = fat.find_entry_in_dir(f, iosubsys_cluster, pdr_name_83)

    if existing_entry is not None:
        old_cluster = fat.parse_entry_cluster(existing_entry)
        old_size = fat.parse_entry_size(existing_entry)
        print(f"  Found existing NTMINI.PDR at 0x{existing_offset:X} (cluster {old_cluster}, size {old_size})")
        entry_offset = existing_offset
        # Free old clusters
        if old_cluster >= 2:
            c = old_cluster
            freed = 0
            while c >= 2 and c < 0x0FFFFFF8:
                next_c = fat.read_fat_entry(f, c)
                fat.write_fat_entry(f, c, 0)
                freed += 1
                c = next_c
            print(f"  Freed {freed} old clusters")
    else:
        print(f"  No existing NTMINI.PDR, creating new entry")
        entry_offset, is_end = fat.find_free_entry(f, iosubsys_cluster)
        if is_end:
            next_off = entry_offset + 32
            f.seek(next_off)
            b = f.read(1)
            if b and b[0] != 0x00:
                f.seek(next_off)
                f.write(b'\x00' * 32)

    # Allocate clusters and write data
    clusters = fat.allocate_clusters(f, clusters_needed)
    for i, c in enumerate(clusters):
        chunk_start = i * fat.CLUSTER_SIZE
        chunk = vxd_data[chunk_start:chunk_start + fat.CLUSTER_SIZE]
        if len(chunk) < fat.CLUSTER_SIZE:
            chunk = chunk + b'\x00' * (fat.CLUSTER_SIZE - len(chunk))
        offset = fat.cluster_to_offset(c)
        f.seek(offset)
        f.write(chunk)

    # Write directory entry
    new_entry = fat.build_dir_entry(pdr_name_83, clusters[0], file_size)
    f.seek(entry_offset)
    f.write(new_entry)
    print(f"  Deployed {file_size} bytes as NTMINI.PDR ({len(clusters)} clusters)")
    return True


def edit_system_ini(f):
    """Add device=NTMINI.VXD to [386Enh] in SYSTEM.INI."""
    print("\n--- Edit SYSTEM.INI ---")

    # Navigate to WINDOWS directory
    win_cluster, _ = find_dir(f, fat.ROOT_CLUSTER, b'WINDOWS    ')
    if not win_cluster:
        print("ERROR: WINDOWS not found")
        return False

    # Find SYSTEM.INI
    sysini_entry, sysini_offset = fat.find_entry_in_dir(f, win_cluster, b'SYSTEM  INI')
    if sysini_entry is None:
        print("ERROR: SYSTEM.INI not found")
        return False

    sysini_cluster = fat.parse_entry_cluster(sysini_entry)
    sysini_size = fat.parse_entry_size(sysini_entry)
    print(f"  SYSTEM.INI at cluster {sysini_cluster}, size {sysini_size}")

    # Read content
    raw = read_file_content(f, sysini_cluster)
    content = raw[:sysini_size].decode('ascii', errors='replace')

    # Check if already present (case-insensitive, match any path containing NTMINI)
    device_line = 'device=C:\\WINDOWS\\SYSTEM\\NTMINI.VXD'
    lines = content.split('\r\n')
    for line in lines:
        if 'device=' in line.lower() and 'ntmini' in line.lower():
            print(f"  NTMINI device= already present in SYSTEM.INI: '{line.strip()}'")
            return True

    # Find [386Enh] section and insert after the header
    insert_idx = None
    for i, line in enumerate(lines):
        if line.strip().lower() == '[386enh]':
            insert_idx = i + 1
            break

    if insert_idx is None:
        print("ERROR: [386Enh] section not found in SYSTEM.INI")
        return False

    # Insert the device= line (full path form)
    lines.insert(insert_idx, device_line)
    new_content = '\r\n'.join(lines)
    new_bytes = new_content.encode('ascii')
    new_size = len(new_bytes)

    print(f"  Original size: {sysini_size}, new size: {new_size}")

    # Check if it fits in existing clusters
    chain = fat.get_cluster_chain(f, sysini_cluster)
    total_space = len(chain) * fat.CLUSTER_SIZE
    if new_size > total_space:
        print(f"  ERROR: New content ({new_size}) exceeds cluster space ({total_space})")
        print("  Need to allocate more clusters (not implemented)")
        return False

    # Write back
    if not write_file_content(f, sysini_cluster, new_bytes):
        print("  ERROR: Failed to write")
        return False

    # Update file size in directory entry
    f.seek(sysini_offset + 28)
    f.write(struct.pack('<I', new_size))
    print(f"  Updated SYSTEM.INI size to {new_size}")
    print(f"  Added '{device_line}' to [386Enh] section")
    return True


def edit_msdos_sys(f):
    """Normalize MSDOS.SYS boot options for unattended normal-mode boots.

    This Win98 image reports BOOTLOG=1 as an invalid MSDOS.SYS setting during
    early boot. Debug capture comes from QEMU debugcon, so forcing BOOTLOG here
    is unnecessary and perturbs timing. Failed crash runs also leave the image
    prone to the startup menu or Safe Mode path, so force the normal GUI boot
    path here instead of relying on QEMU key injection.
    """
    print("\n--- Edit MSDOS.SYS ---")

    entry, offset = fat.find_entry_in_dir(f, fat.ROOT_CLUSTER, b'MSDOS   SYS')
    if entry is None:
        print("  MSDOS.SYS not found in root directory")
        return False

    start_cluster = fat.parse_entry_cluster(entry)
    file_size = fat.parse_entry_size(entry)
    raw = read_file_content(f, start_cluster)
    content = raw[:file_size].decode('ascii', errors='replace')
    newline = '\r\n' if '\r\n' in content else '\n'

    lines = content.splitlines()
    new_lines = []
    options_idx = None
    seen_options = {}
    forced_options = {
        'autoscan': 'AutoScan=0',
        'bootgui': 'BootGUI=1',
        'bootmenu': 'BootMenu=0',
        'bootmenudefault': 'BootMenuDefault=1',
        'bootmenudelay': 'BootMenuDelay=0',
        'bootwarn': 'BootWarn=0',
        'bootsafe': 'BootSafe=0',
    }

    for line in lines:
        stripped = line.strip()
        lower = stripped.lower()
        if lower.startswith('bootlog='):
            continue

        if lower == '[options]':
            options_idx = len(new_lines)
            new_lines.append(line)
            continue

        key = lower.split('=', 1)[0].strip() if '=' in lower else None
        if key in forced_options:
            if key not in seen_options:
                seen_options[key] = len(new_lines)
                new_lines.append(forced_options[key])
            continue

        new_lines.append(line)

    if options_idx is None:
        if new_lines and new_lines[-1].strip():
            new_lines.append('')
        options_idx = len(new_lines)
        new_lines.append('[Options]')

    insert_idx = options_idx + 1
    for key, line in forced_options.items():
        if key not in seen_options:
            new_lines.insert(insert_idx, line)
            insert_idx += 1

    new_content = newline.join(new_lines)
    if content.endswith('\r\n') or content.endswith('\n'):
        new_content += newline

    if new_content == content:
        print("  MSDOS.SYS boot options already normalized")
        return True

    new_bytes = new_content.encode('ascii', errors='replace')
    print(f"  MSDOS.SYS at cluster {start_cluster}, size {file_size}")

    if not write_file_content(f, start_cluster, new_bytes):
        print("  ERROR: Updated MSDOS.SYS does not fit existing cluster chain")
        return False

    f.seek(offset + 28)
    f.write(struct.pack('<I', len(new_bytes)))
    print(f"  Updated MSDOS.SYS size to {len(new_bytes)}")
    print("  Normalized MSDOS.SYS boot options")
    return True

def remove_from_iosubsys(f):
    """Remove NECATAPI.VXD from IOSUBSYS by marking its dir entry as deleted."""
    print("\n--- Remove NECATAPI from IOSUBSYS ---")
    try:
        iosubsys = fat.traverse_to_iosubsys(f)
    except RuntimeError as e:
        print(f"  ERROR: {e}")
        return False

    entry, offset = fat.find_entry_in_dir(f, iosubsys, b'NECATAPIVXD')
    if entry is None:
        print("  No NECATAPI.VXD found (already clean)")
        return True

    old_cluster = fat.parse_entry_cluster(entry)
    # Free cluster chain
    c = old_cluster
    freed = 0
    while c >= 2 and c < 0x0FFFFFF8:
        next_c = fat.read_fat_entry(f, c)
        fat.write_fat_entry(f, c, 0)
        freed += 1
        c = next_c

    # Mark directory entry as deleted
    f.seek(offset)
    f.write(b'\xE5')
    print(f"  Deleted NECATAPI.VXD (freed {freed} clusters)")
    return True

def suppress_esdi_506(f):
    """Suppress ESDI_506.PDR from IOSUBSYS by marking its dir entry as deleted.

    WARNING: This prevents Win98's built-in IDE driver from loading.
    The boot disk (C:) uses a separate code path (INT 13h) and is not
    affected, but any IDE hard disks that ESDI_506 would normally claim
    will become available for our bridge driver.

    Only call this when the NT5 bridge needs to own HDD devices.
    """
    print("\n--- Suppress ESDI_506.PDR from IOSUBSYS ---")
    try:
        iosubsys = fat.traverse_to_iosubsys(f)
    except RuntimeError as e:
        print(f"  ERROR: {e}")
        return False

    entry, offset = fat.find_entry_in_dir(f, iosubsys, b'ESDI_506PDR')
    if entry is None:
        print("  No ESDI_506.PDR found (already suppressed)")
        return True

    old_cluster = fat.parse_entry_cluster(entry)
    c = old_cluster
    freed = 0
    while c >= 2 and c < 0x0FFFFFF8:
        next_c = fat.read_fat_entry(f, c)
        fat.write_fat_entry(f, c, 0)
        freed += 1
        c = next_c

    f.seek(offset)
    f.write(b'\xE5')
    print(f"  Deleted ESDI_506.PDR (freed {freed} clusters)")
    return True


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 deploy_sysini.py <VXD_FILE> [--pdr] [--suppress-esdi]")
        sys.exit(1)

    pdr_mode = '--pdr' in sys.argv
    suppress_esdi = '--suppress-esdi' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    vxd_path = args[0]
    vxd_path = resolve_vxd_input_path(vxd_path)

    vxd_data = open(vxd_path, 'rb').read()
    print(f"VxD: {vxd_path} ({len(vxd_data)} bytes)")
    if pdr_mode:
        print("Mode: PDR (IOSUBSYS, loads during IOS init)")
    else:
        print("Mode: SYSTEM.INI (loads during VMM init)")

    with open(DISK, 'r+b') as f:
        print("\n--- FAT32 geometry ---")
        fat.detect_fat32_geometry(f)

        if pdr_mode:
            deploy_to_iosubsys(f, vxd_data)
        else:
            deploy_to_system_dir(f, vxd_data)
            edit_system_ini(f)

        edit_msdos_sys(f)
        remove_from_iosubsys(f)
        if suppress_esdi:
            suppress_esdi_506(f)

        # Clear FAT dirty flags
        print("\n--- Clear FAT dirty flags ---")
        part_byte = fat.PARTITION_LBA * fat.BPS
        for fat_num in range(fat.NFATS):
            fat_base = (fat.PARTITION_LBA + fat.RESERVED + fat_num * fat.FAT32SZ) * fat.BPS
            f.seek(fat_base)
            e0 = struct.unpack('<I', f.read(4))[0]
            e1 = struct.unpack('<I', f.read(4))[0]
            fixed = False
            if e0 != 0x0FFFFFF8:
                f.seek(fat_base)
                f.write(struct.pack('<I', 0x0FFFFFF8))
                fixed = True
            if (e1 & 0x0FFFFFFF) != 0x0FFFFFFF:
                f.seek(fat_base + 4)
                f.write(struct.pack('<I', 0x0FFFFFFF))
                fixed = True
            elif (e1 & 0x0C000000) != 0x0C000000:
                f.seek(fat_base + 4)
                f.write(struct.pack('<I', e1 | 0x0C000000))
                fixed = True
            if fixed:
                print(f"  Fixed FAT{fat_num+1} entries")
        f.seek(part_byte + 0x41)
        bpb_dirty = f.read(1)[0]
        if bpb_dirty != 0x00:
            f.seek(part_byte + 0x41)
            f.write(bytes([0x00]))
            print(f"  Fixed BPB dirty byte (was 0x{bpb_dirty:02x})")

    if pdr_mode:
        print("\nDone. PDR will load from IOSUBSYS during IOS init on next boot.")
    else:
        print("\nDone. VxD will load via SYSTEM.INI [386Enh] device= on next boot.")

if __name__ == '__main__':
    main()
