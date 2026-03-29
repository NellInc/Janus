---
topic: NT5 WDM driver - debug file I/O and PE loading after WPE fix
date: 2026-03-29
status: NOT_STARTED
stepsCompleted: 0
prerequisite: nt5_debug_wpe_20260328.md (steps 1-5 complete, WPE RESOLVED)
---

# Debug NT5 VxD File I/O and PE Loading

## What Was Fixed This Session (3 critical bugs)

### Bug 1: Out_Debug_String ordinal crash
The VMM service Out_Debug_String was mapped to ordinal 0x1D, which on
retail Win98 VMM is actually Map_Lin_To_VM_Addr. Calling it with ESI
pointing to a string caused an immediate WPE.

**Fix**: Replaced all Out_Debug_String `int 0x20` calls with direct
port 0xE9 byte-by-byte output (QEMU debugcon). Two call sites fixed:
- _ntmini_init init banner (VXDWRAP_NASM.ASM:153-156)
- VxD_Debug_Printf (VXDWRAP_NASM.ASM:487-490)

### Bug 2: Self-relative fixup corruption in LE merger (ROOT CAUSE of WPE)
The LE header fixer (build_nt5_fixed.py) merges 3 LE objects into 1.
When rewriting fixup records, it hardcoded src_type=0x07 (32-bit offset)
for ALL internal fixups. But 389 out of 1341 fixups (29%) have
src_type=0x08 (32-bit self-relative), used by CALL and JMP rel32
instructions. Writing 0x07 for these caused every cross-object function
call to jump to the wrong address.

**Fix**: `src_type & 0x0F` preserves the original src_type (0x07 or 0x08).
Line 155 in build_nt5_fixed.py.

### Bug 3: VMM R0 file I/O services don't exist in Win98 SE
The NASM file used VMM services 0x02DE-0x02E1 (R0_OpenCreateFile,
R0_ReadFile, R0_CloseFile, R0_GetFileSize) for ring-0 file I/O.
These ordinals (~734 decimal) don't exist in Win98 SE's VMM service
table. Calling them via `int 0x20` crashes QEMU (GP fault with no
handler).

**Fix**: Replaced with IFSMgr Ring0_FileIO (device 0x0040, service
0x0032). This is a cdecl stack-based API. The working V4 path
(VXDWRAP_V4.ASM) uses this same API. Function codes: 0xD500 (open),
0xD600 (read), 0xD700 (close).

### Additional fix: Infinite recursion
ios_register_port_driver() called nt5_init(), which called
ios_register_port_driver() back. Removed the circular call from
NT5LOADER.C.

### Additional fix: Init timing
The VxD control dispatcher routed Sys_Critical_Init, Device_Init, AND
Init_Complete all to the same init handler. IFSMgr Ring0_FileIO isn't
available during the earlier phases. Fixed to only run init at
Init_Complete (when the filesystem is mounted).

### Additional fix: IFSMgr warm-up
IFSMgr Ring0_FileIO fails on the FIRST call after Init_Complete but
works on subsequent calls. A dummy open of MSDOS.SYS before the real
file open "primes" IFSMgr. This is a known Win98 timing issue.

## Current State

The VxD loads, initializes at Init_Complete, opens W2K_ATAP.SYS
successfully (handle=1), then the debug log goes silent. No WPE.
Windows 98 boots to desktop.

Debug log (last successful run):
```
NT!  (x15 - heartbeat for each VMM control message)
NTMINI: NT Miniport Compatibility Shim v1.0
A  (breadcrumb: about to call ios_register_port_driver)
C  (breadcrumb: entered ios_register_port_driver C function)
NTMINI: ios_register_port_driver()
D  (breadcrumb: about to call nt5_init)
NT5: ======================================
NT5: NT5 ATAPI.SYS Loader starting
NT5: Target: %s IDE channel
NT5: ======================================
NT5: Opening file: C:\WINDOWS\SYSTEM\W2K_ATAP.SYS
NT5: Test open MSDOS.SYS = 0  (expected - S/H/R attributes)
NT5: File_Open returned: 1  (SUCCESS!)
NT!  (x2 - more control messages)
```

The silence after "File_Open returned: 1" means either:
1. VxD_HeapAllocate (VMM ordinal 0x0256) is wrong and hangs/crashes silently
2. VxD_File_Read via Ring0_FileIO hangs in an infinite loop
3. The file read works but the PE loader crashes

## Next Steps

### Step 1: Add breadcrumbs to file read path

In NT5LOADER.C nt5_load_file(), add dbg_mark() calls:
- After VxD_HeapAllocate (marker 'H')
- Before each VxD_File_Read chunk (marker 'R')
- After the read loop (marker 'L' for loop done)
- After VxD_File_Close (marker 'X')

### Step 2: Verify VMM HeapAllocate ordinal

VMM_HeapAllocate is ordinal 0x0256 (598 decimal). V5REAL.VXD also uses
this ordinal and works. But the C code calls VxD_HeapAllocate to
allocate NT5_MAX_IMAGE_SIZE bytes (check the value - may be very large).
If allocation fails silently, add a NULL check with debug output.

### Step 3: Debug the file read

Ring0_FileIO R0_READFILE (0xD600) parameter order (cdecl):
```
IFSMgr_Ring0_FileIO(R0_READFILE, pBuffer, hFile, count, position)
```

Check: is the parameter order correct? The V4 ASM comments say
"(func, pBuffer, hFile, count, position)" but some references show
"(func, hFile, pBuffer, count, position)". Verify by trying both orders.

Also check: what does R0_READFILE return? EAX or ECX? The V4 code checks
both. The current code does:
```nasm
test    eax, eax
jnz     .fr_done
mov     eax, ecx
```

### Step 4: If file loads, debug the PE loader

PELOAD.C loads the PE sections, processes relocations, resolves imports.
The import resolution is against ntoskrnl/HAL shim functions. If any
import can't be resolved, the PE load fails.

### Step 5: If PE loads, debug DriverEntry

The W2K atapi.sys DriverEntry expects:
- A DRIVER_OBJECT with correct size and layout
- A UNICODE_STRING registry path
- ntoskrnl/HAL functions accessible via IAT

## Build Pipeline

```bash
# 1. Compile
cd history/source && ./build.sh clean && ./build.sh nt5

# 2. Fix LE headers (CRITICAL: preserves self-relative fixups now)
cd ../.. && python3 src/build_nt5_fixed.py

# 3. Deploy
export MTOOLSRC=/tmp/win98vm/mtoolsrc
mcopy -o src/NT5FIXED.VXD c:/WINDOWS/SYSTEM/NTMINI.VXD

# 4. Boot (BootMenuDefault=1 in MSDOS.SYS, auto-selects Normal)
rm -f /tmp/vxd_debug.log
qemu-system-i386 -m 64 -M pc -accel tcg -cpu pentium \
  -hda /tmp/win98vm/win98_nogeom.img -boot c -vga cirrus \
  -rtc base=localtime -display none \
  -monitor tcp:127.0.0.1:4444,server,nowait \
  -chardev file,id=dbgcon,path=/tmp/vxd_debug.log \
  -device isa-debugcon,iobase=0xe9,chardev=dbgcon \
  -cdrom /tmp/win98vm/test_cd.iso &

# 5. Wait ~150s for full boot, then read log
sleep 150 && cat /tmp/vxd_debug.log
# Screenshot: echo "screendump /tmp/s.ppm" | nc -w2 127.0.0.1 4444
```

## MSDOS.SYS Settings (already deployed)

```
AutoScan=0          (skip ScanDisk)
BootMenu=1          (always show boot menu)
BootMenuDefault=1   (default to Normal, not Safe mode)
BootMenuDelay=30    (30s timeout)
BootSafe=0          (don't force Safe mode)
```

## Key Files Modified

### VXDWRAP_NASM.ASM
- Replaced Out_Debug_String with port 0xE9 output
- Added dbg_mark() ASM function (callable from C)
- Replaced VMM R0 file I/O with IFSMgr Ring0_FileIO
- Fixed control dispatcher: only init at Init_Complete
- Added breadcrumbs around ios_register_port_driver call
- Removed VMM_R0_* service definitions, added IFSMgr defines

### build_nt5_fixed.py
- Line 155: `src_type & 0x0F` instead of hardcoded `0x07`
- This preserves self-relative fixups (0x08) for CALL/JMP

### NT5LOADER.C
- Removed ios_register_port_driver() call (broke recursion)
- Fixed atapi path: W2K_ATAP.SYS (8.3 format matches deployment)
- Added dbg_mark extern and diagnostic markers
- Added IFSMgr warm-up call (dummy MSDOS.SYS open)

### IOSBRIDGE.C
- Added dbg_mark extern and entry breadcrumbs

### Makefile
- Added -dDEBUG to CFLAGS (enables DBGPRINT macro)

### link_nt5.lnk
- Added option map=NT5RAW.MAP

## VMM Service Ordinal Status

| Service | Ordinal | Status |
|---------|---------|--------|
| _PageAllocate | 0x0053 | WORKS (V5REAL.VXD uses same) |
| _HeapAllocate | 0x0256 | UNTESTED (next to verify) |
| _HeapFree | 0x0257 | UNTESTED |
| Out_Debug_String | 0x001D | WRONG (was Map_Lin_To_VM_Addr) |
| R0_OpenCreateFile | 0x02DE | DOESN'T EXIST (replaced with IFSMgr) |
| R0_ReadFile | 0x02E1 | DOESN'T EXIST (replaced with IFSMgr) |
| R0_CloseFile | 0x02E0 | DOESN'T EXIST (replaced with IFSMgr) |
| Get_System_Time | 0x003F | UNTESTED |
| _MapPhysToLinear | 0x006C | UNTESTED |
| _RegOpenKey | 0x0100 | UNTESTED (vmdisp9x says 0x0148) |

## Disk Image Location

All in /tmp/win98vm/ (volatile, survives until reboot):
- win98_nogeom.img (8 GB raw image)
- test_cd.iso (ISO 9660 test disc)
- mtoolsrc (mtools configuration)
- MSDOS.SYS (local copy, already deployed with boot menu settings)

## Environment

- Repo: /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport
- Docker: nt5-vxd-builder (Open Watcom + NASM)
- QEMU: 10.2.2 (Homebrew, ARM64 macOS TCG)
- mtools: /opt/homebrew/bin/mtools
