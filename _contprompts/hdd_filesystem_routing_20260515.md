---
stepsCompleted: 1
status: IN_PROGRESS
predecessor: hdd_orphan_dcb_routing_20260515.md
session_date: 2026-05-15
---

# HDD Filesystem I/O Routing — Next Steps (2026-05-15 late evening)

## What Was Achieved (This Session)

### VALIDATED: Direct ATA I/O reads correct FAT16 data

Boot-time validation at the MBR detection stage:
- **LBA 63** (FAT16 boot sector): byte 0 = 0xEB (JMP), offset 54 = "FAT16" ✓
- **LBA 136** (cluster 3 = SECOND.TXT): starts with "HDD_" → "HDD_SECOND_TXT: This file was read from the FAT16 hard disk image" ✓

The direct ATA port I/O to the secondary slave IDE reads correct data at both the filesystem metadata and file content levels. The hardware I/O path is proven.

### Key Bugs Found and Fixed

1. **ISO FSD was intercepting ALL drive letters**: `IFSMgr_RegisterMount` + `IFSMgr_InstallFSHook` registered an ISO 9660 FSD that served cached CD content for D: regardless of the actual device. Every test (V14-V18) returned "SECOND FILE FROM REAL ISO" (26 bytes) because the FSD short-circuited before IOS routing. **Fixed**: gated FSD/IFS registration on CD-ROM mode only.

2. **Orphan DCB has no filesystem**: ISP_ASSOCIATE_DCB + ISP_DEVICE_ARRIVED successfully assign a drive letter to our orphan DCB (F:), but Win98 never mounts a filesystem on it. VFAT needs an AEP_CONFIG_DCB broadcast to probe the device — which we skip in HDD mode because it deadlocks with ESDI_506. V19 confirmed: F:\SECOND.TXT returns ERROR_PATH_NOT_FOUND (error 3).

3. **ISP_CREATE_DCB does NOT auto-assign letters in HDD-only mode**: Unlike CD+HDD mode (where the orphan got F:), HDD-only mode leaves the DCB letterless. The scan code (`@` marker) confirmed no letter found; fallback to ISP_ASSOCIATE_DCB worked (assigned F:) but useless without a filesystem mount.

4. **Splice into D:'s chain works positionally but breaks filesystem**: V20 spliced our handler at depth 1 in D:'s chain (above ESDI_506 at depth 3). Chain walk confirms correct position. But D:\SECOND.TXT returns ERROR_ACCESS_DENIED (error 5) — the splice disrupts VFAT's ability to mount D: because our handler changes I/O behavior mid-boot.

### ISP_PICK_DRIVE_LETTER Explained

Returns letter[0]=0x00 (A:) because there's nothing left to pick — IOS already consumed available slots. The struct is correct; the call is just unnecessary when ISP_ASSOCIATE_DCB is available.

## Current Architecture Problem

```
Win98 Filesystem (IFSMgr / VFAT)
  ↓ (filesystem → block translation)
IOS Layer (IOR routing via DCB calldown chain)
  ↓
D: DCB calldown chain:
  [0] TSD layer handler
  [1] Our ios_wdm_ior_entry  ← V20 splice position
  [2] Middle layer
  [3] ESDI_506.PDR port driver
  
Problem: Our handler at [1] receives IORs from the TSD, but:
- During boot, VFAT reads boot sector through this chain before our handler exists
- After our splice, ESDI_506 below us never fires (we complete the IOR ourselves)
- But VFAT already mounted D: via ESDI_506's data — our splice changes the data source mid-session
- Result: VFAT cache disagrees with our data → ACCESS_DENIED
```

## Next Steps (Priority Order)

### 1. Test: Splice PLUS pass-through during mount

Modify `ios_wdm_ior_entry` for HDD mode: instead of calling `ata_direct_ior`, pass the IOR to the **next handler** in the chain (ESDI_506). This validates that the splice position works for I/O routing without changing the data source. If VFAT can mount and read D: through our handler → ESDI_506 → hardware, the chain is proven functional.

Implementation:
```c
static void ior_handler(PIOR ior, PDCB dcb) {
    PBRIDGE_DEVICE dev = find_device_for_dcb(dcb);
    if (!dev) { complete_ior(ior, IORS_ERROR_DESIGNTR); return; }
    
    if (dev->device_type == DCB_TYPE_DISK) {
        /* For now: pass through to next handler (ESDI_506) to validate chain */
        /* Later: intercept and serve from ata_direct_ior */
        IOS_BD_Send_Command(ior);  /* sends to next in chain */
        return;
    }
    // ... existing CD-ROM path ...
}
```

### 2. Test: Selective interception

Once pass-through works, selectively intercept ONE file read (SECOND.TXT at known LBA) while passing everything else to ESDI_506. Compare SREAD output: if it returns 150 bytes "HDD_SECOND_TXT:", our ata_direct_ior serves filesystem reads correctly through the IOS chain.

### 3. Full interception

Replace ESDI_506 entirely for D:'s I/O. All reads/writes go through ata_direct_ior. This is the end goal: our driver serves the HDD independently.

### 4. Alternative: ESDI_506 suppression for secondary channel only

If splicing doesn't work, suppress ESDI_506 for the secondary IDE channel only (not the primary C: boot disk). Win98 registry or SYSTEM.INI may support per-channel driver configuration. With ESDI_506 gone from the secondary channel, our driver is the only option and Win98 must use our DCB for D:.

## Commits (this session)

```
a39ad83 SCSI: VALIDATED — direct ATA reads FAT16 boot sector + SECOND.TXT from test HDD
52869e8 SCSI: HDD splice into D: chain above ESDI_506, gate ISO FSD on CD-ROM mode
f519a86 SCSI: Contprompt — orphan DCB routing, ISP_PICK_DRIVE_LETTER returns A:
10e057a SCSI: Fix DCB probe — type-filtered drive letter scan, orphan DCB routing for HDD mode
```

## Key Files

- `history/source/IOSBRIDGE.C` — splice logic, FSD gate, probe loop
- `history/source/NT5LOADER.C` — MBR detection, FAT16 boot sector + SECOND.TXT validation
- `src/deploy_sysini.py` — deployment (--suppress-esdi flag available)
- `artifacts.../harness/mkpe_sread.py` — PE generator (sed for drive letter variants)

## Debug Markers Quick Reference (additions this session)

- `V` + hex bytes → FAT16 boot sector validated (0xEB, "FAT16")
- `D` + hex bytes → SECOND.TXT data validated ("HDD_")
- `v` / `d` → validation read failed
- `H S` → HDD splice entry
- `s` + hex16 → ISP_INSERT_CALLDOWN result for HDD splice
- `J` → HDD splice succeeded
- `~#0...#N~` → chain walk with handler addresses
- `*` → our handler found in chain
- `L` → orphan letter scan entry
- `@` + hex8 → orphan auto-assigned to drive N
- `N` → no auto-assigned letter
- `a` + hex16 + hex8 → manual associate result + letter
- `f` → HDD mode, FSD/IFS registration skipped
