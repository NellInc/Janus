---
stepsCompleted: 0
status: READY
predecessor: hdd_pdr_read_failure_20260515.md
session_date: 2026-05-16
---

# HDD PDR: ScanDisk Suppression + VxD Load Verification (2026-05-16)

## Goal

Get the VxD to LOAD (confirmed by debug output) and then diagnose + fix the HDD read failure.

## What Was Discovered (2026-05-15 night session)

### Root Cause of 0-byte Debug Log: ScanDisk, NOT VxD Binary

The V26 PDR test's 0-byte debug log was caused by **ScanDisk blocking Win98 boot**, not by the VxD failing to load. `qemu-img convert` from the qcow2 always produces an image with dirty shutdown markers. ScanDisk takes 20-30 minutes to scan the 8GB disk in QEMU, which exceeds the test timeout.

Evidence:
- Screenshots show ScanDisk text mode at T+100s, still at 0% complete at T+220s
- Even a PRISTINE image (no VxD) shows ScanDisk blocking
- `AutoScan=0` in MSDOS.SYS does NOT prevent ScanDisk on this Win98SE build
- Earlier working tests (SREAD_WITH_HDD at 13:59) used a disk image from a PREVIOUS clean shutdown — never hit ScanDisk

### LE Binary Fixes Applied (may or may not be needed)

These changes were made to `build_nt5_fixed.py` and `VXDWRAP_NASM.ASM` but haven't been validated yet because ScanDisk prevented booting:

1. **preload_pages: 0 → NP (40)** — ESDI_506 preloads 2; we preload all for single merged object
2. **Object flags: 0x2045 → 0x2047** — added WRITABLE bit for merged code+data object
3. **Entry table flags: 0x01 → 0x03** — matching ESDI_506 (exported+shared)
4. **Resident name: "NT5RAW" → "NTMINI"** — matching DDB name
5. **Dynamic init → IOS_Register** — `.do_init` now falls through to `.do_ios_register` so PDRs (which get `Sys_Dynamic_Device_Init` not `Init_Complete`) also call `ios_register_port_driver()`

### Key Technical Findings

- **DDB_Control_Proc = 0 in file is NORMAL** — relocated by fixup records at load time. Both ESDI_506 and NTMINI have this.
- **ESDI_506's layout**: loader → fixups → pages (standard LE). NTMINI's layout: loader → pages → fixups (non-standard but offsets in header are correct, so IOS/VXDLDR reads them correctly).
- **"IOS dp formula"** (`align_up(et_off, 32)`) is project-local shortcut, NOT how IOS actually locates pages. IOS reads `data_pages_off` from LE+0x80 directly.
- **IOS loads ALL .PDR files from IOSUBSYS** via VXDLDR (no registry required).
- **VXDLDR sends `Sys_Dynamic_Device_Init` to PDRs**, not `Init_Complete`.

### ScanDisk Suppression Applied to Disk Image

1. Deleted directory entries for SCANDISK.EXE, SCANDSKW.EXE, SCANDISK.INI, SCANDISK.PIF
2. Renamed DSKMAINT.DLL → DSKMAINX.DLL (IO.SYS calls this)
3. FAT dirty flags cleared via deploy_sysini.py

### Disk Image State

After `qemu-img convert`:
- Must run `disk_ops.py clean_bpb` AND `deploy_sysini.py` (clears FAT dirty flags)
- Must suppress ScanDisk (delete SCANDISK.EXE, rename DSKMAINT.DLL)
- NTMINI.PDR deployed to IOSUBSYS (182,990 bytes, verified on disk)

## Definitive Finding: PDR Crashes, SYSTEM.INI Works

V29 (modified VxD) and V30 (original VxD) BOTH crash when deployed as PDR.
- Splash screen appears at 60s (ScanDisk suppressed — working!)
- System reboots at ~100s (VxD crash during VXDLDR loading)
- SeaBIOS "Booting from Hard Disk..." at 120s (reboot confirmed)
- Debug log: 0 bytes (crash before first port 0xE9 write)

Earlier SYSTEM.INI tests (SREAD_WITH_HDD at 13:59) produced `NT!` heartbeats successfully.
**The crash is PDR-specific.** VXDLDR handles the binary differently from VMM's static loader.

Root cause hypothesis: ~~LE layout mismatch~~ **DISPROVEN** — V31B tested with ESDI-compatible layout (fixups before pages), same crash. The layout is not the issue. Something else about our 183KB merged single-object VxD causes VXDLDR to crash. Candidates:
- **Single merged object too large** (40 pages = 160KB, vs ESDI's 4 objects totalling 6 pages)
- **Fixup records corrupt** — object merger translates fixup targets, but a bug could produce invalid entries
- **DDB Control_Proc relocation** — the fixup for DDB+0x18 targets obj1 offset 0, which is `NTMINI_Control`. If the relocation produces a wrong address, the control proc call crashes
- **preload_pages = 40** is too large for VXDLDR's page preload buffer

## Next Steps

### Step 1: DONE — Layout restructured (fixups before pages)
V31B tested, crash persists. Layout is NOT the issue.

### Step 2: Confirm SYSTEM.INI path still works
Deploy via `deploy_sysini.py binaries/NT5FIXED.VXD` (non-PDR mode) on the ScanDisk-suppressed image. If `NT!` heartbeats appear, the VxD binary is valid — the issue is PDR-loading-specific.

### Step 3: Bisect the PDR crash
Possible causes (test one at a time):
a) **preload_pages = 40** — try preload_pages = 2 (matching ESDI) instead of NP
b) **Single merged object** — ESDI has 4 objects; maybe VXDLDR can't handle 40-page single object
c) **Fixup corruption** — dump and validate fixup records, compare relocation results with SYSTEM.INI-loaded VxD's memory map
d) **VxD too large** — 183KB is much larger than typical PDRs (ESDI is 24KB); VXDLDR may have a size limit

### Step 4: Validate via minimal PDR
Create a TINY test VxD (just DDB + heartbeat, no C code) and deploy as PDR. If it loads, the issue is size/complexity. If it crashes, VXDLDR fundamentally can't load our LE format.

### Step 5: (if VxD loads via SYSTEM.INI) Diagnose READ failure
Once VxD loads, look for debug markers in the calldown chain.

### Step 3: Diagnose READ GEN_FAILURE
Once VxD loads, look for:
- `D` markers → ata_direct_ior was called
- `$`/`~`/`E` → ATA command failed
- No markers → ior_handler never dispatched

### Step 4: HDD must be on IDE (not SCSI)
The test runner uses LSI SCSI when available. Force IDE fallback:
```bash
HDD_ARGS=(-drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw")
```
Our `ata_direct_ior` programs IDE ports 0x170-0x177 (secondary channel). SCSI is a different path entirely.

## Build & Deploy

```bash
cd /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport

# 1. Fresh disk image
qemu-img convert -f qcow2 -O raw ~/Documents/VMs/win98vm/en_win98_se.qcow2 ~/Documents/VMs/win98vm/win98.img

# 2. Suppress ScanDisk (run the mmap script — see Step 1 notes)

# 3. Build + deploy
bash history/source/build.sh nt5
python3 src/build_nt5_fixed.py
python3 src/deploy_sysini.py binaries/NT5FIXED.VXD --pdr

# 4. Test HDD
python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img
rm -f /tmp/win98vm/test_cd.iso

# 5. Run test (force IDE for HDD)
ART=artifacts_search_ctx_instrument_20260514_143819
# Edit run_unix_once.sh to force IDE, then:
bash "$ART/run_unix_once.sh" "$ART" "TEST_NAME" "SREAD.EXE" "SREAD.TXT" 540
```

## Key Files (updated line numbers)

- `src/build_nt5_fixed.py` — LE fixup with preload/writable/flags fixes
- `history/source/VXDWRAP_NASM.ASM` — NASM wrapper with dynamic init → IOS_Register fix
- `history/source/IOSBRIDGE.C:3241` — aep_handler (has `dbg_mark('H')` at entry)
- `history/source/IOSBRIDGE.C:2375` — ntmini_device_init
- `history/source/IOSBRIDGE.C:2436` — ios_register_port_driver

## Commits

```
163c69d SCSI: Contprompt for next session — PDR read failure diagnosis
275c598 SCSI: IOS-compatible PDR build + DRP_NT_PD priority + PDR deploy to IOSUBSYS
```
