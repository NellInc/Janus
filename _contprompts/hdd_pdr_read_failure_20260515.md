---
stepsCompleted: 0
status: READY
predecessor: hdd_filesystem_routing_20260515.md
session_date: 2026-05-15
---

# HDD PDR Read Failure — Diagnose and Fix (2026-05-15 night)

## Goal

Get SREAD.EXE to read 150 bytes ("HDD_SECOND_TXT:...") from D:\SECOND.TXT on a FAT16 test hard disk, served by our VxD's `ata_direct_ior` through the Win98 IOS calldown chain.

## What Was Achieved (Previous Sessions)

### Hardware I/O: PROVEN
Direct ATA port I/O reads correct data from the test HDD (secondary IDE slave, ports 0x170-0x177):
- FAT16 boot sector at LBA 63: byte 0 = 0xEB, offset 54 = "FAT16" ✓
- SECOND.TXT content at LBA 136: "HDD_SECOND_TXT:..." ✓
- Validation code in NT5LOADER.C after MBR detection (commit a39ad83)

### Timing Problem: SOLVED via PDR loading
Win98 IOS completes device enumeration before SYSTEM.INI VxDs load. Solution: deploy as a .PDR to IOSUBSYS so the driver loads during IOS init, alongside ESDI_506.

- `build_nt5_fixed.py` now produces IOS-compatible LE layout (page data at `ios_dp = align_up(et_off, 32)` — "IOS dp matches page data: YES")
- `deploy_sysini.py --pdr` deploys to `WINDOWS\SYSTEM\IOSUBSYS\NTMINI.PDR`
- `DRP_LGN` changed from `1 << 0x16` (ESDI_506 level) to `1 << 0x15` (DRP_NT_PD, higher priority)
- IOS_Register enabled (was previously skipped with `result = 0`)

### V26 PDR Test Result: FILE FOUND, READ FAILS
```
SREAD_START
ATTR=0x00000020 ATTR_LE=0x00000000       ← file exists! archive attribute
CREATE_HANDLE=0x0000000C CREATE_LE=0x00000000  ← file opened OK
READ_OK=0x00000000 READ_N=0x00000000 READ_LE=0x0000001F  ← ReadFile FAILED, 0 bytes, error 31
```
ERROR_GEN_FAILURE (31) = generic I/O failure during the read. The filesystem mounted, the directory entry exists, but the sector read failed.

**VxD debug log was EMPTY (0 bytes at all checkpoints).** Either:
1. PDR-loaded VxDs don't output to port 0xE9 (QEMU -debugcon) by default
2. The PDR loaded but crashed before any dbg_mark calls
3. The PDR didn't fully initialize

### What Was Tried and Failed (for context, don't retry)
- **IFSMgr_NotifyVolumeArrival(3)** after splice: call returns instantly, VFAT doesn't re-read
- **Orphan DCB + ISP_ASSOCIATE_DCB**: letter assigned (F:) but no filesystem mounts on it
- **HDD splice into D: chain** (SYSTEM.INI load): too late, VFAT already mounted via ESDI_506
- **ESDI_506 suppression** (SYSTEM.INI load): IOS enumeration already done
- **ISO FSD was intercepting ALL drive letters** — gated on CD-ROM mode only (commit 52869e8)

## Current State

### Build
```bash
cd /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport
bash history/source/build.sh nt5           # compile
python3 src/build_nt5_fixed.py             # LE fixup (IOS-compatible)
```

### Deploy (PDR mode)
```bash
qemu-img convert -f qcow2 -O raw ~/Documents/VMs/win98vm/en_win98_se.qcow2 ~/Documents/VMs/win98vm/win98.img
python3 src/deploy_sysini.py binaries/NT5FIXED.VXD --pdr
python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img
rm -f /tmp/win98vm/test_cd.iso   # HDD-only mode
```

### Test
```bash
ART=artifacts_search_ctx_instrument_20260514_143819
bash "$ART/run_unix_once.sh" "$ART" "TEST_NAME" "SREAD.EXE" "SREAD.TXT" 540
# Check: guest-output/SREAD.TXT for content
# Check: TEST_NAME/vxd_debug.final.log for markers
```

### Key Files
- `history/source/IOSBRIDGE.C` — IOS bridge, calldown splice, ata_direct_ior (~5220 lines)
- `history/source/NT5LOADER.C` — PE loader, MBR detection, FAT16 validation (~1750 lines)
- `history/source/NTKSHIM.C` — ntoskrnl/HAL shim (~3200 lines)
- `src/build_nt5_fixed.py` — LE fixup, produces IOS-compatible layout
- `src/deploy_sysini.py` — FAT32 deployment (--pdr for IOSUBSYS)
- `src/create_test_hdd.py` — 16MB FAT16 test HDD image
- `artifacts_.../run_unix_once.sh` — QEMU test runner

### Test HDD Layout
- 16MB, CHS 32/16/63, FAT16 partition at LBA 63
- Partition: reserved=1, FATs=2, FAT size=16, root entries=512
- Data start: LBA 128 (relative to partition: sector 65)
- Cluster 2 (TESTFILE.TXT, 105 bytes): LBA 128
- Cluster 3 (SECOND.TXT, 150 bytes): LBA 136
- Content: "HDD_SECOND_TXT: This file was read from the FAT16 hard disk image"

### Architecture
```
SREAD.EXE → GetFileAttributesA("D:\SECOND.TXT") → VFAT
  → IOS calldown chain for D: DCB
    [0] TSD (type-specific driver)
    [1] Our ios_wdm_ior_entry → ior_handler → ata_direct_ior
    [2] ESDI_506 (if present, below us)
  → ata_direct_ior: programs IDE registers 0x170-0x177
  → reads sectors from secondary slave → fills IOR buffer
  → complete_ior(IORS_SUCCESS) → VFAT returns data to SREAD
```

### IOSBRIDGE.C Key Functions (line numbers approximate, may shift)
- `ios_register_port_driver()` ~2440 — DRP setup, IOS_Register call
- `ios_late_create_device()` ~2637 — DCB creation, calldown insert, probe loop, HDD splice
- `aep_handler()` / `aep_config_dcb()` ~3380 — AEP dispatch, DCB claiming
- `ios_wdm_ior_entry()` ~3717 — IOS calldown entry point (receives IOP)
- `ior_handler()` ~3920 — dispatch: DCB_TYPE_DISK → ata_direct_ior, CD-ROM → miniport
- `ata_direct_ior()` ~3879 — direct IDE port I/O (channel-aware: primary/secondary)
- `complete_ior()` ~4971 — sets status, calls IOS_BD_Command_Complete

### Debug Markers
- `W` + `<func>` + `>` → ios_wdm_ior_entry called with IOR function
- `!` → ior_handler entry
- `D` + hex16(func) + hex32(lba) + hex8(channel) → disk I/O dispatch
- `$` → BSY timeout in ata_direct_ior
- `~` → drive select timeout
- `E` → DRQ wait failed
- `V` + hex bytes → FAT16 boot sector validated
- `k`/`K` → IFSMgr_NotifyVolumeArrival entry/return
- `H S` → HDD splice entry
- `f` → HDD mode, FSD/IFS registration skipped

## Next Steps (Priority Order)

### Step 1: Fix VxD debug output for PDR mode
The V26 debug log was 0 bytes. Without debug output, we can't diagnose the read failure.

Hypothesis: PDR-loaded VxDs use `dbg_mark` (port 0xE9 writes) same as SYSTEM.INI VxDs. The empty log might mean:
a) The PDR loaded but crashed during `ntmini_device_init()` before any output
b) QEMU's `-debugcon file:PATH` isn't capturing port 0xE9 from PDR-loaded code
c) The PDR wasn't loaded at all (LE header issue)

Test: Add a VERY EARLY debug marker at the start of the VxD init — before any complex code. The VxD's DDB control dispatcher receives `Device_Init` (or `Dynamic_Init` for PDR). Find where `ntmini_device_init` is called from the DDB and add `dbg_mark('Z')` as the very first instruction.

If `Z` appears in the log: PDR loaded, init started. Crash is later.
If log still empty: PDR didn't load. Check LE header more carefully.

### Step 2: Diagnose the READ GEN_FAILURE
Once debug output works, run the test again. Look for:
- `D` markers in the log → ata_direct_ior was called with specific LBA/count
- `$` or `~` or `E` → ATA command failed
- No `D` markers → ior_handler never dispatched to ata_direct_ior (device entry missing or type mismatch)

The READ GEN_FAILURE (error 31) maps to `IORS_CMD_FAILED` in IOS. Check which code path in ata_direct_ior returns this status.

### Step 3: Fix the read path
Based on Step 2 diagnostics:
- If LBA is wrong: check IOS partition offset translation (IOR_start_addr may be partition-relative)
- If ATA command fails: check drive select byte (master vs slave), BSY state, DRQ timing
- If device entry missing: check aep_config_dcb ran for D:'s DCB with our DRP_NT_PD priority
- If ior_handler never fires: check ios_wdm_ior_entry DCB lookup path

### Step 4: Validate end-to-end
SREAD output should show:
```
READ_OK=0x00000001 READ_N=0x00000096 READ_LE=0x00000000
TEXT_BEGIN
HDD_SECOND_TXT: This file was read from the FAT16 hard disk image
TEXT_END
```
150 bytes (0x96) of HDD-specific content = complete validation.

### Step 5: CD-ROM regression test
Deploy with --pdr, add test CD ISO, verify SREAD still reads CD-ROM content from D: when CD is present. The bridge should detect ISO 9660 in smoke test and switch to CD-ROM mode.

## Also Available: SCSI Miniport Path

A patched QEMU 10.2.2 with LSI controller fixes is built at `/tmp/qemu-patched/qemu-10.2.2/build/qemu-system-i386`. The SCSI miniport port plan is at `_contprompts/scsi_miniport_port_plan_20260515.md`. This is a parallel path that avoids IDE/ESDI_506 conflicts entirely but requires porting ~15 VXDWRAP.ASM externs from PA repo. Defer unless the PDR approach fails.

## Commits (most recent session)
```
275c598 SCSI: IOS-compatible PDR build + DRP_NT_PD priority + PDR deploy to IOSUBSYS
e280cc4 SCSI: Three-approach HDD filesystem routing — all tested, timing blocker found
a39ad83 SCSI: VALIDATED — direct ATA reads FAT16 boot sector + SECOND.TXT from test HDD
52869e8 SCSI: HDD splice into D: chain above ESDI_506, gate ISO FSD on CD-ROM mode
10e057a SCSI: Fix DCB probe — type-filtered drive letter scan, orphan DCB routing for HDD mode
```
