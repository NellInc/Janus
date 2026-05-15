---
stepsCompleted: 0
status: READY
predecessor: cdrom_fileio_validated_20260515.md
session_date: 2026-05-15
---

# ATA HDD + IRQHOOK + Multi-Device — Next Steps (2026-05-15)

## What Was Achieved (Previous Session)

### Priority 1: DONE — First-Boot CD-ROM Reliability
NT5 WDM atapi.sys serves real CD-ROM file I/O on first boot from fresh restore. SREAD.EXE reads 391 bytes ("SECOND FILE FROM REAL ISO") correctly.

**Fixes committed (c8b4004, ae148e6):**
- `clean_bpb` in `disk_ops.py` + `run_unix_once.sh` — prevents ScanDisk on boot
- Escape-only dismiss loop (T+60 to T+240, 10s intervals) — Alt-F4 was opening Win98's shutdown dialog!
- Screenshots at every checkpoint (T+60/120/180/240/300/360) in `run_unix_once.sh`
- Smoke test retry (5 attempts, 500ms delays) in NT5LOADER.C for ATAPI spin-up
- Hardware wizard ("New Hardware Found: PCI VGA-Compatible Display Adapter") auto-resolves — it's Win98 PCI re-enumeration, not our VxD code

### Priority 2: IN PROGRESS — ATA HDD Support
Full HDD code cherry-picked from commit `deea226` and validated (ae148e6):
- `aep_config_dcb`: accepts `DCB_TYPE_DISK` alongside CD-ROM
- `ntmini_device_init`: skips ISO PVD caching for ATA disks (DeviceType 0x00)
- `bridge_enumerate_devices`: `is_atapi=FALSE` for device_type 0x00
- NT5LOADER: `nt5_read_lba_ex` with configurable sector size, MBR detection at LBA 0
- Proper FAT16 test HDD image: `src/create_test_hdd.py` (16MB, CHS 32/16/63, TESTFILE.TXT)

**Validated:** No BSOD with HDD attached, CD-ROM still works (391-byte SREAD correct).

**NOT yet validated:** HDD I/O through the bridge. Win98's ESDI_506.PDR claims the HDD before our port driver sees it, so the bridge never gets the HDD DCB.

### Priority 3: ANALYZED — IRQHOOK
`#if 0` at NTKSHIM.C:1840 disables VPICD virtualization. Proposed deferred hook not yet implemented.

## Current State

- **VxD binary**: `binaries/NT5FIXED.VXD` (~178KB)
- **Build**: `bash history/source/build.sh nt5` then `python3 src/build_nt5_fixed.py` then `python3 src/deploy_sysini.py binaries/NT5FIXED.VXD`
- **QEMU**: `-cpu pentium2 -m 256`. Runner: `artifacts_search_ctx_instrument_20260514_143819/run_unix_once.sh`
- **Disk**: Restore from `~/Documents/VMs/win98vm/en_win98_se.qcow2` via `qemu-img convert -f qcow2 -O raw`
- **Test CD ISO**: `/tmp/win98vm/test_cd.iso` (921KB, ISO 9660 with TESTFILE.TXT)
- **Test HDD**: `python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img` (16MB FAT16, TESTFILE.TXT 105 bytes)
- **IRQHOOK**: `#if 0` in `ntk_IoConnectInterrupt`. Polling mode works.
- **Runner improvements**: BPB clean, Escape-only dismiss, screenshots at checkpoints, all in `artifacts_search_ctx_instrument_20260514_143819/run_unix_once.sh`
- **Boot timing with HDD**: first boot ~400s (ScanDisk/hardware detection on new HDD adds ~200s), second boot ~120s

## Commits Pushed (6 total this session)
```
ae148e6 SCSI: Full ATA hard disk support — cherry-pick from deea226
0f6d5a4 SCSI: Update contprompt with session progress
b68bf9f Revert "SCSI: Restore DCB_TYPE_DISK filter from deea226"
f5415ed SCSI: Restore DCB_TYPE_DISK filter from deea226
c8b4004 SCSI: Smoke test retry + first-boot reliability fixes
9dba071 SCSI: Contprompt for next session — CD-ROM file I/O validated
```

## Next Steps (Priority Order)

### 1. Test HDD-Only Configuration (No CD-ROM)
The MBR detection path (`nt5_read_lba_ex(0, mbr_buf, 512)` checking for 0x55AA) only triggers when the ISO smoke test FAILS. To test HDD I/O, either:
- **Option A**: Remove the test CD ISO (`rm /tmp/win98vm/test_cd.iso`) and boot with only the HDD. The smoke test will fail (no CD001), then MBR detection will fire, bridge will set DeviceType=0x00 and SectorSize=512.
- **Option B**: Keep CD ISO but also test with a modified SREAD that reads from the HDD drive letter (E: or F:).

Option A is simpler — validates the MBR detection + 512-byte sector path without needing a new guest EXE.

**Test command:**
```bash
rm /tmp/win98vm/test_cd.iso  # Force MBR path
ART=artifacts_search_ctx_instrument_20260514_143819
bash "$ART/run_unix_once.sh" "$ART" "HDD_ONLY_TEST" "SREAD.EXE" "SREAD.TXT" 540
# NOTE: SREAD.EXE reads from D:\TESTFILE.TXT. With HDD-only, D: may be the HDD.
# Check VxD debug for "MBR detected, device is ATA disk" marker.
```

**Caveat**: SREAD.EXE hardcodes `D:\TESTFILE.TXT` as the target path. With HDD-only, Win98 might assign D: to the HDD partition. The test HDD's FAT16 has `TESTFILE.TXT` with 105 bytes content ("NTMINI HDD TEST DATA..."). If SREAD reads 105 bytes instead of 26 bytes, the HDD I/O path is working.

### 2. ESDI_506.PDR Suppression (If HDD-Only Doesn't Work)
If Win98's ESDI_506.PDR still claims the HDD even without a CD-ROM, suppress it:
```bash
# In disk_ops.py or deploy_sysini.py, delete ESDI_506.PDR from IOSUBSYS:
python3 disk_ops.py delete system ESDI_506.PDR
```
Or rename it via deploy_sysini.py to prevent loading. WARNING: this may break Win98's boot disk (C:) if ESDI_506 handles the primary IDE channel too. Test carefully.

### 3. IRQHOOK — Deferred VPICD Hook
**Analysis from this session:**
- `ntk_IoConnectInterrupt` (NTKSHIM.C:1769) has the `#if 0` block at lines 1840-1850
- The disabled code calls `VxD_VPICD_Virtualize_IRQ` with `ntk_IsrTrampoline` as the ISR
- Problem: stray IRQs crash before ISR bridge is ready
- Fix: defer VPICD hook until after `pnp_start_device` completes and ServiceRoutine is set

**Implementation plan:**
1. Add function `ntk_ConnectDeferredVpicd(ULONG irq)` in NTKSHIM.C that:
   - Checks `g_ConnectedInterrupts[irq] != NULL && g_ConnectedInterrupts[irq]->ServiceRoutine != NULL`
   - Calls `VxD_VPICD_Virtualize_IRQ` and sets `ShimConnected`
2. Call it from NT5LOADER.C after `ntk_IoConnectInterrupt` returns (line ~786), before `nt5_test_read`
3. Remove the `#if 0` / `#endif` around the VPICD block (keep it disabled by default, new function replaces it)

**Testing:**
```bash
# After implementing, build and test:
bash history/source/build.sh nt5
python3 src/build_nt5_fixed.py
# Restore, deploy, run without HDD first (CD-ROM only)
# Check: does the VxD boot? Does the smoke test pass? Any crashes?
# Look for "NTMINI IRQ: hooked successfully" in vxd_debug.final.log
```

### 4. Multi-Device Support
Once both CD-ROM and HDD I/O work independently:
- Test simultaneous CD-ROM + HDD with different DCBs
- Bridge uses `g_bridge.devices[MAX_DEVICES=8]` array, supports multiple devices
- Single-SRB serialization (`active_srb` in `g_bridge`) means only one I/O at a time
- Each device gets its own calldown splice in its DCB chain

### 5. Create HDD Read Test EXE
Modify `mkpe_sread.py` (or create `mkpe_hread.py`) to read from the HDD drive letter instead of D:. The HDD may be E: or F: depending on Win98's drive letter assignment. Build with:
```bash
python3 $ART/harness/mkpe_hread.py HREAD.EXE E:\\TESTFILE.TXT HREAD.TXT
```

## Key Architecture Notes

### Why ESDI_506.PDR blocks HDD detection
Win98's IOS port driver loading order: ESDI_506.PDR loads first (built-in IDE driver) and claims all IDE hard disks. Our VxD loads via SYSTEM.INI `device=` AFTER ESDI_506. By the time `aep_config_dcb` runs for our driver, the HDD DCB is already owned by ESDI_506. Only unclaimed devices (like the CD-ROM after NECATAPI.VXD removal) reach us.

### VxD Debug Markers Quick Reference
- `H` → AEP handler entry
- `O` → AEP_BOOT_COMPLETE
- `G` → AEP_CONFIG_DCB
- `T` → late CDROM attach probe entry
- `t` → after IFSMgr_CDROM_Attach returns
- `Z` → late CDROM attach timer callback
- `W` → IOR processing entry
- `J` → IFS hook all-call log
- `I` → IFS hook D: drive specific
- `V` → IFSMgr_NotifyVolumeArrival
- `c`/`d` → bridge set to CD-ROM mode
- `H`/`D` → bridge set to ATA disk mode (from deea226)
- `Smoke test PASSED` / `Smoke test failed` — ISO 9660 CD001 detection

### Test Validation Command
```bash
ART=artifacts_search_ctx_instrument_20260514_143819
# CD-ROM test (restore disk first):
bash "$ART/run_unix_once.sh" "$ART" "TEST_NAME" "SREAD.EXE" "SREAD.TXT" 420
# Check: guest-output/SREAD.TXT should be 391 bytes with ATTR=0x00000001, READ_OK, SREAD_END

# HDD-only test (remove CD ISO first):
rm /tmp/win98vm/test_cd.iso
bash "$ART/run_unix_once.sh" "$ART" "HDD_TEST" "SREAD.EXE" "SREAD.TXT" 540
# Check: VxD log for "MBR detected", guest-output for TESTFILE.TXT content
```

## Key Files
- `history/source/IOSBRIDGE.C` — IOS bridge, FSD hooks, calldown splice, DCB filter (~4700 lines)
- `history/source/NTKSHIM.C` — ntoskrnl/HAL shim, IoConnectInterrupt `#if 0` block (~3200 lines)
- `history/source/NT5LOADER.C` — PE loader, smoke test, MBR detection, bridge context (~1650 lines)
- `history/source/IRQHOOK.C` — VPICD IRQ virtualization (~400 lines)
- `src/deploy_sysini.py` — FAT32 deployment to disk image
- `src/create_test_hdd.py` — FAT16 test HDD image creator
- `artifacts_search_ctx_instrument_20260514_143819/` — QEMU test harness, runner, guest EXEs, disk_ops.py
