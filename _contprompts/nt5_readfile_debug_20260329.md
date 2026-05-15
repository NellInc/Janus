---
topic: NT5 WDM driver - debug IFSMgr Ring0_FileIO R0_READFILE
date: 2026-03-29
status: NOT_STARTED
stepsCompleted: 0
prerequisite: nt5_fileio_debug_20260329.md (HeapAllocate + handle bugs fixed)
---

# Debug IFSMgr Ring0_FileIO R0_READFILE Data Transfer

## What Was Fixed This Session

### Bug 4: HeapAllocate crashes (VMM ordinal 0x0256)
VMM_HeapAllocate (ordinal 0x0256) crashes when allocating 256KB during
Init_Complete. The ordinal is correct (confirmed against V5REAL.VXD) but
HeapAllocate may not support large allocations or isn't available this early.

**Fix**: Switched to `_PageAllocate` (ordinal 0x0053, PAGEFIXED), which is
proven working in V5REAL.VXD. Later switched to a static BSS buffer to
eliminate address-space concerns.

### Bug 5: File handle returned in EBX, not EAX
R0_OPENCREATFILE returns:
- EAX = status/action code (1 = success)
- **EBX = file handle** (e.g. 0x30 = 48)
- ECX = action taken (4 = opened existing)

The wrapper was returning EAX (=1), so VxD_File_Read received handle=1
(invalid), causing every read to fail with error code 1.

**Fix**: `mov eax, ebx` before returning from VxD_File_Open.

### Bug 6: R0_READFILE return value interpretation (partial)
With the wrong handle (=1), every read returned EAX=1 which the wrapper
misinterpreted as "1 byte read". This masked the actual failure.

## Current State

With all fixes applied, the debug log shows:
```
NT5: File_Open returned: 9   (handle > 9, probably 0x30)
M                             (buffer allocated OK)
R                             (about to read)
Z                             (0 bytes read)
L X                           (loop done, file closed)
[     ]                       (buffer[0..3] = 0x20 = garbage, NOT 'MZ')
PELOAD: bad DOS signature     (PE loader fails)
```

R0_READFILE returns EAX=0, ECX=0 (carry clear) = "success, 0 bytes".
The buffer is never written to. The file is 88,048 bytes.

## Hypotheses for Next Session

### H1: Wrong parameter order (MOST LIKELY)
We tried two orderings with no difference:
- `(func, pBuffer, hFile, count, position)` — matches V5REAL.VXD MASM
- `(func, hFile, pBuffer, count, position)` — matches some DDK docs

Neither produced data in the buffer. Try remaining permutations:
- `(func, hFile, count, pBuffer, position)`
- `(func, count, pBuffer, hFile, position)`

### H2: IFSMgr VxDCall thunk is register-based, not stack-based
IFSMgr_Ring0_FileIO may use registers for some R0_ functions even though
the open (which works) uses stack. Test by setting up registers:
```nasm
mov     eax, R0_READFILE
mov     ebx, hFile
mov     ecx, count
mov     edx, pBuffer
mov     edi, position
int     0x20
dd      IFSMgr_Ring0_FileIO
```

### H3: Need to use IFSMgr_Ring0_ReadFile directly
There may be a separate `IFSMgr_Ring0_ReadFile` service (different ordinal
from Ring0_FileIO). Check the IFSMgr service table.

### H4: V86 INT 21h fallback
If IFSMgr_Ring0_FileIO R0_READFILE truly doesn't work, use V86-mode
DOS INT 21h file services instead:
- AH=3Dh (open), AH=3Fh (read), AH=3Eh (close)
- Requires Begin_Nest_V86_Exec / End_Nest_V86_Exec

### H5: File position issue
R0_OPENCREATFILE may leave the file position at EOF or at an invalid state.
Try `R0_GETFILESIZE` (0xD800) first to confirm the file size, then
read with explicit position=0.

## Build Pipeline

```bash
# 1. Compile
cd history/source && ./build.sh clean && ./build.sh nt5

# 2. Fix LE headers
python3 src/build_nt5_fixed.py

# 3. Deploy
export MTOOLSRC=/tmp/win98vm/mtoolsrc
mcopy -o src/NT5FIXED.VXD c:/WINDOWS/SYSTEM/NTMINI.VXD

# 4. Boot (send Enter at boot menu after ~45s)
rm -f /tmp/vxd_debug.log
qemu-system-i386 -m 64 -M pc -accel tcg -cpu pentium \
  -hda /tmp/win98vm/win98_nogeom.img -boot c -vga cirrus \
  -rtc base=localtime -display none \
  -monitor tcp:127.0.0.1:4444,server,nowait \
  -chardev file,id=dbgcon,path=/tmp/vxd_debug.log \
  -device isa-debugcon,iobase=0xe9,chardev=dbgcon \
  -cdrom /tmp/win98vm/test_cd.iso &
sleep 45 && echo "sendkey ret" | nc -w 2 127.0.0.1 4444
# Wait 120-150s for full boot, then:
tail -30 /tmp/vxd_debug.log
```

## MSDOS.SYS Settings (deployed)
```
AutoScan=0
BootMenu=1
BootMenuDefault=1
BootMenuDelay=5
BootSafe=0
```
After crash, Win98 shows Startup Menu that requires Enter keypress.

## Key Files Modified This Session

### VXDWRAP_NASM.ASM
- VxD_File_Open: returns EBX (real handle) instead of EAX (status code)
- VxD_File_Read: parameter order currently `(func, pBuffer, hFile, count, pos)`
  matching V5REAL.VXD MASM. Return value uses original EAX-first logic.

### NT5LOADER.C
- nt5_load_file: uses static BSS buffer `g_file_buffer[96KB]` instead of
  PageAllocate/HeapAllocate (eliminated address-space issues)
- Added dbg_mark breadcrumbs: h/H/M (alloc), R (read), D/Z/N (result),
  L (loop), X (close), [bytes] (buffer verify)

### build_nt5_fixed.py
- Line 158: `src_type & 0x0F` preserves self-relative fixups (from prev session)

## Disk Image
/tmp/win98vm/ (volatile, survives until reboot):
- win98_nogeom.img, mtoolsrc, test_cd.iso
- SYSTEM.INI has `device=C:\WINDOWS\SYSTEM\NTMINI.VXD`
- W2K_ATAP.SYS deployed at C:\WINDOWS\SYSTEM\ (verified: starts with 'MZ')

## Environment
- Repo: /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport
- Docker: nt5-vxd-builder (Open Watcom + NASM)
- QEMU: 10.2.2 (Homebrew, ARM64 macOS TCG)
- mtools: /opt/homebrew/bin/mtools
