# Project Status

## What Works (NT4 ScsiPort Path)

- LE binary correction pipeline (all header fields validated against ESDI_506.PDR reference)
- FAT32 aware deployment (auto detects geometry, traverses directories, allocates clusters)
- Ring 0 PE loader: maps unmodified NT4 atapi.sys, processes relocations, resolves imports
- ScsiPort API shim: 22 functions matching MSVC calling conventions (including assembly stubs for 8-byte struct returns)
- HwFindAdapter multi-pass detection across four IDE channels
- HwStartIo with DeviceFlags patching for channel identity remapping
- HwScsiAdapterControl support for NT5 ScsiPort miniports
- Port I/O remapping (secondary IDE presented as primary)
- PCI config space reads via direct port access (0xCF8/0xCFC)
- IOS registration: REMAIN_RESIDENT confirmed
- ILB acquisition from APIX driver via DDB chain walking
- DCB creation (CD-ROM device type)
- Calldown chain installation in IOS request routing
- Drive letter association
- IOR-to-SRB translation: CDB construction for READ(10), WRITE(10), VERIFY(10), INQUIRY, REQUEST SENSE, TEST UNIT READY, READ CAPACITY, PREVENT ALLOW MEDIUM REMOVAL, START STOP UNIT
- SRB completion handling via ScsiPortNotification with IOR queue drain
- Win98 SE boots to desktop with driver loaded
- ISO 9660 sector 16 read confirmed ("CD001" magic bytes)
- QEMU based testing with debug output capture (debugcon + serial)

## NT5 WDM Support (Code Written, Untested)

Five new modules implement a WDM compatibility layer for hosting NT5 driver stacks
(pciidex.sys + pciide.sys + atapi.sys) inside the Win9x VxD environment.

- **NTKSHIM**: ntoskrnl.exe and HAL.dll function shim built on Win98 VMM services. Provides memory allocation, spinlocks, DPC queuing, registry stubs, and string utilities for NT5 kernel mode drivers.
- **IRPMGR**: NT I/O manager implementation. Handles device object lifecycle, IRP allocation with stack locations, IoCallDriver dispatch, IoCompleteRequest completion, and driver object management.
- **PNPMGR**: Minimal PnP and Power manager. Calls AddDevice to create FDOs, fabricates CM_RESOURCE_LISTs from known hardware parameters, sends IRP_MN_START_DEVICE, and provides PoXxx power stubs.
- **PCIBUS**: PCI bus enumeration and configuration for IDE controllers. Scans the PCI bus, creates PDOs for discovered controllers, and implements BUS_INTERFACE_STANDARD for pciidex.sys config access.
- **WDMBRIDGE**: Bridges the NT5 WDM IDE driver stack to Win9x IOS. Translates IOS I/O Requests into WDM IRPs with SCSI SRBs, dispatches them through the NT5 device stack, and translates completion status back to IOR format.

## SCSI HDD (sym_hi.sys + LSI 53C810, via PA repo)

- **SCSI HDD READ: WORKING** (9/9 sectors verified, READ(10) through ScsiPort shim)
- **SCSI HDD WRITE+VERIFY: WORKING** (9/9 sectors, WRITE(10) + VERIFY(10))
- LE merger tool available at `reference/tools/le_merge_objects.py` for single-object VxD output (required for Win98 VMM32 loading)
- QEMU PA cache fix proven: `lsi_fixup_addr` cache-before-CPU-check eliminates spurious DMA stalls
- Patched QEMU source and build scripts at `reference/qemu-patches/`

## Universal NT5→9x Driver Translation Framework (2026-05-17)

11 driver class shims (~14,200 lines total), 4 proven in QEMU:

| Class | Driver | Result |
|-------|--------|--------|
| ScsiPort (SCSI storage) | sym_hi.sys (LSI 53C810) | 9/9 READ+WRITE ✅ |
| NDIS 4.0 (network) | rtl8029.sys (NE2000 PCI) | Full ARP TX+RX ✅ |
| NDIS 5.0 (XP network) | rtl8139.sys (XP v5.397) | Full ARP TX+RX ✅ |
| VideoPort (display) | Built-in test | PCI GPU detect + VGA regs ✅ |

### Critical DMA Fix (2026-05-17)

Win9x maps recursive page tables at **PDE index 0x3FE** (VA 0xFFBFE000/0xFF800000),
not 0x3FF like Windows NT. All Win9x VMM services for VA→PA translation fail
(_PageAllocate PhysAddr output, _CopyPageTable, _MapPhysToLinear for RAM).
The fix reads PTEs directly from the Win9x self-map addresses.

### All 11 Shim Files

| File | Lines | Port Driver | Status |
|------|-------|-------------|--------|
| NTMINI_V5.C | 7033 | SCSIPORT.SYS | **PROVEN** |
| NDIS_SHIM.C | 3499 | NDIS.SYS + HAL.dll | **PROVEN** |
| VIDEO_SHIM.C | 679 | VIDEOPRT.SYS | **PROVEN** |
| USB_SHIM.C | 355 | USBD.SYS | Written |
| AUDIO_SHIM.C | 808 | PORTCLS.SYS | Written |
| DDRAW_SHIM.C | 598 | DirectDraw/D3D HAL | Written |
| HID_SHIM.C | 156 | HIDCLASS.SYS | Written |
| JOYSTICK_SHIM.C | 176 | VJOYD bridge | Written |
| WIFI_SHIM.C | 230 | 802.11 OID handler | Written |
| AGP_SHIM.C | 411 | AGPLIB.SYS | Written |
| PCIIDE_SHIM.C | 487 | PCIIDEX.SYS | Written |

## What's Next

- Fix HandleInterrupt crash in RTL8139 (NDIS structures not fully populated for RX path)
- Test real XP GPU miniport (needs VideoPortInt10 for VESA mode switching)
- USB test with USBSTOR.SYS or USB NIC
- Audio test with XP WDM audio miniport (e.g., CMI8738 cmipci.sys)
- Real hardware validation (tested only in QEMU so far)
