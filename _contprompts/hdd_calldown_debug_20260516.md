---
stepsCompleted: 2
status: IN_PROGRESS
predecessor: hdd_pdr_ios_register_20260516.md
session_date: 2026-05-16
---

# HDD Calldown Chain: Debug WPE After Splice (2026-05-16)

## Goal

Win98 crashes (WPE) ~30s after our calldown handler is spliced into D:'s IOS calldown chain. Debug and fix the crash so SREAD.EXE can read D:\SECOND.TXT through our driver.

## Previous Session Results

### Blocker 1: Build Script — SOLVED

Four crash-causing changes in `build_nt5_fixed.py` were identified and reverted:

| Change | Fix |
|--------|-----|
| Alignment formula (LE-rel vs file-abs) | `data_pages_file = (MZ_SIZE + fixup_end + 0x1FF) & ~0x1FF` |
| obj_flags 0x2047 (+WRITABLE) | Reverted to 0x2045 |
| resident_name hardcoded NTMINI | Read from original file (NT5RAW) |
| preload_pages 0→NP | SAFE, kept |

Build script also has **static DRP injection** (DRP at merged offset 0x27CA0, fixups for DDB_Reference_Data and DRP.aer). This is harmless in SYSTEM.INI mode but was an experiment for PDR loading (which turned out to be a dead end).

### Blocker 2: IOS Registration — SOLVED

**Key finding: PDR mode (IOSUBSYS) is a dead end.** IOS-loaded PDRs only receive `Sys_Critical_Init` (0x00) and `Create_VM` (0x1D) — never `Init_Complete` or `Device_Init`.

**Solution: Deploy via SYSTEM.INI** (`python3 deploy_sysini.py binaries/NT5FIXED.VXD` — no `--pdr`). The VxD gets the full lifecycle:
```
m00000000 (Sys_Critical_Init) → return success
m0000001D (Create_VM) x6 → return success
m0000001E (VM_Terminate) x6 → return success
m00000001 (Device_Init) → return success
m00000002 (Init_Complete) → calls ios_register_port_driver()
```

Init_Complete triggers:
1. `ntmini_device_init()` → `nt5_init(0)` → loads ATAPI.SYS (full PE loader, 35KB+ debug output)
2. `ios_register_port_driver()` → `IOS_Register()` (may fail for late registration)
3. If IOS_Register fails: `find_existing_ilb()` steals ILB from APIX, then `ios_late_create_device()` creates DCBs manually

### New Blocker: WPE After Calldown Splice

The driver successfully:
- Loads ATAPI.SYS, detects MBR + FAT16 on test HDD
- Registers with IOS (failure path → ILB steal → late-create)
- Finds D: DCB, splices our calldown above ESDI_506
- Configures HDD mode

Then Win98 crashes ~30s later:
```
NTMINI: HDD splice into D: DCB %08lx (above ESDI_506)
NTMINI: HDD device entry: target=%d dcb=%08lx
NTMINI: HDD mode — skipping FSD/IFS registration
g   ← g_ios_registered = 1
z   ← success from .do_ios_register
H T ← AEP_SYSTEM_SHUTDOWN (unusual during boot)
m00000005 (Sys_VM_Init2/System_Exit)
m00000006 (Sys_VM_Terminate) ← WPE
```

No `W` markers from `ios_wdm_ior_entry` — our calldown handler was never invoked before the crash. The splice itself may corrupt IOS state.

## Bisected: Splice Into Active DCB Is The Crash

**Confirmed by experiment:**
- With test HDD + late-create + splice: crash at T+30 (QEMU exits, "Guest has not initialized display")
- With test HDD + late-create + splice NOP'd: survives to T+85+ (60s longer)
- Without test HDD (CD-ROM path, no HDD splice): survives indefinitely
- Without late-create entirely: survives indefinitely, boots to desktop

**The ISP_INSERT_CALLDOWN into D:'s active DCB during Init_Complete is the crash cause.**
IOS doesn't expect calldown chain modifications on a live DCB at this point.

**Potential fixes:**
1. **Defer splice to AEP_BOOT_COMPLETE** — splice after boot is done, D: is idle
2. **Use proper ISP flags** — the splice uses flags=0x00000100 which is DCB_dmd_not_512, not an ISP calldown flag. The proper AEP_CONFIG_DCB path (line 3544) uses ISPCDF_BOTTOM | ISPCDF_PORT_DRIVER.
3. **Use AEP_CONFIG_DCB broadcast** — instead of manual splice, broadcast a CONFIG_DCB AEP for our DCB. The HDD path skips this (comment: "ESDI_506 probes the IDE channel"), but maybe with modified AEP parameters it can work.
4. **Direct calldown struct write** — instead of ISP_INSERT_CALLDOWN, directly modify the DCB's calldown list (risky, requires knowing the exact memory layout)

## Hypotheses (original, partially resolved)

### H1: Calldown function pointer is wrong
The splice uses `ios_wdm_preserve_stub` as the calldown function (ASM thunk that calls `ios_wdm_ior_entry`). If the function pointer was corrupted by fixup translation or the merged-object relocation, IOS would crash trying to call through it.

**Test**: Add a debug breadcrumb at the very start of `ios_wdm_preserve_stub` and check if it appears before the crash.

### H2: DCB internal state corrupted by splice
`ISP_INSERT_CALLDOWN` modifies the DCB's calldown list. If our DDB pointer or calldown entry structure is invalid, IOS could crash when walking the chain for any I/O on D:.

**Test**: Compare the DCB calldown chain before and after our splice (hex dump DCB+calldown offsets).

### H3: AEP_SYSTEM_SHUTDOWN during boot is a red flag
The `H T` markers show `aep_handler` receiving `AEP_SYSTEM_SHUTDOWN` right after registration. This normally only happens at shutdown. Its presence during boot suggests IOS detected an error and is shutting down.

**Test**: Check IOS error state after `IOS_Register`. Print `DRP.reg_result` and any error codes.

### H4: The late-create path has a known DCB corruption bug
The `ios_late_create_device()` function was designed for the CD-ROM use case. The HDD adaptation may have incorrect DCB type, geometry, or partition information.

**Test**: Compare the DCB we create/modify against ESDI_506's DCB for the same drive.

## Environment Setup

**CRITICAL**: Fresh qcow2 conversion needs ONE GUI boot to dismiss "Add New Hardware Wizard" before VxDs produce debug output.

```bash
# First-time setup (only needed after qcow2 conversion):
qemu-img convert -f qcow2 -O raw ~/Documents/VMs/win98vm/en_win98_se.qcow2 ~/Documents/VMs/win98vm/win98.img
# Run ScanDisk suppression (see predecessor contprompt for mmap script)
# Boot with -monitor telnet:127.0.0.1:4444,server,nowait
# Wait 90s, send: echo "sendkey esc" | nc 127.0.0.1 4444 (repeat 5x)
# Then: echo "quit" | nc 127.0.0.1 4444
# Clear FAT dirty flags via deploy_sysini.py
```

## Deploy + Test Pipeline

```bash
cd /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport

# Build (includes static DRP, harmless in SYSTEM.INI mode)
python3 src/build_nt5_fixed.py

# Deploy via SYSTEM.INI (NOT --pdr!)
python3 src/deploy_sysini.py binaries/NT5FIXED.VXD

# Test HDD
python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img
rm -f /tmp/win98vm/test_cd.iso

# Boot
DISK="$HOME/Documents/VMs/win98vm/win98.img"
rm -f /tmp/nt5dbg_test.log
qemu-system-i386 -m 256 -M pc -cpu pentium2 \
  -drive "file=$DISK,format=raw" -boot c -vga std \
  -rtc base=localtime -display none \
  -debugcon file:/tmp/nt5dbg_test.log \
  -monitor telnet:127.0.0.1:4444,server,nowait \
  -drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw" &
```

## Key Files

| File | Purpose |
|------|---------|
| `src/build_nt5_fixed.py` | LE fixup + static DRP — FIXED, includes bisection results |
| `history/source/VXDWRAP_NASM.ASM` | Control dispatch, DDB. Has Sys_Critical_Init→ios_register (196b66e) but NT5RAW.VXD not compiled from it |
| `history/source/IOSBRIDGE.C:2436` | `ios_register_port_driver()` — the if/else that skips late-create on success |
| `history/source/IOSBRIDGE.C:2662` | `ios_late_create_device()` — creates DCBs, splices calldown |
| `history/source/IOSBRIDGE.C:3773` | `ios_wdm_ior_entry()` — calldown handler (never reached before crash) |
| `history/source/IOSBRIDGE.C:3879` | `ata_direct_ior()` — ATA I/O for HDD reads |

## Binary Patches Applied (temporary, need Docker to make permanent)

1. **Late-create JE patch**: NT5RAW.VXD offset 0xF4D6, byte 0xBE→0xA0. Changes the success-path JE to jump to `ios_late_create_device()` instead of skipping it. **Currently reverted** — the unpatched NT5RAW.VXD is restored. Re-apply for testing:
   ```python
   data = bytearray(open('history/source/NT5RAW.VXD', 'rb').read())
   assert data[0xF4D6] == 0xBE
   data[0xF4D6] = 0xA0
   open('history/source/NT5RAW.VXD', 'wb').write(data)
   ```

## Docker Required For

- Recompile NT5RAW.VXD from current ASM/C sources
- `bash history/source/build.sh nt5`
- Current NT5RAW.VXD compiled from pre-196b66e sources (no Sys_Critical_Init IOS call in binary)
- Proper fix for late-create: modify IOSBRIDGE.C to always call `ios_late_create_device()` after `IOS_Register`

## What NOT to Retry

- PDR mode (`--pdr`) for loading — PDRs never get Init_Complete
- `ios_register_port_driver()` from Sys_Critical_Init — crashes (nt5_init too early)
- Static DRP via DDB_Reference_Data for PDR discovery — IOS doesn't use this mechanism
- Build script with WRITABLE obj_flags (0x2047) — crashes
- Build script with SHARED entry_flags (0x03) — crashes
- Build script with hardcoded NTMINI resident name — crashes
- Fresh qcow2 images without first-boot Hardware Wizard dismissal — no debugcon output

## SREAD.EXE Expected Success Output

```
SREAD_START
ATTR=0x00000020
CREATE_HANDLE=0x0000000C
READ_OK=0x00000001 READ_N=0x00000096 READ_LE=0x00000000
TEXT_BEGIN
HDD_SECOND_TXT: This file was read from the FAT16 hard disk image
TEXT_END
```
