---
topic: NT5 compile and integration testing
date: 2026-03-28
status: NOT_STARTED
stepsCompleted: 0
---

# NT5 WDM Driver: Compile, Debug, and Integration Testing

## Context

A complete WDM compatibility layer has been written (9,256 lines across 13 source files) that enables loading Windows 2000's atapi.sys inside a Win98 VxD. The code covers all 115 ntoskrnl/HAL/WMILIB imports. None of it has been compiled or tested yet.

Key architectural insight: W2K atapi.sys has zero link-time dependency on pciidex.sys. All interaction is IRP-based. We replace the PnP orchestration directly with NT5LOADER.C, which fabricates a PDO, calls AddDevice, sends START_DEVICE with IDE resources, and dispatches SCSI IRPs.

The NT4 ScsiPort path (NTMINI_V5.C + IOSBRIDGE.C + PELOAD.C + VXDWRAP.ASM) is already working: IOS registration, DCB creation, calldown chain, CD001 read confirmed. That path is the reference implementation.

## What Exists

### New NT5 source files (in history/source/)

| File | Lines | Purpose |
|------|-------|---------|
| NTKSHIM.C | 2583 | ntoskrnl/HAL shim: ~115 functions (spinlocks, DPC, timers, pool, MDL, MmMapIoSpace, interrupts, strings, IRP builders, device queues, work items, interlocked ops, C runtime, 64-bit math, exported variables) |
| NTKSHIM.H | 713 | Types and prototypes: UNICODE_STRING, MDL, KINTERRUPT, KDEVICE_QUEUE, IO_WORKITEM, CONFIGURATION_INFORMATION, etc. |
| NTKEXPORTS.C | 454 | Export tables: ~115 ntoskrnl + 18 HAL + 2 WMILIB entries mapping function names to shim implementations. DLL_EXPORT_TABLE for multi-DLL PE loader. |
| IRPMGR.C | 622 | IRP infrastructure: IoAllocateIrp, IoCallDriver, IoCompleteRequest, IoCreateDevice, IoDeleteDevice, IoAttachDeviceToDeviceStack, device object tracking |
| IRPMGR.H | 380 | IRP/IO_STACK_LOCATION/DEVICE_OBJECT/DRIVER_OBJECT structures, IRP_MJ_xxx codes |
| PNPMGR.C | 434 | PnP: pnp_start_device (fabricates CM_RESOURCE_LIST), pnp_call_add_device. Power: PoStartNextPowerIrp, PoCallDriver, PoRequestPowerIrp, PoSetPowerState. |
| PNPMGR.H | 235 | PnP minor codes, power types, CM_RESOURCE_LIST |
| PCIBUS.C | 519 | PCI IDE scan via 0xCF8/0xCFC, PDO creation, BUS_INTERFACE_STANDARD |
| PCIBUS.H | 177 | PCI config structures, PCI_IDE_DEVICE |
| WDMBRIDGE.C | 867 | IOR-to-IRP translation, WDM stack assembly, IOS calldown handler |
| WDMBRIDGE.H | 111 | WDM_BRIDGE_CONTEXT |
| NT5LOADER.C | 722 | Integration: PE-loads W2K atapi.sys, calls DriverEntry, AddDevice, START_DEVICE, SCSI test read, IOS registration |
| PELOAD.C | 1439 | Extended with pe_load_image_multi() for multi-DLL import resolution + 32-slot stub generator |

### Existing NT4 source files (working, do not modify unless necessary)

| File | Purpose |
|------|---------|
| NTMINI_V5.C | ScsiPort shim (22 functions + HwScsiAdapterControl), main NT4 path |
| IOSBRIDGE.C | IOS registration, DRP, DCB, IOR handler, calldown chain |
| PELOAD.C | PE loader (original single-DLL path preserved) |
| IRQHOOK.C | VPICD interrupt virtualization (written, not wired) |
| VXDWRAP.ASM | DDB, control procedure, VMM/VPICD trampolines, file I/O wrappers |
| W9XDDK.H | Win98 DDK type definitions |
| PORTIO.H | Port I/O inline functions |

### Binary

- binaries/w2k_atapi.sys (88,048 bytes): Windows 2000 atapi.sys, all imports from ntoskrnl.exe (100), HAL.dll (13), WMILIB.SYS (2).

### Build infrastructure (in src/)

- build_sysini_fixed.py: LE header correction
- deploy_to_iosubsys.py / deploy_sysini.py: FAT32 deployment
- verify_and_launch.py: QEMU launcher with debug capture

## What Needs To Happen

### Step 1: Create Watcom Makefile

Create a makefile (or wmake script) that compiles the NT5 path. The NT4 path already has a known-working build sequence (documented in reference/BUILD.TXT and the Vogons post).

New compilation units for the NT5 VxD:
```
VXDWRAP.ASM         (NASM, OMF output, existing)
NTMINI_V5.C         (existing, HwScsiAdapterControl added)
PELOAD.C            (extended with multi-DLL loader)
IOSBRIDGE.C         (existing IOS bridge)
NTKSHIM.C           (ntoskrnl/HAL shim)
NTKEXPORTS.C        (export tables)
IRPMGR.C            (IRP infrastructure)
PNPMGR.C            (PnP/Power)
PCIBUS.C            (PCI bus simulation)
WDMBRIDGE.C         (WDM-to-IOS bridge)
NT5LOADER.C         (integration glue)
```

Watcom compile flags (same as existing):
```
wcc386 -bt=windows -3s -s -zl -d0 -i=.
```

The VxD will be significantly larger than the NT4 version (~9K lines vs ~3K). May need to adjust LE header correction in build_sysini_fixed.py for more pages/objects.

Acceptance: all .C files compile without errors. Link produces a valid LE VxD.

### Step 2: Fix Compilation Errors

Expect these categories of errors:
- **Type redefinitions**: NTKSHIM.H and W9XDDK.H both define base types (ULONG, PVOID, etc.). Need include guards or conditional compilation.
- **Struct layout conflicts**: DEVICE_OBJECT is defined in both IRPMGR.H and NTKRNL.H (the old NT4 stub file). Consolidate to one definition.
- **Forward declaration issues**: NTKSHIM.H forward-declares IRP, DEVICE_OBJECT, etc. but the full definitions are in IRPMGR.H. Include order matters.
- **Calling convention**: __stdcall vs __cdecl mismatches between shim functions and what the PE loader expects. The export table uses (PVOID) casts which suppress warnings but won't fix actual calling convention issues at runtime.
- **Missing extern declarations**: NT5LOADER.C references functions from NTKSHIM, IRPMGR, PNPMGR, PELOAD. All must be properly declared.

Strategy: compile one file at a time, fix errors, move to the next. Start with NTKSHIM.C (standalone, fewest dependencies), then IRPMGR.C, then PNPMGR.C, etc. NT5LOADER.C last (depends on everything).

Acceptance: clean compile of all 11 .C files.

### Step 3: Fix Linker Errors

Expect:
- **Duplicate symbols**: If multiple .C files define the same helper function or global variable.
- **Unresolved externals**: VMM service wrappers (VxD_PageAllocate, VxD_HeapAllocate, VxD_Debug_Printf, VxD_MapPhysToLinear) declared as extern in .C files but defined in VXDWRAP.ASM. May need new ASM wrappers for services the NT5 code needs.
- **Export table**: The VxD must export NTMINI_DDB. The existing link command handles this.

Key ASM additions that may be needed in VXDWRAP.ASM:
- VxD_MapPhysToLinear wrapper (for MmMapIoSpace)
- VxD_SetTimer / VxD_CancelTimer wrappers (for KeSetTimer)
- Additional VPICD wrappers for IoConnectInterrupt

Acceptance: wlink produces NTMINI.VXD without errors.

### Step 4: LE Header Correction

build_sysini_fixed.py was designed for a ~38KB VxD. The NT5 version will be much larger (potentially 80-120KB with all the new code). Verify:
- data_pages_off is still correct
- Page count and object table are correct
- Fixup section sizes accommodate the larger binary
- Extended LE fields (LE+0xB8, LE+0xBC, LE+0xC0) are still valid

May need to run compare_le_headers.py against ESDI_506.PDR again to validate structure.

Acceptance: corrected VxD passes check_extended_le.py validation.

### Step 5: Deploy and First Boot

Deploy the NT5 VxD + w2k_atapi.sys to the Win98 disk image:
```
python3 src/deploy_sysini.py                    # deploy VxD
python3 src/deploy_to_iosubsys.py               # or IOSUBSYS path
# also deploy w2k_atapi.sys to C:\WINDOWS\SYSTEM\
```

Boot QEMU with debug capture:
```
qemu-system-i386 -m 128 -M pc -cpu pentium \
  -drive file=win98.img,format=raw -boot c -vga std \
  -rtc base=localtime -display none \
  -debugcon file:/tmp/vxd_debug.log \
  -serial file:/tmp/serial.log \
  -cdrom test_cd.iso
```

Watch for "NT5:" messages in debug log. The loading sequence should produce:
```
NT5: Loading W2K atapi.sys...
NT5: PE loaded, entry=0x????????
NT5: Calling DriverEntry...
NT5: DriverEntry returned 0x00000000
NT5: AddDevice called...
NT5: Sending START_DEVICE...
NT5: Start device returned 0x00000000
NT5: Sending test READ(10)...
NT5: ISO 9660 CD001 FOUND!
```

Acceptance: VxD loads without crashing Win98. Debug output shows progress.

### Step 6: Debug DriverEntry Failures

Most likely first failure point. W2K atapi.sys DriverEntry will:
1. Call IoAllocateDriverObjectExtension -- our shim must provide a valid extension
2. Register MajorFunction handlers (IRP_MJ_PNP, IRP_MJ_POWER, IRP_MJ_SCSI, IRP_MJ_SYSTEM_CONTROL)
3. Set DriverExtension->AddDevice callback
4. May call RtlQueryRegistryValues to read HKLM\SYSTEM\CurrentControlSet\Services\atapi\Parameters -- our stub returns STATUS_SUCCESS with no data

**Critical struct layout issue**: Our DRIVER_OBJECT must match the NT binary layout exactly. Key offsets:
- DriverInit: the DriverEntry function pointer
- DriverExtension: pointer to DRIVER_EXTENSION containing AddDevice
- MajorFunction[28]: IRP dispatch table
- DeviceObject: head of created device list

If atapi.sys accesses DriverObject fields by offset and our struct has different padding, it reads garbage. Verify struct sizes match NT expectations:
- sizeof(DRIVER_OBJECT) should be 0xA8 (168 bytes) on NT5
- sizeof(DRIVER_EXTENSION) should be 0x18 (24 bytes)
- sizeof(DEVICE_OBJECT) should be 0x1B8 (440 bytes)

**How to diagnose**: If DriverEntry crashes or returns an error, check:
1. Are any "NTK: STUB called for X" messages? Missing shim functions.
2. Is the DRIVER_OBJECT being written to unexpected offsets? Struct layout mismatch.
3. Does it crash in a string function? sprintf/swprintf format string issues.

Acceptance: DriverEntry returns STATUS_SUCCESS (0x00000000).

### Step 7: Debug AddDevice Failures

After DriverEntry succeeds, NT5LOADER calls AddDevice with our fake PDO. atapi.sys will:
1. Call IoCreateDevice to create its FDO
2. Call IoAttachDeviceToDeviceStack(FDO, PDO)
3. May call IoGetDeviceProperty on the PDO (NOT YET SHIMMED -- likely first failure)
4. May send PnP IRPs down to the PDO expecting responses

**Missing shim: IoGetDeviceProperty**. atapi.sys likely calls this to query:
- DevicePropertyHardwareID
- DevicePropertyCompatibleIDs
- DevicePropertyBusTypeGuid
- DevicePropertyLegacyBusType

Add IoGetDeviceProperty to NTKSHIM.C. Return fake but plausible values:
- HardwareID: "PCI\\VEN_8086&DEV_7010" (Intel PIIX IDE)
- BusType: PCIBus (5)
- LegacyBusType: Isa (1)

**How to diagnose**: If AddDevice fails, check if atapi.sys called functions we haven't shimmed (watch for STUB messages). If it crashes, the PDO's DEVICE_OBJECT layout may be wrong.

Acceptance: AddDevice completes, FDO is attached to PDO stack.

### Step 8: Debug START_DEVICE Failures

pnp_start_device sends IRP_MN_START_DEVICE with a fabricated CM_RESOURCE_LIST containing IDE I/O ports and IRQ.

**The CM_RESOURCE_LIST binary layout is critical.** atapi.sys parses this to find:
- CmResourceTypePort entries for command block (0x1F0-0x1F7) and control block (0x3F6)
- CmResourceTypeInterrupt entry for IRQ 14 (primary) or IRQ 15 (secondary)

If the resource list layout is wrong, atapi.sys gets wrong port addresses and all hardware I/O fails silently.

**Verify** that our CM_PARTIAL_RESOURCE_DESCRIPTOR struct matches NT's binary layout:
- Type (UCHAR), ShareDisposition (UCHAR), Flags (USHORT) at offsets 0,1,2
- u.Port.Start (LARGE_INTEGER) at offset 4, u.Port.Length (ULONG) at offset 12
- u.Interrupt.Level (ULONG) at offset 4, u.Interrupt.Vector (ULONG) at offset 8, u.Interrupt.Affinity (ULONG) at offset 12

**How to diagnose**: After START_DEVICE, check if atapi.sys calls MmMapIoSpace with the expected physical address (0x1F0). If it maps a different address, the resource list parsing is wrong.

Acceptance: START_DEVICE returns STATUS_SUCCESS. MmMapIoSpace is called with correct addresses.

### Step 9: Debug SCSI IRP Dispatch

After START_DEVICE, NT5LOADER sends a test SCSI IRP (READ(10) for sector 16).

atapi.sys handles IRP_MJ_SCSI (which is IRP_MJ_INTERNAL_DEVICE_CONTROL = 0x0F). It expects:
- IO_STACK_LOCATION.Parameters.Scsi.Srb pointing to a valid SRB
- SRB.Function = SRB_FUNCTION_EXECUTE_SCSI
- SRB.Cdb containing the SCSI command
- SRB.DataBuffer pointing to a read buffer
- SRB.DataTransferLength set
- SRB.OriginalRequest = IRP pointer

**Interrupt delivery is critical here.** W2K atapi.sys uses interrupt-driven I/O, not polling. After submitting a command to the hardware, it waits for an interrupt. If IoConnectInterrupt didn't properly wire the IRQ via VPICD, the interrupt never fires and the request hangs forever.

**Workaround if interrupts don't work**: Temporarily patch atapi.sys to use polled mode (if such an option exists), or modify the ISR shim to poll the status register and fire the ISR callback manually.

**How to diagnose**:
1. Check debug log for "NTK: IoConnectInterrupt" with the vector/IRQ
2. After SCSI IRP is dispatched, check if the ISR fires (look for VPICD callback messages)
3. If the IRP never completes, the interrupt path is broken

Acceptance: Test SCSI IRP completes. Buffer contains "CD001" at expected offset.

### Step 10: IOS Integration

Once SCSI works, wire up IOS:
1. Register with IOS using existing DRP pattern from IOSBRIDGE.C
2. Create DCB for the CD-ROM device
3. Install calldown handler (wdm_calldown_handler from WDMBRIDGE.C)
4. Bridge IOS IORs through the NT5 WDM stack

This reuses proven infrastructure from the NT4 path. The only new piece is that IORs route through wdm_ior_to_irp (WDMBRIDGE.C) instead of build_srb_from_ior (IOSBRIDGE.C).

Acceptance: Win98 sees the CD-ROM drive. File manager can browse a CD.

## Likely Failure Modes (Ordered by Probability)

1. **Struct layout mismatch** (DRIVER_OBJECT, DEVICE_OBJECT, IRP, CM_RESOURCE_LIST) -- offset-based field access reads garbage. Fix: verify sizeof() matches NT expectations, add padding if needed.
2. **Missing shim function** (IoGetDeviceProperty, IoRegisterDeviceInterface, etc.) -- STUB messages in debug log. Fix: implement the missing function.
3. **Calling convention mismatch** -- __stdcall vs __cdecl for shimmed functions. NT5 drivers expect __stdcall for most kernel APIs. Fix: ensure all exports use correct convention.
4. **IRP completion semantics** -- IoCompleteRequest must walk the stack correctly, call completion routines, handle STATUS_MORE_PROCESSING_REQUIRED. Fix: trace IRP flow in debug output.
5. **Interrupt delivery** -- IoConnectInterrupt → VPICD path may not fire ISR. Fix: verify VPICD setup, add polling fallback.
6. **CM_RESOURCE_LIST parsing** -- wrong offsets → wrong ports → silent hardware failure. Fix: dump resource list bytes in debug output before sending START_DEVICE.

## Debug Strategy

Every shim function already logs with prefixed messages:
- "NTK:" -- ntoskrnl/HAL shim functions
- "IRP:" -- IRP infrastructure
- "PNP:" -- PnP manager
- "PCI:" -- PCI bus scan
- "WDM:" -- WDM bridge
- "NT5:" -- NT5 loader orchestration
- "NTK: STUB called for X" -- unimplemented function was called

First boot will produce a wall of debug output. Read it sequentially:
1. PE load messages (imports resolved, stubs created)
2. DriverEntry sequence
3. AddDevice sequence
4. START_DEVICE resource delivery
5. First SCSI IRP dispatch
6. Interrupt/completion

The first message that looks wrong or the last message before a crash/hang identifies the failure point.

## Key Files to Reference During Debugging

- reference/BUILD.TXT: Original build instructions and toolchain notes
- reference/IOS_PROGRESS.md: IOS integration discoveries (DRP packing, ILB, LGN)
- ARCHITECTURE.md: Full system architecture with layer diagrams
- DEVELOPMENT.md: Build workflow, debug log prefixes, adding new shim functions

## Environment

- Repo: /Users/nellwatson/Documents/GitHub/nt5-9x-driver-backport/
- GitHub: https://github.com/NellInc/nt5-9x-driver-backport
- Build: Open Watcom v2 (cross-compile on macOS)
- Assembler: NASM
- Test VM: QEMU with Win98 SE disk image
- Debug: tail -f /tmp/vxd_debug.log during QEMU boot
- Win98 disk image location: set WIN98_IMG env var
