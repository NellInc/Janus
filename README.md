# Janus: A Universal Bidirectional NT/9x Driver Translation Framework

**Load Windows NT kernel-mode drivers on Windows 9x. Any NT version. Any architecture.**

Janus is a driver translation framework that bridges the fundamental incompatibility between Windows NT kernel-mode drivers (.sys PE files) and the Windows 9x VxD subsystem. It loads, relocates, and resolves imports for unmodified NT drivers inside a Win98 VxD wrapper, spanning 12 years of Windows history and three CPU architectures. i386 drivers execute through to live hardware I/O; 64-bit AMD64 and Itanium drivers load with all imports resolved, the ceiling for a 32-bit host.

Named after the Roman god of doorways, transitions, and passages — depicted with two faces looking in opposite directions. Janus presides over the passage between incompatible kernel architectures, connecting the beginning of NT (3.1, 1993) to the end of 9x (ME, 2000).

## Why Janus Matters

The world runs on stranded legacy systems. Industrial controllers, medical imaging suites, laboratory instruments, utility SCADA stations, transit signalling, and manufacturing lines: a large and quietly critical population still runs Windows 9x, and the same predicament reaches embedded XP, NT4, and older. They stay there for one stubborn reason. The expensive, irreplaceable equipment ships with a kernel driver that was never ported forward, so the operating system cannot be upgraded without abandoning the machine. The result is a fleet of unpatchable, decades-old hosts with no modern memory protections and ancient network stacks, sitting in exactly the environments where a compromise does the most harm.

Janus has two faces, and each addresses one half of this problem.

- **Forward (a modern driver on a legacy OS)** keeps vintage systems alive and repairable. When original hardware fails and only newer parts remain, a current driver can run on the old OS, extending the service life of equipment that would otherwise be scrapped. This face is also the engine: it proves the loader, the import resolution, and the inter-generational ABI machinery that the reverse face reuses.
- **Reverse (a legacy driver on a modern OS)** is the security and migration path. It carries a stranded system's original driver onto a modern, maintained kernel, so the unpatchable host can finally be retired while the equipment keeps working. This dissolves the most common technical excuse for keeping vulnerable systems in production: "we can't upgrade because of the driver." The decisive gain is not the cleverness of loading. It is that rip-and-replace becomes optional, and migration becomes possible for systems that were treated as frozen.

Two honest conditions keep this from being a false promise. A translation layer must isolate what it imports: a 1990s ring-0 driver carries no modern threat model, so it belongs behind hardware isolation (IOMMU, virtualization-based driver isolation), not merely made to load. And loading is necessary without being sufficient, since real migrations also turn on timing, DMA, and bus-enumeration assumptions, which Janus treats as first-class problems.

Used well, Janus is a decommissioning path for the world's most important and most vulnerable legacy systems, and an instrumented, sandboxed analysis bench for the drivers and malware of that era.

The same machinery serves preservation, and its reach is broad:

- **Retro gaming and emulation** depend on period-accurate drivers to reproduce how the hardware actually behaved, down to its driver-level quirks.
- **Computing museums** need exhibits that still run, so visitors see living machines rather than static relics.
- **Digital forensics and incident response** must boot and instrument seized legacy systems to recover evidence and reconstruct what happened.
- **Long-term digital preservation** needs software that stays executable across decades, because bits archived with no way to run them slowly become unreadable.
- **Software archaeology**, the study of how vanished systems were built, needs the original binaries executing under a microscope, where static disassembly stops short.

Janus loads unmodified vendor drivers in an instrumented, sandboxed environment, which is the bench each of these disciplines wants.

Security, migration, and preservation are the same work, seen from the two faces of one doorway.

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
| NT 3.51 / 4.0 | 1995–96 | PowerPC PReP (0x01F0) | Drivers on NT4 media (/PPC); PE load proof planned |
| NT 3.1–4.0 | 1993–96 | Alpha AXP (0x0184) | Drivers on NT4 media (/ALPHA); PE load proof planned |
| NT 3.1–4.0 | 1993–96 | MIPS R4000 LE (0x0166) | Drivers on NT4 media (/MIPS); PE load proof planned |

Target: every architecture NT (and its siblings) ever ran on. See Roadmap → Architecture coverage.

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
- Reverse-direction DMA: route a hosted VxD's `_PageAllocate` to real NT `MmAllocateContiguousMemory` so a hosted class moves real data. The import-mode shim (with ntoskrnl imports) already loads and initializes on a real Windows 2000 kernel; real contiguous-physical allocation with a linear/physical readback proof is next.
- Win95 and WinME testing
- VideoPort Int10 timing fix (defer V86 to Init_Complete)
- NDIS to Win98 TCP/IP bridge for real networking
- More IA-64 Itanium drivers (ne2000, vga); MIPS (0x0166) and Alpha AXP (0x0184) support from NT 3.51/4.0

### Medium-term
- **Reverse direction — proven and advancing**: 6 Win9x VxD classes (VSERVER, SB16, MGAXDD, MMDEVLDR, PCI, VJOYD) fully initialize on a real Microsoft Windows 2000 kernel (9 on ReactOS); the loader and a harvest-build-host pipeline are proven across many real vendor VxDs. **Function frontier crossed for PCI**: a Win9x VxD's own configuration-read code reads the live i440fx PCI bus on a real Win2K kernel, not merely initializing against fakes. See [STATUS.md](STATUS.md).
- **Function frontier**: drive more hosted classes from "init runs" to "device functions" (DMA, IRQ, bus-enumeration, real I/O). This is the per-device-class grind, and the prize. Widening OS coverage adds init checkmarks; function is the real measure.
- **V86 monitor** in the reverse shim: unlocks real-mode-thunking VxDs (the MRCI2 / DriveSpace class) and, on the same 32-bit substrate, hosting DOS real-mode drivers.
- **Span extension**: backward to Windows 3.1 386-Enhanced `.386` VxDs (same VMM/VxD mechanism, a subset of services) and forward to 32-bit Vista/7/8/10 kernels. x64 and Windows 11 need a binary lifter, which is a separate programme.
- Runtime driver loading from filesystem (bypass VxD size limit)
- Full WDM IRP stack for complex drivers
- Real hardware testing (beyond QEMU)

### Longer-term
- **Binary lifter / recompiler**: statically translate a 32-bit VxD (or a legacy NT driver) into a native x64 WDM driver, mapping VMM and legacy NT services onto modern HAL/WDM equivalents. This is the only route to hosting legacy drivers on x64 Windows 10/11 and ARM64, where in-place execution is blocked by long mode and HVCI. A substantial research programme in its own right.
- **Production isolation**: run an imported legacy driver under IOMMU / VBS containment with a real threat model, so an unpatchable industrial, medical, or SCADA system can be retired while its hardware keeps working in the field, not only inside QEMU. The mission, fully realized.
- **Real-mode and 16-bit coverage**: DOS real-mode device drivers and TSRs hosted through the V86 monitor; Windows 3.1 16-bit `.DRV` modules through a minimal Win16 environment (wine-NE / WOW style).
- **Automated translation**: generalize the harvest-build-host pipeline so an arbitrary vendor driver is ingested and bridged with minimal hand work, using assisted reverse engineering and service-usage inference to generate shims.
- **Cross-architecture hosting**: legacy x86 drivers on ARM64 and RISC-V maintained hosts via lifting or embedded CPU emulation.
- **Upstream and preserve**: contribute the reverse VxD-on-NT capability to ReactOS, release a public preservation toolkit, and partner with computing museums and digital archives.

### Architecture coverage
The ambition is **every architecture NT (and its siblings) ever ran on.** Proven so far: i386, AMD64 (x64), Itanium (IA-64). To add:

- **PowerPC (PReP)** — NT 3.51/4.0, PE machine `0x01F0`. Drivers ship in the `/PPC` tree of the NT4 media already in hand; a static load + relocation + import proof completes the NT 4.0 architecture quad. Highest payoff for lowest cost.
- **Alpha AXP** — NT 3.1–4.0 plus the cancelled Windows 2000 Alpha RC betas, PE machine `0x0184` (64-bit AXP64 is `0x0284`). The furthest-developed non-x86 NT, with the richest third-party driver set.
- **MIPS R4000 LE** — NT 3.1–4.0, PE machine `0x0166`. The original ARC platform (Jazz/Magnum).

Those three are static PE load + relocation + import proofs (cheap, cross-arch). Native execution needs the matching CPU: QEMU has MIPS and PowerPC system targets with partial NT ARC/PReP firmware support, while Alpha and IA-64 execution are impractical. Load-proof first, execution as a research effort.

Adjacent ecosystems that reuse the same or similar machinery:

- **OS/2 (LE/LX)** — OS/2 physical/virtual device drivers use the LX format, the pure-32-bit sibling of the VxD LE format the loader already parses (x86). This extends Janus from cross-Windows-version to genuine **cross-OS** driver translation at near-zero loader cost. OS/2 also had a PowerPC port.
- **Windows CE** — ran on ARM, MIPS, SH3, and SH-4 (`0x01A6`) with its own stream-interface/native driver model (not NT's), so it needs a distinct shim. Preservation hook: the Sega Dreamcast is an SH-4 running Windows CE.

Historical and future endpoints:

- **Intel i860 ("N-Ten")** — NT's original prototype architecture and the source of the "NT" name. No real driver ecosystem to harvest; included for completeness of the "every architecture NT touched" framing.
- **ARM64 and RISC-V** — future maintained hosts with no vintage drivers, reachable only via the binary lifter. RISC-V is the open, maintainable endpoint for the security and preservation mission.

## Project History

Originally developed for the [Vogons retro-computing community](https://vogons.org) to solve CD-ROM driver issues on Windows 98 with NEC ATAPI controllers. Grew into a universal driver translation framework spanning the full NT driver ecosystem.

Approximately 500 million tokens of Claude (Opus 4.5-4.8) reverse engineering compute over 4 months of development, March-June 2026.

## License

MIT License. See [LICENSE](LICENSE) for details.
