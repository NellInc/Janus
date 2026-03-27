# ARCHITECTURE: NT5 Miniport Backport to Windows 9x

## Overview

This project backports Windows NT5 (2000/XP era) miniport storage drivers to run
under Windows 98 SE. The central artifact is NTMINI.VXD, a ring-0 VxD that loads
unmodified NT .sys miniport binaries, exposes a ScsiPort API shim for them to call
into, and bridges the result into Win9x's IOS (I/O Supervisor) subsystem. The
driver occupies layer 5 of the Win98 storage stack: the Port Driver (PDR) slot.

The system comprises five principal source modules, a PE image loader, an interrupt
virtualization layer, and a build pipeline that produces corrected LE binaries
suitable for VMM loading.


## Win98 Storage I/O Stack

```
+-------------------------------------------+
|  File System Driver (VFAT, CDFS, UDF)     |   Layer 1
+-------------------------------------------+
|  Volume Tracker (VOLTRACK.VXD)            |   Layer 2
+-------------------------------------------+
|  Type-Specific Driver (FS to block I/O)   |   Layer 3
+-------------------------------------------+
|  Vendor Filters (optional)                |   Layer 4
+-------------------------------------------+
|  Port Driver (PDR)  <-- NTMINI.VXD       |   Layer 5
+-------------------------------------------+
|  Hardware                                 |
+-------------------------------------------+
```

IOS communicates with port drivers through two packet types. AEP (Async Event
Packets) deliver system lifecycle events: initialization, device arrival, boot
completion. IOR (I/O Requests) carry actual data transfer commands. NTMINI.VXD
must handle both.


## Source Modules

### NTMINI_V5.C (~2700 lines): Main VxD Logic

This file contains the PE loader front end, the ScsiPort API shim, and hardware
detection logic. At load time it reads an NT4 .sys binary from a buffer, allocates
ring-0 memory through VMM, processes PE relocations, and resolves imports against
the internal ScsiPort function table.

The ScsiPort shim implements 22 functions that NT miniport drivers expect to find
at link time. These include ScsiPortGetDeviceBase, ScsiPortGetPhysicalAddress,
ScsiPortNotification, ScsiPortReadPortUchar, and others. Port I/O is performed
through Watcom inline pragmas rather than HAL calls, since Win9x provides no HAL.

Hardware detection calls the miniport's HwFindAdapter entry point, which probes
standard IDE base addresses (0x1F0, 0x170) and returns adapter configuration.
The configuration feeds into DCB creation during IOS registration.

### IOSBRIDGE.C (~1600 lines): IOS Integration

This module handles everything between IOS and the miniport. It registers with IOS
via a DRP (Device Registration Packet), creates DCBs (Device Control Blocks) for
detected hardware, installs calldown chain entries, and translates IOR packets into
SCSI Request Blocks (SRBs) that the miniport can process.

Three breakthroughs define this module's history:

**DRP struct packing.** The DRP structure must use #pragma pack(push,1). Without
byte packing, the C compiler inserts padding after the DRP_revision field at
offset +0x24, which shifts every subsequent field by 3 to 8 bytes. IOS reads
DRP fields by absolute offset, so misalignment causes silent registration failure.
This was the root cause of months of debugging.

**LGN (Logical Group Number) encoding.** The LGN field requires a bitmask, not a
bit index. The correct value is 0x00400000 (1 << 0x16), corresponding to
DRP_ESDI_PD. Using the raw bit number 0x16 as the value produces a completely
wrong device class. ESDI_506.PDR from the Windows 98 distribution confirmed the
correct encoding.

**ILB (Import Library Base) acquisition.** IOS does not provide the ILB pointer
to drivers that register after boot. The workaround walks the VMM VxD DDB chain
starting from VMM_Get_DDB(0x0010), searches each registered driver's DRP for an
eyecatcher string "XXXXXXXX", and steals the ILB function pointer from whichever
APIX driver already has one. This is the only documented method for late
registration.

### PELOAD.C (~900 lines): PE Image Loader

Reads PE/COFF headers from a memory buffer containing an NT .sys binary. Maps each
section into ring-0 linear address space allocated via VMM. Applies base relocations
so the image runs at its loaded address rather than its preferred base. Resolves
import directory entries against the ScsiPort function table defined in NTMINI_V5.C.
Returns the virtual address of the miniport's DriverEntry function.

### IRQHOOK.C (~400 lines): Interrupt Virtualization

Integrates with VPICD (Virtual Programmable Interrupt Controller Device) to
virtualize hardware IRQ lines. When the physical IDE controller asserts an
interrupt, the VPICD callback in this module forwards the event to the miniport's
HwInterrupt routine. Handles EOI signaling, IRQ masking, and DIRQL
synchronization to prevent reentrant interrupt servicing.

This code is written but not yet wired into the runtime path.

### VXDWRAP.ASM: Assembly Entry Points

Provides the DDB (Device Descriptor Block) header with the device name
"NTMINI_DDB" and the control procedure that VMM calls during VxD lifecycle events.
Handles Sys_Dynamic_Device_Init and Sys_Dynamic_Device_Exit messages. Contains
service trampolines for VPICD and VMM calls that cannot be issued from C.


## Calling Conventions and Data Structures

### ILB Service Calls

```
ILB_service_rtn(ISP *packet):
    Load EDX with pointer to ISP packet
    Push EDX onto stack
    CALL [ILB_service_rtn address]
    Read result from ISP+0x02
```

### ISP Packet Layout (packed)

```
Offset  Size     Field
+0x00   USHORT   func (1=CREATE_DCB, etc.)
+0x02   USHORT   result
+0x04   ...      reserved
+0x0C   ...      extended fields (function-specific)
```

Key ISP function codes: ISP_CREATE_DCB (1), ISP_INSERT_CALLDOWN, ISP_ASSOCIATE_DCB.

### DRP Layout (packed, #pragma pack(push,1))

```
Offset  Size     Field
+0x00   ...      initial fields
+0x24   BYTE     DRP_revision
+0x25   BYTE     DRP_feature_code
  ...
+0x2C   DWORD    DRP_reg_result (set to REMAIN_RESIDENT on success)
```

### AEP Header

Effective size is 12 bytes (9 packed bytes plus 3 bytes of IOS padding). Extended
AEP data begins at offset +0x0C.

### IOR to SRB Translation

IOS sends IOR packets describing block read/write operations. IOSBRIDGE.C must
translate each IOR into an SRB (SCSI Request Block) with the appropriate CDB
(Command Descriptor Block). The translation covers INQUIRY, READ(10),
TEST UNIT READY, and other SCSI commands needed by the miniport.


## LE Binary Correction Pipeline

Win98 VMM loads VxDs from LE (Linear Executable) format binaries. The build
toolchain does not produce LE files that VMM accepts without correction.
build_sysini_fixed.py post-processes the linker output to fix five categories
of errors.

### File Layout

```
Offset 0x00:   MZ DOS header (0x80 bytes)
Offset 0x80:   LE header (0xC4 bytes)
LE-relative:   LOADER SECTION
                 Object table
                 Page map
                 Resident names
                 Entry table
               FIXUP SECTION
                 Fixup Page Table (FPT)
                 Fixup Record Table (FRT)
               [alignment padding]
File-absolute: PAGE DATA (code and data pages)
File-absolute: Nonresident name table
```

### Corrections Applied

1. **data_pages_off**: This offset must be file-absolute, not LE-relative. VMM
   seeks directly to this file position when loading page data. An LE-relative
   value causes VMM to read from the wrong location.

2. **Loader section ordering**: The four loader structures (object table, page map,
   resident names, entry table) must be contiguous and in that exact order. Gaps
   or reordering cause VMM to misparse the loader section.

3. **Fixup section placement**: The FPT and FRT must follow immediately after the
   loader section with no intervening data. VMM calculates fixup offsets relative
   to the fixup section start.

4. **Extended LE header fields**: Three fields at the end of the LE header require
   specific values. LE+0xB8 holds the end-of-module-data offset. LE+0xBC must
   contain approximately 0x1F4 (purpose not fully characterized, but omission
   causes load failure). LE+0xC0 must be 0x04000000, the Win386 device identifier.

5. **Nonresident name table**: Must appear after page data at a file-absolute
   offset recorded in the LE header. Import table offsets must point to valid
   file positions even when the tables are empty.


## Current State

### Working

IOS registration succeeds (DRP_reg_result returns REMAIN_RESIDENT). The ILB
pointer is acquired from the APIX driver via DDB chain walking. A DCB is created
with CD-ROM device type. The calldown chain entry is installed so IOS routes
requests to NTMINI.VXD. A drive letter is associated. Win98 boots to desktop
with the driver loaded.

### Not Yet Working

The IOR handler is a stub. When IOS sends an I/O request through the calldown
chain, the driver does not yet translate it into SRB commands for the miniport.
Full VMM page allocation for DMA buffers is not implemented. PE section mapping
at runtime is incomplete. VPICD interrupt wiring is written but not connected.

### Remaining Work

1. **IOR handler**: translate IOS block requests into SCSI CDBs, dispatch through
   the miniport's HwStartIo, handle completion via the DPC mechanism.
2. **DMA buffer allocation**: use VMM_PageAllocate to obtain physically contiguous
   memory below 16 MB for ISA DMA, or use scatter/gather descriptors for PCI.
3. **Runtime PE mapping**: fully map all .sys sections with correct page
   protections, not just code and import sections.
4. **Interrupt wiring**: connect IRQHOOK.C's VPICD callbacks into the main driver
   path so hardware interrupts reach the miniport's HwInterrupt.
5. **SCSI command coverage**: implement remaining CDB formats beyond INQUIRY,
   READ(10), and TEST UNIT READY. MODE SENSE, REQUEST SENSE, and READ CAPACITY
   are the most critical.


## Build and Deploy

The build produces an LE binary via the Watcom toolchain. build_sysini_fixed.py
applies the corrections described above. The corrected NTMINI.VXD is placed in
the Win98 guest's WINDOWS\SYSTEM\IOSUBSYS directory. IOS loads PDRs from this
directory at boot time. The driver's SYSTEM.INI entry triggers VMM to load it
during the VxD initialization phase.

Testing uses a Win98 SE virtual machine. Serial debug output from the VxD is
captured via the VM's virtual COM port. IOS registration, DCB creation, and
calldown installation are verified through debug trace messages before attempting
any I/O operations.
