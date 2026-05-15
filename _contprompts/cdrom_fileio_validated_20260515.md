---
stepsCompleted: 1
status: IN_PROGRESS
session_date: 2026-05-15
---

# CD-ROM File I/O Validated — Next Steps (2026-05-15)

## What Was Achieved (Previous Session)

NT5 WDM atapi.sys (Windows 2000 driver) now serves real CD-ROM file I/O through Win98's IFS layer. Both SREAD.EXE and WREAD.EXE read correct ISO 9660 file content (391 bytes each) on a restored Win98 SE disk with no native CD-ROM support.

### Three-Layer Fix (commit `64044bd`)

1. **IOS calldown splice**: `ios_wdm_ior_entry` inserted into D:'s existing DCB chain via `ISP_INSERT_CALLDOWN`. BRIDGE_DEVICE entry reads real DCB fields. Chain walk verified with `*` marker.

2. **IFS hook at init**: `IFSMgr_InstallFSHook` moved from lazy (inside `ntmini_fsd_open`) to init time (after `IFSMgr_RegisterMount`). Without this, `GetFileAttributesA` never reaches the FSD.

3. **IOS_BD_Command_Complete for internal IOPs**: `IFSMgr_CDROM_Attach` triggers IOS internal probes. `IOS_BD_Complete_IOP` deadlocks (recursive chain). `IOS_BD_Command_Complete` (IOR-level) avoids re-entry. Also: `ios_schedule_late_cdrom_attach_probe()` called after FSD registration (was dead code).

### Other Fixes (commit `4f0f639`)
- `deploy_sysini.py`: BPB dirty byte was set to 0x03 (dirty+error) instead of 0x00 (clean). Win98 triggers ScanDisk on 0x03.

### 4 Commits Pushed to `origin/main`
- `64044bd` — Three-layer IOS/IFS/CDROM fix
- `b1dea6e` — Contprompt documentation
- `8fbd408` — ATA HDD test contprompt
- `4f0f639` — BPB dirty byte fix

## Current State

- **VxD binary**: `binaries/NT5FIXED.VXD` (175KB, built from `history/source/`)
- **Build**: Docker + Open Watcom v2 + NASM. `history/source/build.sh nt5` then `python3 src/build_nt5_fixed.py` then `python3 src/deploy_sysini.py binaries/NT5FIXED.VXD`
- **QEMU**: `-cpu pentium2 -m 256` required. Runner: `artifacts_search_ctx_instrument_20260514_143819/run_unix_once.sh`
- **Disk**: Restored from `~/Documents/VMs/win98vm/en_win98_se.qcow2`. ScanDisk runs on first boot (~150s). Empty CONFIG.SYS/AUTOEXEC.BAT (no native CD-ROM).
- **IRQHOOK**: `#if 0` in `ntk_IoConnectInterrupt`. Polling mode works. VPICD virtualization crashes on restored disk.
- **SREAD timing**: First run sometimes gets mount=1 (hardware wizard delays desktop). Second run reliably gets mount=2 + full file I/O. An `IFSMgr_NotifyVolumeArrival` call from timer context crashes boot — don't do that.

## Next Steps (Priority Order)

### 1. Suppress Hardware Wizard on Boot
The PCI bus scan in our VxD creates device nodes that trigger Win98's "Add New Hardware Wizard". This blocks the desktop, delaying `run=` programs. Options:
- Delete the PnP device registry entries after boot (via disk_ops.py pre-deployment)
- Suppress `IoReportDetectedDevice` calls
- Pre-answer the wizard by creating appropriate registry entries

### 2. ATA Hard Disk Testing
Code at commit `deea226`. Needs:
- Test HDD image with proper CHS geometry (raw FAT16 at IDE index=3 crashes Win98 boot)
- Bridge context `DeviceType` switching (0x05 CD-ROM vs 0x00 disk)
- 512-byte sector translation
- WARNING: attaching a bad HDD image corrupts Win98's hardware state, requiring full disk restore

### 3. IRQHOOK Debugging
Enable `#if 0` block in `ntk_IoConnectInterrupt`. The VPICD hook installs but stray IRQs crash before ISR bridge is ready. Potential fix: defer VPICD hook until after StartDevice completes and first I/O succeeds.

### 4. Multi-Device Support
Can both CD-ROM (D:) and ATA HDD (E:?) work simultaneously? Currently single-device bridge.

## Session Progress (2026-05-15)

### Priority 1: DONE — First-Boot Reliability
- **Root cause**: wizard is Win98 PCI re-enumeration (VGA adapter), not our VxD code
- **BPB clean_bpb** added to `disk_ops.py` and `run_unix_once.sh` — prevents ScanDisk
- **Removed Alt-F4** from runner dismiss loop — was opening Win98 shutdown dialog!
- **Escape-only** dismiss loop from T+60-240s is safe and sufficient
- **Screenshots at checkpoints** added to runner for diagnostics
- **Smoke test retry** (5 attempts, 500ms delays) in NT5LOADER.C — handles ATAPI spin-up
- **Validated**: SREAD.EXE reads correct 391-byte ISO file on first boot from fresh restore
- Commits: `c8b4004` (smoke test retry + create_test_hdd.py)

### Priority 2: IN PROGRESS — ATA HDD Testing
- **Proper FAT16 HDD image**: `src/create_test_hdd.py` creates 16MB image with correct CHS geometry (32/16/63)
- **Win98 boots with HDD attached**: no crash (old bare-MBR caused hangs)
- **CD-ROM still works with HDD present**: smoke test passes, SREAD reads correctly
- **First boot with HDD is slow**: ~300s at splash screen (ScanDisk/hardware detection)
- **HDD not detected by bridge**: ESDI_506.PDR claims the HDD before our port driver sees it
- **DCB_TYPE_DISK filter alone causes BSOD**: commit `deea226` had 3 changes, not just the filter:
  1. Filter: accept DCB_TYPE_DISK in aep_config_dcb
  2. ISO PVD bypass: skip ISO caching for ATA disks (DeviceType=0x00)
  3. ATAPI vs ATA detection: is_atapi=FALSE for device_type 0x00
  All three must be applied together. Filter alone crashes because ISO PVD caching runs on non-CD device.
- **Reverted**: DCB filter commit reverted (`b68bf9f`) to restore CD-ROM stability
- **Next**: cherry-pick all 3 IOSBRIDGE.C changes from deea226 + NT5LOADER.C changes

### Priority 3: ANALYZED — IRQHOOK
- `#if 0` at NTKSHIM.C:1840 disables VPICD virtualization
- Proposed: `ntk_ConnectDeferredVpicd()` after IoConnectInterrupt, before first I/O
- NT5LOADER.C ~line 786 is the safe insertion point
- Not yet implemented

## Key Files
- `history/source/IOSBRIDGE.C` — IOS bridge, FSD hooks, calldown splice (4700+ lines)
- `history/source/NTKSHIM.C` — ntoskrnl/HAL shim (3200 lines)
- `history/source/NT5LOADER.C` — PE loader + smoke test (1600 lines)
- `history/source/IRQHOOK.C` — VPICD IRQ virtualization (400 lines)
- `src/deploy_sysini.py` — FAT32 deployment to disk image
- `artifacts_search_ctx_instrument_20260514_143819/` — QEMU test harness + guest EXEs

## Test Validation Command
```bash
ART=artifacts_search_ctx_instrument_20260514_143819
bash "$ART/run_unix_once.sh" "$ART" "TEST_NAME" "WREAD.EXE" "WREAD.TXT" 300
# Check: guest-output/WREAD.TXT should be 391 bytes with ATTR=0x00000001, READ_OK, WREAD_END
```
