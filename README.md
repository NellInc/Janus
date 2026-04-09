# NT Miniport Backport for Windows 9x

Run unmodified NT4 SCSI miniport drivers inside Windows 98.

NTMINI.VXD loads NT4's `atapi.sys` at ring 0, provides the ScsiPort API it expects, and bridges the result into Win98's IFSMgr file system layer. The same 27,600-byte binary that ships with NT4 SP6a, running inside Win98's kernel, providing CD-ROM file access where the native driver stack fails.

## Why

Windows NT from version 4 onward has a clean, well-documented miniport driver architecture. Storage drivers are small, hardware-specific modules that talk through a standard ScsiPort interface.

Windows 9x has none of this. Its storage stack depends on vendor-specific VxDs and port drivers (.PDR files) that have been abandonware for two decades. If the built-in `ESDI_506.PDR` doesn't support your controller, or your optical drive isn't detected, your options are limited.

Rather than writing yet another Win9x driver from scratch, this project loads the unmodified NT4 miniport binary and provides the runtime environment it expects. In principle, this approach could work with any NT4 SCSI miniport: `atapi.sys`, `sym_hi.sys` for Symbios/LSI SCSI, `aic78xx.sys` for Adaptec. The current implementation uses `atapi.sys` because IDE controllers are everywhere and easy to test.

## Current Status

Full file access pipeline working end to end in QEMU:

```
SP: Adapter FOUND! Calling HwInitialize...
SP: HwInitialize OK!
FSD: MOUNT D:
FSD: >>> NetOpen ENTRY <<<
FSD: OPEN 'README.TXT'
FSD: OPEN OK lba=0x00000019 sz=0x00000013
V5: "NTMINI CD-ROM TEST"
V5: *** FSD FILE ACCESS SUCCESS ***
```

VxD loads, PE loader maps `atapi.sys` at ring 0, DriverEntry runs, ScsiPortInitialize calls HwFindAdapter across four IDE channels, HwInitialize succeeds, and the miniport executes READ(10) commands against the CD-ROM. On top of that, the IFSMgr FSD layer provides real file operations: Open, Read, Close, directory listing (Search), and file attributes.

**Looking for real hardware testers.** Everything has been developed in QEMU. See [What Would Help](#what-would-help) below.

## Quick Start

### Pre-built Binary

Download `NTMINI.VXD` from the [latest release](../../releases) or from `deploy-package/`.

1. Copy `NTMINI.VXD` to `C:\WINDOWS\SYSTEM\`
2. Add to `C:\WINDOWS\SYSTEM.INI` under `[386Enh]`:
   ```
   device=C:\WINDOWS\SYSTEM\NTMINI.VXD
   ```
3. Restart Windows

Or run `INSTALL.BAT` from the deploy package.

### Building from Source

#### Prerequisites

- [Open Watcom C 2.0](https://github.com/open-watcom/open-watcom-v2) (`wcc386`)
- [NASM](https://www.nasm.us/) assembler
- Python 3 (for the LE header post-processor)

#### Docker Build (recommended)

```bash
# Build the Docker image (one-time setup)
cd source
docker build -t ntmini-builder .

# Compile
cd ..
docker run --rm -v "$PWD/source:/src" -v "$PWD/builds:/out" ntmini-builder:latest \
  sh -c "cd /src && \
    wcc386 -bt=windows -3s -s -zl -d0 NTMINI_V5.C -fo=/out/V5FULL_c.obj && \
    wcc386 -bt=windows -3s -s -zl -d0 PELOAD.C -fo=/out/V5FULL_pe.obj && \
    nasm -f obj -o /out/V5FULL_asm.obj VXDWRAP_V4.ASM"

# Link
docker run --rm -v "$PWD/builds:/src" ntmini-builder:latest \
  sh -c "cd /src && wlink @link_v5full.lnk"

# Post-process LE headers for Win98 VMM compatibility
python3 build_sysini_fixed.py

# The linker produces V5SMALL.VXD; rename for deployment
cp builds/V5SMALL.VXD deploy-package/NTMINI.VXD
```

#### Native Build

```bash
wcc386 -bt=windows -3s -s -zl -d0 source/NTMINI_V5.C -fo=builds/V5FULL_c.obj
wcc386 -bt=windows -3s -s -zl -d0 source/PELOAD.C -fo=builds/V5FULL_pe.obj
nasm -f obj -o builds/V5FULL_asm.obj source/VXDWRAP_V4.ASM
cd builds && wlink @link_v5full.lnk && cd ..
python3 build_sysini_fixed.py
cp builds/V5SMALL.VXD deploy-package/NTMINI.VXD
```

## Architecture

```
NT4 atapi.sys (unmodified PE binary, 27,600 bytes)
    |
    v
NTMINI.VXD (Win98 LE VxD)
    |
    +-- ScsiPort Shim (29 functions)
    |       |
    |       +-- Port I/O remapping (secondary -> primary)
    |       +-- PCI config space (direct 0xCF8/0xCFC)
    |       +-- DMA/interrupt stubs
    |       |
    |       v
    |   HwFindAdapter -> HwInitialize -> HwStartIo
    |       |
    |       v
    |   ATAPI PACKET commands (direct hardware I/O)
    |
    +-- IFSMgr FSD Layer
    |       |
    |       +-- RegisterMount -> mount callback claims D:
    |       +-- Entry table: Open, Search, FileAttr, GetDiskInfo, ...
    |       +-- IFS hook: Read, Close
    |       |
    |       v
    |   ISO 9660 parser (PVD, root directory, file lookup)
    |
    +-- IOS Bridge
    |       |
    |       +-- DRP registration, DCB creation
    |       +-- ILB acquisition via DDB chain walking
    |       +-- Calldown chain installation
    |
    +-- PE Loader
            |
            +-- Section mapping, base relocations
            +-- Import resolution (ScsiPort API)
```

### Source Files

| File | Purpose |
|------|---------|
| `NTMINI_V5.C` | ScsiPort shim (29 functions), IOS bridge, IFSMgr FSD, IFS hook, ISO 9660 parser, ATAPI I/O |
| `VXDWRAP_V4.ASM` | VxD DDB, control procedure, VMM/VPICD trampolines, ScsiPort struct-return stubs, IFSMgr/IOS service wrappers, Ring0_FileIO wrappers |
| `PELOAD.C` | Ring 0 PE image loader with section mapping, base relocations, and import resolution |
| `ATAPI_EMBEDDED.H` | NT4 `atapi.sys` as a 27,600-byte C array |
| `build_sysini_fixed.py` | Post-linker: fixes LE headers for Win98 VMM compatibility |
| `deploy_sysini.py` | Deploys VXD to a FAT32 disk image for VM testing |
| `test_ios_chain.py` | Automated VM boot, deployment, and log analysis |

### Boot Sequence

1. **Sys_Critical_Init**: Initialize COM1 serial debug output
2. **Device_Init**: PE-load `atapi.sys` into ring 0, resolve all ScsiPort imports, call DriverEntry. DriverEntry calls ScsiPortInitialize, which calls HwFindAdapter in a loop (4 passes across primary, secondary, tertiary, quaternary IDE channels). HwInitialize sets up the adapter, DeviceFlags are patched, test READ(10) SRB submitted.
3. **Init_Complete**: Register with IOS, acquire ILB, create DCB, register FSD with IFSMgr via RegisterMount, trigger CDROM_Attach to mount D:, install IFS hook.

## Technical Discoveries

### 1. MSVC vs Watcom ABI: The 8-Byte Struct Return Problem

NT4's `atapi.sys` is compiled with MSVC. Our VxD uses Open Watcom. These compilers disagree on how to return small structs.

`ScsiPortConvertUlongToPhysicalAddress` returns an 8-byte `SCSI_PHYSICAL_ADDRESS`. MSVC returns this in EDX:EAX. Watcom would use a hidden pointer parameter. If you implement this in C with Watcom, the calling convention mismatch corrupts the stack.

Fix: write these functions in assembly with explicit EDX:EAX returns.

### 2. HwStartIo Device Extension Layout

After HwFindAdapter detected the CD-ROM, HwStartIo kept returning `SRB_STATUS_SELECTION_TIMEOUT`. HwStartIo reads DeviceFlags from `DevExt + 0x44 + TargetId*2`. Because we present secondary hardware as primary via port remapping, HwStartIo reads from the channel-0 offset and finds zero.

```
HW_DEVICE_EXTENSION layout (reverse-engineered):
  0x00: CurrentSrb
  0x04: BaseIoAddress1
  0x0C: BaseIoAddress2
  0x44: DeviceFlags[TargetId] (WORD array)
        Bit 0 = DFLAGS_DEVICE_PRESENT
        Bit 1 = DFLAGS_ATAPI_DEVICE
        Bit 4 = DFLAGS_REMOVABLE_DRIVE
  0xC0: IDENTIFY PACKET DEVICE data (512 bytes)
```

Fix: after HwFindAdapter returns, check whether DeviceFlags[0] is empty and populate it from the channel where the device was actually found.

### 3. Port Remapping: The Channel Identity Crisis

The CD-ROM lives on secondary IDE (0x170, control 0x376). If you tell the miniport "here's a secondary channel," it stores device information at channel-1 offsets. Then HwStartIo, expecting the device at channel 0, finds nothing.

Fix: present primary IDE addresses (0x1F0, 0x3F6) in the access ranges, then remap all port I/O transparently to secondary hardware. The miniport thinks it's talking to primary; all actual I/O hits secondary.

### 4. HwFindAdapter Multi-Pass Detection

NT4's `atapi.sys` HwFindAdapter has an internal state machine that iterates across four IDE channels over multiple calls. ScsiPortInitialize calls HwFindAdapter in a loop while the miniport sets `*again = TRUE`. You must zero the access ranges between calls so the miniport advances its channel index.

### 5. IOS Registration: DRP Struct Packing

Root cause of months of debugging. The DRP (Device Registration Packet) must use `#pragma pack(push,1)`. Without byte packing, the compiler inserts padding after `DRP_revision`, shifting every subsequent field. IOS reads fields by absolute offset, so misalignment causes silent registration failure.

### 6. ILB Acquisition via DDB Chain Walking

IOS doesn't provide the ILB (Import Library Base) pointer to drivers that register after boot. Workaround: walk the VMM VxD DDB chain starting from `VMM_Get_DDB(0x0010)`, search each registered driver's reference data for a DRP eyecatcher, and copy the ILB function pointer from whichever APIX driver already has one.

### 7. LGN Bitmask Encoding

The DRP's LGN (Logical Group Number) field requires a bitmask, not a bit index. The correct value is `0x00400000` (`1 << 0x16`), corresponding to `DRP_ESDI_PD`. Using the raw bit number as the value produces a wrong device class.

### 8. QEMU IDE Status Register Quirks

Two QEMU-specific behaviors: master returns 0x00 after DEVICE RESET (real hardware returns 0x50), and non-existent slave echoes master status. Both cause the miniport to hang or misdetect devices. Workarounds intercept status register reads and should be conditionally applied on real hardware.

### 9. PCI Config Space: Roll Your Own

Win98 doesn't expose PCI config space reads as a VxD service during Device_Init. Fix: hit ports 0xCF8 (address) and 0xCFC (data) directly.

### 10. The BPB Corruption Bug

Code that tried to clear the FAT32 "dirty" flag by writing to BPB offset 0x25 was actually corrupting `BPB_FATSz32` (offsets 0x24-0x27), making every cluster lookup wrong. The FAT32 dirty flag lives in the FAT itself, in cluster 1's entry (FAT offset + 4 bytes, bit 27). Never touch BPB offsets 0x24-0x27.

### 11. IFSMgr FSD Entry Table Calling Convention

Not documented anywhere we could find. The DDK says each FSD entry table function receives a single argument (pointer to ioreq). That's wrong. IFSMgr calls entry table functions with five arguments:

```c
int _cdecl handler(int fn, int drive, int resType, int cpid, ULONG pir)
```

The ioreq pointer is the fifth argument. If you declare these functions with a single parameter, your "ioreq pointer" is actually the IFS function number, and every field read goes to garbage memory.

### 12. IFSMgr Entry Table Indexing

The FSD entry table has a count header at position [0] followed by function pointers. When setting `ir_gi` (the entry table pointer) during mount, you must point past the header. IFSMgr uses 0-based IFSFN indexing, so if `ir_gi` includes the header, every dispatch lands on the wrong function.

### 13. IFS Hook Function Numbers

The `fn` parameter in IFS hooks is a byte offset into IFSMgr's internal 6-byte far pointer dispatch table:

```
fn = IFSFN * 6
```

fn=0 is READ, fn=36 (0x24) is OPEN, fn=66 (0x42) is CLOSE. Getting this wrong means your hook intercepts the wrong operations.

### 14. Ring0_FileIO Handle Values

`IFSMgr_Ring0_FileIO` returns file handles as kernel-space pointers in the 0xC0000000 range. These are valid handles despite appearing negative as signed 32-bit integers. Any wrapper that checks `handle > 0` or uses `cmp eax, 0; jl` will reject every valid handle.

### 15. READ ioreq Layout

For file read operations through the IFS hook, the byte count is at ioreq+0x00 and the data buffer pointer at ioreq+0x14. This doesn't match DDK documentation. Discovered by dumping the ioreq and correlating field values with known parameters.

## File Operations

| Operation | Method | Description |
|-----------|--------|-------------|
| Open | FSD entry table (NetOpen) | ISO 9660 root directory lookup, returns IFSMgr handle |
| Read | IFS hook (fn=0) | Sequential reads with position tracking (8 file slots, best with sequential open-read-close patterns) |
| Close | IFS hook (fn=11) | Frees file slot |
| Search | FSD entry table (NetSearch) | FindFirst/FindNext enumeration of root directory |
| FileAttr | FSD entry table | Returns read-only attribute for Explorer compatibility |
| GetDiskInfo | FSD entry table | Returns volume info (sectors, cluster size) |

## Limitations

- **Root directory only**: does not traverse subdirectories
- **D: drive hardcoded**: assumes CD-ROM is drive D:
- **Read-only**: no write support (appropriate for CD-ROM)
- **Polling mode**: VPICD interrupt code is written but not yet wired (uses polling)
- **QEMU only**: untested on real hardware
- **Sequential file access**: 8 file slots, optimized for open-read-close patterns rather than interleaved I/O

## What Would Help

**Real hardware testing.** If you have Pentium-class hardware with an IDE CD-ROM drive and would be willing to try this, that would be extremely valuable. Reports of what works (or doesn't) on real controllers would help identify where the QEMU-specific workarounds end and where genuine hardware edge cases begin.

**Win32 application testing.** The file access pipeline works from ring 0, but hasn't been tested with user-mode programs doing `CreateFile`/`ReadFile` on D:, or with Explorer browsing D:. The IFSMgr handles should be valid for Win32 access, but it hasn't been verified.

## Project Structure

```
.
├── source/
│   ├── NTMINI_V5.C          # Main driver source (~4,700 lines)
│   ├── VXDWRAP_V4.ASM       # Assembly wrapper
│   ├── PELOAD.C              # PE image loader
│   ├── ATAPI_EMBEDDED.H      # NT4 atapi.sys binary
│   └── Dockerfile            # Build environment
├── builds/
│   └── link_v5full.lnk       # Linker script
├── deploy-package/
│   ├── NTMINI.VXD            # Pre-built binary
│   ├── INSTALL.BAT           # Automated installer
│   └── README.txt            # End-user instructions
├── build_sysini_fixed.py     # LE header post-processor
├── deploy_sysini.py          # VM deployment tool
├── test_ios_chain.py         # Automated test harness
├── LICENSE                   # MIT License
└── README.md                 # This file
```

## License

MIT License. See [LICENSE](LICENSE) for details.

## Acknowledgments

Built with assistance from Claude (Anthropic). The undocumented IFSMgr behaviors documented above were discovered through systematic binary analysis of Win98's `IFSMgr.VXD` and `CDFS.VXD`.

Thanks to Björn Korneli for the inspiration.
