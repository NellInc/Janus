# Janus: Project Status

> **Mission:** improved global security and migration for the world's most important and often highly vulnerable legacy systems, by translating the device drivers that keep them stranded on unsupported Windows. The reverse direction (Win9x VxD on a modern NT kernel) is the migration path that lets unpatchable hosts be retired without scrapping the equipment. The same machinery serves digital preservation and software archaeology: retro gaming and emulation, computing museums, digital forensics, and keeping vintage software runnable. See the README "Why Janus Matters" for the full rationale.

## Reverse Direction — Janus's Other Face (the migration & security path)

The reverse loader runs unmodified Win9x VxD drivers as native kernel modules on a modern NT-family kernel. Validated headless in cloud x86 emulation (QEMU TCG, no KVM) on **both ReactOS 0.4.15 and a genuine Microsoft Windows 2000 kernel.**

**Nine Win9x VxD classes fully initialize on ReactOS; six of them also fully initialize on a real Windows 2000 kernel** — DDB located, the full `Sys_Critical_Init → Device_Init → Init_Complete` lifecycle returns OK, no bugcheck:

| VxD | Class | ReactOS | Real Win2K |
|-----|-------|:-------:|:----------:|
| VSERVER | File/print sharing server | ✓ | ✓ |
| SB16 | Sound Blaster 16 audio | ✓ | ✓ |
| MGAXDD | Matrox display | ✓ | ✓ |
| MMDEVLDR | Multimedia device loader | ✓ | ✓ |
| PCI | PCI bus enumerator | ✓ | ✓ |
| VJOYD | Joystick (input) | ✓ | ✓ |
| VNETBIOS | NetBIOS / network | ✓ | — |
| DSOUND | DirectSound audio | ✓ | — |
| ISAPNP | Plug-and-Play enumerator | ✓ | — |

The Win2K-kernel control-proc addresses (`0xBF9Exxxx`, kernel space) confirm these run on the genuine Microsoft kernel, not ReactOS. Delivered via a fully unattended floppy install plus registry auto-load (Start=2), no `sc.exe`.

**Harvest → build → host pipeline:** VJOYD was sourced fresh from Windows 98 SE media, embedded, built, and hosted end-to-end, proving the workflow on an arbitrary real vendor VxD rather than a hand-picked set.

**Loader generality:** the LE loader, source-list fixup engine, parameterized DDB-name scan, and control-proc dispatch are proven across **12+ VxDs** (every one loads, DDB located at +0x0C, control proc invoked, no BSOD). Those above complete the full init sequence; the rest load and dispatch but their `Device_Init` declines or faults (shim-caught, no crash) when it probes hardware absent from the emulator or hits an unsatisfied VMM service. That is a per-driver service-coverage frontier, not a loader limitation.

### Function frontier — beyond "init runs"
Initialization completing is necessary but not sufficient; the real measure is the hosted driver performing its function (DMA, IRQ, bus-enumeration, real I/O). **Two classes crossed at this frame bar:**

- **am1500t (AMD Lance ISA NIC, am1500t.PDR) — a TX frame on the wire.** The hosted VxD's own send path puts a real Ethernet frame onto the emulated network (captured in pcap), the operational trigger synthesized by the shim. This is the strongest reverse result: not configuration reads, but a packet egressing under the VxD's own code.
- **PCI.VXD — live bus enumeration on a real Windows 2000 kernel.** The hosted ring-0 context performs real PCI configuration I/O (ports 0xCF8/0xCFC) on the live i440fx bus and reads back the actual device list (440FX host bridge, PIIX3 ISA/IDE, PIIX4 ACPI, VGA, NIC); PCI.VXD's own configuration-read primitive reads back four distinct correct vendor:device IDs from the live bus, the VxD's own enumeration code doing real hardware I/O, not merely initializing against fakes.

**DMA / real NT memory:** an import-mode reverse shim (linking real `ntoskrnl.exe` imports, the first non-standalone reverse shim) loads and initializes on a real Windows 2000 kernel — the step toward routing a hosted driver's allocation to genuine NT contiguous-physical memory (`MmAllocateContiguousMemory`) for real DMA.

**Depth methodology:** the MRCI2 (DriveSpace compression) VxD was driven through three chained `Device_Init` fixes to a V86 real-mode boundary, validating a repeatable fault → disassemble → stub → rebuild loop for promoting decline/fault classes toward full init.

This is the face that lets a system stranded on Windows 9x carry its irreplaceable driver onto a modern, maintained kernel.

## Proven Drivers

Two ladders, kept separate. **Function-level** crossings (the driver's own code moved real data against faithful stock QEMU, unfakeable sentinel, adversarial review) and **load-proven** drivers (full PE load + imports resolved). The patched-QEMU set is held below the function bar.

### i386 PE32 — Function-level crossings (18, faithful stock QEMU)
| Device / Driver | Class | Sentinel |
|-----------------|-------|----------|
| ne2000 / rtl8029.sys | NDIS | ARP on the wire + TX-ring port I/O |
| AMD PCnet | NDIS | busmaster ring PA + received ARP via own HandleInterrupt + ARP TX |
| Realtek rtl8139 | NDIS 5.0 | TSAD0 ring PA + full ARP TX+RX |
| DEC tulip / dc21x4 | NDIS | ARP on the wire (0x0806) via own MiniportSend → busmaster DMA + own ISR DPC reclaim |
| Intel eepro100 / 8255x (e100bnt5.sys) | NDIS | own-code ARP on the wire + own ISR/DPC TX completion (MMIO-CSR) |
| virtio-net / netkvm.sys | NDIS 5.1 | own-code ARP on the wire + TX vring avail-idx post |
| Intel e1000 / 82540EM (e1000325.sys) | NDIS 5.1 | own-MAC ARP on the wire + SLIRP reply, busmaster DMA |
| Intel e1000e / 82574L (e1q5132.sys) | NDIS 5.2 | own-MAC ARP on the wire + SLIRP reply, TDH/TDT post |
| virtio-scsi / vioscsi.sys | ScsiPort | host-seeded READ(10) sector via virtio ring |
| VMware PVSCSI / pvscsi.sys | ScsiPort | host-seeded LBA1 sentinel via own HwStartIo READ(10) + busmaster DMA |
| LSI MegaRAID SAS1078 / megasas.sys | ScsiPort (MFI) | host-seeded LBA1 sentinel via own PD_SCSI_IO READ(10) + busmaster DMA |
| USB-EHCI MSC / usbehci | USB / EHCI | INQUIRY 'QEMU' + seeded sector via shim-built CBW |
| USB-MSC / usbstor.sys | USB / BOT | own SRB → CBW → BOT READ(10), seeded LBA 0x10 |
| ATAPI / atapi.sys | IDE/ATAPI | IDE command-block register crossing |
| serial 16550 / serial.sys | serial | own DLAB + divisor on 0x3F8 |
| i8042 keyboard START / i8042prt | input | own 0x60/0x64 START sequence |
| parport LPT / parport.sys | parallel | own OUT 0x378 + device-computed IN 0x379 → 0xD8 |
| Cirrus display DAC | VideoPort | V86-free OUT 0x3C8/0x3C9 DAC write |

### i386 PE32 — Load / DriverEntry proven
| Driver | NT Version | Class | Test |
|--------|-----------|-------|------|
| vga.sys (21K) | XP SP3 | VideoPort | DriverEntry + VideoPortInit |
| hidparse.sys (25K) | XP SP3 | ntoskrnl | DriverEntry + 30 HidP_* exports |
| hidgame.sys (9K) | XP SP3 | HID | DriverEntry + HidRegisterMinidriver |
| es1371mp.sys (41K) | XP SP3 | PortCls audio | 60/60 imports |
| aha154x.sys (9K) | **NT 3.51 (1995)** | ScsiPort | 17/17 imports |
| aha154x.sys (15K) | **NT 3.1 (1993)** | ScsiPort | all imports |
| ncr53c9x.sys (12K) | **NT 3.51** | ScsiPort | all imports |

### Demonstrated against a patched QEMU (below the stock-QEMU function bar)
| Device / Driver | Demonstrated | Why not stock QEMU |
|-----------------|--------------|---------------------|
| sym_hi.sys (LSI 53C810) | 9/9 sector READ+WRITE | needed a 5-patch QEMU for the 53C8xx SCRIPTS engine |
| LSI 53C895a SCRIPTS | READ+WRITE crossed | QEMU SCRIPTS MOVE-MEM device-model fidelity bug |
| buslogic.sys (8.5K, NT 3.51) | load + ScsiPort | no faithful QEMU path for BT-958 at the function bar |

### AMD64 PE32+ — PE Load Proven
| Driver | NT Version | Class | Imports |
|--------|-----------|-------|---------|
| hidparse.sys (41K) | XP x64 SP2 | ntoskrnl | all |
| pciide.sys (6K) | XP x64 SP2 | PCIIDEX | all |
| hidgame.sys (12K) | XP x64 SP2 | HIDCLASS | all |
| vgapnp.sys (33K) | XP x64 SP2 | VideoPort | all |
| ne2000.sys (22K) | XP x64 SP2 | NDIS | all |

### IA-64 PE32+ (Itanium) — PE Load Proven
| Driver | NT Version | Class | Imports |
|--------|-----------|-------|---------|
| pciide.sys (6.7K) | XP IA-64 | PCIIDEX | all |

## Shim Coverage

| Shim | Functions | Status | Proven Drivers |
|------|-----------|--------|---------------|
| ScsiPort (NTMINI_V5.C) | 22+ | **Production** | pvscsi, megasas, vioscsi (seeded-sector READ; megasas also WRITE+readback); aha154x, ncr53c9x (load) |
| NDIS (NDIS_SHIM.C) | 75+ | **Production** | ne2000, rtl8139, pcnet, tulip, eepro100, virtio-net, e1000, e1000e (8 NICs, frames on the wire) |
| StorPort (STORPORT_SHIM.C) | — | Written | XP/Server 2003+ import layer; Method-C port pending |
| VideoPort (VIDEO_SHIM.C) | 47+ | **Working** | Cirrus (DAC write); vga.sys, vgapnp x64 (init) |
| ntoskrnl+HAL (NTOS_SHIM.C) | 130+ | **Working** | hidparse, hidgame, pciide, ne2000 (x64) |
| HID (HID_SHIM.C) | 3 | **Working** | hidgame (load); live-report walled (headless input) |
| PCI IDE (PCIIDE_SHIM.C) | 3 | **Working** | pciide (x64, IA-64) |
| PortCls (AUDIO_SHIM.C) | 20+ | Partial | es1370 (register sub-crossing; full path structural) |
| USBD (USB_SHIM.C) | 15 | **Working** | usbstor, usbehci (seeded sector) |
| DirectDraw (DDRAW_SHIM.C) | 15+ | Written | — |
| WiFi (WIFI_SHIM.C) | 8 | Written | — |
| AGP (AGP_SHIM.C) | 10 | Written | — |
| Joystick (JOYSTICK_SHIM.C) | 5 | Written | — |

## PE Loader Capabilities

| Format | Machine | Architecture | Status |
|--------|---------|-------------|--------|
| PE32 | 0x014C | i386 (x86-32) | Full load + execute |
| PE32+ | 0x8664 | AMD64 (x86-64) | Full load + import resolution |
| PE32+ | 0x0200 | IA-64 (Itanium) | Full load + import resolution |
| PE32 | 0x0166 | MIPS R4000 LE | Planned (drivers on NT4 media) |
| PE32 | 0x0184 | Alpha AXP | Planned (drivers on NT4 media) |
| PE32 | 0x01F0 | PowerPC (PReP) | Planned (drivers on NT4 media) |

## Key Technical Achievements

1. **DMA VA→PA on Win9x** — Discovered Win9x page table self-map at PDE 0x3FE
   (not 0x3FF like NT). Only working method for physical address resolution.

2. **PE32+ loader** — Full 64-bit PE support including DIR64 relocations,
   8-byte IAT thunks, and cross-architecture import resolution.

3. **V86 INT 10h** — BIOS call execution via VMM nested V86 services
   (Begin_Nest_V86_Exec + Exec_Int + End_Nest_Exec).

4. **Multi-driver chains** — hidparse.sys loaded as library, exports registered,
   then hidgame.sys loaded with cross-DLL import resolution.

5. **12-year span** — NT 3.1 (1993) through XP x64 (2005) drivers on Win98.

## Extracted Driver Archives (832+)

| Source | Drivers |
|--------|---------|
| XP SP3 x86 | 24 |
| XP x64 SP2 (AMD64) | 214 |
| XP IA-64 (Itanium) | 191 |
| NT 3.1 (1993) | 127 |
| NT 3.5 (1994) | 146 |
| NT 3.51 (1995) | 154 |
