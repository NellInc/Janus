---
stepsCompleted: 4
status: IN_PROGRESS
predecessor: driver_backport_generalization_20260517.md
session_date: 2026-05-17
budget: ~$50
---

# Universal NT5→9x Driver Translation Framework (2026-05-17)

## TL;DR

11 driver class shims built (~14,200 lines total). 3 proven in QEMU. Single
infrastructure blocker remaining: DMA VA↔PA coherence. Once fixed, all
DMA-based drivers come alive simultaneously.

## What's Proven

| Class | Driver | Result |
|-------|--------|--------|
| ScsiPort (SCSI storage) | sym_hi.sys (LSI 53C810) | 9/9 READ+WRITE ✅ |
| NDIS 4.0 (network) | rtl8029.sys (NE2000 PCI) | Full ARP TX+RX ✅ |
| NDIS 5.0 (XP network) | rtl8139.sys (XP v5.397) | Full ARP TX+RX ✅ |
| VideoPort (display) | Built-in test | PCI GPU detect + VGA regs ✅ |

## All 11 Shim Files

| # | File | Lines | Port Driver | Status |
|---|------|-------|-------------|--------|
| 1 | NTMINI_V5.C | 7033 | SCSIPORT.SYS | **PROVEN** |
| 2 | NDIS_SHIM.C | 3299 | NDIS.SYS + HAL.dll | **PROVEN** |
| 3 | VIDEO_SHIM.C | 679 | VIDEOPRT.SYS (47 funcs) | **PROVEN** |
| 4 | USB_SHIM.C | 355 | USBD.SYS (15 funcs) | Written |
| 5 | AUDIO_SHIM.C | 808 | PORTCLS.SYS (20 funcs + COM vtables) | Written |
| 6 | DDRAW_SHIM.C | 598 | DirectDraw/D3D HAL callbacks | Written |
| 7 | HID_SHIM.C | 156 | HIDCLASS.SYS (3 funcs) | Written |
| 8 | JOYSTICK_SHIM.C | 176 | VJOYD bridge (HID→gamepad) | Written |
| 9 | WIFI_SHIM.C | 230 | 802.11 OID handler | Written |
| 10 | AGP_SHIM.C | 411 | AGPLIB.SYS (10 funcs) | Written |
| 11 | PCIIDE_SHIM.C | 487 | PCIIDEX.SYS (3 funcs + UDMA query) | Written |

All files at: `~/Documents/GitHub/PA/research/win98-dvd-fix/ntmini/source/`

## Architecture

```
Win98 Guest (QEMU)
─────────────────────────────────────────
NTMINI.VXD (single LE object, SYSTEM.INI loaded)
  ├── PE Loader (PELOAD.C, ~1213 lines)
  │   └── Multi-DLL import resolution via PORT_DRIVER_SHIM registry
  │
  ├── Class Shims (one per driver category):
  │   ├── ScsiPort    → IOS VxD bridging (HDD/CD-ROM read/write)
  │   ├── NDIS        → Direct packet TX/RX (ARP send+receive)
  │   ├── VideoPort   → PCI GPU detect, VGA register access, framebuffer map
  │   ├── USBD        → USB descriptor parsing, URB building
  │   ├── PortCls     → COM vtable IPortWaveCyclic/IDmaChannel
  │   ├── DirectDraw  → Surface create/lock/blt, D3D stub
  │   ├── HID         → HidRegisterMinidriver
  │   ├── Joystick    → HID report → VJOYD axis/button translation
  │   ├── WiFi        → 802.11 OID handling (scan, associate, WEP)
  │   ├── AGP         → GART aperture init, rate setting
  │   └── PCI IDE     → IDE controller properties, UDMA mode detection
  │
  └── VxD Infrastructure (VXDWRAP_V4.ASM, ~2600 lines)
      ├── VMM service wrappers, DDB, Device_Init dispatch
      ├── Safe_HwFindAdapter (IDT fault catching for init)
      ├── VxD_PageAllocateDMA (contiguous physical memory)
      └── PCI config space access (0xCF8/0xCFC)
```

## Key Bug Fix This Session

**NdisMSetAttributesEx was missing its 5th parameter (`AdapterType`).**
This caused a 4-byte stack drift that corrupted the return address of
MiniportInitialize. The RTL8139 XP driver now initializes successfully.
Lesson: verify EVERY shim function's parameter count against the DDK.

## DMA Infrastructure — SOLVED

**Root cause found:** Win9x maps the recursive page table at **PDE index 0x3FE**
(VA 0xFFBFE000 for page directory, 0xFF800000 for page tables), NOT index
0x3FF (0xFFFFF000/0xFFC00000) like Windows NT.

All previous PA resolution methods failed:
- VMM `_PageAllocate` doesn't populate the PhysAddr output on Win98
- VMM `_CopyPageTable` with CPT_SYSTEM returns 0 for system-allocated pages
- VMM `_MapPhysToLinear` refuses to map guest RAM addresses
- The NT self-map at 0xFFC00000/0xFFFFF000 is UNMAPPED on Win9x
- `_LinPageLock` doesn't help CopyPageTable succeed

**Fix:** `ndis_va_to_pa()` in NDIS_SHIM.C reads PTEs directly from Win9x
self-map: `PDE at 0xFFBFE000[idx]`, `PTE at 0xFF800000[pageNum]`.
VxD_GetPhysAddr in ASM also updated. Correct PA confirmed via QEMU QMP
`gva2gpa`/`xp` commands — real PA was 0x0D129000, not the 0x0B984000 that
VA-0xC0000000 heuristic produced.

**Result:** RTL8139 XP driver full ARP TX+RX in 1 poll iteration. DMA works
in both directions. This fix unblocks all 11 shim classes.

## Build Commands

### NDIS/RTL8139 (current focus for DMA fix)
```bash
cd ~/Documents/GitHub/PA/research/win98-dvd-fix/ntmini

docker run --rm -v "$PWD/source:/src" -v "$PWD/builds:/out" \
  ntmini-builder:latest sh -c "cd /src && \
  nasm -f obj -o /out/V5FULL_asm.obj VXDWRAP_V4.ASM && \
  wcc386 -bt=windows -3s -s -zl -d0 -nc=LCODE -nt=_LTEXT -nd=_LDATA \
    -zc -zdp -dNTMINI_USE_SCSI=3 NTMINI_V5.C -fo=/out/V5NDIS_c.obj && \
  wcc386 -bt=windows -3s -s -zl -d0 -nc=LCODE -nt=_LTEXT -nd=_LDATA \
    -zc -zdp PELOAD.C -fo=/out/V5FULL_pe.obj && \
  wcc386 -bt=windows -3s -s -zl -d0 -nc=LCODE -nt=_LTEXT -nd=_LDATA \
    -zc -zdp -dNDIS_TEST_RTL8139 NDIS_SHIM.C -fo=/out/NDIS_SHIM.obj && \
  cd /out && wlink @link_v5ndis.lnk"

python3 tools/le_merge_objects.py builds/V5NDIS.VXD builds/V5NDIS_MERGED.VXD
```

### VideoPort test
```bash
# Use -dNTMINI_USE_SCSI=4 and VIDEO_SHIM.C with -dVIDEO_TEST_BUILTIN
# Linker: builds/link_v5video.lnk
```

### Deploy + Test
```bash
cp builds/V5NDIS_MERGED.VXD V5FIXED4.VXD
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

/tmp/qemu-patched/qemu-10.2.2/build/qemu-system-i386 \
  -drive "file=/tmp/win98vm/win98.img,format=raw,if=ide,index=0" -m 256 \
  -drive "file=/tmp/win98vm/test_cd.iso,media=cdrom,if=ide,index=2" \
  -debugcon "file:/tmp/win98vm/debug_rtl8139.log" -display none \
  -qmp "unix:/tmp/win98vm/qmp_ndis.sock,server,nowait" \
  -netdev user,id=net0 -device rtl8139,netdev=net0
```

### Safe Mode dismissal (QMP)
```python
import socket, json, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.settimeout(3)
s.connect('/tmp/win98vm/qmp_ndis.sock'); s.recv(4096)
s.send(json.dumps({'execute':'qmp_capabilities'}).encode() + b'\n')
time.sleep(0.2); s.recv(4096)
for i in range(40):
    s.send(json.dumps({'execute':'human-monitor-command','arguments':{'command-line':'sendkey 1'}}).encode() + b'\n')
    time.sleep(0.3); s.recv(4096, socket.MSG_DONTWAIT) if True else None
    s.send(json.dumps({'execute':'human-monitor-command','arguments':{'command-line':'sendkey ret'}}).encode() + b'\n')
    time.sleep(0.7); s.recv(4096, socket.MSG_DONTWAIT) if True else None
```

## Next Steps (Priority Order)

1. **Fix HandleInterrupt crash** — RTL8139's HandleInterrupt faults on
   uninitialized NDIS structures during RX processing. Need to provide:
   - Valid NdisAllocatePacket/Buffer pools accessible from HandleInterrupt
   - Proper ReturnPacket flow so driver can recycle RX descriptors
   - Then full bidirectional network (TCP/IP) becomes possible
2. **Load real XP GPU miniport** — need cirrus.sys or vga.sys from XP install media
3. **VideoPortInt10** — V86 BIOS call for VESA mode switching (VMM Exec_VxD_Int)
4. **USB test with real driver** — need USBSTOR.SYS or USB NIC from XP
5. **Audio test** — need XP WDM audio miniport (e.g., CMI8738 cmipci.sys)

## Dead Ends (Don't Retry)

| Approach | Why It Fails |
|----------|-------------|
| NdisMSetAttributesEx with 4 params | Missing AdapterType → 4-byte stack drift → crash |
| ndis_StallExecution as safe-func for MB slots | Wrong __stdcall cleanup for unknown arg counts |
| MB data slots zeroed (0) for NDIS 5.0 | Driver reads DeviceObject, dereferences NULL → fault |
| MB data slots self-referential page | Driver loops infinitely reading self-pointers |
| Safe_HwFindAdapter IDT manipulation as crash cause | Direct call also crashes → IDT not the issue |
| RTL8139 timer struct zeroing (0x50 bytes) | Timer offsets correct, didn't cause crash |
| PA = VA - 0xC0000000 for DMA | Wrong for dynamically allocated pages (proven: real PA=0x0D129000 vs fallback=0x0B984000) |
| _CopyPageTable with CPT_SYSTEM | Returns 0 for PG_SYS allocated pages on Win98 |
| _MapPhysToLinear on RAM addresses | Win98 VMM refuses to map guest RAM (returns -1) |
| _PageAllocate PhysAddr output param | Win98 VMM doesn't populate it (sentinel 0xDEAD0000 survives) |
| Self-map at 0xFFC00000/0xFFFFF000 | That's NT's address. Win9x uses 0xFF800000/0xFFBFE000 (PDE index 0x3FE) |
| Unmasking PIC IRQ during RX poll | Win98 dispatches the IRQ, driver's ISR clears the ROK bit before polling sees it |

## Key Source Files

| File | Lines | Purpose |
|------|-------|---------|
| NTMINI_V5.C | 7033 | Main VxD: ScsiPort shim, init dispatch |
| NDIS_SHIM.C | 3299 | NDIS: 80+ functions, packet TX/RX, RTL8139 support |
| VIDEO_SHIM.C | 679 | VideoPort: 47 functions, PCI, VGA, framebuffer |
| AUDIO_SHIM.C | 808 | PortCls: 20 functions + 4 COM vtable objects |
| DDRAW_SHIM.C | 598 | DirectDraw: surface mgmt, BLT, D3D stubs |
| PCIIDE_SHIM.C | 487 | PCI IDE: controller props, UDMA detection |
| AGP_SHIM.C | 411 | AGP: capability detect, rate setting, GART stubs |
| USB_SHIM.C | 355 | USBD: descriptor parsing, URB creation |
| WIFI_SHIM.C | 230 | WiFi: 802.11 OID handler |
| JOYSTICK_SHIM.C | 176 | Joystick: HID report → VJOYD translation |
| HID_SHIM.C | 156 | HID: minidriver registration |
| PELOAD.C | 1213 | Multi-DLL PE loader |
| VXDWRAP_V4.ASM | 2600 | VxD infrastructure, VMM services, DMA |

## Docker / VM Images

- Builder: `ntmini-builder:latest` (Open Watcom 2.0 + NASM)
- Fresh disk: `~/Documents/VMs/win98vm/win98_fresh.img` (8.5GB, VxD at cluster 72127)
- Patched QEMU: `/tmp/qemu-patched/qemu-10.2.2/build/qemu-system-i386`
- XP drivers available: `/tmp/win98vm/rtl8139_xp.sys` (25,434 bytes)

## Repos

- **PA**: `~/Documents/GitHub/PA/research/win98-dvd-fix/ntmini/` (primary dev)
- **nt5-9x-driver-backport**: `~/Documents/GitHub/nt5-9x-driver-backport/` (public)
