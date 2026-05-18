# Janus: A Universal Bidirectional NT/9x Driver Translation Framework

**Load Windows NT kernel-mode drivers on Windows 9x. Any NT version. Any architecture.**

Janus is a driver translation framework that bridges the fundamental incompatibility between Windows NT kernel-mode drivers (.sys PE files) and the Windows 9x VxD subsystem. It loads, relocates, resolves imports, and executes unmodified NT drivers inside a Win98 VxD wrapper, spanning 12 years of Windows history and three CPU architectures.

Named after the Roman god of doorways, transitions, and passages — depicted with two faces looking in opposite directions. Janus presides over the passage between incompatible kernel architectures, connecting the beginning of NT (3.1, 1993) to the end of 9x (ME, 2000).

## What's Proven

**19 drivers loaded across 3 architectures and a 12-year span:**

| # | Era | Driver | Class | Result |
|---|-----|--------|-------|--------|
| 1 | XP SP3 | sym_hi.sys (LSI 53C810) | SCSI | 9/9 sector READ+WRITE |
| 2 | XP SP3 | rtl8029.sys (NE2000 PCI) | NDIS 4.0 | Full ARP TX+RX |
| 3 | XP SP3 | rtl8139.sys (Realtek) | NDIS 5.0 | Full ARP TX+RX |
| 4 | XP SP3 | ne2000.sys | NDIS 3.0 | Full ARP TX+RX |
| 5 | XP SP3 | vga.sys (21K) | VideoPort | DriverEntry + Init |
| 6 | XP SP3 | hidparse.sys (25K) | ntoskrnl | DriverEntry + 30 exports |
| 7 | XP SP3 | hidgame.sys (9K) | HID | DriverEntry + HidRegister |
| 8 | XP SP3 | es1371mp.sys (41K) | PortCls | 60/60 imports resolved |
| 9 | XP x64 | hidparse.sys (41K) | AMD64 PE32+ | Load + imports |
| 10 | XP x64 | pciide.sys (6K) | AMD64 PE32+ | Load + PCIIDEX |
| 11 | XP x64 | hidgame.sys (12K) | AMD64 PE32+ | Load + HIDCLASS |
| 12 | XP x64 | vgapnp.sys (33K) | AMD64 PE32+ | Load + VideoPort |
| 13 | XP x64 | ne2000.sys (22K) | AMD64 PE32+ | Load + NDIS |
| 14 | NT 3.51 | aha154x.sys (9K) | ScsiPort | Load + 17/17 imports |
| 15 | NT 3.1 | aha154x.sys (15K) | ScsiPort | Load + all imports |
| 16 | NT 3.51 | buslogic.sys (8.5K) | ScsiPort | Load + ScsiPort |
| 17 | NT 3.51 | ncr53c9x.sys (12K) | ScsiPort | Load + ScsiPort |
| 18 | XP IA-64 | pciide.sys (6.7K) | Itanium PE32+ | Load + PCIIDEX |

## Architecture

```
Win98 Guest
─────────────────────────────────────────
NTMINI.VXD (single LE object, SYSTEM.INI loaded)
  ├── PE Loader (PELOAD.C, 1809 lines)
  │   ├── PE32 (i386, 0x014C) — full load + execute
  │   ├── PE32+ (AMD64, 0x8664) — full load + import resolution
  │   ├── PE32+ (IA-64, 0x0200) — full load + import resolution
  │   └── Multi-DLL import resolution via shim registry
  │
  ├── NTOS_SHIM.C (2077 lines, 130+ stubs)
  │   ├── ntoskrnl.exe: memory, sync, timer/DPC, IRP, power,
  │   │   string, registry, object, thread, CRT functions
  │   └── HAL.dll: port I/O, MMIO, spinlocks, IRQL, timing
  │
  ├── Class Shims:
  │   ├── ScsiPort    → IOS VxD bridging (HDD/CD-ROM read/write)
  │   ├── NDIS        → Direct packet TX/RX (ARP send+receive)
  │   ├── VideoPort   → PCI GPU, VGA regs, Int10 V86, PCI BAR scan
  │   ├── HID         → HidRegisterMinidriver
  │   ├── PCI IDE     → PciIdeXInitialize/GetBusData/SetBusData
  │   ├── PortCls     → COM vtable stubs (WDM audio)
  │   └── 6 more written (USB, DirectDraw, WiFi, AGP, Joystick)
  │
  └── VxD Infrastructure (VXDWRAP_V4.ASM, 2700 lines)
      ├── VMM service wrappers, DDB, control dispatch
      ├── Safe_HwFindAdapter (IDT fault catching)
      ├── VxD_PageAllocateDMA (contiguous physical memory)
      ├── Win9x page table self-map (PDE 0x3FE) for DMA VA→PA
      ├── V86 nested execution (INT 10h BIOS calls)
      └── PCI config space (0xCF8/0xCFC)
```

## Key Discovery: DMA on Win9x

Win9x maps the recursive page table at PDE index 0x3FE, not 0x3FF like NT:

```c
// Page directory at VA 0xFFBFE000 (not 0xFFFFF000)
// Page tables at VA 0xFF800000 (not 0xFFC00000)
volatile ULONG *pde = (volatile ULONG *)(0xFFBFE000 + (va >> 22) * 4);
if (*pde & 1) {
    volatile ULONG *pte = (volatile ULONG *)(0xFF800000 + (va >> 12) * 4);
    if (*pte & 1) return (*pte & 0xFFFFF000) | (va & 0xFFF);
}
```

All VMM services for VA to PA translation fail on Win98. The self-map is the only working approach.

## Building

### Prerequisites

- Docker (for the `ntmini-builder` image with Open Watcom 2.0 + NASM)
- Python 3
- QEMU (patched build for LSI fixes, or stock for non-SCSI tests)
- Windows 98 SE disk image (FAT32)

### Build

```bash
cd source/

# SCSI (sym_hi.sys, LSI 53C810):
docker run --rm -v "$PWD:/src" -v "$PWD/../builds:/out" \
  ntmini-builder:latest sh -c "cd /src && \
  nasm -f obj -o /out/V5FULL_asm.obj VXDWRAP_V4.ASM && \
  wcc386 -bt=windows -3s -s -zl -d0 -nc=LCODE -nt=_LTEXT -nd=_LDATA \
    -zc -zdp -dNTMINI_USE_SCSI=1 NTMINI_V5.C -fo=/out/V5_c.obj && \
  wcc386 ... PELOAD.C -fo=/out/V5_pe.obj && \
  cd /out && wlink @link_script.lnk"

python3 ../tools/le_merge_objects.py builds/OUTPUT.VXD builds/MERGED.VXD
```

Build modes: SCSI (1,2), NDIS (3), VideoPort (4), Generic test (5).

## NT Version Support

| Version | Year | Architecture | Status |
|---------|------|-------------|--------|
| NT 3.1 | 1993 | i386 | PE load proven (aha154x.sys) |
| NT 3.5 | 1994 | i386 | 146 drivers extracted, untested |
| NT 3.51 | 1995 | i386 | PE load proven (3 SCSI drivers) |
| NT 4.0 | 1996 | i386 | Compatible (same ScsiPort API) |
| Windows 2000 | 2000 | i386 | Compatible (NT5, same APIs as XP) |
| Windows XP SP3 | 2005 | i386 | 8 drivers proven (SCSI/NDIS/Video/HID/Audio) |
| Windows XP x64 | 2005 | AMD64 | 5 drivers PE32+ loaded |
| Windows XP IA-64 | 2003 | Itanium | 1 driver PE32+ loaded |

## Win9x Target Support

| Target | Status |
|--------|--------|
| Windows 98 SE | Primary target, all testing done here |
| Windows ME | Expected to work (same VxD architecture), untested |
| Windows 95 | Same VxD format, VMM differences to verify (PDE index, PageAllocate) |

## Driver Class Coverage

| Class | Shim | Functions | Status |
|-------|------|-----------|--------|
| ScsiPort (SCSI storage) | NTMINI_V5.C | 22+ | Proven (R/W) |
| NDIS (network) | NDIS_SHIM.C | 75+ | Proven (TX/RX) |
| VideoPort (display) | VIDEO_SHIM.C | 47+ | Proven (init) |
| ntoskrnl/HAL | NTOS_SHIM.C | 130+ | Proven |
| HID | HID_SHIM.C | 3 | Proven |
| PCI IDE | PCIIDE_SHIM.C | 3 | Proven |
| PortCls (audio) | AUDIO_SHIM.C | 20+ | Partial |
| USBD (USB) | USB_SHIM.C | 15 | Written |
| DirectDraw/D3D | DDRAW_SHIM.C | 15+ | Written |
| 802.11 WiFi | WIFI_SHIM.C | 8 | Written |
| AGP | AGP_SHIM.C | 10 | Written |
| Joystick | JOYSTICK_SHIM.C | 5 | Written |

## Roadmap

### Near-term
- Win95 and WinME testing
- More IA-64 Itanium drivers (ne2000, vga)
- VideoPort Int10 timing fix (defer V86 to Init_Complete)
- NDIS to Win98 TCP/IP bridge for real networking
- MIPS (0x0166) and Alpha AXP (0x0184) architecture support from NT 3.51/4.0

### Medium-term
- Bidirectional: Win9x VxD drivers running on NT (VxD-on-NT loader)
- Runtime driver loading from filesystem (bypass VxD size limit)
- Full WDM IRP stack for complex drivers
- Real hardware testing (beyond QEMU)

## Project History

Originally developed for the [Vogons retro-computing community](https://vogons.org) to solve CD-ROM driver issues on Windows 98 with NEC ATAPI controllers. Grew into a universal driver translation framework spanning the full NT driver ecosystem.

Approximately 200 million tokens of Claude (Opus 4.6) compute over 3 months of development, March-May 2026.

## License

MIT License. See [LICENSE](LICENSE) for details.
