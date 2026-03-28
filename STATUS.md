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

## Needs End-to-End Testing

- IOR handler (NT4 path): fully implemented with CDB builders, queue management, and status translation, but not yet tested with live IOS traffic
- VPICD interrupt virtualization: code written, not wired into runtime path (currently using polling)
- Entire NT5 WDM path: all five modules compiled, no integration testing with NT5 binaries performed

## What's Next

- Integration testing with NT5 IDE driver binaries (pciidex.sys, pciide.sys, atapi.sys)
- Multi-DLL PE loader: extend PELOAD.C to handle cross-image imports across multiple .sys files
- End-to-end NT5 IDE stack test: PCI scan, driver load, PnP start, IOR through WDM bridge, data return
- Real hardware validation (tested only in QEMU so far)
- QEMU specific workarounds (status register patching) need conditional application for real hardware
