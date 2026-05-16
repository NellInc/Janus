---
stepsCompleted: 0
status: READY
predecessor: hdd_sread_success_20260516.md
session_date: 2026-05-16
---

# HDD I/O Complete, SCSI Port Phase 2 Next (2026-05-16)

## Goal

Continue backporting XP drivers to Win9x. HDD reads+writes are working.
Next: complete the SCSI miniport port (sym_hi.sys for LSI53C895A), then
test dual-device (IDE HDD + SCSI CD-ROM simultaneously).

## What's Working (validated 2026-05-16)

The NT5 WDM atapi.sys miniport runs on Win98 SE via the NTMINI VxD bridge.
Three test harnesses pass end-to-end through the IOS calldown chain:

| Test | Result | Details |
|------|--------|---------|
| SREAD | PASS | READ_OK=1, READ_N=0x96 (150 bytes), D:\SECOND.TXT |
| HREAD | PASS | Multi-drive probe, D: found, E/F/G correctly absent |
| SWRITE | PASS | WRITE_OK=1, write 54 bytes + readback verified |

CD-ROM reads also work (validated 2026-05-15, separate config via miniport SCSI).

### Architecture (HDD mode)

```
Win32 app → ReadFile/WriteFile("D:\file") → VFAT → IOS calldown for D: DCB
  [OUR HANDLER - ios_wdm_ior_entry]  ← ISPCDF_PORT_DRIVER (above ESDI_506)
    IOR_READ/WRITE → ior_handler → ata_direct_ior (ports 0x170-0x177)
    Other funcs → g_late_next_handler (passthrough to ESDI_506)
  [ESDI_506] ← stock IDE driver (handles C:, can't handle secondary slave)
```

Key design decisions:
- **Skip IOS_Register** — it causes boot hang by routing ALL disk DCBs through us
- **Non-BOTTOM splice** — BOTTOM position only gets internal housekeeping, never real I/O
- **Direct ILB access** — find ESDI_506's ILB via DDB chain walk, call ISP services directly
- **Passthrough** — `g_late_next_handler` captured post-splice for unhandled IOR funcs

### Four Bugs Fixed (commits 1072639, cf88885)

1. **IOS_Register boot hang** — claimed C:'s DCB from ESDI_506. Fix: skip entirely.
2. **BOTTOM calldown blindness** — flags 0x003 never sees real I/O. Fix: flags 0x002.
3. **IOR_xfer_count** — ata_direct_ior didn't report actual bytes. Fix: set before complete.
4. **FAT12/16 misclassification** — 4080 clusters < 4085 threshold. Fix: cluster_size 4096→2048.

## Current State of All Key Files

| File | Lines/Size | Purpose |
|------|-----------|---------|
| `history/source/IOSBRIDGE.C` | 5214 lines | Main driver: IOS splice, calldown, ATA I/O |
| `history/source/NTMINI_V5.C` | 6574 lines | V5 dual-mode VxD (SCSI+IDE). Just copied from PA. |
| `history/source/VXDWRAP_NASM.ASM` | 1937 lines | ASM thunks, DDB, control dispatch, passthrough |
| `history/source/NT5RAW.VXD` | 181552 bytes | Compiled from Docker (IOSBRIDGE.C + VXDWRAP_NASM.ASM) |
| `binaries/NT5FIXED.VXD` | 183306 bytes | LE-fixed output of build_nt5_fixed.py |
| `deploy-package/NTMINI.VXD` | 183306 bytes | = NT5FIXED.VXD, ready for end users |
| `src/build_nt5_fixed.py` | LE fixup + static DRP injection |
| `src/deploy_sysini.py` | Deploy to SYSTEM.INI, clear dirty flags, edit MSDOS.SYS |
| `src/create_test_hdd.py` | 16MB FAT16 with TESTFILE/SECOND/WDMTEST.TXT |
| `scripts/run_hdd_tests.sh` | Automated SREAD/HREAD/SWRITE test pipeline |

## Build + Deploy + Test Pipeline

### Build (requires Docker with Open Watcom)

```bash
cd /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport
bash history/source/build.sh nt5          # Compile → NT5RAW.VXD
python3 src/build_nt5_fixed.py            # LE fixup → NT5FIXED.VXD
```

### Deploy

```bash
python3 src/deploy_sysini.py binaries/NT5FIXED.VXD   # VxD + dirty flags
python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img  # FAT16 test disk
ART=artifacts_search_ctx_instrument_20260514_143819
python3 "$ART/disk_ops.py" write windows SREAD.EXE "$ART/harness/SREAD.EXE"
python3 "$ART/disk_ops.py" winini_run "C:\\WINDOWS\\SREAD.EXE"
```

### Test

```bash
# Manual single test:
qemu-system-i386 -m 256 -M pc -cpu pentium2 \
  -drive "file=$HOME/Documents/VMs/win98vm/win98.img,format=raw" \
  -boot c -vga std -rtc base=localtime -display none \
  -debugcon file:/tmp/nt5dbg.log \
  -drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw" \
  -monitor unix:/tmp/qemu-mon.sock,server,nowait &

# Wait 130s, dismiss wizard via monitor socket, wait 150s more, kill QEMU
# Retrieve: python3 "$ART/disk_ops.py" read windows SREAD.TXT /tmp/result.txt

# Automated:
bash scripts/run_hdd_tests.sh 300
```

### First-Boot (one-time for fresh images)

The Hardware Wizard ("Standard PCI Graphics Adapter") appears on every boot.
It does NOT block test execution (WIN.INI `run=` works behind it), but it
persists because the VGA driver was never installed. Keystrokes to dismiss:

```python
# Via QEMU monitor unix socket:
for _ in range(6):
    send('sendkey esc'); sleep(1)
    send('sendkey ret'); sleep(2)
```

CRITICAL: Our VxD causes splash-screen hang if deployed BEFORE the very
first boot of a freshly converted qcow2 image. Procedure for fresh images:
1. Convert qcow2 → raw, suppress ScanDisk
2. Boot WITHOUT VxD → wizard appears → dismiss → graceful shutdown
3. THEN deploy VxD

The image at ~/Documents/VMs/win98vm/win98.img is already post-first-boot.
If you reconvert from en_win98_se.qcow2, redo the first-boot procedure.

## SCSI Port Status (Phase 1 Complete)

Files copied from PA repo (commit 7f3f7fb):
- `NTMINI_V5.C` (6574 lines, dual-mode: IDE + SCSI)
- `SYM_HI_EMBEDDED.H`, `ATAPI_EMBEDDED.H`, `embed_sys.py`
- `reference/qemu-patches/` (LSI DMA fix, SCRIPTS MMIO, build script)

### Phase 2: ASM Wrapper Sync (NEXT STEP)

29 extern symbols in NTMINI_V5.C are missing from VXDWRAP_NASM.ASM.
These must be added for `build.sh v5scsi` to link.

**VxD service wrappers needed:**
- `VxD_Hook_IRQ`, `VxD_Chain_IRQ` — IRQ handling
- `VxD_PageAllocateDMA` — DMA buffer allocation for SCRIPTS engine
- `VxD_MapPhysToLinear` — MMIO mapping (already partially in VXDWRAP)
- `VxD_GetPhysAddr` — physical address for DMA
- `Set_Global_Timeout` — timer

**IOS service wrappers needed:**
- `IOS_BD_Complete`, `IOS_Register_Driver`, `IOS_Requestor`
- `IOS_Get_Device_List_Wrapper`, `IOS_Get_Version_Test`
- `ISP_Insert_Calldown`, `Call_ILB_Internal_Request`

**ScsiPort emulation:**
- `Safe_HwFindAdapter` — calls miniport's HwFindAdapter with fault protection
- `sp_GetPhysAddr_asm`, `sp_ConvertUlong_asm` — ScsiPort helper stubs
- `pe_load_image` — PE loader for sym_hi.sys
- `timer_callback_bridge` — timer callback from VxD to C

**Data symbols:**
- `g_irq_hw_int_func`, `g_irq_devext`, `g_fault_addr`, `g_fault_code`, `g_fault_eip`
- `g_fsd_provider_id`, `g_fsd_registered`
- `gdl_regs[6]` — register save area

**Reference:** PA repo's `VXDWRAP_V4.ASM` has all these symbols. Diff against
target's `VXDWRAP_NASM.ASM` and merge missing entries. The ASM files use
different assembler syntax (V4 = MASM/WASM, NASM = NASM) so translation needed.

### Phase 3: Build System

Add Makefile target `v5scsi`:
- CFLAGS: `-dNTMINI_USE_SCSI=1`
- New linker file `link_v5scsi.lnk`
- `build.sh v5scsi` → produces `V5SCSI.VXD`

### Phase 4: Integration Test

Boot patched QEMU with `-device lsi53c895a` + SCSI CD-ROM.
V5SCSI.VXD should: find LSI PCI device, init SCRIPTS DMA, detect SCSI CD.

## What NOT to Retry

- **IOS_Register for HDD**: Hangs boot (steals C: from ESDI_506)
- **ISPCDF_BOTTOM (flags 0x003/0x001)**: Never receives real I/O
- **ISP_INSERT_CALLDOWN flags 0x100**: WPE crash (chain corruption)
- **PDR mode (--pdr)**: PDRs never get Init_Complete
- **Cluster size 4096 for 16MB disk**: Creates 4080 clusters → FAT12 misclassification
- **Killing QEMU without waiting for writes**: SWRITE.TXT on C: appears truncated
  (lazy writeback). Wait 150s+ after wizard dismissal before QEMU termination.

## Debug Log Interpretation

Normal boot produces ~37-38KB of debugcon output (port 0xE9).

Key markers in `ios_wdm_ior_entry`:
- `W` = handler invoked (should see 8+ during boot)
- `<XXXX>` = IOR_func (0001=READ, 0002=WRITE, 0009=MEDIA_CHECK)
- `P` = passthrough to next handler (18+ during boot)
- `j` = internal IOP forwarded
- `N` = next handler captured (9+ during boot)

After SREAD runs: log grows from ~37K to ~38K (new W markers from D: reads).

If log stays at 36,293 and screenshot shows splash screen → VxD is hanging boot.
If log is 0 bytes → VxD not deployed, or ScanDisk blocking (clear dirty flags).
