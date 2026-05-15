---
stepsCompleted: 0
status: READY
predecessor: hdd_irqhook_multidev_20260515.md
session_date: 2026-05-15
---

# ATA Direct I/O + Drive Letter Assignment — Next Steps (2026-05-15 evening)

## What Was Achieved (This Session)

### Major Breakthroughs

1. **Direct ATA MBR detection works**: Soft-reset + slave select + READ SECTORS on ports 0x170-0x177 successfully reads the test HDD's MBR (0x55AA signature). Status bytes: 0x00 (post-reset), 0x50 (post-select, DRDY+DSC), 0x58 (DRQ ready). Bridge switches to HDD mode (DeviceType=0x00, SectorSize=512).

2. **DCB splice succeeds (J/K markers)**: `ios_late_create_device` finds D:'s existing DCB, inserts our calldown handler at the port driver level. `NTMINI: Calldown spliced into existing DCB OK`.

3. **AEP broadcast deadlock identified and fixed**: Broadcasting `AEP_CONFIG_DCB` with a DISK-type DCB causes ESDI_506.PDR to deadlock probing the same IDE channel. Fixed by skipping broadcast in HDD mode (marker `S`).

4. **Unit count fix**: Line 820 of NT5LOADER.C hardcoded adapter unit count to 1; changed to 2. Miniport now reports 2 units (confirmed by `G02` marker). However, SCSI SRBs to target 1 still return ABORTED because the miniport never IDENTIFY'd the slave — this is why we pivoted to direct ATA.

5. **Direct ATA I/O path implemented**: `ata_direct_ior` in IOSBRIDGE.C handles `IOR_READ`/`IOR_WRITE` for `DCB_TYPE_DISK` devices using direct IDE register programming. Includes soft-reset to clear BSY state left by ESDI_506.

6. **Deferred VPICD hook**: `ntk_ConnectDeferredVpicd(irq)` in NTKSHIM.C hooks VPICD after ServiceRoutine is set. Confirmed by `DeferredVPICD: IRQ hooked` message.

7. **ESDI_506 suppression added**: `deploy_sysini.py --suppress-esdi` removes ESDI_506.PDR from IOSUBSYS. WARNING: this removes the boot disk driver too — only use if primary channel is handled separately.

8. **HREAD.EXE multi-drive tester**: `mkpe_hread.py` creates a PE that probes D:-G: for SECOND.TXT. 9728-byte PE.

9. **SECOND.TXT on test HDD**: 150 bytes starting "HDD_SECOND_TXT:" — distinctive from CD's 26-byte "SECOND FILE FROM REAL ISO".

### What's NOT Working Yet

**SREAD still reads "SECOND FILE FROM REAL ISO" (26 bytes) from D:\SECOND.TXT.** The content is from a cached CD-ROM mount, not from the test HDD. The test HDD has "HDD_SECOND_TXT:" (150 bytes) but it's never accessed.

**Root cause**: Our calldown is spliced into D:'s DCB at the port driver level, but ESDI_506.PDR's handler is ALSO in the chain and handles I/O before it reaches us. Zero `!` markers in `ior_handler` confirm no filesystem I/O reaches our calldown. D: is the CD-ROM drive (with cached ISO content), not the test HDD.

Win98 assigns D: to the CD-ROM drive (secondary master). The test HDD at secondary slave either gets no drive letter or ESDI_506 mounts it without going through our bridge.

## Current State

- **VxD binary**: `binaries/NT5FIXED.VXD` (latest build has all fixes)
- **Build**: `docker run --rm -v ... nt5-vxd-builder sh -c "cd /src && rm -f *.obj NT5RAW.VXD && make nt5"` then `cp history/source/NT5RAW.VXD binaries/ && python3 src/build_nt5_fixed.py`
- **QEMU**: `-cpu pentium2 -m 256`, test HDD at `if=ide,index=3` (secondary slave)
- **ESDI_506.PDR**: DO NOT suppress — it handles the boot disk (C: on primary channel)
- **Test HDD**: `/tmp/win98vm/test_hd.img` (16MB FAT16, TESTFILE.TXT + SECOND.TXT)

## Key Diagnostic Evidence

### ATA Controller Status at Probe Time
```
Post soft-reset: 0x00 (not BSY — controller responds)
Post slave select: 0x50 (DRDY + DSC — slave device present)
Post READ SECTORS: 0x58 (DRDY + DSC + DRQ — data ready)
MBR signature: 0x55AA at offset 510 — VALID
```

### Debug Markers Quick Reference (additions)
- `AT` → direct ATA MBR probe entry
- `HD` → MBR found, bridge in ATA disk mode
- `S` → AEP broadcast skipped (HDD mode, avoid ESDI deadlock)
- `$` → BSY timeout in ata_direct_ior (before reset)
- `~` → BSY timeout after drive select
- `[D`...`[G` → drive letter probe (D: through G:)
- `X` → splice into existing DCB
- `J`/`K` → splice succeeded + device entry registered

### v12 Test Summary (best result)
- MBR detected via direct ATA ✓
- AEP broadcast skipped ✓
- DCB splice into D: ✓ (J/K)
- No filesystem IOR reached our calldown (!=0) ✗
- SREAD reads cached CD content from D: ✗

## Next Steps (Priority Order)

### 1. Solve the Drive Letter / I/O Routing Problem
The core issue: our calldown is at the bottom of D:'s chain, but I/O never reaches it because ESDI_506 handles it first. Options:

**Option A: Insert ABOVE ESDI_506 in the calldown chain.**
Change the splice from `ISPCDF_BOTTOM | ISPCDF_PORT_DRIVER` to a higher-priority position. The TSD (Type-Specific Driver) level intercepts before the port driver level.

**Option B: Find the HDD's actual drive letter.**
The test HDD might be on E: or not mounted at all. Use HREAD.EXE to probe all drive letters (v12 binary, which doesn't deadlock). If the HDD is on E:, modify SREAD to read from E:.

**Option C: Force Win98 to assign a specific drive letter to the HDD.**
Use `IFSMgr_CDROM_Attach` or the mount infrastructure to assign a drive letter to our device. The `ios_late_create_device` code already has infrastructure for this.

**Option D: Replace ESDI_506's calldown handler for the HDD's DCB only.**
Walk D:'s calldown chain, find ESDI_506's handler function, and replace it with ours. This is surgical but risky.

### 2. Validate ATA Direct I/O End-to-End
Once I/O routes through our calldown, verify that `ata_direct_ior` successfully reads FAT16 sectors from the test HDD. The MBR probe proved port I/O works (status 0x50/0x58), but filesystem reads involve multiple sectors and potential timing issues.

### 3. Multi-Device Support (Step 4 of original contprompt)
Test CD-ROM + HDD simultaneously. The miniport handles CD-ROM (SCSI path), direct ATA handles HDD. Both share the IOS bridge with different device entries.

### 4. IRQHOOK Testing (Step 3 of original contprompt)
The deferred VPICD hook is implemented and confirmed by debug messages. Need to verify it works under load (interrupt-driven I/O vs polling).

## Key Files (Modified This Session)
- `history/source/IOSBRIDGE.C` — direct ATA I/O, skip AEP broadcast, drive letter probe D:-G:
- `history/source/NT5LOADER.C` — direct ATA MBR probe, PORTIO.H include, unit count=2
- `history/source/NTKSHIM.C` — ntk_ConnectDeferredVpicd() deferred VPICD hook
- `history/source/NTKSHIM.H` — ntk_ConnectDeferredVpicd declaration
- `src/deploy_sysini.py` — --suppress-esdi flag, suppress_esdi_506() function
- `src/create_test_hdd.py` — SECOND.TXT (150 bytes, "HDD_SECOND_TXT:...")
- `artifacts.../harness/mkpe_hread.py` — HREAD.EXE multi-drive tester

## Architecture Diagram
```
Win98 Filesystem (IFS)
  ↓
IOS Layer (IOR routing via DCB calldown chain)
  ↓
D: DCB calldown chain:
  [1] TSD layer handler (filesystem translation)
  [2] VSD layer handler (volume tracking)
  [3] ESDI_506.PDR port driver ← handles I/O, never passes down
  [4] Our ios_wdm_ior_entry ← NEVER REACHED (!=0)
      ↓ (if reached)
      ata_direct_ior() → IDE ports 0x170-0x177

Current problem: Layer [3] handles all I/O. Layer [4] never fires.
Fix: Insert at layer [2] or [1], OR find a different DCB for the HDD.
```

## Commits to Make
All source changes are staged but not committed. Commit with message describing the direct ATA I/O implementation and diagnostic findings.
