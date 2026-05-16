---
stepsCompleted: 3
status: COMPLETED
predecessor: hdd_pdr_bisect_20260516.md
session_date: 2026-05-16
---

# HDD PDR: IOS Registration + Integration Test (2026-05-16)

## Blocker 1: SOLVED — Build Script Fixed

Four changes in `build_nt5_fixed.py` each independently crash the VxD binary:

| Change | Working (0f6d5a4) | Broken (HEAD) | Verdict |
|--------|-------------------|---------------|---------|
| Alignment formula | `(MZ_SIZE + fixup_end + 0x1FF) & ~0x1FF` | `MZ_SIZE + ((fixup_end + 0x1FF) & ~0x1FF)` | **CRASH** — pages must be sector-aligned in file-absolute space |
| obj_flags | 0x2045 | 0x2047 (+WRITABLE) | **CRASH** — Win9x LE loader rejects WRITABLE on merged code+data |
| entry_flags | 0x01 | 0x03 (+SHARED) | **CRASH** — SHARED bit invalid for VxD DDB exports |
| resident_name | Read from file (NT5RAW) | Hardcoded NTMINI | **CRASH** — must match original linker module name |
| preload_pages | 0 | NP (40) | **SAFE** — more debug output, keep this |

All fixes applied and committed to `src/build_nt5_fixed.py`.

## Blocker 2: IOS Registration — APPROACH NARROWED

### What we know

1. **PDRs loaded by IOS only receive `Sys_Critical_Init` (0x00) and `Create_VM` (0x1D).** No `Device_Init`, `Init_Complete`, or `Sys_Dynamic_Device_Init`. This is confirmed and final.

2. **Calling `ios_register_port_driver()` from Sys_Critical_Init crashes** because it calls `ntmini_device_init()` → `nt5_init(0)` which does PE loading too early. Confirmed with 280s test (0 bytes, definitive crash).

3. **Static DRP via DDB_Reference_Data: unverified.** We implemented it (DRP in last page's free space, fixups added) but ALL fresh-image tests produce 0 bytes — even the known-good baseline binary. The fresh qcow2 conversion produces a first-boot image that shows "Add New Hardware Wizard" on desktop, and IOSUBSYS PDRs may not load during first boot.

4. **ESDI_506.PDR's DDB_Reference_Data does NOT point to its DRP.** `Reference_Data = 0x128` points to code, not "XXXXXXXX" eyecatcher. The DRP with eyecatcher exists at VA 0x5128 (different location). This suggests IOS doesn't use DDB_Reference_Data for PDR discovery — that mechanism is for inter-driver ILB sharing only.

### Three approaches for next session

**A) Minimal IOS_Register (most promising)**
- Requires Docker (for ASM recompilation)
- Create a new function `ios_register_minimal()` that ONLY builds a DRP and calls `IOS_Register()`, without `ntmini_device_init()` / `nt5_init()`
- Call from `Sys_Critical_Init`
- Defer heavy init (`ntmini_device_init`) to `AEP_INITIALIZE` callback
- The existing `ios_register_port_driver()` already guards with `if (g_nt5_init_done)` and calls `ntmini_device_init()` if not — split this logic

**B) Static DRP via DDB_Reference_Data (needs fresh-image fix first)**
- Already implemented in `build_nt5_fixed.py`
- DRP at merged offset 0x27CA0, eyecatcher "XXXXXXXX", aer→ios_aep_bridge
- 2 fixup records added for DDB+0x2C and DRP+0x0C
- **Blocked by**: fresh image doesn't load PDRs (first-boot Hardware Wizard issue)
- **Also uncertain**: ESDI_506 doesn't use this mechanism, so IOS may not either

**C) Boot a GUI session to complete first-boot setup (environment fix)**
- Fresh qcow2 → raw conversion needs one GUI boot to dismiss Hardware Wizard
- After first boot, save the image and use it for all future tests
- This unblocks all testing, regardless of approach

## Environment Issue: First-Boot Images Don't Load PDRs

The qcow2 source (`en_win98_se.qcow2`) is a clean Win98 SE install. Converting to raw produces an image where:
- Win98 boots to desktop (confirmed via screendump)
- "Add New Hardware Wizard" appears (Standard PCI Graphics Adapter)
- ScanDisk is suppressed but first-boot hardware detection runs
- IOSUBSYS PDRs DO NOT produce debugcon output (even the known-good baseline)

The persistent image that worked earlier had already been through first boot in a prior session. I overwrote it with fresh conversions.

**Fix**: Boot with `-display sdl` or `-display gtk`, click through Hardware Wizard, save image. OR: find a qcow2 snapshot that's post-first-boot.

## Key Files

| File | State |
|------|-------|
| `src/build_nt5_fixed.py` | FIXED — alignment, obj_flags, resident_name reverted |
| `history/source/VXDWRAP_NASM.ASM` | Has Sys_Critical_Init → ios_register_port_driver (committed 196b66e but NOT compiled into NT5RAW.VXD) |
| `history/source/NT5RAW.VXD` | Compiled from pre-196b66e sources (no Sys_Critical_Init IOS call) |
| `binaries/NT5FIXED.VXD` | Current: includes static DRP (approach B). Rebuild without DRP for clean baseline. |
| `history/source/IOSBRIDGE.C` | `ios_register_port_driver()` at line 2436, `aep_handler` at line 3241, `find_existing_ilb` at line 2605 |

## Docker Required For

- Recompile NT5RAW.VXD from current ASM/C sources (approach A)
- `bash history/source/build.sh nt5`

## Deploy + Test Pipeline

```bash
cd /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport

# Build
python3 src/build_nt5_fixed.py

# Deploy as PDR
python3 src/deploy_sysini.py binaries/NT5FIXED.VXD --pdr

# Test HDD
python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img
rm -f /tmp/win98vm/test_cd.iso

# Boot (use POST-FIRST-BOOT image!)
DISK="$HOME/Documents/VMs/win98vm/win98.img"
rm -f /tmp/nt5dbg_test.log
qemu-system-i386 -m 256 -M pc -cpu pentium2 \
  -drive "file=$DISK,format=raw" -boot c -vga std \
  -rtc base=localtime -display none \
  -debugcon file:/tmp/nt5dbg_test.log \
  -drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw" &
# Wait 150s, then check log
```

## What NOT to Retry

- `IOS_Register` from `Sys_Critical_Init` with full `ntmini_device_init()` — crashes
- Build script with WRITABLE obj_flags (0x2047) — crashes
- Build script with SHARED entry_flags (0x03) — crashes
- Build script with hardcoded NTMINI resident name — crashes
- Fresh qcow2 images without completing first-boot Hardware Wizard — no debugcon output
