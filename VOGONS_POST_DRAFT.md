# Backporting NT5 miniport drivers to Windows 9x: a VxD shim for atapi.sys

## The Idea

Windows NT from version 4 onward has a clean, well documented miniport driver architecture. Storage drivers are small, hardware-specific modules that talk through a standard ScsiPort interface. NT4's `atapi.sys` is 27,600 bytes of battle-tested code that handles basically every IDE/ATAPI controller you can throw at it.

Windows 9x has none of this. Its storage stack depends on vendor-specific VxDs and port drivers (.PDR files) that have been abandonware for two decades. If the built-in ESDI_506.PDR does not support your controller, or your optical drive is not detected, your options are limited.

So I built a shim. NTMINI.VXD is a Win98 VxD that loads an unmodified NT4 miniport binary at ring 0, provides a ScsiPort API for it to call into, and bridges the result to Win98's IOS (I/O Supervisor) subsystem. No modifications to the NT driver. The same binary that ships with NT4 SP6a, running inside Win98's kernel.

In principle, this approach could work with any NT4 SCSI miniport: atapi.sys, sym_hi.sys for Symbios/LSI SCSI, aic78xx.sys for Adaptec, and so on. The current proof of concept uses atapi.sys because IDE controllers are everywhere and easy to test.

The source is on GitHub: https://github.com/NellInc/nt5-9x-driver-backport

## Architecture

```
NTMINI.VXD (Win98 LE VxD)
  |
  +-- VXDWRAP.ASM       : DDB, control procedure, VMM/VPICD trampolines,
  |                        ScsiPort struct-return stubs (MSVC ABI compat)
  |
  +-- NTMINI_V5.C       : ScsiPort shim (22 functions), port I/O remapping,
  |                        PCI config space, HwFindAdapter loop, DeviceFlags
  |                        patching, QEMU status register quirk workarounds
  |
  +-- IOSBRIDGE.C        : IOS registration (DRP), DCB creation, calldown
  |                        chain installation, IOR-to-SRB translation stubs
  |
  +-- PELOAD.C           : Ring 0 PE image loader with section mapping,
  |                        base relocations, and import resolution
  |
  +-- IRQHOOK.C          : VPICD interrupt virtualization (written, not
  |                        yet wired)
  |
  +-- ATAPI_EMBEDDED.H   : NT4 atapi.sys as a 27,600-byte C array
```

The boot sequence:

1. **Sys_Critical_Init**: Initialize COM1 serial debug output
2. **Device_Init**: PE-load atapi.sys into ring 0, resolve all ScsiPort imports, call DriverEntry
3. DriverEntry calls **ScsiPortInitialize**, which calls **HwFindAdapter** in a loop (4 passes across primary, secondary, tertiary, quaternary IDE channels)
4. **HwInitialize** sets up the adapter, we patch DeviceFlags, then submit a test READ(10) SRB
5. **Init_Complete**: Register with IOS, acquire ILB, create DCB, install calldown chain

## Key Technical Discoveries

### 1. MSVC vs Watcom ABI: The 8-Byte Struct Return Problem

NT4's atapi.sys is compiled with MSVC. Our VxD uses Open Watcom. These compilers disagree on how to return small structs.

`ScsiPortConvertUlongToPhysicalAddress` returns an 8-byte `SCSI_PHYSICAL_ADDRESS`. MSVC returns this in EDX:EAX. Watcom would use a hidden pointer parameter. If you implement this in C with Watcom, the calling convention mismatch corrupts the stack.

The fix: write these functions in assembly.

```nasm
sp_ConvertUlong_asm:
    push    ebp
    mov     ebp, esp
    mov     eax, [ebp+8]       ; Addr -> LowPart in EAX
    xor     edx, edx           ; HighPart = 0 in EDX
    pop     ebp
    ret     4                   ; clean 1 param (__stdcall)
```

### 2. HwStartIo Device Extension Layout

After HwFindAdapter detected the CD-ROM, HwStartIo kept returning SRB_STATUS_SELECTION_TIMEOUT. Disassembling the miniport by hand revealed the problem.

HwStartIo reads DeviceFlags from `DevExt + 0x44 + TargetId*2`. Our CD-ROM was on the secondary IDE channel, so HwFindAdapter stored flags at the channel-1 offset. But because we present secondary hardware as primary via port remapping (so PathId=0 SRBs work), HwStartIo reads from the channel-0 offset and finds zero.

```
HW_DEVICE_EXTENSION layout (reverse-engineered):
  0x00: CurrentSrb (PVOID)
  0x04: BaseIoAddress1 (PVOID)
  0x0C: BaseIoAddress2 (PVOID)
  0x44: DeviceFlags[TargetId] (WORD array)
        Bit 0 = DFLAGS_DEVICE_PRESENT
        Bit 1 = DFLAGS_ATAPI_DEVICE
        Bit 4 = DFLAGS_REMOVABLE_DRIVE
  0xC0: IDENTIFY PACKET DEVICE data (512 bytes)
```

The fix: after HwFindAdapter returns, check whether DeviceFlags[0] is empty and populate it.

### 3. Port Remapping: The Channel Identity Crisis

The CD-ROM lives on secondary IDE (0x170, control 0x376, IRQ 15). If you tell the miniport "here's a secondary channel," it stores device information at channel-1 offsets. Then HwStartIo, expecting the device at channel 0 because we send PathId=0 SRBs, finds nothing.

Solution: present primary IDE addresses (0x1F0, 0x3F6) in the access ranges, then remap all port I/O transparently to secondary hardware. The miniport's internal world is consistent (it thinks it is talking to primary), while all actual I/O hits secondary.

### 4. HwFindAdapter Multi-Pass Detection

NT4's atapi.sys HwFindAdapter has an internal state machine that iterates across four IDE channels over multiple calls. ScsiPortInitialize calls HwFindAdapter in a loop while the miniport sets `*again = TRUE`. You must zero the access ranges between calls so the miniport advances its channel index. If you call HwFindAdapter once, it checks primary only and returns NOT_FOUND even when your CD-ROM is on secondary.

### 5. IOS Registration: DRP Struct Packing

This was the root cause of months of debugging. The DRP (Device Registration Packet) must use `#pragma pack(push,1)`. Without byte packing, the compiler inserts padding after `DRP_revision` at offset +0x24, shifting every subsequent field by 3 to 8 bytes. IOS reads fields by absolute offset, so misalignment causes silent registration failure.

### 6. ILB Acquisition via DDB Chain Walking

IOS does not provide the ILB (Import Library Base) pointer to drivers that register after boot. The workaround: walk the VMM VxD DDB chain starting from `VMM_Get_DDB(0x0010)`, search each registered driver's reference data for a DRP eyecatcher, and copy the ILB function pointer from whichever APIX driver already has one.

### 7. LGN Bitmask Encoding

The DRP's LGN (Logical Group Number) field requires a bitmask, not a bit index. The correct value is 0x00400000 (`1 << 0x16`), corresponding to `DRP_ESDI_PD`. Using the raw bit number 0x16 as the value produces a wrong device class. ESDI_506.PDR confirmed the correct encoding.

### 8. QEMU IDE Status Register Quirks

Two QEMU-specific behaviors that trip up the miniport:

**Master after DEVICE RESET returns 0x00.** Real hardware returns 0x50 (DRDY | DSC). The miniport busy-loops waiting for DRDY and hangs. Workaround: intercept status register reads and patch 0x00 to 0x50.

**Non-existent slave echoes master status.** QEMU echoes the master's status when you select a non-existent slave, making the miniport think there are two devices. Workaround: track the drive select register and force status to 0x00 for the slave.

These should be conditionally applied and would not be needed on real hardware.

### 9. PCI Config Space: Roll Your Own

Win98 does not expose PCI config space reads as a convenient VxD service during Device_Init. The miniport needs ScsiPortGetBusData to probe PCI. Solution: hit ports 0xCF8 (address) and 0xCFC (data) directly.

### 10. The BPB Corruption Bug

During deployment testing, the VM stopped booting. After hours of debugging: code that tried to clear the FAT32 "dirty" flag by writing to BPB offset 0x25 in the boot sector was actually corrupting `BPB_FATSz32` (offsets 0x24-0x27), shifting the entire data area calculation and making every cluster lookup wrong.

The FAT32 dirty flag lives in the FAT itself, in cluster 1's entry (FAT offset + 4 bytes, bit 27). Never touch BPB offsets 0x24-0x27.

## Current Status

**Working:**

```
SP: HwFindAdapter returned 0x00000000 again=0x00000000
SP: Adapter FOUND! Calling HwInitialize...
SP: HwInitialize OK!
SP: FIX: set DevFlags[0]=0x0013 (ATAPI+PRESENT+REMOVABLE)
SP: ISO 9660 CD001 FOUND!
```

The full pipeline works end to end. VxD loads, PE loader maps atapi.sys at ring 0, DriverEntry runs, ScsiPortInitialize calls HwFindAdapter across four IDE channels, HwInitialize succeeds, and HwStartIo executes a READ(10) that returns "CD001" from ISO 9660 sector 16.

IOS integration is working: registration succeeds (REMAIN_RESIDENT), ILB acquired from APIX driver via DDB chain walking, DCB created (CD-ROM type), calldown chain installed, drive letter associated. Win98 boots to desktop with the driver loaded.

The IOR-to-SRB translation layer is implemented with CDB construction for READ(10), WRITE(10), VERIFY(10), INQUIRY, REQUEST SENSE, TEST UNIT READY, READ CAPACITY, and media control commands. Queue management handles the single-threaded StartIo model. Completion flows back through ScsiPortNotification to IOS_BD_Command_Complete.

VPICD interrupt virtualization code is written but not yet wired into the runtime path (using polling for now).

**Tested in QEMU only.** The QEMU-specific status register workarounds (master DRDY after reset, slave echo suppression) would not be needed on real hardware and should be conditionally applied.

## What Would Help

The main thing this project needs is **real hardware testing**. Everything has been developed and validated in QEMU. If you have Pentium-class hardware with an IDE CD-ROM drive and would be willing to try this, that would be extremely valuable.

Reports of what works (or does not) on real controllers would help identify where the QEMU-specific workarounds end and where genuine hardware edge cases begin.

The source is on GitHub: https://github.com/NellInc/nt5-9x-driver-backport

---

*Built on macOS, targeting a 1998 operating system, loading a 1996 kernel driver, to read a CD-ROM over an interface standardized in 1986.*

*Thanks to Björn Korneli for the inspiration.*
