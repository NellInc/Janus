---
stepsCompleted: 5
status: IN_PROGRESS
predecessor: scsi_hdd_read_20260517.md
session_date: 2026-05-17
budget: ~$30
---

# NT Driver Backport — Full Generalization (2026-05-17)

## TL;DR

SCSI HDD READ+WRITE proven (9/9), ScsiPort shim generalized for any miniport,
ISA 8237 DMA implemented, LE object merger solves Win98 boot. Next: implement
VideoPort/NDIS/PortCls shims for graphics/network/audio driver backporting.

## What's Working (All Validated 2026-05-17)

| Test | Result | Driver | Controller |
|------|--------|--------|-----------|
| SCSI HDD READ | 9/9 SUCCESS | sym_hi.sys | LSI 53C810 |
| SCSI HDD WRITE+VERIFY | 9/9 SUCCESS | sym_hi.sys | LSI 53C810 |
| SCSI CD-ROM detection | 9/9 FOUND | sym_hi.sys | LSI 53C810 |
| IDE D: mount | PASS | (old VxD) | IDE |
| IDE HDD READ/WRITE | PASS | atapi.sys (WDM) | IDE (in backport repo) |

## Architecture Overview

```
Win98 Guest                         QEMU Host
─────────────────────────────────────────────────
NTMINI.VXD (our VxD, single LE object)
  ├── PE Loader (PELOAD.C) — loads any .sys at ring 0
  ├── ScsiPort API shim (37 functions, polling mode)
  │   ├── Generic: GetBusData, GetPhysAddr, Notification, Port/Register I/O
  │   ├── LSI-specific: SCRIPTS DMA, GPREG/EEPROM, SIGP (#ifdef NTMINI_LSI_SCRIPTS)
  │   └── ISA DMA: FlushDma, IoMapTransfer (8237 controller, for aha154x)
  ├── sym_hi.sys (NT ScsiPort miniport, PE loaded)
  ├── PCI detection: class 01h, subclass 00h/04h/07h (SCSI/RAID/SAS)
  └── VxD_PageAllocateDMA → DMA-safe buffers

QEMU lsi53c895a.c (patched, 6 fixes):
  ├── lsi_fixup_addr: PA cache lookup BEFORE current_cpu check ← THE KEY FIX
  ├── lsi_bar1_self_check: MMIO self-routing
  ├── SCNTL1 CON preservation
  ├── STIME1 no immediate GEN
  ├── read_dword/lsi_do_command/status/msgin via lsi_mem_read/write
  └── Table-indirect + Memory Move via lsi_mem_read/write
```

## Three Key Discoveries (Session)

### 1. QEMU PA Cache Fix (Root Cause of HDD Hang)

`lsi_fixup_addr()` checked `current_cpu` BEFORE the 64-entry PA translation
cache. When SCSI disk I/O completed asynchronously (from QEMU's event loop),
`current_cpu` was NULL, the cache was skipped, and `read_dword()` returned
zeros for all SCRIPTS instruction fetches. SCRIPTS marched through 500 empty
opcodes per batch, never executing the reselection handler.

**Fix**: Move cache lookup before `current_cpu` check (2 lines). Cache is
populated during command submission (when CPU context exists), so async
callbacks resolve PAs from cache. File: `lsi53c895a.c:470-498`.

### 2. Win98 VMM32 Requires 1-Object VxDs

Win98's VMM32 loader crashes when loading multi-object LE VxDs from
`SYSTEM.INI device=` lines. Watcom's wlink produces 2+ objects because
string literals create a CONST segment with class DATA (separate from LCODE).

**Fix**: `le_merge_objects.py` post-link tool merges LE objects into 1.
Adjusts all fixup records (promotes 16-bit to 32-bit offsets when merged
object offsets exceed 0xFFFF). Build flags `-nc=LCODE -nt=_LTEXT -nd=_LDATA
-zc -zdp` minimize objects before merging. PELOAD.C must also be compiled
with these flags.

### 3. QEMU format=raw Required

Without explicit `format=raw` for the boot disk, QEMU restricts write
operations ("write operations on block 0 will be restricted"). Win98 needs
to write during boot (registry, pagefile). Use:
`-drive "file=win98.img,format=raw,if=ide,index=0"` instead of `-hda`.

## Dead Ends (Don't Retry)

| Approach | Why It Fails |
|----------|-------------|
| lsi_disconnect reselect intercept | Hangs SeaBIOS SCSI enumeration with HDD present |
| lsi_transfer_data inline reselect | Unnecessary — PA cache fix alone is sufficient |
| deploy_sysini.py for different-size VxDs | FAT cluster reallocation not needed; direct cluster overwrite works |
| Changing SYSTEM.INI file size | VMM32 doesn't care about INI size but edits risk corruption |
| Multi-object VxDs (2+) in SYSTEM.INI | VMM32 crashes. Must be exactly 1 LE object |
| `-hda` without format=raw | Restricts writes, hangs boot |
| `option offset=0x0` in wlink | Changes base address but doesn't reduce object count |
| `segment type flat` in wlink | Invalid directive for VxD format |
| Nesting `#ifdef` for segment class | Watcom ignores pragma segment directives in flat model |

## Build Pipeline

### SCSI VxD (sym_hi.sys)

```bash
cd ~/Documents/GitHub/PA/research/win98-dvd-fix/ntmini

# 1. Compile (Docker + Open Watcom)
docker run --rm -v "$PWD/source:/src" -v "$PWD/builds:/out" \
  ntmini-builder:latest sh -c "cd /src && \
  nasm -f obj -o /out/V5FULL_asm.obj VXDWRAP_V4.ASM && \
  wcc386 -bt=windows -3s -s -zl -d0 -nc=LCODE -nt=_LTEXT -nd=_LDATA \
    -zc -zdp -dNTMINI_USE_SCSI=1 NTMINI_V5.C -fo=/out/V5SCSI_c.obj && \
  wcc386 -bt=windows -3s -s -zl -d0 -nc=LCODE -nt=_LTEXT -nd=_LDATA \
    -zc -zdp PELOAD.C -fo=/out/V5FULL_pe.obj && \
  cd /out && wlink @link_v5scsi.lnk"

# 2. Merge LE objects (1-object requirement)
python3 tools/le_merge_objects.py builds/V5SCSI.VXD builds/V5SCSI_MERGED.VXD

# 3. Verify
python3 -c "
import struct
with open('builds/V5SCSI_MERGED.VXD', 'rb') as f: data = f.read(512)
print(f'Objects: {struct.unpack_from(\"<I\", data, 0x80+68)[0]}')  # Must be 1
"

# 4. Deploy (direct cluster overwrite, no FAT changes)
cp builds/V5SCSI_MERGED.VXD V5FIXED4.VXD
cp ~/Documents/VMs/win98vm/win98_fresh.img /tmp/win98vm/win98.img
python3 -c "
import struct, sys; sys.path.insert(0, '.'); import deploy_to_iosubsys as fat
with open('V5FIXED4.VXD', 'rb') as f: vxd = f.read()
vxd_padded = vxd + b'\x00' * (177162 - len(vxd))
with open('/tmp/win98vm/win98.img', 'r+b') as f:
    fat.detect_fat32_geometry(f)
    chain = fat.get_cluster_chain(f, 72127)
    pos = 0
    for c in chain:
        f.seek(fat.cluster_to_offset(c))
        f.write(vxd_padded[pos:pos+4096])
        pos += 4096
"
```

### QEMU (Patched)

```bash
# Build from scratch (if /tmp wiped):
bash research/win98-dvd-fix/ntmini/qemu-patches/build-patched-qemu.sh
cd /tmp/qemu-patched/qemu-10.2.2/build && ninja qemu-system-i386

# Binary: /tmp/qemu-patched/qemu-10.2.2/build/qemu-system-i386
```

### Test Command

```bash
/tmp/qemu-patched/qemu-10.2.2/build/qemu-system-i386 \
  -drive "file=/tmp/win98vm/win98.img,format=raw,if=ide,index=0" -m 256 \
  -drive "file=/tmp/win98vm/test_cd.iso,media=cdrom,if=ide,index=2" \
  -debugcon "file:/tmp/win98vm/debug_pci.log" -display none \
  -qmp "unix:/tmp/win98vm/qmp_pci.sock,server,nowait" \
  -device lsi53c810,id=scsi0 \
  -device "scsi-cd,drive=scsicd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0" \
  -drive "id=scsicd0,file=/tmp/win98vm/test_cd.iso,if=none,media=cdrom,readonly=on" \
  -device "scsi-hd,drive=scsihd0,bus=scsi0.0,channel=0,scsi-id=1,lun=0" \
  -drive "id=scsihd0,file=/tmp/win98vm/scsi_hd.img,if=none,format=raw"

# Send keypresses via QMP (Safe Mode dismissal):
# "1" + Enter repeatedly for ~60s, then wait ~90s for boot + VxD init

# Success markers in debug_pci.log:
# SP: *** SCSI HDD READ SUCCESS ***
# SP: *** SCSI HDD WRITE+VERIFY SUCCESS ***
# SP: *** SCSI CD-ROM FOUND ***
```

## ScsiPort Generalization Status

### Functions by Category

**Fully Generic (works for any miniport):**
ScsiPortInitialize, ScsiPortGetBusData, ScsiPortSetBusDataByOffset,
ScsiPortGetPhysicalAddress, ScsiPortGetUncachedExtension,
ScsiPortNotification, ScsiPortCompleteRequest, ScsiPortMoveMemory,
ScsiPortStallExecution, ScsiPortLogError, ScsiPortConvertUlongToPhysicalAddress,
all ReadPort/WritePort (Uchar/Ushort/Ulong), all ReadPortBuffer/WritePortBuffer,
all ReadRegister/WriteRegister (Uchar/Ushort/Ulong), all ReadRegisterBuffer/WriteRegisterBuffer

**LSI-Specific (guarded with #ifdef NTMINI_LSI_SCRIPTS):**
GPREG/EEPROM intercepts in ReadPortUchar/WritePortUchar/ReadRegisterUchar/WriteRegisterUchar,
SIGP flush in WritePortUchar, SCRIPTS DMA patching (scripts_patch_mmio_va),
FAKE_ROM setup, pre-HwStartIo shadow register snapshots, GetDeviceBase BAR0→BAR1 redirect

**ISA DMA (for aha154x and similar):**
ScsiPortFlushDma (masks channel), ScsiPortIoMapTransfer (programs 8237).
Guarded by g_isa_dma_channel sentinel (0xFFFFFFFF = not ISA).

**Partial/Stub:**
ScsiPortGetVirtualAddress (only reverses g_dma_virt range; misses DevExt/SrbExt)

### Ready Miniport Drivers

| Miniport | Controller | PCI/ISA | Expected Status |
|----------|-----------|---------|----------------|
| sym_hi.sys | LSI 53C810/895A | PCI | PROVEN WORKING |
| symc8xx.sys | LSI 53C8xx (older) | PCI | Should work (same family) |
| aic78xx.sys | Adaptec 7870/7880 | PCI | Should work (generic ScsiPort) |
| mraid35x.sys | MegaRAID | PCI | Should work (subclass 0x04 accepted) |
| aha154x.sys | Adaptec 1540/1542 | ISA | Should work (8237 DMA implemented) |

## Next Steps: Multi-Class Driver Framework

### Design Document

`~/Documents/GitHub/nt5-9x-driver-backport/reference/DRIVER_FRAMEWORK.md`

Covers PORT_DRIVER_SHIM struct, registration API, per-class shim specs,
multi-DLL PE loader extension, and priority ranking.

### Implementation Priority (by impact × feasibility / effort)

1. **VideoPort shim** (videoprt.sys → Win98 mini-VDD)
   - Enables NT graphics miniport drivers on Win98
   - Key functions: VideoPortInitialize, VideoPortGetDeviceBase, VideoPortMapMemory
   - Example drivers: vga.sys, ati.sys, nv4_mini.sys, s3virge.sys
   - Bridges to: Win98 VDD/mini-VDD display driver model
   - Estimated: ~3000 lines of shim code
   - Highest user demand for retro computing

2. **NDIS shim** (ndis.sys → Win98 NDIS 4.0)
   - Enables NT network miniport drivers on Win98
   - Key functions: NdisInitializeWrapper, NdisMRegisterMiniport, NdisMIndicateReceivePacket
   - Example drivers: ne2000.sys, e1000.sys, rtl8139.sys
   - Bridges to: Win98 NDIS 4.0 layer (ndis.vxd)
   - Estimated: ~4000 lines
   - NDIS API similar across versions, easiest non-storage class

3. **PortCls shim** (portcls.sys → Win98 MMSYSTEM)
   - Enables NT audio miniport drivers on Win98
   - Key functions: PcInitializeAdapterDriver, PcNewPort, PcNewMiniport
   - Example drivers: ac97.sys, ich.sys, es1370.sys
   - Bridges to: Win98 MMSYSTEM wave/mixer VxD model
   - Estimated: ~5000 lines

4. **StorPort shim** (storport.sys → Win98 IOS)
   - Enables XP+ SATA/NVMe storage drivers on Win98
   - Similar to ScsiPort but with queue management and DPC support
   - Example drivers: ahci.sys, nvme.sys
   - Estimated: ~3500 lines

5. **USB host controller shim** (usbd.sys → Win98 UHCD/OPENHCI VxDs)
   - Enables NT USB host controller miniport drivers on Win98
   - Key functions: USBD_CreateConfigurationRequestEx, USBD_ParseConfigurationDescriptor,
     URB submission/completion, pipe management
   - Example drivers: uhci.sys (UHCI), openhci.sys (OHCI), usbehci.sys (EHCI)
   - Bridges to: Win98 USBD.SYS / UHCD.VXD / OPENHCI.VXD stack
   - Estimated: ~6000 lines (highest complexity — URB/IRP translation + hub driver)
   - Win98 already has USB 1.1 support; value is backporting EHCI (USB 2.0) drivers

### Multi-DLL PE Loader Extension

PELOAD.C currently loads single .sys files. For driver stacks
(pciidex.sys + pciide.sys + atapi.sys), needs:
- LOADED_IMAGE registry for cross-image import resolution
- Load order: dependencies first
- Shared ntoskrnl/HAL export table as pseudo-shim
- See DRIVER_FRAMEWORK.md for detailed design

## Key Source Locations

| File | Repo | Purpose |
|------|------|---------|
| `source/NTMINI_V5.C` | PA | Main VxD: ScsiPort shim, SCSI init, HDD test |
| `source/PELOAD.C` | PA | Generic PE loader for .sys files |
| `source/VXDWRAP_V4.ASM` | PA | ASM thunks, DDB, VxD service wrappers |
| `tools/le_merge_objects.py` | PA | LE object merger (1-object VxDs) |
| `qemu-patches/lsi53c895a-patched.c` | PA | Patched QEMU source |
| `history/source/IOSBRIDGE.C` | backport | WDM bridge: IOS splice, ATA I/O |
| `history/source/VXDWRAP_NASM.ASM` | backport | ASM thunks (29 SCSI symbols present) |
| `reference/DRIVER_FRAMEWORK.md` | backport | Generic framework design doc |

## Repos

- **PA**: `~/Documents/GitHub/PA/research/win98-dvd-fix/ntmini/`
  (primary development, SCSI miniport path)
- **nt5-9x-driver-backport**: `~/Documents/GitHub/nt5-9x-driver-backport/`
  (public repo, IDE+SCSI, framework design)

## Docker

- Image: `ntmini-builder:latest` (Open Watcom + NASM)
- Start Docker if needed: `open -a Docker` then wait ~2 min

## VM Images

- Fresh: `~/Documents/VMs/win98vm/win98_fresh.img` (8.5GB, has old NTMINI at cluster 72127)
- Clean: `~/Documents/VMs/win98vm/en_win98_se.qcow2` (no NTMINI, convert with qemu-img)
- SCSI HDD: `/tmp/win98vm/scsi_hd.img` (16MB raw, recreated by test if missing)
- Test ISO: `/tmp/win98vm/test_cd.iso` (recreated with hdiutil if missing)

## Deploy Gotchas

1. **Direct cluster overwrite only** — deploy_sysini.py's FAT reallocation
   works correctly per the FAT32 spec but is unnecessary. Overwrite VxD
   content at existing clusters (72127 chain), pad to 177162 bytes, don't
   change directory entry size.

2. **LE merger MUST run** — Watcom produces 2+ object VxDs. Win98 VMM32
   crashes on multi-object VxDs from SYSTEM.INI. Always merge before deploy.

3. **format=raw MUST be specified** — QEMU restricts writes without it.

4. **PELOAD.C needs same compiler flags** — `-nc=LCODE -nt=_LTEXT -nd=_LDATA
   -zc -zdp` must be used for PELOAD.C too, otherwise it creates extra LE
   objects that the merger has to handle (still works, just bigger).
