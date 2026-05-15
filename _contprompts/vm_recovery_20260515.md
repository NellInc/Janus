---
stepsCompleted: 16
status: IN_PROGRESS
---

# VM Recovery & Infrastructure Fix — 2026-05-15

## Critical Configuration Change
The Win98 VM MUST use `-cpu pentium2` (not `-cpu pentium`).
The runner at `artifacts_search_ctx_instrument_20260514_143819/run_unix_once.sh` has been updated.
Also `-m 256` instead of `-m 128`.

## What Happened
Rapid QEMU kill/restart cycles corrupted the Win98 disk image FAT. The `deploy_sysini.py` script had a bug that further corrupted FAT entry 0 and the BPB dirty byte on each deploy cycle. The original working disk was lost when it was overwritten during recovery attempts.

## What Was Fixed
1. `deploy_sysini.py` — Fixed FAT dirty flag handling to repair all three indicators (FAT entry 0, FAT entry 1 clean bits, BPB byte at offset 0x41)
2. Restored disk from `en_win98_se.qcow2` backup
3. Discovered `-cpu pentium2` is required for this disk image (pentium hangs at splash, pentium3 boots but SCSI reads fail with 0x0E)
4. `run_unix_once.sh` updated to use `-cpu pentium2 -m 256`

## Current State
- **VxD loads**: DriverEntry, AddDevice, StartDevice all succeed
- **Smoke test PASSES**: NT5 atapi.sys reads CD001 from ISO PVD via SCSI READ(10)
- **IRQ hook**: VPICD virtualization fails on this disk (pre-IRQHOOK binary works, IRQHOOK binary crashes)
- **Guest harness**: SREAD.EXE starts (`SREAD_START` in output) but file operations don't complete — likely IOS/FSD timing issue

## Working Binary
SHA: `8a06a5a45b946b0dbc1d3bb4318a96235c9b8d44bfed6238a67ae5e4c652ae7d`
Built from commit `d1847f7` (pre-IRQHOOK NTKSHIM.C)

## Next Steps
1. Debug why SREAD.EXE file operations don't complete (IOS registration timing? Drive letter assignment?)
2. Investigate why IRQHOOK binary crashes on this disk (VPICD hook works but something else fails)
3. Re-validate WREAD, FWILD3 once SREAD file operations work
4. ATA hard disk support (code committed at deea226, needs testing)

## Git Commits This Session
- `d1847f7` — Full CD-ROM file access via NT5 WDM atapi.sys path
- `19f2d97` — Wire IRQHOOK.C into IoConnectInterrupt (works on original disk, crashes on restored disk)
- `deea226` — Add ATA hard disk support alongside CD-ROM
- `739f6c8` — Add ISR dispatch diagnostic markers ($+/-)

## Key Finding: IRQHOOK Crash
The IRQHOOK binary (commit 19f2d97) works on the original disk but crashes on the restored disk. The crash produces 0 bytes of debug output (vs 232 bytes from pre-IRQHOOK binary's stall, vs 23KB from successful boot). The VPICD hook installs but likely receives a stray IRQ that crashes the system before the ISR bridge is ready.

## Queue Investigation Conclusion
`g_ntk_force_device_queue_start = 1` is correct and permanent. See `_contprompts/queue_fix_20260515.md`.

## SREAD.EXE File I/O Investigation (Session 2026-05-15 09:00-10:00)

### Root Cause Chain (3 layers)

**Layer 1: IOS Calldown (FIXED)**
D:'s existing DCB (e.g. CB5AC690) had its own calldown chain. Our late-created DCB was an orphan — IOS never routed file ops to it. Fix: splice `ios_wdm_ior_entry` into D:'s real DCB chain via ISP_INSERT_CALLDOWN.
Verified: chain walk shows `*` at our handler, ISP_INSERT_CALLDOWN returned 0.

**Layer 2: IFS Hook Timing (FIXED)**
`IFSMgr_InstallFSHook` was installed lazily (only inside `ntmini_fsd_open` on first file open). But `GetFileAttributesA` uses a different IFS path that never triggers OPEN. Fix: install IFS hook at init time, right after `IFSMgr_RegisterMount` succeeds.
Verified: 96 J< markers (all-drive hook calls) vs zero before.

**Layer 3: D: Drive Letter Not Visible to IFS (CURRENT BLOCKER)**
`IFSMgr_NotifyVolumeArrival(3)` + `IFSMgr_RegisterMount` don't make D: visible to IFS. Zero `I<` markers (D:-specific IFS traffic). All 96 hook calls are for drive 03 (C:). The mount probe (`M{`) is never called.

`ios_schedule_late_cdrom_attach_probe()` fires the timer → `ios_late_cdrom_attach_probe()` → `IFSMgr_CDROM_Attach_Wrapper(3, &vrp)`. But this call triggers an IOS internal IOP through D:'s calldown chain. Our handler at the bottom sets IOR_status=0 and returns, but the IOP never completes — the system hangs at this point.

**Why it hangs**: The internal IOP (func=0x0000, flags byte 0x6D has bit 2 set) is an IOS probe. As the bottom of the calldown chain, we must properly complete it. Three approaches tried:
1. Return without completion → CDROM_Attach hangs waiting
2. Call IOS_BD_Complete_IOP → recursive chain processing, deadlock
3. Set IOR_status=0 and return → CDROM_Attach still hangs

The real port driver below us in the chain (ESDI_506.PDR at depths 6-7) should handle this, but inserting at ISPCDF_BOTTOM may have replaced it as the terminal handler.

### Source Changes Made (in IOSBRIDGE.C)
1. Added `g_existing_dcb_ptr` global for tracking D:'s real DCB
2. After ISP_GET_DCB returns D:'s existing DCB, splice our calldown via ISP_INSERT_CALLDOWN
3. Register BRIDGE_DEVICE entry for existing DCB (reads real fields from DCB struct)
4. Walk and verify the spliced chain (diagnostic output)
5. `ios_wdm_ior_entry` rewritten to dispatch to `ior_handler` instead of diagnostic stub
6. IFS hook installed at init time (not deferred to first OPEN)
7. `ios_schedule_late_cdrom_attach_probe()` called after FSD registration
8. `dbg_mark('!')` added to ior_handler for triage

### Test Artifacts
- `SREAD_DCB_SPLICE_REALISO` — Splice only. 24KB log, 0 D: traffic. Hung at hardware wizard.
- `SREAD_FSHOOK_INIT_REALISO` — IFS hook at init. 1.1MB log (96 J< markers for C:), 0 I< (D:).
- `SREAD_CDROM_ATTACH_REALISO` — Added CDROM attach timer. 140KB log. Hung at ZTW<0000>j.
- `SREAD_IOP_COMPLETE_REALISO` — IOS_BD_Complete_IOP for internal. 42KB. Deadlock at same point.
- `SREAD_IOP_NOCOMPL_REALISO` — IOR_status=0 return. 128KB. Same hang.

### Layer 3 Resolution: IOS_BD_Command_Complete

The fix: complete internal IOPs via `IOS_BD_Command_Complete(ior)` (IOR-level), NOT `IOS_BD_Complete_IOP(iop)` (IOP-level). The IOP-level completion triggers recursive chain processing and deadlocks. The IOR-level completion operates on the IOR directly, avoiding chain re-entry.

**Result**: D: became fully visible to IFS:
- mount=2 (mount probe called twice — boot + WREAD access)
- IFS_D=12 (D:-specific IFS requests)
- ATTR_D=1 (file attribute handler called)
- IFS function codes: 0x1E, 0x20, 0x22 (mount/enumerate), 0x21 (info), 0x24 (GetFileAttributesA), 0x00, 0x0B (close)
- 500+ internal IOPs completed successfully (jc pattern)
- 1.7MB debug output (vs 24KB without fixes)

### WREAD vs SREAD Divergence
WREAD test (WREAD_IOR_COMPLETE_V2_REALISO) shows D: file I/O lifecycle completing:
fn=0x24 → fn=0x00 → fn=0x0B. SREAD test hangs at GetFileAttributesA with IFS_D=6 (never increases). WREAD triggers a second mount (mount=2) while SREAD doesn't. Possible timing difference in when the guest EXE accesses D: relative to the mount stabilization.

### Key Discovery: Restored Disk Has No CD-ROM Support
CONFIG.SYS and AUTOEXEC.BAT are empty. No MSCDEX, no OAKCDROM.SYS. The entire CD-ROM drive visibility depends on our VxD: ISP_CREATE_DCB + ISP_INSERT_CALLDOWN + IFSMgr_RegisterMount + IFSMgr_CDROM_Attach + IFSMgr_NotifyVolumeArrival.

### Test Artifacts (continued)
- `SREAD_IOR_COMPLETE_REALISO` — IOS_BD_Command_Complete for internal IOPs. 1.7MB, mount=1, IFS_D=6. SREAD still 13 bytes.
- `WREAD_IOR_COMPLETE_V2_REALISO` — Same build, WREAD.EXE. mount=2, IFS_D=12, ATTR_D=1. Pending final retrieval.

### WREAD SUCCESS — Full File I/O Confirmed (2026-05-15 09:58)

WREAD_IOR_COMPLETE_V2_REALISO: **391 bytes, all operations succeeded.**
```
ATTR=0x00000001 ATTR_LE=0x00000000
CREATE_HANDLE=0x0000000C CREATE_LE=0x00000000
READ_OK=0x00000001 READ_N=0x0000001A READ_LE=0x00000000
TEXT_BEGIN
WDMTEST SECONDARY PAYLOAD
TEXT_END
CLOSE_OK=0x00000001 CLOSE_LE=0x00000000
WREAD_OPS_DONE
```

NT5 WDM atapi.sys served real CD-ROM file I/O through Win98's IFS layer on the restored disk. All three fix layers verified working together.

### Next Steps
1. **DONE** — Three-layer fix committed as `64044bd`. Both SREAD and WREAD pass.
2. **DONE** — SREAD divergence was timing variance (race with second mount cycle). Rerun succeeded.
3. **ATA hard disk support** — code committed at deea226, needs test HDD image (`/tmp/win98vm/test_hd.img`). Create a small FAT16 image with test files, then run WREAD/SREAD targeting the HDD.
4. **IRQHOOK crash** — VPICD virtualization is `#if 0` in current build. Polling mode works. Enable and debug when interrupt-driven I/O is needed (DMA, better performance).
5. **Push to remote** — `64044bd` is local only. Push when ready.
