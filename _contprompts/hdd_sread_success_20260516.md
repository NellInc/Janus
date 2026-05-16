---
stepsCompleted: 5
status: SUCCESS
predecessor: hdd_sread_endtoend_20260516.md
session_date: 2026-05-16
---

# HDD SREAD Success + Next Steps (2026-05-16)

## Result

SREAD.EXE reads D:\SECOND.TXT through the IOS calldown chain. Our VxD intercepts
the I/O above ESDI_506, performs direct ATA port I/O on the secondary IDE channel
(ports 0x170-0x177), and returns data to VFAT → Win32 ReadFile.

```
READ_OK=0x00000001 READ_N=0x00000040 READ_LE=0x00000000
TEXT: HDD_SECOND_TXT: This file was read from the FAT16 hard disk imag
```

READ_N=0x40 (64 bytes) is the SREAD.EXE buffer limit (mkpe_sread.py line 147),
not a driver issue. Buffer increased to 256 bytes; retest pending.

## Three Bugs Fixed (commit 1072639)

### 1. IOS_Register caused boot hang

IOS_Register makes IOS route ALL disk DCBs through our port driver, including
the C: boot disk. ESDI_506 loses its C: handler → system hangs at splash screen.

**Fix**: Skip IOS_Register entirely. Go straight to late-create using ESDI_506's
existing ILB (found via find_existing_ilb DDB chain walk).

### 2. BOTTOM position never receives real I/O

With ISPCDF_BOTTOM | ISPCDF_PORT_DRIVER (0x003), our handler only received
internal housekeeping calls with invalid IOP pointers. Never saw actual reads.

**Fix**: Use ISPCDF_PORT_DRIVER only (0x002) — positions us ABOVE ESDI_506.
After splice, walk chain to capture next handler in g_late_next_handler.
ios_wdm_ior_entry passes unhandled I/O (non-READ/WRITE) to next handler.

### 3. IOR_xfer_count not updated

ata_direct_ior read full sectors but left IOR_xfer_count at the input value.
VFAT saw the original request size, not actual bytes transferred.

**Fix**: Set `ior->IOR_xfer_count = sector_count * 512` before completing.

## First-Boot Procedure (CRITICAL)

Our VxD MUST NOT be present during Win98 first-boot hardware wizard.
IOS_Register (even if skipped) + NTMINI PE loading interacts badly with
CONFIGMG hardware enumeration.

**Correct sequence for fresh images:**
1. `qemu-img convert -f qcow2 -O raw en_win98_se.qcow2 win98.img`
2. Suppress ScanDisk (directory entry deletion)
3. Boot WITHOUT VxD deployed — wizard appears, dismiss with ESC+Enter
4. Graceful shutdown
5. THEN deploy VxD via `python3 src/deploy_sysini.py binaries/NT5FIXED.VXD`

The current image at ~/Documents/VMs/win98vm/win98.img is post-first-boot
(wizard dismissed) but the wizard may reappear if the image is re-converted.

## Architecture (working)

```
SREAD.EXE → ReadFile("D:\SECOND.TXT") → VFAT → IOS calldown for D: DCB
  [OUR HANDLER - ios_wdm_ior_entry]  ← ISPCDF_PORT_DRIVER, above ESDI_506
    IOR_READ/WRITE → ior_handler → ata_direct_ior (ports 0x170-0x177)
    Other funcs → g_late_next_handler (passthrough to ESDI_506)
  [ESDI_506] ← stock IDE driver (handles C:, fails on secondary slave D:)
```

## Key Files

| File | Purpose |
|------|---------|
| `history/source/IOSBRIDGE.C` | Main driver. IOS registration (skip), late-create (L2665), calldown (L3776), ATA I/O (L3882) |
| `history/source/VXDWRAP_NASM.ASM` | ASM thunks, passthrough stub (L407) |
| `src/build_nt5_fixed.py` | LE fixup builder |
| `src/deploy_sysini.py` | Deploy to SYSTEM.INI + clear dirty flags |
| `artifacts_.../harness/SREAD.EXE` | Test app (256-byte buffer after rebuild) |

## Next Steps

### Immediate
- [ ] Verify full 150-byte read with 256-byte SREAD buffer
- [ ] Dismiss wizard permanently (make image truly headless-bootable)
- [ ] Test HREAD.EXE and WREAD.EXE for broader I/O validation

### Near-term
- [ ] Test larger files (multi-sector reads)
- [ ] Test write path (IOR_WRITE via ata_direct_ior)
- [ ] CD-ROM path validation (ATAPI via miniport)
- [ ] Automated test pipeline (run_unix_once.sh integration)

### Architectural
- [ ] Consider whether IOS_Register should be restored with a filter
      (only claim secondary channel DCBs, not primary)
- [ ] Investigate why find_existing_ilb fails on fresh images
      (ILB found on corrupted images but not fresh post-first-boot)

## What NOT to Retry
- **IOS_Register**: Hangs boot. Skip it.
- **ISPCDF_BOTTOM**: Never receives real I/O.
- **ISP_INSERT_CALLDOWN flags=0x100**: WPE crash.
- **PDR mode (--pdr)**: PDRs don't get Init_Complete.
