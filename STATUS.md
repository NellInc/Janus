# Project Status

## What Works

- LE binary correction pipeline (all header fields validated against ESDI_506.PDR reference)
- FAT32 aware deployment (auto detects geometry, traverses directories, allocates clusters)
- Ring 0 PE loader: maps unmodified NT4 atapi.sys, processes relocations, resolves imports
- ScsiPort API shim: 22 functions matching MSVC calling conventions (including assembly stubs for 8-byte struct returns)
- HwFindAdapter multi-pass detection across four IDE channels
- HwStartIo with DeviceFlags patching for channel identity remapping
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

## Needs End-to-End Testing

- IOR handler: fully implemented with CDB builders, queue management, and status translation, but not yet tested with live IOS traffic
- VPICD interrupt virtualization: code written, not wired into runtime path (currently using polling)

## What's Next

- Real hardware validation (tested only in QEMU so far)
- End-to-end IOR testing: verify IOS block reads flow through the calldown chain and return data
- QEMU specific workarounds (status register patching) need conditional application for real hardware
- Testing with additional miniport drivers beyond NEC ATAPI
