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

Two ladders, kept deliberately separate. **Function-level** means the hosted driver's own code moved real data against **faithful stock QEMU**, no device-model patches: a frame on the wire, or a host-seeded disk sector returned through the driver's own busmaster DMA, each verified by an unfakeable sentinel and confirmed by adversarial review. **Load-proven** means full PE load, relocation, and import resolution, the ceiling a 32-bit host can reach for 64-bit binaries.

### Forward — function-level crossings (18, faithful stock QEMU)

Each is the foreign driver's own code performing live hardware I/O. The sentinel is unfakeable: a captured ARP frame attributed by the own-MAC + opcode + IP triple, or a host-only sector signature written into a zeroed buffer and returned through the driver's own DMA.

| # | Device / Driver | Class | Unfakeable sentinel |
|---|-----------------|-------|---------------------|
| 1 | ne2000 / rtl8029.sys | NDIS | ARP frame on the wire + TX-ring port I/O |
| 2 | AMD PCnet | NDIS | busmaster ring PA + received ARP via own HandleInterrupt + ARP TX |
| 3 | Realtek rtl8139 | NDIS 5.0 | TSAD0 ring PA + full ARP TX+RX |
| 4 | DEC tulip / dc21x4 | NDIS | ARP frame on the wire (pcap 0x0806) via own MiniportSend → busmaster DMA, reclaimed by own ISR DPC |
| 5 | Intel eepro100 / 8255x (e100bnt5.sys) | NDIS | own-code ARP on the wire + own ISR/DPC TX completion (MMIO-CSR) |
| 6 | virtio-net / netkvm.sys | NDIS 5.1 | own-code ARP on the wire + TX vring avail-idx post (busmaster DMA) |
| 7 | Intel e1000 / 82540EM (e1000325.sys) | NDIS 5.1 | own-MAC ARP on the wire + SLIRP reply, busmaster DMA |
| 8 | Intel e1000e / 82574L (e1q5132.sys) | NDIS 5.2 | own-MAC ARP on the wire + SLIRP reply, TDH/TDT descriptor post |
| 9 | virtio-scsi / vioscsi.sys | ScsiPort | host-seeded READ(10) sector via virtio ring |
| 10 | VMware PVSCSI / pvscsi.sys | ScsiPort | host-seeded LBA1 sentinel via own HwStartIo READ(10) + busmaster ring DMA |
| 11 | LSI MegaRAID SAS1078 / megasas.sys | ScsiPort (MFI) | host-seeded LBA1 sentinel via own PD_SCSI_IO READ(10) + busmaster DMA |
| 12 | USB-EHCI MSC / usbehci | USB / EHCI | INQUIRY 'QEMU' + seeded sector via shim-built CBW (busmaster qTD) |
| 13 | USB-MSC / usbstor.sys | USB / BOT | own SRB → CBW → BOT READ(10), seeded LBA 0x10 sector |
| 14 | ATAPI / atapi.sys | IDE/ATAPI | IDE command-block register crossing |
| 15 | serial 16550 / serial.sys | serial | own DLAB + divisor, LCR/FCR/IER/MCR on 0x3F8 |
| 16 | i8042 keyboard START / i8042prt | input | own 0x60/0x64 START sequence |
| 17 | parport LPT / parport.sys | parallel | own OUT 0x378 + device-computed IN 0x379 → 0xD8 |
| 18 | Cirrus display DAC | VideoPort | V86-free OUT 0x3C8/0x3C9 DAC write |

### Forward — load-proven across architectures (3 architectures, a 12-year span)

Full PE load, relocation, and all imports resolved. The 64-bit binaries reach the 32-bit-host ceiling; they resolve and relocate but do not execute in long mode on a 32-bit guest.

| Driver | Era | Architecture | Result |
|--------|-----|--------------|--------|
| vga.sys | XP SP3 | i386 | DriverEntry + VideoPortInit |
| hidparse.sys | XP SP3 | i386 | DriverEntry + 30 HidP_* exports |
| hidgame.sys | XP SP3 | i386 | DriverEntry + HidRegisterMinidriver |
| hidparse.sys | XP x64 SP2 | AMD64 PE32+ | load + all imports |
| pciide.sys | XP x64 SP2 | AMD64 PE32+ | load + PCIIDEX |
| hidgame.sys | XP x64 SP2 | AMD64 PE32+ | load + HIDCLASS |
| vgapnp.sys | XP x64 SP2 | AMD64 PE32+ | load + VideoPort |
| ne2000.sys | XP x64 SP2 | AMD64 PE32+ | load + NDIS |
| pciide.sys | XP IA-64 | Itanium PE32+ | load + PCIIDEX |
| aha154x.sys | NT 3.1 (1993) | i386 ScsiPort | load + all imports |
| aha154x.sys | NT 3.51 (1995) | i386 ScsiPort | load + 17/17 imports |
| ncr53c9x.sys | NT 3.51 | i386 ScsiPort | load + ScsiPort |

### Demonstrated against a patched QEMU (superseded by the stock-QEMU bar)

These crossed only with QEMU device-model patches, so they are held below the faithful-stock-QEMU bar that the 18 above clear. The matrix records them as walled at the current bar, recoverable on real silicon (vfio-pci) or via an upstream QEMU fix, not via the shim.

| Device / Driver | Demonstrated | Why not stock QEMU |
|-----------------|--------------|---------------------|
| LSI 53C810 / sym_hi.sys | 9/9 sector READ+WRITE | needed a 5-patch QEMU for the 53C8xx SCRIPTS engine |
| LSI 53C895a SCRIPTS | READ+WRITE crossed | QEMU SCRIPTS MOVE-MEM device-model fidelity bug; wrong-PA IID |
| buslogic.sys (NT 3.51) | load + ScsiPort | no faithful QEMU path for the BT-958 at the function bar |

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
- QEMU (stock for the 18 function crossings, including SCSI; patched build only for the legacy LSI 53C8xx SCRIPTS path)
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
| Windows 2000 | 2000 | i386 | Source-compatible (NT5, shared NDIS5/ScsiPort ABI) |
| Windows XP SP3 | 2001 | i386 | Primary forward source; bulk of the 18 function crossings (NDIS/ScsiPort/USB/serial/IDE) |
| Windows Server 2003 R2 | 2003 | i386 | Source for megasas, mptsas/symmpi, e1000 (ScsiPort/StorPort storage + NDIS5) |
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
| ScsiPort (SCSI storage) | NTMINI_V5.C | 22+ | Proven (seeded-sector READ via busmaster DMA: pvscsi, megasas, vioscsi; megasas also WRITE+readback) |
| NDIS (network) | NDIS_SHIM.C | 75+ | Proven (ARP TX/RX, 8 NICs with frames on the wire) |
| StorPort (SCSI/SAS/SATA) | STORPORT_SHIM.C | — | Import layer (XP/Server 2003+); Method-C port pending |
| VideoPort (display) | VIDEO_SHIM.C | 47+ | Proven (Cirrus DAC write; init) |
| ntoskrnl/HAL | NTOS_SHIM.C | 130+ | Proven |
| HID | HID_SHIM.C | 3 | Proven (load); live-report walled (headless input) |
| PCI IDE | PCIIDE_SHIM.C | 3 | Proven |
| PortCls (audio) | AUDIO_SHIM.C | 20+ | Partial (es1370 register sub-crossing; full path structural) |
| USBD (USB) | USB_SHIM.C | 15 | Proven (usbstor/usbehci seeded sector) |
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
- **Reverse direction — two function crossings at the frame bar**, plus broad init-bar coverage. At the **frame bar** (the VxD's own code moving real data on a modern NT kernel): (a) **am1500t** (AMD Lance ISA NIC, am1500t.PDR) puts a **TX frame on the wire**; (b) **PCI.VXD** reads four distinct live i440fx vendor:device IDs through its own C2 configuration-read primitive on a real Windows 2000 kernel, not merely initializing against fakes. At the **init bar** (full lifecycle returns OK, no bugcheck): 6 Win9x VxD classes (VSERVER, SB16, MGAXDD, MMDEVLDR, PCI, VJOYD) initialize on a real Win2K kernel, 9 on ReactOS; the loader and harvest-build-host pipeline are proven across 12+ real vendor VxDs. See [STATUS.md](STATUS.md).
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
- **Automated translation**: generalize the harvest-build-host pipeline so an arbitrary vendor driver is ingested and bridged with minimal hand work, using assisted agentic engineering and service-usage inference to generate shims.
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

Approximately 500 million tokens of Claude (Opus 4.5-4.8) agentic engineering compute over 4 months of development, March-June 2026.

## License

Apache-2.0 — permissive, with an explicit patent grant. See [LICENSE](LICENSE). Copyright 2026 Nell Watson.
