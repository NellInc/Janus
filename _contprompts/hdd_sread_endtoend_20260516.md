---
stepsCompleted: 0
status: READY
predecessor: hdd_calldown_debug_20260516.md
session_date: 2026-05-16
---

# HDD SREAD End-to-End: First Interactive Boot + Read Test (2026-05-16)

## Goal

1. Complete the Win98 first-boot setup (interactive GUI boot, one time only)
2. Run SREAD.EXE to read D:\SECOND.TXT through the IOS calldown chain
3. Get `READ_OK=0x00000001` with SECOND.TXT content in the output

## What's Already Working (from previous sessions)

All three code blockers are solved. The driver loads, registers with IOS, and receives I/O:

```
VxD loads via SYSTEM.INI → Init_Complete → ios_register_port_driver()
  → IOS_Register OK → ios_late_create_device()
    → ISP_CREATE_DCB (phantom DCB for HDD geometry)
    → ISP_INSERT_CALLDOWN into existing D: DCB (flags=0x003)
  → 8 I/O operations flow through calldown (W markers in debug log)
  → Win98 boots without crash
```

Debug output: 36,293 bytes. IOS_Register succeeds. 8 calldown invocations confirmed.
No crash (WPE fixed by correcting ISP_INSERT_CALLDOWN flags from 0x100 to 0x003).

## BLOCKING ISSUE: First-Boot Image Setup

The qcow2 source (`en_win98_se.qcow2`) produces a first-boot image that hangs on the Win98 splash screen indefinitely. This is NOT caused by our driver — a fresh image without ANY VxD deployed also hangs. Win98 needs interactive GUI input to complete its first-boot hardware detection (Add New Hardware Wizard).

**This must be done ONCE before any headless testing works.**

### Step 1: Create and prepare the raw image

```bash
cd /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport

# Convert qcow2 to raw
qemu-img convert -f qcow2 -O raw \
  ~/Documents/VMs/win98vm/en_win98_se.qcow2 \
  ~/Documents/VMs/win98vm/win98.img

# Suppress ScanDisk (deletes SCANDSKW/SCANDISK/DSKMAINT directory entries)
python3 -c "
import os, mmap
path = os.path.expanduser('~/Documents/VMs/win98vm/win98.img')
with open(path, 'r+b') as f:
    mm = mmap.mmap(f.fileno(), 0); count = 0
    for pattern in [b'SCANDSKW', b'SCANDISK', b'DSKMAINT']:
        idx = 0
        while True:
            idx = mm.find(pattern, idx)
            if idx == -1: break
            if idx + 11 < len(mm):
                ext = bytes(mm[idx+8:idx+11]); attr = mm[idx+11]
                if attr < 0x40 and ext in [b'EXE', b'COM', b'DLL', b'INI', b'PIF', b'   ']:
                    mm[idx] = 0xE5; count += 1
            idx += 1
    mm.close(); print(f'Suppressed {count}')
"
```

### Step 2: Deploy the driver

```bash
# Build (Docker must be running for recompilation, but binaries/ has the last build)
# If Docker is up: bash history/source/build.sh nt5
python3 src/build_nt5_fixed.py

# Deploy via SYSTEM.INI (NOT --pdr! PDR mode is a dead end)
python3 src/deploy_sysini.py binaries/NT5FIXED.VXD
```

### Step 3: Interactive first boot (CRITICAL — must do this once)

```bash
qemu-system-i386 -m 256 -M pc -cpu pentium2 \
  -drive "file=$HOME/Documents/VMs/win98vm/win98.img,format=raw" \
  -boot c -vga std -rtc base=localtime
```

A window will open. Wait ~90-120s for Win98 to boot. The "Add New Hardware Wizard" will appear (Standard PCI Graphics Adapter). Click **Cancel** (or press Escape, then Enter to confirm). The desktop will appear. Then:

- Start → Shut Down → Shut down → OK
- Wait for "It's now safe to turn off your computer" screen
- Close the QEMU window

The image is now in post-first-boot state and will boot headlessly in all future tests.

**Alternative** (headless, less reliable): boot with `-display none -monitor telnet:127.0.0.1:4444,server,nowait`, wait 100s, then send:
```bash
# Dismiss wizard
for i in $(seq 1 10); do echo "sendkey esc" | nc 127.0.0.1 4444; sleep 3; done
# Wait for desktop, then quit
sleep 30 && echo "quit" | nc 127.0.0.1 4444
```
This worked once but is fragile (nc connectivity to telnet monitor is unreliable). Interactive is more reliable.

### Step 4: Redeploy + clear dirty flags + install SREAD

```bash
cd /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport

# Redeploy (also clears FAT dirty flags from the QEMU quit)
python3 src/deploy_sysini.py binaries/NT5FIXED.VXD

# Create test HDD (16MB FAT16 with TESTFILE.TXT and SECOND.TXT)
python3 src/create_test_hdd.py /tmp/win98vm/test_hd.img
rm -f /tmp/win98vm/test_cd.iso

# Install SREAD.EXE and configure WIN.INI to auto-launch it
ART=artifacts_search_ctx_instrument_20260514_143819
python3 "$ART/disk_ops.py" write windows SREAD.EXE "$ART/harness/SREAD.EXE"
python3 "$ART/disk_ops.py" winini_run "C:\\WINDOWS\\SREAD.EXE"
```

### Step 5: Run the SREAD test

```bash
# Option A: Full harness (recommended — handles ESC dismissal, checkpoints, retrieval)
ART=artifacts_search_ctx_instrument_20260514_143819
FORCE_IDE=1 bash "$ART/run_unix_once.sh" "$ART" "SREAD_TEST" "SREAD.EXE" "SREAD.TXT" 300

# Check result:
cat "$ART/guest-output/SREAD.TXT"

# Option B: Manual QEMU (faster iteration)
DISK="$HOME/Documents/VMs/win98vm/win98.img"
rm -f /tmp/nt5dbg_test.log
qemu-system-i386 -m 256 -M pc -cpu pentium2 \
  -drive "file=$DISK,format=raw" -boot c -vga std \
  -rtc base=localtime -display none \
  -debugcon file:/tmp/nt5dbg_test.log \
  -drive "file=/tmp/win98vm/test_hd.img,if=ide,index=3,format=raw" &
# Wait 150s (post-first-boot image boots in ~60-100s)
# Check: wc -c /tmp/nt5dbg_test.log (should be >36000 bytes)
# Kill QEMU, retrieve SREAD.TXT from disk
```

## Expected SREAD.TXT Success Output

```
SREAD_START
ATTR=0x00000020
CREATE_HANDLE=0x0000000C
READ_OK=0x00000001 READ_N=0x00000096 READ_LE=0x00000000
TEXT_BEGIN
HDD_SECOND_TXT: This file was read from the FAT16 hard disk image
TEXT_END
```

## Current SREAD.TXT Output (from a stale prior run)

```
SREAD_START
ATTR=0x00000020 ATTR_LE=0x00000000
CREATE_HANDLE=0x0000000C CREATE_LE=0x00000000
READ_OK=0x00000000 READ_N=0x00000000 READ_LE=0x0000001F
TEXT_BEGIN
TEXT_END
```

File is found (ATTR=0x20), handle opens (0x0C), but **ReadFile fails with ERROR_GEN_FAILURE (0x1F)**. This was from a test with `ISPCDF_BOTTOM` flags (our handler below ESDI_506), where ESDI_506 handled the read. ESDI_506 found the directory entry but couldn't complete the sector read. Possible causes:
- Test HDD on IDE index 3 (secondary slave) — ESDI_506 may not support this
- Our calldown at BOTTOM never receives the I/O to handle it ourselves
- Need to test with our handler ABOVE ESDI_506 (which requires non-BOTTOM flags)

**Key insight**: with `ISPCDF_BOTTOM | ISPCDF_PORT_DRIVER` (0x003), we're below ESDI_506 and never intercept reads. We need to be ABOVE ESDI_506 to intercept and handle D: reads ourselves via `ata_direct_ior`. But removing `ISPCDF_BOTTOM` (using just `ISPCDF_PORT_DRIVER` = 0x002) caused a crash at T+25 because our handler doesn't properly pass unhandled I/O to the next handler in the chain.

## Next Steps After First Boot Works

### Fix calldown handler to pass-through

The crash with flags=0x002 (above ESDI_506) happens because `ios_wdm_ior_entry` tries to handle ALL I/O without passing unhandled requests to the next handler. Fix:

1. After `ISP_INSERT_CALLDOWN`, save the previous handler address (from the DCB calldown chain)
2. In `ios_wdm_ior_entry`, check if we should handle this I/O (e.g., only for our target device)
3. For I/O we don't handle, call the next handler (pass-through)

The ASM `ios_wdm_passthru_stub` already implements pass-through via `g_late_next_handler`. The fix is to:
- Set `g_late_next_handler` to the previous handler after splice
- Use `ios_wdm_passthru_stub` as the calldown function instead of `ios_wdm_ior_entry`
- OR modify `ios_wdm_ior_entry` to fall through to `g_late_next_handler` for non-matching I/O

### Alternative: handle I/O from BOTTOM position

With `ISPCDF_BOTTOM`, our handler only sees I/O that ESDI_506 passes down (or fails on). We could:
- Check if ESDI_506 completed the I/O successfully — if not, retry with our `ata_direct_ior`
- This avoids the chain position issue entirely

## Architecture

```
SREAD.EXE → ReadFile("D:\SECOND.TXT") → VFAT
  → IOS calldown chain for D: DCB
    [above] TSD (type-specific driver)
    [above] Other layers
    [?????] Our ios_wdm_ior_entry (position depends on flags)
    [below] ESDI_506.PDR (stock IDE driver)
  → Handler reads sectors via ata_direct_ior (direct IDE port I/O 0x170-0x177)
  → Fills IOR buffer → IOS_BD_Command_Complete
  → VFAT returns data to SREAD
```

## Test HDD Layout

16MB FAT16, CHS 32/16/63. Partition at LBA 63. IDE index 3 (secondary slave, ports 0x170-0x177).
- Cluster 2 (TESTFILE.TXT, 105 bytes): LBA 128
- Cluster 3 (SECOND.TXT, 150 bytes): LBA 136
- Content: "HDD_SECOND_TXT: This file was read from the FAT16 hard disk image"

## Key Files

| File | Purpose |
|------|---------|
| `src/build_nt5_fixed.py` | LE fixup + static DRP injection. FIXED — all bisection fixes applied |
| `history/source/IOSBRIDGE.C` | Main driver: IOS registration (L2436), late-create (L2662), calldown (L3773), ATA I/O (L3879) |
| `history/source/VXDWRAP_NASM.ASM` | Control dispatch, DDB, ASM thunks. Has Sys_Critical_Init IOS call (unused) |
| `history/source/NT5RAW.VXD` | Compiled from Docker (181700 bytes). Includes both C fixes (always-late-create + flags) |
| `src/deploy_sysini.py` | Deploy to SYSTEM.INI or IOSUBSYS. Use WITHOUT --pdr |
| `artifacts_.../run_unix_once.sh` | Test harness. FORCE_IDE=1 for ATA direct I/O path |
| `artifacts_.../harness/SREAD.EXE` | Win32 test app that reads D:\SECOND.TXT |

## Recent Commits

```
d73c7bf SCSI: Contprompt — all code blockers solved, blocked on test image setup
2006689 SCSI: Fix ISP_INSERT_CALLDOWN flags + always run late-create on success
17278e8 SCSI: Root-cause ISP_INSERT_CALLDOWN crash — wrong flags value
90c418c SCSI: Bisect calldown crash — ISP_INSERT_CALLDOWN into active DCB causes WPE
f27282c SCSI: Fix 4 crash-causing build_nt5_fixed.py changes + SYSTEM.INI loading discovery
```

## What NOT to Retry

- **PDR mode** (`--pdr`): PDRs never get Init_Complete. Must use SYSTEM.INI `device=` line.
- **IOS_Register from Sys_Critical_Init**: crashes (nt5_init does PE loading too early)
- **ISP_INSERT_CALLDOWN flags=0x100**: corrupts calldown chain → WPE
- **ISP_INSERT_CALLDOWN flags=0x002 (PORT_DRIVER only)**: crashes at T+25 (handler doesn't pass through)
- **Build script with**: WRITABLE obj_flags (0x2047), SHARED entry_flags (0x03), hardcoded NTMINI resident name — all crash the LE loader
- **Fresh qcow2 images** without completing first-boot Hardware Wizard — splash screen hang

## Debug Log Interpretation

Normal successful init (36,293 bytes) ends with:
```
NTMINI: ios_register_port_driver()
R                          ← calling IOS_Register
r                          ← returned
s                          ← SUCCESS
NTMINI: IOS_Register OK, running late-create for HDD splice
L v u                     ← DRP status
M                          ← find_existing_ilb (called because IOS_Register succeeded but ILB not provided)
e                          ← ILB not found (normal for successful late registration)
w                          ← late-create returned error (no ILB available)
g                          ← g_ios_registered = 1
[AEP events]              ← H K (AEP_SYSTEM_SHUTDOWN, AEP_CRIT_SHUTDOWN — normal boot messages)
[control messages]         ← m00000005 m00000006 m00000026
```

The `W` marker in the log = our calldown handler `ios_wdm_ior_entry` was invoked. 8 occurrences confirmed.

If late-create's ILB steal fails (marker `e`), it means IOS_Register was successful and IOS managed the calldown setup internally via AEP. The phantom DCB creation still fails (no ILB for ISP_CREATE_DCB), but the IOS-managed registration works.

## Docker

Docker is needed for C/ASM recompilation: `bash history/source/build.sh nt5`

If Docker is down, binary patches can be applied to NT5RAW.VXD:
| Offset | From | To | Purpose |
|--------|------|----|---------|
| 0xF4D6 | 0xBE | 0xA0 | Late-create JE: always run ios_late_create_device |
| 0xFE34 | 00 01 00 00 | 03 00 00 00 | Splice flags: ISPCDF_BOTTOM\|ISPCDF_PORT_DRIVER |

These patches are already compiled into the current NT5RAW.VXD (commit 2006689).
