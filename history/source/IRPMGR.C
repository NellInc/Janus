/*
 * IRPMGR.C - NT I/O Manager Implementation for Win9x Shim Layer
 *
 * Implements the core NT I/O manager functions needed to host a
 * Windows 2000/XP WDM driver inside a Windows 9x VxD environment.
 *
 * All memory allocation goes through VxD_HeapAllocate/VxD_HeapFree.
 * There is no paging, no thread scheduling, no security. This is
 * the bare minimum to make IoCallDriver, IoCompleteRequest, and
 * the device object lifecycle work correctly.
 *
 * Device objects are tracked in a simple static array (max 32).
 * IRP stack locations are allocated contiguously after the IRP
 * structure, matching the NT layout so that the standard accessor
 * macros (IoGetCurrentIrpStackLocation, etc.) work unchanged.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#include "IRPMGR.H"
#include "W9XDDK.H"

extern void dbg_mark(char c);

/* ================================================================
 * INTERNAL STATE
 * ================================================================ */

/* Simple device object tracking array */
static PDEVICE_OBJECT g_device_list[IRPMGR_MAX_DEVICES];
static ULONG g_device_count = 0;

/* ================================================================
 * INTERNAL HELPERS
 * ================================================================ */

static void irp_zero_mem(PVOID dst, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = 0;
    }
}

static void irp_copy_mem(PVOID dst, PVOID src, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    PUCHAR s = (PUCHAR)src;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

static void irp_mark_hex8(UCHAR v)
{
    UCHAR hi = (UCHAR)((v >> 4) & 0x0F);
    UCHAR lo = (UCHAR)(v & 0x0F);
    dbg_mark((char)(hi < 10 ? ('0' + hi) : ('A' + hi - 10)));
    dbg_mark((char)(lo < 10 ? ('0' + lo) : ('A' + lo - 10)));
}

static void irp_mark_hex32(ULONG v)
{
    irp_mark_hex8((UCHAR)((v >> 24) & 0xFF));
    irp_mark_hex8((UCHAR)((v >> 16) & 0xFF));
    irp_mark_hex8((UCHAR)((v >> 8) & 0xFF));
    irp_mark_hex8((UCHAR)(v & 0xFF));
}

/* ================================================================
 * PART 1: IRP ALLOCATION
 *
 * IRPs are allocated as a single contiguous block containing the
 * IRP structure followed by StackSize IO_STACK_LOCATION entries.
 * This matches the NT layout:
 *
 *   +-------------------+
 *   | IRP               |
 *   +-------------------+
 *   | Stack[0]          |  <- "bottom" (closest to hardware)
 *   | Stack[1]          |
 *   | ...               |
 *   | Stack[N-1]        |  <- "top" (closest to requestor)
 *   +-------------------+
 *
 * CurrentLocation starts at StackCount and decrements as the IRP
 * moves down the stack. CurrentStackLocation points to the active
 * entry. IoGetNextIrpStackLocation returns CurrentStackLocation-1.
 * ================================================================ */

PIRP __cdecl IrpMgr_IoAllocateIrp(CHAR StackSize, BOOLEAN ChargeQuota)
{
    PIRP irp;
    PIO_STACK_LOCATION stack_base;
    ULONG alloc_size;

    (void)ChargeQuota; /* Not used on Win9x */

    if (StackSize < 1) {
        StackSize = 1;
    }

    /* Calculate total allocation: IRP + (StackSize * IO_STACK_LOCATION) */
    alloc_size = sizeof(IRP) + ((ULONG)StackSize * sizeof(IO_STACK_LOCATION));

    irp = (PIRP)VxD_HeapAllocate(alloc_size, HEAPF_ZEROINIT);
    if (!irp) {
        VxD_Debug_Printf("IRP: IoAllocateIrp FAILED (size=%lu)\n", alloc_size);
        return NULL;
    }

    /* Initialize IRP fields */
    irp->Type = 6; /* IO_TYPE_IRP */
    irp->Size = (USHORT)alloc_size;
    irp->StackCount = StackSize;
    irp->CurrentLocation = StackSize + 1; /* One above top: not yet dispatched */
    irp->MdlAddress = NULL;
    irp->AssociatedIrp.SystemBuffer = NULL;
    irp->Cancel = FALSE;
    irp->CancelRoutine = NULL;
    irp->UserBuffer = NULL;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;

    /* Stack locations follow the IRP in memory.
     * CurrentStackLocation initially points one past the top
     * (will be decremented by IoCallDriver before first use). */
    stack_base = (PIO_STACK_LOCATION)((PUCHAR)irp + sizeof(IRP));
    irp->Tail.Overlay.CurrentStackLocation = stack_base + StackSize;
    if (StackSize > 0) {
        *(PVOID *)((PUCHAR)irp + 0x60) =
            (PVOID)((PUCHAR)(stack_base + StackSize - 1) + 0x24);
    }

    VxD_Debug_Printf("IRP: IoAllocateIrp stacks=%d size=%lu -> %lx\n",
                     (int)StackSize, alloc_size, (ULONG)irp);

    return irp;
}


VOID __cdecl IrpMgr_IoFreeIrp(PIRP Irp)
{
    if (!Irp) {
        return;
    }

    VxD_Debug_Printf("IRP: IoFreeIrp %lx\n", (ULONG)Irp);
    dbg_mark('i');
    VxD_HeapFree(Irp, 0);
}


/* ================================================================
 * PART 2: IRP DISPATCH
 *
 * IoCallDriver is the core dispatch mechanism. It:
 *   1. Decrements CurrentLocation (moving to the next driver)
 *   2. Updates CurrentStackLocation pointer
 *   3. Sets the stack location's DeviceObject
 *   4. Looks up the MajorFunction from the driver's dispatch table
 *   5. Calls the dispatch routine
 *
 * This is the single path through which all IRP dispatching flows.
 * ================================================================ */

NTSTATUS __cdecl IrpMgr_IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION irp_sp;
    PDRIVER_OBJECT driver;
    PDRIVER_DISPATCH dispatch;
    UCHAR major;
    NTSTATUS status;

    if (!DeviceObject || !Irp) {
        VxD_Debug_Printf("IRP: IoCallDriver NULL param!\n");
        return STATUS_INVALID_PARAMETER;
    }

    /* Advance to the next stack location */
    Irp->CurrentLocation--;
    Irp->Tail.Overlay.CurrentStackLocation--;

    /* Get the current (now active) stack location */
    irp_sp = Irp->Tail.Overlay.CurrentStackLocation;
    *(PVOID *)((PUCHAR)Irp + 0x60) = (PVOID)irp_sp;

    /* Set the device object for this stack entry */
    irp_sp->DeviceObject = DeviceObject;

    /* Look up the dispatch routine */
    driver = DeviceObject->DriverObject;
    if (!driver) {
        VxD_Debug_Printf("IRP: IoCallDriver device has no driver!\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    major = irp_sp->MajorFunction;
    if (major > IRP_MJ_MAXIMUM_FUNCTION) {
        VxD_Debug_Printf("IRP: IoCallDriver bad major %d\n", (int)major);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    dispatch = driver->MajorFunction[major];
    if (!dispatch) {
        VxD_Debug_Printf("IRP: IoCallDriver no handler for major %d\n",
                         (int)major);
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    VxD_Debug_Printf("IRP: IoCallDriver dev=%lx major=%d loc=%d\n",
                     (ULONG)DeviceObject, (int)major,
                     (int)Irp->CurrentLocation);
    if (major == IRP_MJ_SCSI) {
        PUCHAR ext = (PUCHAR)DeviceObject->DeviceExtension;
        dbg_mark('D');
        irp_mark_hex32((ULONG)DeviceObject);
        dbg_mark('E');
        irp_mark_hex32((ULONG)ext);
        dbg_mark('K');
        irp_mark_hex32((ULONG)irp_sp);
        dbg_mark('B');
        irp_mark_hex32((ULONG)irp_sp->Parameters.Scsi.Srb);
        dbg_mark('S');
        irp_mark_hex8((UCHAR)DeviceObject->StackSize);
        dbg_mark('Z');
        irp_mark_hex32((ULONG)driver->DriverStartIo);
        if (ext) {
            ULONG ext_a0;
            dbg_mark('e');
            irp_mark_hex32(*(ULONG *)(ext + 0x00));
            dbg_mark('L');
            irp_mark_hex32(*(ULONG *)(ext + 0x5C));
            dbg_mark('U');
            ext_a0 = *(ULONG *)(ext + 0xA0);
            irp_mark_hex32(ext_a0);
            dbg_mark('H');
            irp_mark_hex32(*(ULONG *)(ext + 0x108));
            if (ext_a0 >= 0x80000000UL) {
                dbg_mark('u');
                irp_mark_hex32(*(ULONG *)((PUCHAR)ext_a0 + 0x40));
            }
        }
    }
    if (major == IRP_MJ_PNP) {
        ULONG pnp_raw4;
        ULONG pnp_raw8;
        ULONG pnp_i;
        dbg_mark('M');
        dbg_mark('P');
        dbg_mark((char)('0' + (irp_sp->MinorFunction & 0x0F)));
        pnp_raw4 = *(ULONG *)((PUCHAR)irp_sp + 0x04);
        pnp_raw8 = *(ULONG *)((PUCHAR)irp_sp + 0x08);
        dbg_mark('R');
        irp_mark_hex32(pnp_raw4);
        dbg_mark('T');
        irp_mark_hex32(pnp_raw8);
        if (irp_sp->MinorFunction == IRP_MN_START_DEVICE &&
            pnp_raw8 >= 0x80000000UL) {
            PUCHAR pnp_res = (PUCHAR)pnp_raw8;
            dbg_mark('r');
            for (pnp_i = 0; pnp_i < 48; pnp_i++) {
                irp_mark_hex8(pnp_res[pnp_i]);
            }
        }
        VxD_Debug_Printf("IRPDBG: PNP minor=%d information=%lx status=%lx\n",
                         (int)irp_sp->MinorFunction,
                         (ULONG)Irp->IoStatus.Information,
                         (ULONG)Irp->IoStatus.Status);
    }

    /* Call the dispatch routine.
     * On real NT, this is __stdcall. Our shim drivers may use __cdecl
     * internally, but the function pointer type is PDRIVER_DISPATCH
     * which is NTAPI (__stdcall). */
    dbg_mark('>');
    VxD_Debug_Printf("IRPDBG: before dispatch\n");
    status = dispatch(DeviceObject, Irp);
    dbg_mark('<');
    VxD_Debug_Printf("IRPDBG: after dispatch status=%lx\n", status);
    if (major == IRP_MJ_PNP) {
        dbg_mark('m');
        dbg_mark('P');
        dbg_mark((char)('0' + (irp_sp->MinorFunction & 0x0F)));
        VxD_Debug_Printf("IRPDBG: PNP after status=%lx information=%lx iostatus=%lx\n",
                         (ULONG)status,
                         (ULONG)Irp->IoStatus.Information,
                         (ULONG)Irp->IoStatus.Status);
    }

    return status;
}


NTSTATUS FASTCALL IrpMgr_IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    dbg_mark('o');
    return IrpMgr_IoCallDriver(DeviceObject, Irp);
}


/* ================================================================
 * PART 3: IRP COMPLETION
 *
 * IoCompleteRequest walks the IRP stack backwards (from current
 * location toward the top) calling any completion routines that
 * were set. After all completion routines have run, the IRP is
 * freed.
 *
 * Completion routines can return STATUS_MORE_PROCESSING_REQUIRED
 * to halt the completion walk (the caller retains ownership of
 * the IRP and must eventually re-complete or free it).
 * ================================================================ */

VOID __cdecl IrpMgr_IoCompleteRequest(PIRP Irp, CHAR PriorityBoost)
{
    PIO_STACK_LOCATION irp_sp;
    PIO_STACK_LOCATION stack_base;
    NTSTATUS comp_status;
    CHAR location;

    (void)PriorityBoost; /* No thread scheduling on Win9x */

    if (!Irp) {
        return;
    }

    VxD_Debug_Printf("IRP: IoCompleteRequest irp=%lx status=%lx info=%lu\n",
                     (ULONG)Irp, Irp->IoStatus.Status,
                     (ULONG)Irp->IoStatus.Information);

    /* Stack base is immediately after the IRP structure */
    stack_base = (PIO_STACK_LOCATION)((PUCHAR)Irp + sizeof(IRP));

    /* Walk backwards from current location to the top of the stack.
     * Each stack entry may have a completion routine set by the
     * driver above it. */
    location = Irp->CurrentLocation;
    while (location <= Irp->StackCount) {
        irp_sp = &stack_base[location - 1];

        if (irp_sp->CompletionRoutine) {
            /* Check if we should invoke the completion routine based
             * on the current IRP status and the Control flags. */
            BOOLEAN invoke = FALSE;

            if (NT_SUCCESS(Irp->IoStatus.Status) &&
                (irp_sp->Control & SL_INVOKE_ON_SUCCESS)) {
                invoke = TRUE;
            }
            if (!NT_SUCCESS(Irp->IoStatus.Status) &&
                (irp_sp->Control & SL_INVOKE_ON_ERROR)) {
                invoke = TRUE;
            }
            if (Irp->Cancel &&
                (irp_sp->Control & SL_INVOKE_ON_CANCEL)) {
                invoke = TRUE;
            }

            if (!invoke) {
                dbg_mark('q');
                irp_mark_hex8(irp_sp->Control);
            }

            if (invoke) {
                dbg_mark('Q');
                irp_mark_hex8(irp_sp->Control);
                VxD_Debug_Printf("IRP: Calling completion at loc=%d\n",
                                 (int)location);

                comp_status = irp_sp->CompletionRoutine(
                    irp_sp->DeviceObject,
                    Irp,
                    irp_sp->Context);
                dbg_mark('Y');
                irp_mark_hex32((ULONG)comp_status);

                if (comp_status == STATUS_MORE_PROCESSING_REQUIRED) {
                    /* Completion routine wants to keep the IRP.
                     * Stop the completion walk. The driver that
                     * returned this status owns the IRP now. */
                    VxD_Debug_Printf("IRP: MORE_PROCESSING_REQUIRED at loc=%d\n",
                                     (int)location);
                    return;
                }

                /* Diagnostic: atapi.sys completion routines can consume
                 * IoAllocateIrp IRPs themselves via IoFreeIrp. Returning here
                 * avoids touching or freeing a possibly-consumed IRP while we
                 * isolate the StartDevice path. */
                dbg_mark('y');
                return;
            }
        }

        location++;
    }

    /* Diagnostic: do not auto-free IRPs from IoCompleteRequest. NT miniports
     * often pair IoAllocateIrp with a completion routine that frees the IRP
     * explicitly. */
    dbg_mark('v');
}


VOID FASTCALL IrpMgr_IofCompleteRequest(PIRP Irp, CHAR PriorityBoost)
{
    dbg_mark('O');
    IrpMgr_IoCompleteRequest(Irp, PriorityBoost);
}


PIRP NTAPI IrpMgr_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode,
    PDEVICE_OBJECT DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    PKEVENT Event,
    PIO_STATUS_BLOCK IoStatusBlock)
{
    PIRP irp;
    PIO_STACK_LOCATION irp_sp;
    PVOID sysbuf = NULL;
    ULONG sysbuf_len;
    CHAR stack_size;

    (void)Event;

    if (!DeviceObject) {
        return NULL;
    }

    stack_size = DeviceObject->StackSize;
    if (stack_size < 1) {
        stack_size = 1;
    }

    dbg_mark('j');
    irp_mark_hex32(IoControlCode);
    irp_mark_hex8((UCHAR)(InternalDeviceIoControl ? 1 : 0));
    irp_mark_hex8((UCHAR)stack_size);

    irp = IrpMgr_IoAllocateIrp(stack_size, FALSE);
    if (!irp) {
        return NULL;
    }

    sysbuf_len = InputBufferLength;
    if (OutputBufferLength > sysbuf_len) {
        sysbuf_len = OutputBufferLength;
    }

    if (sysbuf_len && (InputBuffer || OutputBuffer)) {
        sysbuf = ExAllocatePoolWithTag(0, sysbuf_len, 'BDIO');
        if (!sysbuf) {
            IrpMgr_IoFreeIrp(irp);
            return NULL;
        }
        irp_zero_mem(sysbuf, sysbuf_len);
        if (InputBuffer && InputBufferLength) {
            irp_copy_mem(sysbuf, InputBuffer, InputBufferLength);
        }
    }

    irp->AssociatedIrp.SystemBuffer = sysbuf;
    irp->UserBuffer = OutputBuffer;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;

    if (IoStatusBlock) {
        IoStatusBlock->Status = STATUS_SUCCESS;
        IoStatusBlock->Information = 0;
    }

    irp_sp = irp->Tail.Overlay.CurrentStackLocation - 1;
    irp_zero_mem(irp_sp, sizeof(*irp_sp));
    irp_sp->MajorFunction = InternalDeviceIoControl
        ? IRP_MJ_INTERNAL_DEVICE_CONTROL
        : IRP_MJ_DEVICE_CONTROL;
    irp_sp->Parameters.DeviceIoControl.OutputBufferLength = OutputBufferLength;
    irp_sp->Parameters.DeviceIoControl.InputBufferLength = InputBufferLength;
    irp_sp->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    irp_sp->Parameters.DeviceIoControl.Type3InputBuffer = InputBuffer;

    return irp;
}


/* ================================================================
 * PART 4: IRP STACK LOCATION HELPERS (Function Versions)
 *
 * These provide function-call versions of the macros defined in
 * NTKRNL.H. Some callers (especially assembly or indirectly
 * through function pointers) need actual functions rather than
 * macros.
 * ================================================================ */

PIO_STACK_LOCATION __cdecl IrpMgr_IoGetCurrentIrpStackLocation(PIRP Irp)
{
    return Irp->Tail.Overlay.CurrentStackLocation;
}


PIO_STACK_LOCATION __cdecl IrpMgr_IoGetNextIrpStackLocation(PIRP Irp)
{
    return Irp->Tail.Overlay.CurrentStackLocation - 1;
}


VOID __cdecl IrpMgr_IoSetNextIrpStackLocation(PIRP Irp)
{
    Irp->CurrentLocation--;
    Irp->Tail.Overlay.CurrentStackLocation--;
}


VOID __cdecl IrpMgr_IoCopyCurrentIrpStackLocationToNext(PIRP Irp)
{
    PIO_STACK_LOCATION current;
    PIO_STACK_LOCATION next;
    ULONG copy_size;

    current = Irp->Tail.Overlay.CurrentStackLocation;
    next = current - 1;

    /* Copy everything except the CompletionRoutine and Context.
     * The copy size is from the start of IO_STACK_LOCATION up to
     * but not including CompletionRoutine. */
    copy_size = (ULONG)((PUCHAR)&current->CompletionRoutine -
                        (PUCHAR)current);
    irp_copy_mem(next, current, copy_size);

    /* Clear the completion routine in the next location */
    next->CompletionRoutine = NULL;
    next->Context = NULL;
    next->Control = 0;
}


VOID __cdecl IrpMgr_IoSetCompletionRoutine(
    PIRP Irp,
    PIO_COMPLETION_ROUTINE Routine,
    PVOID Context,
    BOOLEAN OnSuccess,
    BOOLEAN OnError,
    BOOLEAN OnCancel)
{
    PIO_STACK_LOCATION next_sp;

    next_sp = Irp->Tail.Overlay.CurrentStackLocation - 1;
    next_sp->CompletionRoutine = Routine;
    next_sp->Context = Context;
    next_sp->Control = 0;

    if (OnSuccess) {
        next_sp->Control |= SL_INVOKE_ON_SUCCESS;
    }
    if (OnError) {
        next_sp->Control |= SL_INVOKE_ON_ERROR;
    }
    if (OnCancel) {
        next_sp->Control |= SL_INVOKE_ON_CANCEL;
    }
}


/* ================================================================
 * PART 5: DEVICE OBJECT MANAGEMENT
 *
 * Device objects are the core of the NT device model. Each device
 * object represents one "device" and belongs to exactly one driver.
 * Drivers can create multiple device objects.
 *
 * Device objects are linked in two ways:
 *   1. Driver's device list (NextDevice): all devices owned by a driver
 *   2. Device stack (AttachedDevice): layered devices (FDO above PDO)
 *
 * We allocate device objects from the VxD heap and track them in
 * a simple static array for cleanup.
 * ================================================================ */

NTSTATUS __cdecl IrpMgr_IoCreateDevice(
    PDRIVER_OBJECT DriverObject,
    ULONG ExtensionSize,
    PUNICODE_STRING DeviceName,
    ULONG DeviceType,
    ULONG Characteristics,
    BOOLEAN Exclusive,
    PDEVICE_OBJECT *DeviceObject)
{
    PDEVICE_OBJECT dev;
    PVOID extension;
    ULONG dev_size;
    ULONG extension_alloc_size;

    (void)DeviceName;    /* Named devices not supported in shim */
    (void)Exclusive;     /* Exclusive access not enforced in shim */

    if (!DriverObject || !DeviceObject) {
        return STATUS_INVALID_PARAMETER;
    }

    *DeviceObject = NULL;

    /* Check tracking array capacity */
    if (g_device_count >= IRPMGR_MAX_DEVICES) {
        VxD_Debug_Printf("IRP: IoCreateDevice FULL (%lu devices)\n",
                         g_device_count);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Allocate the device object */
    dev_size = sizeof(DEVICE_OBJECT);
    dev = (PDEVICE_OBJECT)VxD_HeapAllocate(dev_size, HEAPF_ZEROINIT);
    if (!dev) {
        VxD_Debug_Printf("IRP: IoCreateDevice alloc FAILED\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Allocate the device extension if requested */
    extension = NULL;
    if (ExtensionSize > 0) {
        extension_alloc_size = ExtensionSize + 0x400;
        extension = VxD_HeapAllocate(extension_alloc_size, HEAPF_ZEROINIT);
        if (!extension) {
            VxD_Debug_Printf("IRP: IoCreateDevice extension alloc FAILED\n");
            VxD_HeapFree(dev, 0);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /* Initialize the device object */
    dev->Type = 3; /* IO_TYPE_DEVICE */
    dev->Size = (USHORT)dev_size;
    dev->ReferenceCount = 0;
    dev->DriverObject = DriverObject;
    dev->NextDevice = NULL;
    dev->AttachedDevice = NULL;
    dev->CurrentIrp = NULL;
    dev->DeviceExtension = extension;
    dev->DeviceType = DeviceType;
    dev->Characteristics = Characteristics;
    dev->Flags = DO_DEVICE_INITIALIZING;
    dev->AlignmentRequirement = 0;
    dev->StackSize = 1; /* Minimum: just this device */
    dev->DeviceQueue.Type = 4;
    dev->DeviceQueue.Size = sizeof(KDEVICE_QUEUE);
    InitializeListHead(&dev->DeviceQueue.DeviceListHead);
    dev->DeviceQueue.Busy = FALSE;
    InitializeListHead(&dev->Queue.ListEntry);

    /* Link into the driver's device list (insert at head) */
    dev->NextDevice = DriverObject->DeviceObject;
    DriverObject->DeviceObject = dev;

    /* Track in our global array */
    g_device_list[g_device_count] = dev;
    g_device_count++;

    VxD_Debug_Printf("IRP: IoCreateDevice type=%lx ext=%lu -> dev=%lx\n",
                     DeviceType, ExtensionSize, (ULONG)dev);

    *DeviceObject = dev;
    return STATUS_SUCCESS;
}


VOID __cdecl IrpMgr_IoDeleteDevice(PDEVICE_OBJECT DeviceObject)
{
    PDRIVER_OBJECT driver;
    PDEVICE_OBJECT *pp;
    ULONG i;

    if (!DeviceObject) {
        return;
    }

    VxD_Debug_Printf("IRP: IoDeleteDevice dev=%lx\n", (ULONG)DeviceObject);

    /* Remove from the driver's device list */
    driver = DeviceObject->DriverObject;
    if (driver) {
        pp = &driver->DeviceObject;
        while (*pp) {
            if (*pp == DeviceObject) {
                *pp = DeviceObject->NextDevice;
                break;
            }
            pp = &(*pp)->NextDevice;
        }
    }

    /* Remove from our tracking array */
    for (i = 0; i < g_device_count; i++) {
        if (g_device_list[i] == DeviceObject) {
            /* Shift remaining entries down */
            ULONG j;
            for (j = i; j < g_device_count - 1; j++) {
                g_device_list[j] = g_device_list[j + 1];
            }
            g_device_list[g_device_count - 1] = NULL;
            g_device_count--;
            break;
        }
    }

    /* Free the device extension */
    if (DeviceObject->DeviceExtension) {
        VxD_HeapFree(DeviceObject->DeviceExtension, 0);
    }

    /* Free the device object itself */
    VxD_HeapFree(DeviceObject, 0);
}


/* ================================================================
 * PART 6: DEVICE STACK MANAGEMENT
 *
 * NT device stacks are built by attaching one device on top of
 * another. The AttachedDevice pointer on the lower device points
 * to the higher device. When IRPs are dispatched, they enter at
 * the top of the stack and flow down.
 *
 * IoAttachDeviceToDeviceStack walks to the top of the target's
 * stack and attaches the source device there. It also sets the
 * source device's StackSize to match (so IRPs allocated for this
 * device have enough stack locations).
 * ================================================================ */

PDEVICE_OBJECT __cdecl IrpMgr_IoAttachDeviceToDeviceStack(
    PDEVICE_OBJECT SourceDevice,
    PDEVICE_OBJECT TargetDevice)
{
    PDEVICE_OBJECT top;

    if (!SourceDevice || !TargetDevice) {
        return NULL;
    }

    /* Walk to the current top of the target's stack */
    top = TargetDevice;
    while (top->AttachedDevice) {
        top = top->AttachedDevice;
    }

    /* Attach source above the current top */
    top->AttachedDevice = SourceDevice;

    /* The source device's StackSize must accommodate the entire
     * stack below it, plus one for itself. */
    SourceDevice->StackSize = top->StackSize + 1;

    /* Store the lower device reference for the source.
     * On real NT this is stored in a device extension or via
     * IoGetAttachedDeviceReference. We use the ShimLowerDevice
     * field added to DEVICE_OBJECT in NTKRNL.H. */
    SourceDevice->ShimLowerDevice = top;

    VxD_Debug_Printf("IRP: IoAttachDeviceToDeviceStack src=%lx -> top=%lx stk=%d\n",
                     (ULONG)SourceDevice, (ULONG)top,
                     (int)SourceDevice->StackSize);

    return top;
}


VOID __cdecl IrpMgr_IoDetachDevice(PDEVICE_OBJECT TargetDevice)
{
    if (!TargetDevice) {
        return;
    }

    VxD_Debug_Printf("IRP: IoDetachDevice target=%lx attached=%lx\n",
                     (ULONG)TargetDevice,
                     (ULONG)TargetDevice->AttachedDevice);

    TargetDevice->AttachedDevice = NULL;
}


PDEVICE_OBJECT __cdecl IrpMgr_IoGetAttachedDeviceReference(
    PDEVICE_OBJECT DeviceObject)
{
    PDEVICE_OBJECT top;

    if (!DeviceObject) {
        return NULL;
    }

    /* Walk to the top of the device stack */
    top = DeviceObject;
    while (top->AttachedDevice) {
        top = top->AttachedDevice;
    }

    /* On real NT, this increments the reference count.
     * Our shim doesn't track references, so just return the pointer. */

    return top;
}


/* ================================================================
 * PART 7: ERROR LOGGING (STUBS)
 *
 * NT error logging writes entries to the system event log. On
 * Win9x, we have no event log, so these are no-ops. Drivers that
 * call IoAllocateErrorLogEntry / IoWriteErrorLogEntry will simply
 * have their calls silently succeed (or return NULL).
 * ================================================================ */

PVOID __cdecl IrpMgr_IoAllocateErrorLogEntry(PVOID IoObject, UCHAR EntrySize)
{
    (void)IoObject;
    (void)EntrySize;

    /* Return NULL: the caller should check for NULL and skip the
     * IoWriteErrorLogEntry call. Most well-written drivers do. */
    return NULL;
}


VOID __cdecl IrpMgr_IoWriteErrorLogEntry(PVOID ElEntry)
{
    (void)ElEntry;
    /* No-op: no event log on Win9x */
}
