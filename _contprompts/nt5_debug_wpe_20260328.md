---
topic: NT5 WDM driver - debug Windows Protection Error during init
date: 2026-03-28
status: NOT_STARTED
stepsCompleted: 0
prerequisite: nt5_compile_test_20260328.md (steps 1-5 complete)
---

# Debug NT5 VxD Windows Protection Error

## What Happened

The NT5 WDM compatibility layer VxD (9,256 lines across 10 C files + 1 ASM)
compiles, links, and loads inside Win98. VMM accepts the LE binary, finds the
DDB, dispatches Device_Init. The heartbeat code in NTMINI_Control writes "NT!"
to port 0xE9 (confirmed in QEMU debug log). Then the init path crashes with:

```
While initializing device NTMINI:
Windows protection error. You need to restart your computer.
```

## Crash Location (Narrowed)

The call chain from the heartbeat to the crash:

```
NTMINI_Control          (ASM, writes "NT!" to port 0xE9)  ← WORKS
  → .do_init
    → _ntmini_init      (ASM, VXDWRAP_NASM.ASM:147)
      → Out_Debug_String (VMM service 0x0001001D via int 0x20) ← SUSPECT
      → ios_register_port_driver  (C, IOSBRIDGE.C:317)
        → first DBGPRINT("NTMINI: ios_register_port_driver()")  ← NEVER REACHED
```

The crash happens AFTER the heartbeat but BEFORE the first DBGPRINT in C code.
Two candidates:

1. **The Out_Debug_String call** at VXDWRAP_NASM.ASM:155 uses VMM service
   ordinal 0x0001001D. This ordinal may be WRONG for this Win98 version.
   The vmdisp9x project says Out_Debug_String should be ordinal 194 (0xC2),
   which would make it 0x000100C2. Ordinal 0x1D maps to Map_Lin_To_VM_Addr
   per vmdisp9x. Calling Map_Lin_To_VM_Addr with ESI pointing to a string
   would likely crash.

2. **The call to ios_register_port_driver** might crash due to a stack or
   calling convention issue.

## Most Likely Fix

**Replace the Out_Debug_String int 0x20 call** in _ntmini_init with direct
port 0xE9 output (matching what we already do in VxD_Debug_Printf). Or simply
NOP it out. The init banner is cosmetic.

Then, if the crash moves to ios_register_port_driver or nt5_init, add more
port 0xE9 breadcrumbs to narrow the location.

## VMM Service Ordinal Concern

The VXDWRAP_NASM.ASM file has manually-assigned VMM service ordinals.
Several are KNOWN to differ from the vmdisp9x reference (a working Win9x
VxD project). The ordinals that match the working MASM-compiled binary
(V5REAL.VXD) may be from a non-standard DDK. Key discrepancies:

| Service | NASM file | vmdisp9x | Status |
|---------|-----------|----------|--------|
| _PageAllocate | 0x53 | 0x53 (83) | MATCH |
| _HeapAllocate | 0x256 | 0x4F (79) | MISMATCH |
| _HeapFree | 0x257 | 0x51 (81) | MISMATCH |
| Out_Debug_String | 0x1D | 0xC2 (194) | MISMATCH |
| Cancel_Time_Out | 0x3D | 0x3E (62) | MISMATCH |
| _RegOpenKey | 0x100 | 0x148 (328) | MISMATCH |

If these ordinals are wrong, every VMM service call except _PageAllocate
will call the wrong function, causing crashes or silent corruption. The
working binary (V5REAL.VXD) was compiled with MASM + DDK headers that
auto-generate correct ordinals. The NASM file was manually created.

**Recommended approach**: Either fix ALL ordinals to match vmdisp9x, or
extract correct ordinals from V5REAL.VXD's int 0x20 call sites and use those.

V5REAL.VXD INT 0x20 sites (extracted):
- VMM 0x001D, 0x0053, 0x0069, 0x0146, 0x0188, 0x0189, 0x018A, 0x0256, 0x0257
- IOS 0x0000, 0x0003, 0x0005, 0x0007, 0x0008, 0x0012
- VPICD (Dev 0x0033) 0x0002, 0x000B, 0x000E, 0x0033, 0x0034, 0x0036, 0x0039
- IFSMgr (Dev 0x0040) 0x0032 (Ring0_FileIO)
- Dev 0x0044: 0x0001, 0x0006

## Build Pipeline (Working)

```bash
# 1. Compile (Docker + Open Watcom + NASM)
cd history/source
./build.sh                          # produces NT5RAW.VXD (51,992 bytes)

# 2. Fix LE headers
cd ../../src
python3 build_nt5_fixed.py          # produces NT5FIXED.VXD (54,794 bytes)

# 3. Deploy via mtools (NOT deploy_sysini.py - that corrupts raw images)
export MTOOLSRC=/tmp/win98vm/mtoolsrc
# mtoolsrc contains: drive c: file="/tmp/win98vm/win98_nogeom.img" partition=1
mcopy NT5FIXED.VXD c:/WINDOWS/SYSTEM/NTMINI.VXD
mcopy ../binaries/w2k_atapi.sys c:/WINDOWS/SYSTEM/W2K_ATAP.SYS
# Edit SYSTEM.INI: add device=C:\WINDOWS\SYSTEM\NTMINI.VXD to [386Enh]
mcopy c:/WINDOWS/SYSTEM.INI /tmp/win98vm/SYSTEM.INI
# ... edit the file ...
mcopy -o /tmp/win98vm/SYSTEM.INI c:/WINDOWS/SYSTEM.INI

# 4. Boot QEMU (use -hda for auto-geometry, NOT explicit cyls/heads/secs)
qemu-system-i386 -m 64 -M pc -accel tcg -cpu pentium \
  -hda /tmp/win98vm/win98_nogeom.img \
  -boot c -vga cirrus -rtc base=localtime \
  -display none \
  -monitor tcp:127.0.0.1:4444,server,nowait \
  -chardev file,id=dbgcon,path=/tmp/vxd_debug.log \
  -device isa-debugcon,iobase=0xe9,chardev=dbgcon \
  -cdrom /tmp/win98vm/test_cd.iso

# 5. Monitor commands
echo "screendump /tmp/screen.ppm" | nc -w 2 127.0.0.1 4444
# Convert: sips -s format png /tmp/screen.ppm --out /tmp/screen.png
```

## Critical Deployment Gotchas

1. **Never use explicit CHS geometry** with raw images. Use `-hda` which
   auto-detects. Explicit `cyls=1044,heads=255,secs=63` causes boot failure.

2. **Never use deploy_sysini.py** on raw-converted qcow2 images. It corrupts
   the filesystem. Use mtools instead.

3. **The qcow2 boots natively** but raw conversion is needed for mtools.
   Create via: `qemu-img convert -f qcow2 -O raw source.qcow2 target.img`

4. **Win98 boots SLOWLY** under TCG on ARM64 macOS. Allow 2-3 minutes for
   ScanDisk + protected mode. ScanDisk runs every boot due to unclean
   shutdown (no graceful shutdown mechanism in our pipeline).

5. **The LE fixer is required.** Raw wlink output (NT5RAW.VXD) has wrong
   relocation base (0 instead of 0x10000) in its merged object. The fixer
   (build_nt5_fixed.py) corrects this and produces NT5FIXED.VXD.

## Disk Image Location

- Source qcow2: `/tmp/win98vm/en_win98_se.qcow2` (389 MB, archive.org)
- Working raw: `/tmp/win98vm/win98_nogeom.img` (8 GB)
- Test ISO: `/tmp/win98vm/test_cd.iso` (ISO 9660 with README.TXT)
- Deploy ISO: `/tmp/win98vm/deploy.iso` (NTMINI.VXD + W2K_ATAP.SYS)
- mtools config: `/tmp/win98vm/mtoolsrc`

These are in /tmp and will not survive a reboot. To recreate:
```bash
# Download
curl -L -o /tmp/win98vm/en_win98_se.qcow2 \
  "https://archive.org/download/en_win98_se-qemu_5.2/en_win98_se.qcow2"
# Convert
qemu-img convert -f qcow2 -O raw /tmp/win98vm/en_win98_se.qcow2 \
  /tmp/win98vm/win98_nogeom.img
# mtools config
echo 'drive c: file="/tmp/win98vm/win98_nogeom.img" partition=1' \
  > /tmp/win98vm/mtoolsrc
```

## Debugging Strategy

### Step 1: Fix the Out_Debug_String crash

In VXDWRAP_NASM.ASM, replace the init banner code at lines 153-156:
```nasm
    ; Print init banner
    mov     esi, _szInitBanner
    int     0x20
    dd      VMM_Out_Debug_String
```

With port 0xE9 output (or just NOP it):
```nasm
    ; Init banner via port 0xE9 (Out_Debug_String ordinal may be wrong)
    mov     esi, _szInitBanner
.banner_loop:
    lodsb
    test    al, al
    jz      .banner_done
    out     0xE9, al
    jmp     .banner_loop
.banner_done:
```

Rebuild, fix LE, deploy, boot. If the crash moves past the banner, the
Out_Debug_String ordinal was the culprit.

### Step 2: Add breadcrumbs before each major call

In IOSBRIDGE.C ios_register_port_driver(), add port 0xE9 writes before
each step:
```c
/* Direct port 0xE9 write - bypasses VxD_Debug_Printf for safety */
static void dbg_marker(char c) {
    __asm { mov al, c; out 0xE9, al }
}
```
Call dbg_marker('A') before nt5_init, dbg_marker('B') before zero_mem,
dbg_marker('C') before IOS_Register, etc. The debug log will show exactly
how far init gets.

### Step 3: Fix VMM service ordinals if needed

If HeapAllocate (ordinal 0x0256) is called during init and the ordinal
is wrong, it will crash. The safe approach:
1. Try vmdisp9x ordinals for ALL services
2. If that breaks the build, use V5REAL.VXD's extracted ordinals as reference
3. Test one service at a time

### Step 4: Continue with contprompt steps 6-10

Once the WPE is resolved and the VxD initializes:
- Step 6: Debug DriverEntry (struct sizes, DRIVER_OBJECT layout)
- Step 7: Debug AddDevice (IoGetDeviceProperty - flagged as likely first gap)
- Step 8: Debug START_DEVICE (CM_RESOURCE_LIST binary layout)
- Step 9: Debug SCSI IRP dispatch (interrupt delivery)
- Step 10: IOS integration

## Files Modified This Session

### New files created:
- `history/source/Makefile` - GNU make for Docker build
- `history/source/link_nt5.lnk` - wlink directives for NT5 VxD
- `history/source/build.sh` - Docker build wrapper
- `src/build_nt5_fixed.py` - LE header corrector for NT5RAW.VXD

### Modified files:
- `history/source/VXDWRAP_NASM.ASM` - Added: Get_System_Time/MapPhysToLinear/
  Time_Slice_Sleep service IDs and wrappers, VPICD underscore aliases for
  NTKSHIM.C, port 0xE9 debug output in VxD_Debug_Printf, heartbeat in
  NTMINI_Control
- `history/source/NTKRNL.H` - Added #ifndef NTKSHIM_H guard around shared types
- `history/source/IRPMGR.H` - Conditional NTKRNL.H include + NT5 path type defs
- `history/source/PNPMGR.H` - Conditional NTKRNL.H include + power type defs
- `history/source/NTKEXPORTS.C` - Removed NTKRNL.H include, local IMPORT_FUNC_ENTRY,
  cleaned extern declarations, removed static from g_dll_tables, added
  scsiport_funcs stub
- `history/source/NTKSHIM.C` - Added: ntk_KeQueryTimeIncrement, ntk_HalGetInterruptVector,
  ntk_Kf{Acquire,Release}SpinLock, ntk_Kf{Raise,Lower}Irql,
  ntk_KeStallExecutionProcessor, ntk_{READ,WRITE}_PORT_BUFFER_USHORT
- `history/source/IOSBRIDGE.C` - Added nt5_init(1) call before IOS_Register
- `history/source/PCIBUS.C` - Added #include "IRPMGR.H"
- `history/source/WDMBRIDGE.C` - Added #include "IRPMGR.H", local defines

## Environment

- Repo: /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport
- Docker image: `nt5-vxd-builder` (Ubuntu 22.04 + Open Watcom v2 + NASM)
- QEMU: 10.2.2 (Homebrew, ARM64 macOS)
- mtools: available at /opt/homebrew/bin/mtools
- Python 3: for build scripts and LE fixer
