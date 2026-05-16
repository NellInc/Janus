---
stepsCompleted: 0
status: READY
predecessor: hdd_pdr_scandisk_fix_20260516.md
session_date: 2026-05-16
---

# HDD PDR: Bisect Build Breakage + Get IOS Registration Working (2026-05-16)

## Goal

1. Identify which `build_nt5_fixed.py` change broke the PDR binary (crashes on boot)
2. Apply the NASM init fix so `ios_register_port_driver()` runs during PDR loading
3. Get SREAD.EXE to read SECOND.TXT from D: via the HDD calldown chain

## Critical Breakthrough (end of previous session)

**The known-working VxD binary from commit `0f6d5a4` loads successfully as BOTH SYSTEM.INI VxD AND PDR.**

```
SHA: e0ccd36ad1c40f4e891d38e9e244e8b98b1154adc2856bfb13a635066a2596cb
Size: 177,674 bytes
Saved at: /tmp/NT5FIXED_working.VXD  (may need re-extraction: git show 0f6d5a4:binaries/NT5FIXED.VXD)
```

Debug output confirmed at T+100-140s (boot is slow on fresh-converted images):
```
NT!
m00000000    ← Sys_Critical_Init (0x00)
0
NT!
m0000001D    ← Create_VM (0x1D)
u1D
[repeats ~6 times]
```

**Key observation:** The working PDR receives `Sys_Critical_Init` (0x00) and `Create_VM` (0x1D) but NOT `Device_Init` (0x01), `Init_Complete` (0x02), or `Sys_Dynamic_Device_Init` (0x1B). This means `ios_register_port_driver()` NEVER runs via the working binary — neither the SYSTEM.INI nor PDR path triggers it on this boot sequence.

**The current build (HEAD commit 54e06b6) produces a broken binary that crashes during boot.** The changes in `build_nt5_fixed.py` from commit 54e06b6 vs the working build at 0f6d5a4 are:
1. `preload_pages`: 0 → NP (40)
2. `obj_flags`: 0x2045 → 0x2047 (added WRITABLE)  
3. `entry_flags`: 0x01 → 0x03
4. `resident_name`: NT5RAW → NTMINI
5. Layout: fixups before pages → fixups before pages (same in both; intermediate commit 275c598 put pages before fixups which also broke it)

The NASM change (`.do_init` → `.do_ios_register` jump) also changes NT5RAW.VXD size, which may affect fixup offsets.

## Two Blockers to Solve

### Blocker 1: build_nt5_fixed.py changes break the binary

The 0f6d5a4 build script produces a working binary. The current build script produces one that crashes. Need to bisect which change(s) cause the crash.

**Test plan:** Start from the working 0f6d5a4 build script, apply changes one at a time, deploy as PDR, check for `NT!` in debug log at T+140s.

Order (cheapest to test first):
1. Change `resident_name` alone (NT5RAW → NTMINI)
2. Change `entry_flags` alone (0x01 → 0x03)
3. Change `obj_flags` alone (0x2045 → 0x2047)
4. Change `preload_pages` alone (0 → 40, or 0 → 2)
5. All together

### Blocker 2: ios_register_port_driver() never runs

Even the working binary doesn't receive `Init_Complete` or `Sys_Dynamic_Device_Init`. So `ios_register_port_driver()` never runs. The VxD loads but is PASSIVE — it never registers with IOS, never gets AEP callbacks, never inserts into the calldown chain.

The NASM fix (`.do_init → .do_ios_register`) won't help because `.do_init` handles `Sys_Dynamic_Device_Init` (0x1B) which is never sent.

**Alternative approach:** Call `ios_register_port_driver()` from `Init_Complete` handler AND from a different trigger. Options:
a) Hook into `Sys_Critical_Init` (0x00) — but IOS may not be ready this early
b) Hook into `Create_VM` (0x1D) — runs after IOS init, may work
c) Use `Init_Complete` (0x02) — standard path, but not received in PDR testing
d) Use a VMM timer callback to defer registration until IOS is ready

**Key question:** WHY doesn't the PDR receive `Device_Init`/`Init_Complete`? Is this because:
- PDRs loaded by IOS genuinely don't get these messages (IOS-loaded PDRs are different from VXDLDR-loaded dynamic VxDs)?
- The messages arrive later than T+140s (boot is still in progress)?
- Something about our DDB suppresses these messages?

**Experiment:** Run the working binary for 5+ minutes and capture the FULL debug log. Look for `'1'` (Device_Init), `'2'` (Init_Complete), or `'J'` (Dynamic_Init) markers.

## ScanDisk Suppression (REQUIRED for fresh images)

`qemu-img convert` from qcow2 produces dirty-shutdown images. Win98 ScanDisk runs for 20-30 minutes on the 8GB disk, blocking all testing.

**Suppression procedure (must run after every `qemu-img convert`):**
```python
import os, mmap
path = os.path.expanduser('~/Documents/VMs/win98vm/win98.img')
with open(path, 'r+b') as f:
    mm = mmap.mmap(f.fileno(), 0)
    count = 0
    for pattern in [b'SCANDSKW', b'SCANDISK', b'DSKMAINT']:
        idx = 0
        while True:
            idx = mm.find(pattern, idx)
            if idx == -1: break
            if idx + 11 < len(mm):
                ext = bytes(mm[idx+8:idx+11])
                attr = mm[idx+11]
                if attr < 0x40 and ext in [b'EXE', b'COM', b'DLL', b'INI', b'PIF', b'   ']:
                    mm[idx] = 0xE5
                    count += 1
            idx += 1
    mm.close()
    print(f'Suppressed {count} entries')
```
**Warning:** This mmap scan takes 7-8 minutes on 8GB. Consider extracting to a standalone script.

Also run: `deploy_sysini.py` (any mode) to clear FAT dirty flags.

## Full Deploy + Test Pipeline

```bash
cd /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport

# 1. Fresh disk image (only if needed — avoid if image already works)
qemu-img convert -f qcow2 -O raw ~/Documents/VMs/win98vm/en_win98_se.qcow2 ~/Documents/VMs/win98vm/win98.img

# 2. Suppress ScanDisk (7-8 min mmap scan — run the Python above)

# 3. Build
bash history/source/build.sh nt5           # compile in Docker (~30s)
python3 src/build_nt5_fixed.py             # LE fixup

# 4. Deploy as PDR
python3 src/deploy_sysini.py binaries/NT5FIXED.VXD --pdr

# 5. Create test HDD + remove CD
python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img
rm -f /tmp/win98vm/test_cd.iso

# 6. Run test (FORCE_IDE=1 for ATA direct I/O path)
ART=artifacts_search_ctx_instrument_20260514_143819
FORCE_IDE=1 bash "$ART/run_unix_once.sh" "$ART" "TEST_NAME" "SREAD.EXE" "SREAD.TXT" 540

# 7. Check results (output appears at T+100-140s on fresh images)
#    Look for NT! in checkpoints/vxd_*.log
#    Look for SREAD output in guest-output/SREAD.TXT
```

**Quick test without run_unix_once.sh:**
```bash
DISK="$HOME/Documents/VMs/win98vm/win98.img"
rm -f /tmp/nt5dbg_test.log
qemu-system-i386 -m 256 -M pc -cpu pentium2 \
  -drive "file=$DISK,format=raw" -boot c -vga std \
  -rtc base=localtime -display none \
  -debugcon file:/tmp/nt5dbg_test.log \
  -drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw" &
# Wait 140s, then: wc -c /tmp/nt5dbg_test.log && head -40 /tmp/nt5dbg_test.log
```

## Key Files

| File | Purpose |
|------|---------|
| `src/build_nt5_fixed.py` | LE fixup — **this is where the breakage is** |
| `src/build_nt5_fixed_working.py` | Working version from 0f6d5a4 (if still present) |
| `history/source/VXDWRAP_NASM.ASM` | ASM wrapper, DDB, control dispatch, init/cleanup |
| `history/source/IOSBRIDGE.C` | IOS bridge: AEP handler (L3241), calldown (L3773), ata_direct_ior (L3879) |
| `history/source/IOSBRIDGE.C:2375` | `ntmini_device_init()` |
| `history/source/IOSBRIDGE.C:2436` | `ios_register_port_driver()` |
| `artifacts_.../run_unix_once.sh` | Test runner (has `FORCE_IDE=1` support) |
| `reference/ESDI_506.PDR` | Stock Win98 IDE driver for comparison |

## Architecture Reminder

```
SREAD.EXE → ReadFile("D:\SECOND.TXT") → VFAT
  → IOS calldown chain for D: DCB
    [0] TSD (type-specific driver)
    [1] Our ios_wdm_ior_entry → ior_handler → ata_direct_ior
    [2] ESDI_506 (if present, below us)
  → ata_direct_ior: programs IDE 0x170-0x177 (secondary slave)
  → reads sectors → fills IOR buffer → complete_ior(IORS_SUCCESS)
  → VFAT returns data to SREAD
```

## Test HDD Layout

16MB FAT16, CHS 32/16/63. Partition at LBA 63.
- Cluster 2 (TESTFILE.TXT, 105 bytes): LBA 128
- Cluster 3 (SECOND.TXT, 150 bytes): LBA 136
- Content: "HDD_SECOND_TXT: This file was read from the FAT16 hard disk image"

## SREAD.EXE Expected Success Output
```
SREAD_START
ATTR=0x00000020
CREATE_HANDLE=0x0000000C
READ_OK=0x00000001 READ_N=0x00000096 READ_LE=0x00000000
TEXT_BEGIN
HDD_SECOND_TXT: This file was read from the FAT16 hard disk image
TEXT_END
```

## Commits
```
54e06b6 SCSI: PDR crash diagnosis — ScanDisk fix, LE layout restructure, init path fix
163c69d SCSI: Contprompt for next session — PDR read failure diagnosis
275c598 SCSI: IOS-compatible PDR build + DRP_NT_PD priority + PDR deploy to IOSUBSYS
0f6d5a4 SCSI: Update contprompt with session progress  ← WORKING BINARY HERE
```

## What NOT to Retry (dead ends)

- `AutoScan=0` in MSDOS.SYS does NOT prevent ScanDisk on this Win98SE build
- QEMU `-debugcon` default port is 0x402, but QEMU 11.0.0 (Homebrew) captures 0xE9 fine — the 0-byte logs were from broken binaries, not QEMU
- LE layout order (fixups before/after pages) is NOT what caused the crash — both layouts crash with the current build script changes; the 0f6d5a4 layout (fixups before pages) works
- The "IOS dp formula" (`align_up(et_off, 32)`) is project-local, not how IOS works
