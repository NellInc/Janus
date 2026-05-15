---
stepsCompleted: 0
status: READY
predecessor: hdd_ata_direct_drive_letter_20260515.md
session_date: 2026-05-15
---

# HDD Orphan DCB Routing — Next Steps (2026-05-15 evening, session 2)

## What Was Achieved (This Session)

### Major Findings

1. **DCB device type offset resolved**: Real IOS DCBs store device type at raw offset 0x40 (low byte of ULONG), NOT at 0x10 (our DDK struct offset). CD-ROM = 0x05, disk = 0x00. Offset 0x10 is unreliable (shows 0x00 for both disk and CD-ROM DCBs returned by ISP_GET_DCB).

2. **Drive letter mapping discovered** (HREAD_V2 diagnostic, CD+HDD config):
   - D: = HDD (dcb=CB53BC80, type@0x40=0x00) — served by ESDI_506
   - E: = CD-ROM (dcb=CB53B7D0, type@0x40=0x05) — served by our VxD
   - F: = our orphan DCB (dcb=CB53BCD0, type@0x40=0x05) — auto-assigned by ISP_CREATE_DCB
   - G: = not found

3. **Probe loop bug fixed**: Old code broke on first match (D: = CD-ROM); new code scans ALL D:-G:, filters by device type, logs both offset 0x10 and 0x40. Correctly identifies disk vs CD-ROM DCBs.

4. **ISP_PICK_DRIVE_LETTER + ISP_ASSOCIATE_DCB work** (all return 0x0000 success), BUT the picked letter is 0x00 (drive A: = floppy). This is likely wrong — our orphan DCB got the floppy slot, which Win98 doesn't route filesystem requests through for hard disks.

5. **Device entry registration for orphan DCB works** — `K` marker confirms. But no I/O reaches it because the drive letter assignment is wrong.

6. **SREAD.EXE output mechanism works** for both D: and E: paths. But content from BOTH is "SECOND FILE FROM REAL ISO" (26 bytes) — cached CD-ROM content persisted in the qcow2 base image from a previous session. No test HDD content (150 bytes "HDD_SECOND_TXT:") was ever read.

7. **HREAD.EXE and EREAD.EXE never produce output** (0 bytes, no file created). Likely crashed before CreateFileA for output or Win98's run= mechanism failed for these PEs. SREAD.EXE (smaller, simpler) works fine with the same mechanism. Don't block on this — use modified SREAD.EXE for testing.

### Code Changes (committed as 10e057a)

- `IOSBRIDGE.C`: Type-filtered drive letter scan, ISP_PICK_DRIVE_LETTER fallback, device entry for orphan, DCB geometry, skip splice in HDD mode
- ~150 lines added/modified

## What's NOT Working

**Our ata_direct_ior never receives filesystem I/O.** ISP_PICK_DRIVE_LETTER returns letter[0]=0x00 (drive A:), which is the floppy. ISP_ASSOCIATE_DCB "succeeds" but assigning an HDD to A: is meaningless. Win98 routes A: to the floppy controller, not IOS. VxD debug log shows zero growth after setup — no IOR reaches our handler.

## Root Cause Analysis

The ISP_PICK_DRIVE_LETTER packet struct may be wrong:
```c
typedef struct _ISP_PICK_DRIVE_LETTER_PKT {
    USHORT  func;    // 14
    USHORT  result;  
    ULONG   dcb;     
    UCHAR   letter[2];  // ← where IOS writes the picked letter?
    UCHAR   flags;
    UCHAR   pad;
} ISP_PICK_DRIVE_LETTER_PKT;
```

Options:
1. `letter[0]` is the output field but we read it wrong (maybe it's a bitmask or 1-based index)
2. The struct is packed differently and `letter` is at a different offset
3. ISP_PICK_DRIVE_LETTER doesn't work for DCBs created via ISP_CREATE_DCB (only for DCBs from AEP_CONFIG_DCB)
4. ISP_CREATE_DCB already assigned a letter (F: in CD+HDD mode) — in HDD-only mode it might be D: or E: already

## Next Steps (Priority Order)

### 1. Verify orphan DCB's auto-assigned letter (HDD-only mode)

ISP_CREATE_DCB may auto-assign a letter. Add diagnostic markers to dump ISP_GET_DCB for our orphan's drive letter AFTER ISP_CREATE_DCB but BEFORE ISP_PICK:

```c
// After ISP_CREATE_DCB (line ~2700):
{
    UCHAR dl;
    for (dl = 0; dl <= 6; dl++) {
        ISP_GET_DCB_PKT chk;
        zero_mem(&chk, sizeof(chk));
        chk.func = 7; chk.drive = dl;
        Call_ILB_Service(ilb->service_rtn, &chk);
        if (chk.dcb == isp_dcb.dcb_ptr) {
            dbg_mark('@');
            ios_dbg_hex8(dl); // This is the auto-assigned letter
            break;
        }
    }
}
```

If the orphan already has a letter, ISP_PICK is unnecessary. Just register the device entry and let IOS route through the existing assignment.

### 2. Skip ISP_PICK if orphan already has a letter

If Step 1 shows the orphan has a letter (e.g., D: or E: in HDD-only mode), just register the device entry and proceed. The calldown was already inserted into the DCB at line 2706.

### 3. Test with SREAD pointed at the orphan's letter

Once we know the letter, rebuild SREAD to read from that letter. The test HDD has 150 bytes "HDD_SECOND_TXT:..." at SECOND.TXT. If SREAD returns 150 bytes with that content, ata_direct_ior is working end-to-end.

### 4. Fix the stale cache issue

Win98's qcow2 base image has cached CD-ROM content from previous sessions. SREAD reading "SECOND FILE FROM REAL ISO" (26 bytes) from ANY drive letter suggests cached filesystem data, not live device I/O. Verify by:
- Checking if the disk image actually contains SECOND.TXT on D: or E: via `disk_ops.py`
- If cached: the qcow2 needs cleaning, or use a file unique to the HDD test

### 5. Alternative: Direct DCB splice above ESDI_506

Instead of using an orphan DCB, splice into ESDI_506's D: DCB at a higher layer (TSD or VSD level). This intercepts I/O before ESDI_506 handles it. Requires knowing the correct lgn (layer group number) for non-port-driver layers.

## Current Build State

- **Binary**: `binaries/NT5FIXED.VXD` (180788 bytes pre-fixup, sha b52764c6...)
- **Build**: `bash history/source/build.sh nt5 && python3 src/build_nt5_fixed.py`
- **Deploy**: `python3 src/deploy_sysini.py binaries/NT5FIXED.VXD`
- **Restore**: `qemu-img convert -f qcow2 -O raw .../en_win98_se.qcow2 .../win98.img`
- **Test HDD**: `python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img`
- **Modified SREAD**: `sed 's/D:\\/E:\\/' mkpe_sread.py > /tmp/mkpe_esread.py && python3 /tmp/mkpe_esread.py harness/SREAD.EXE SREAD.TXT`

## Test Summary

| Test | Config | Result | Key Finding |
|------|--------|--------|-------------|
| HREAD_DIAG_V1 | CD+HDD, old binary | No output | HREAD.EXE never writes output |
| HREAD_DIAG_V2 | CD+HDD, new binary | No output + diagnostics | D:=HDD, E:=CDROM, F:=orphan |
| HDD_V13 | HDD-only, new binary | Boot hang | Corrupted disk (kill + deploy race) |
| HDD_V14 | HDD-only, new binary | SREAD 26 bytes | All ISP calls succeed, letter=0x00 |
| HDD_V15 | HDD-only, EREAD | No output | EREAD.EXE never writes output |
| HDD_V16 | HDD-only, SREAD→E: | SREAD 26 bytes | E: has cached CD content, not HDD |

## Key Files Modified

- `history/source/IOSBRIDGE.C` — type-filtered probe, ISP_PICK/ASSOCIATE, device entry, geometry, skip-splice
- `binaries/NT5FIXED.VXD` — latest build with all fixes
- `artifacts.../harness/mkpe_sread.py` — PE generator (use sed for drive letter variants)

## Debug Markers Quick Reference (additions)

- `t` + hex8 + `/` + hex8 → device type at offset 0x10 and 0x40
- `Y` (in probe) → type-matched DCB found
- `n` → type mismatch, skipped
- `F` → fallback DCB used (no type match)
- `L` → entered ISP_PICK_DRIVE_LETTER block
- `l` + hex16 + hex8 → pick result + letter
- `A` + hex16 → ISP_ASSOCIATE_DCB result
- `R` → both ISP calls succeeded
- `K` (after orphan) → device entry registered for orphan
- `r` + hex16 → ISP_DEVICE_ARRIVED result
- `h` → HDD mode, splice skipped
