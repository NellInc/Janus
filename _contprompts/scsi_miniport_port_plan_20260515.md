---
topic: SCSI miniport (sym_hi.sys) port from PA to nt5-9x-driver-backport
date: 2026-05-15
status: PLANNED
stepsCompleted: 0
nextStep: 1 (Copy source files and sync ASM wrapper)
---

# SCSI Miniport Port: sym_hi.sys alongside atapi.sys

## Context

PA repo's NTMINI_V5.C (6574 lines) is a complete VxD with dual-mode support: `NTMINI_USE_SCSI=0` loads atapi.sys (IDE), `NTMINI_USE_SCSI=1` loads sym_hi.sys (LSI53C895A SCSI). The target repo's copy (2291 lines) is an older IDE-only version. This is a full file replacement, not a merge.

The SCSI path handles: PCI bus scan for LSI controller, DMA buffer allocation via VxD_PageAllocateDMA, MMIO page mapping for SCRIPTS engine, 93C46 EEPROM emulation, SCRIPTS VA-to-PA patching, SCSI target scan via INQUIRY, IOS registration with ILB/DCB/calldown chain. QEMU requires a patched lsi53c895a.c to fix Win98 DMA address translation bugs.

The NT5/WDM path (NT5LOADER.C + IOSBRIDGE.C) remains unchanged. SCSI uses the V5 build path (separate VXD binary).

## Phase 1: File Copy, Embedding, and ASM Sync [complexity: M]

**Copy from PA to target repo `history/source/`:**
- `NTMINI_V5.C` (replace 2291-line version with 6574-line version)
- `SYM_HI_EMBEDDED.H` (embedded sym_hi.sys binary as C array)
- `embed_sys.py` (tool to regenerate embedded header from .sys file)

**ASM wrapper sync (critical):** The 6574-line NTMINI_V5.C declares externs not in the target's VXDWRAP.ASM: `Safe_HwFindAdapter`, `VxD_Hook_IRQ`, `VxD_Chain_IRQ`, `VxD_PageAllocateDMA`, `VxD_MapPhysToLinear`, `VxD_GetPhysAddr`, `Call_ILB_Service`, `timer_callback_bridge`, `g_irq_hw_int_func`, `g_irq_devext`, `sp_GetPhysAddr_asm`, `sp_ConvertUlong_asm`, and IFS hook trampolines. Diff PA's VXDWRAP.ASM against target's; copy or merge missing symbols. Without this, link fails with unresolved externs.

**Copy QEMU patches to target repo `reference/qemu-patches/`:**
- `lsi-ntmini-full.patch`, `build-patched-qemu.sh`, `ntmini-lsi-test.c`

**Source paths:**
- PA: `research/win98-dvd-fix/ntmini/source/` and `research/win98-dvd-fix/ntmini/qemu-patches/`

**Acceptance:** `NTMINI_V5.C` compiles with `-dNTMINI_USE_SCSI=1`, all externs resolve at link time.

## Phase 2: Build System [complexity: S]

**Makefile additions** (model on existing V5 target):
- New target `v5scsi` producing `V5SCSI.VXD`
- CFLAGS add `-dNTMINI_USE_SCSI=1`
- New linker file `link_v5scsi.lnk` (clone `link_v5.lnk`, output `V5SCSI.VXD`)
- Object files: `V5SCSI_wrap.obj`, `V5SCSI_c.obj`, `V5SCSI_pe.obj`

**build.sh:** New case `v5scsi` forwarded to `make v5scsi`.

**Acceptance:** `./build.sh v5scsi` produces `V5SCSI.VXD`. Verify `./build.sh v5` still succeeds (IDE regression check).

## Phase 3: QEMU Patch and Command Line [complexity: M]

**Build patched QEMU** using `build-patched-qemu.sh` or manual apply:
```
cd qemu-source && git apply lsi-ntmini-full.patch && make
```

**QEMU command line additions** (append to existing IDE launch script):
```
-device lsi53c895a,id=scsi0 \
-drive if=none,id=cd0,media=cdrom,file=cdrom.iso \
-device scsi-cd,drive=cd0,bus=scsi0.0
```

**What the patch fixes:** DMA PA translation (guest page-table walk for VxD_PageAllocateDMA misreported addresses), BAR1 self-read routing for SCRIPTS MOVE MEMORY, SCNTL1 CON bit preservation, STIME1 GEN timer suppression.

**Acceptance:** Patched QEMU boots Win98, LSI controller visible in PCI space, VxD PCI scan finds it.

## Phase 4: Integration Test [complexity: L]

1. Boot Win98 in **patched** QEMU (not stock) with IDE HDD + SCSI CD-ROM
2. V5SCSI.VXD loads via SYSTEM.INI `device=V5SCSI.VXD`
3. Debug log shows: PCI SCSI found, HwFindAdapter OK, HwInitialize OK, SCSI CD-ROM FOUND
4. ISO 9660 file read succeeds via IFSMgr FSD hook on D:
5. No conflict with IDE HDD (separate VxD, separate IRQ)

**Test artifacts:** QEMU serial log showing full init sequence.

## Files Affected (Target Repo)

| File | Action |
|------|--------|
| `history/source/NTMINI_V5.C` | Replace (2291 -> 6574 lines) |
| `history/source/VXDWRAP.ASM` | Merge (add SCSI-path externs) |
| `history/source/SYM_HI_EMBEDDED.H` | New |
| `history/source/embed_sys.py` | New |
| `history/source/link_v5scsi.lnk` | New |
| `history/source/Makefile` | Edit (add v5scsi target) |
| `history/source/build.sh` | Edit (add v5scsi case) |
| `reference/qemu-patches/` | New directory, 3 files |

## Risk

Replacing NTMINI_V5.C may break the existing `v5` build if VXDWRAP.ASM changes are not backward-compatible. The new C file's IDE path (`NTMINI_USE_SCSI=0`) is a superset of the old one, so the breakage vector is ASM symbol mismatches, not C logic. Verify `make v5` after ASM sync.
