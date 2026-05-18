# Janus: Project Status

## Proven Drivers (19)

### i386 PE32 — Full Hardware I/O
| Driver | NT Version | Class | Test |
|--------|-----------|-------|------|
| sym_hi.sys (LSI 53C810) | XP SP3 | ScsiPort | 9/9 sector READ+WRITE |
| rtl8029.sys (NE2000 PCI) | XP SP3 | NDIS 4.0 | Full ARP TX+RX |
| rtl8139.sys (Realtek) | XP SP3 | NDIS 5.0 | Full ARP TX+RX (DMA) |
| ne2000.sys | XP SP3 | NDIS 3.0 | Full ARP TX+RX |

### i386 PE32 — DriverEntry Proven
| Driver | NT Version | Class | Test |
|--------|-----------|-------|------|
| vga.sys (21K) | XP SP3 | VideoPort | DriverEntry + VideoPortInit |
| hidparse.sys (25K) | XP SP3 | ntoskrnl | DriverEntry + 30 HidP_* exports |
| hidgame.sys (9K) | XP SP3 | HID | DriverEntry + HidRegisterMinidriver |

### i386 PE32 — PE Load Proven (all imports resolved)
| Driver | NT Version | Class | Imports |
|--------|-----------|-------|---------|
| es1371mp.sys (41K) | XP SP3 | PortCls audio | 60/60 |
| aha154x.sys (9K) | **NT 3.51 (1995)** | ScsiPort | 17/17 |
| aha154x.sys (15K) | **NT 3.1 (1993)** | ScsiPort | all |
| buslogic.sys (8.5K) | **NT 3.51** | ScsiPort | all |
| ncr53c9x.sys (12K) | **NT 3.51** | ScsiPort | all |

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
| ScsiPort (NTMINI_V5.C) | 22+ | **Production** | sym_hi, aha154x, buslogic, ncr53c9x |
| NDIS (NDIS_SHIM.C) | 75+ | **Production** | rtl8029, rtl8139, ne2000 |
| VideoPort (VIDEO_SHIM.C) | 47+ | **Working** | vga.sys, vgapnp x64 |
| ntoskrnl+HAL (NTOS_SHIM.C) | 130+ | **Working** | hidparse, hidgame, pciide, ne2000 (x64) |
| HID (HID_SHIM.C) | 3 | **Working** | hidgame |
| PCI IDE (PCIIDE_SHIM.C) | 3 | **Working** | pciide (x64, IA-64) |
| PortCls (AUDIO_SHIM.C) | 20+ | Partial | es1371mp (loads, crash in init) |
| USBD (USB_SHIM.C) | 15 | Written | — |
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
| PE32 | 0x0166 | MIPS R4000 | Planned |
| PE32 | 0x0184 | Alpha AXP | Planned |

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
