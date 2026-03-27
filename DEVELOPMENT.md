# Development Guide

## 1. Environment Setup

### Required Software

**Python 3** with the `capstone` disassembly library:

```
pip install capstone
```

**QEMU** (i386 system emulation):

```
brew install qemu
```

**Open Watcom v2** for compiling VxD C and ASM source. Download from
https://github.com/open-watcom/open-watcom-v2 and configure include paths to
point at the Win98 DDK VxD headers (VMM.H, VPICD.H, IOS.H, IOR.H, BLOCKDEV.H).
The DDK header stubs live in `reference/inc/`.

**Windows 98 SE disk image** in raw format, FAT32 partitioned. Set the
environment variable before running any script:

```
export WIN98_IMG=/path/to/win98.img
```

All scripts default to `/tmp/win98vm/win98.img` when the variable is unset.


## 2. Build Workflow

The pipeline reads a compiled VxD binary, corrects its LE (Linear Executable)
header fields, deploys the result to the disk image, then boots QEMU.

### Step 1: Compile C/ASM source with Open Watcom

The historical source files live in `history/source/`. NTMINI.C (or the latest
NTMINI_V5.C) is the main driver. VXDWRAP.ASM provides the DDB and control
procedure boilerplate. The linker scripts (`link.lnk`, `link_v5.lnk`) specify
the VxD target. See `reference/BUILD.TXT` for exact compiler flags.

The compiled output is a raw LE binary (e.g. `V5SMALL.VXD`), stored in
`binaries/`.

### Step 2: Correct the LE structure

```
python3 src/build_sysini_fixed.py [max_fixups] [output_name]
```

This reads `binaries/V5SMALL.VXD`, merges all objects into a single flat
object, translates fixup records, and writes a corrected LE binary. Key
corrections applied:

1. `data_pages_off` is written as a file absolute offset, not LE relative.
2. Loader tables (object table, page map, resident names, entry table) are
   packed contiguously, followed by fixup tables (FPT, FRT).
3. `loader_section_size` reflects the actual byte span of the loader section.
4. Import table offsets point to valid (zero entry) locations.
5. Nonresident names table and extended LE fields (LE+0xB8, LE+0xBC, LE+0xC0)
   match the patterns found in Microsoft's own VxDs.

Default output: `src/V5FIXED4.VXD`.

### Step 3: Deploy to the disk image

Two deployment paths exist.

**IOSUBSYS deployment** (port driver mode, loaded by IOS):

```
python3 src/deploy_to_iosubsys.py src/V5FIXED4.VXD
```

This writes the file as `NTMINI.PDR` into `WINDOWS\SYSTEM\IOSUBSYS` on the
FAT32 image.

**SYSTEM.INI deployment** (static VxD mode, loaded by VMM):

```
python3 src/deploy_sysini.py src/V5FIXED4.VXD
```

This writes the file as `NTMINI.VXD` into `WINDOWS\SYSTEM`, adds
`device=C:\WINDOWS\SYSTEM\NTMINI.VXD` to the `[386Enh]` section of SYSTEM.INI,
and removes any conflicting NECATAPI.VXD from IOSUBSYS.

Both scripts auto detect FAT32 geometry from the MBR partition table and BPB.

### Step 4: Verify and launch QEMU

```
python3 src/verify_and_launch.py
```

This checks that MSDOS.SYS has `BootLog=1`, that SYSTEM.INI contains the
device entry, and that the deployed VxD has a valid LE structure. It then writes
a `launch_debug.sh` script and prints the QEMU command with debug channels:

```
qemu-system-i386 \
  -m 128 -M pc -cpu pentium \
  -drive file=${WIN98_IMG},format=raw \
  -boot c -vga std -rtc base=localtime \
  -display cocoa \
  -debugcon file:/tmp/vxd_debug.log \
  -serial file:/tmp/serial.log
```

Port 0xE9 writes from VxD code appear in `/tmp/vxd_debug.log`. COM1 serial
output appears in `/tmp/serial.log`.


## 3. Diagnostic Workflow

All diagnostic tools live in `diagnostics/`.

**compare_le_headers.py** compares two VxD files field by field across every
LE header offset, plus object tables, page maps, entry tables, and resident
names. Use this when a build fails to load and the reference VxD (ESDI_506.PDR)
loads fine:

```
python3 diagnostics/compare_le_headers.py src/V5FIXED4.VXD diagnostics/ESDI_506PDR.extracted
```

**dump_ddb.py** dumps the Device Descriptor Block from multiple VxD files,
showing DDB_Control_Proc, DDB_Name, DDB_Init_Order, and all other fields. Use
this to confirm the entry table offset points at the correct DDB.

**dump_fixups.py** prints every fixup record in a VxD, flagging anomalies such
as unexpected source types, bad object numbers, or cross boundary references.
Use this when the loader crashes during relocation.

**disasm_ctrl.py** disassembles the control procedure using capstone, annotating
VxDCall service IDs (VMM, IOS, VPICD, etc.) and VxD control message comparisons
(Sys_Critical_Init, Device_Init, etc.). Requires capstone; falls back to
ndisasm or hex dump otherwise.

**extract_reference_vxd.py** pulls a known working VxD from the disk image for
comparison. Defaults to ESDI_506.PDR:

```
python3 diagnostics/extract_reference_vxd.py ESDI_506PDR
```

**survey_vxd_headers.py** scans all VxD and PDR files in IOSUBSYS and SYSTEM,
printing a summary line per file: object count, page count, data_pages_off,
nonresident names offset, import table layout, and the reserved object table
field. Use this to identify common patterns across Microsoft's own drivers.

**check_extended_le.py** inspects the extended LE header fields at offsets
LE+0xB0 through LE+0xC0, comparing them against known working VxDs to verify
that the end of module data pointer (LE+0xB8) matches `nrn_off + nrn_len`.


## 4. Testing: The Edit, Rebuild, Redeploy, Boot Cycle

1. Edit the C or ASM source in `history/source/`.
2. Recompile with Open Watcom. Place the output in `binaries/`.
3. Run `python3 src/build_sysini_fixed.py` to produce the corrected LE binary.
4. Run the appropriate deploy script to write it to the disk image.
5. Boot QEMU via `src/launch_debug.sh` or the command from `verify_and_launch.py`.
6. After Win98 boots and shuts down, inspect `/tmp/vxd_debug.log` and
   `/tmp/serial.log` for debug output from the VxD.

To quickly neuter the control procedure for load testing (does the LE structure
parse correctly, ignoring driver logic?), use:

```
python3 src/neuter_ctrl.py src/V5FIXED4.VXD
```

This replaces `NTMINI_Control` with `CLC; RET` (return success for all
messages), producing a `_NEUTER.VXD` variant.


## 5. Key Gotchas

**data_pages_off is file absolute.** The Win98 VMM loader seeks to this offset
directly. Open Watcom emits it as LE relative. `build_sysini_fixed.py` corrects
this, but any manual binary patching must account for the MZ header size
(0x80 bytes) being included in the offset.

**LE field offsets are from the LE signature, not from the file start.** All
LE header offsets (object table, page map, FPT, FRT, entry table, resident
names) are relative to the LE header position. `data_pages_off` (LE+0x80) and
`nonresident_names_off` (LE+0x88) are the two exceptions: both are file
absolute.

**FAT32 cluster alignment matters.** When writing file data to the disk image,
each chunk must be padded to the full cluster size (typically 4096 bytes).
Short writes corrupt the FAT chain because subsequent clusters expect
contiguous data.

**FAT dirty flags must be cleared.** After modifying the FAT, set bits 27 and
28 in the cluster 1 entry of every FAT copy. Win98 may refuse to boot or run
SCANDISK if these flags indicate an unclean volume.

**LGN mask vs bit number.** The VxD module flags field (LE+0x10) uses bitmask
0x00038000 for the device driver flag set. Do not confuse individual bit numbers
with the combined mask value when checking or setting flags.

**DRP packing.** When the IOS subsystem loads a port driver (.PDR), it reads
the LE structure differently from the VMM static loader. The IOS loader
computes `data_pages_off` as `align_up(entry_table_offset, 32)` (LE relative),
ignoring the header field. If the actual page data does not sit at that
computed offset, IOS loading fails even though SYSTEM.INI loading succeeds.
`build_sysini_fixed.py` prints whether these two methods agree.

**The page map uses big endian page numbers.** Each 4 byte page map entry
stores the page number in the first 3 bytes, most significant byte first, with
the 4th byte as type flags. This is unusual for an otherwise little endian
format.


## 6. File Dependencies

```
src/build_sysini_fixed.py        reads binaries/V5SMALL.VXD, standalone
src/deploy_to_iosubsys.py        standalone, also importable as a module (fat)
src/deploy_sysini.py             imports deploy_to_iosubsys as fat
src/neuter_ctrl.py               standalone
src/verify_and_launch.py         standalone

diagnostics/compare_le_headers.py     standalone
diagnostics/disasm_ctrl.py            standalone, requires capstone
diagnostics/dump_ddb.py               standalone
diagnostics/dump_fixups.py            standalone
diagnostics/extract_reference_vxd.py  imports deploy_to_iosubsys as fat
diagnostics/survey_vxd_headers.py     imports deploy_to_iosubsys as fat
diagnostics/check_extended_le.py      imports deploy_to_iosubsys as fat
```

`deploy_to_iosubsys.py` serves double duty. When run directly, it deploys a
VxD to IOSUBSYS. When imported, it exposes FAT32 utility functions:
`detect_fat32_geometry()`, `find_entry_in_dir()`, `get_cluster_chain()`,
`cluster_to_offset()`, `parse_entry_cluster()`, `parse_entry_size()`,
`allocate_clusters()`, `build_dir_entry()`, and `read_fat_entry()`. Any script
that needs to read or write the disk image's file system imports this module.


## 7. Adding Support for New Miniport Drivers

To wrap a different NT miniport (e.g. a SCSI or NVMe driver):

1. **Obtain the .sys file.** Extract the NT4 or NT5 miniport from the target
   OS distribution. Place it alongside the VxD source.

2. **Update the embedded binary reference.** In the C source (NTMINI.C or its
   successor), change the embedded miniport data or the filename string passed
   to the PE loader. The current code references `atapi.sys`.

3. **Update the DDB name.** The 8 byte DDB_Name field in the ASM boilerplate
   identifies the driver to VMM. Change it to match the new driver's identity.

4. **Adjust ScsiPort shim functions.** If the new miniport calls ScsiPort APIs
   that the current shim stubs out (check the unimplemented areas listed in
   `reference/BUILD.TXT`), those stubs must be filled in. The 22 ScsiPort API
   functions in Part 2 of NTMINI.C are the starting point.

5. **Update the IOS registration.** The AEP (Asynchronous Event Packet)
   handler must declare the correct device type and adapter properties for the
   new hardware.

6. **Recompile and rebuild.** Follow the build workflow from Section 2. The LE
   correction step does not change; it operates on the binary structure, not
   the driver logic.

7. **Test incrementally.** Use `neuter_ctrl.py` to verify LE loading succeeds
   before testing actual driver logic. Then enable one control message at a
   time (Sys_Critical_Init, Device_Init, Init_Complete) and check debug output
   after each boot.
