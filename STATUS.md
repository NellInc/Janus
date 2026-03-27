# Project Status

## What Works

- LE binary correction pipeline (all header fields validated against ESDI_506.PDR reference)
- FAT32 aware deployment (auto detects geometry, traverses directories, allocates clusters)
- IOS registration: driver loads and receives REMAIN_RESIDENT from IOS
- ILB acquisition from APIX driver via DDB chain walking
- DCB creation (CD ROM device type)
- Calldown chain installation in IOS request routing
- Drive letter association
- Win98 SE boots to desktop with driver loaded
- QEMU based testing with debug output capture

## What Doesn't Work Yet

- IOR handler: I/O requests are stubbed, not translated to SCSI commands
- Runtime PE section mapping: headers read but sections not loaded
- VMM memory allocation for DMA buffers and adapter structures
- VPICD interrupt virtualization: code written but not wired into the driver
- Full SCSI CDB translation beyond basic commands

## What's Next

- Real hardware validation (tested only in QEMU so far)
- IOR to SRB translation for actual disc reads
- Testing with additional miniport drivers beyond NEC ATAPI
