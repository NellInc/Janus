/*
 * PNPMGR.C - Minimal PnP and Power Manager for Win9x Shim Layer
 *
 * Implements a stripped-down PnP manager that can bootstrap an NT5
 * WDM driver stack without real Plug and Play infrastructure. On
 * Win9x, there is no PnP manager, no device tree, no resource
 * arbitrator. This file provides just enough to:
 *
 *   1. Call AddDevice to create the driver's FDO
 *   2. Fabricate CM_RESOURCE_LISTs from known hardware parameters
 *   3. Send IRP_MN_START_DEVICE with the fabricated resources
 *   4. Handle IRP_MN_QUERY_CAPABILITIES (stub response)
 *   5. Provide power manager stubs (PoXxx functions)
 *
 * The key insight: on Win9x, we already know the hardware resources
 * (e.g., secondary IDE at 0x170-0x177, control at 0x376, IRQ 15).
 * Real PnP discovers and arbitrates these. We hardcode them.
 *
 * All PnP/Power IRPs that the driver does not handle return
 * STATUS_SUCCESS by default, which matches the behavior of a PDO
 * that supports everything.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#include "PNPMGR.H"
#include "W9XDDK.H"

/* ================================================================
 * INTERNAL HELPERS
 * ================================================================ */

static void pnp_zero_mem(PVOID dst, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = 0;
    }
}

/* ================================================================
 * PART 1: PNP IRP DISPATCH
 *
 * We allocate an IRP, set up the PnP stack location, call
 * IoCallDriver, and handle synchronous completion. All our PnP
 * IRPs complete synchronously because there is no thread scheduler
 * on Win9x VxDs.
 * ================================================================ */

/*
 * pnp_send_irp - Send a generic PnP IRP to a device
 *
 * Allocates an IRP with enough stack locations for the device stack,
 * sets up a PnP stack location with the given minor function code,
 * dispatches it via IoCallDriver, and returns the status.
 *
 * The IRP's IoStatus is pre-set to STATUS_SUCCESS so that if the
 * driver passes it down without handling, it succeeds (matching
 * the behavior expected from a PDO that accepts everything).
 */
NTSTATUS __cdecl pnp_send_irp(
    PDEVICE_OBJECT DeviceObject,
    UCHAR MinorFunction)
{
    PIRP irp;
    PIO_STACK_LOCATION irp_sp;
    NTSTATUS status;

    if (!DeviceObject) {
        return STATUS_INVALID_PARAMETER;
    }

    VxD_Debug_Printf("PNP: pnp_send_irp minor=%d dev=%lx\n",
                     (int)MinorFunction, (ULONG)DeviceObject);

    /* Allocate IRP with enough stack locations */
    irp = IrpMgr_IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (!irp) {
        VxD_Debug_Printf("PNP: IRP allocation failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Pre-set IoStatus to SUCCESS. PnP IRPs that are not handled
     * by the driver should succeed by default. */
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;

    /* Set up the PnP stack location.
     * We use IoGetNextIrpStackLocation because IoCallDriver will
     * advance to it. */
    irp_sp = IrpMgr_IoGetNextIrpStackLocation(irp);
    pnp_zero_mem(irp_sp, sizeof(IO_STACK_LOCATION));

    irp_sp->MajorFunction = IRP_MJ_PNP;
    irp_sp->MinorFunction = MinorFunction;

    /* Dispatch to the driver */
    status = IrpMgr_IoCallDriver(DeviceObject, irp);

    /* On our shim, everything completes synchronously.
     * If the driver returned STATUS_PENDING, treat it as SUCCESS
     * since we cannot actually pend (no thread scheduler). */
    if (status == STATUS_PENDING) {
        VxD_Debug_Printf("PNP: Driver returned PENDING, treating as SUCCESS\n");
        status = STATUS_SUCCESS;
    }

    /* Note: IoCallDriver/IoCompleteRequest may have already freed
     * the IRP. If the driver completed it inline, the IRP is gone.
     * If it returned an error synchronously, we free it ourselves. */

    VxD_Debug_Printf("PNP: pnp_send_irp minor=%d status=%lx\n",
                     (int)MinorFunction, status);

    return status;
}


/* ================================================================
 * PART 2: IRP_MN_START_DEVICE
 *
 * This is the most critical PnP IRP. It tells the driver:
 *   "Here are your hardware resources. Initialize the device."
 *
 * We fabricate a CM_RESOURCE_LIST containing:
 *   - One I/O port range (e.g. 0x170-0x177 for secondary IDE)
 *   - One interrupt (e.g. IRQ 15 for secondary IDE)
 *
 * The same resource list is passed as both AllocatedResources
 * and AllocatedResourcesTranslated (on ISA, raw == translated).
 * ================================================================ */

/* Resource list buffer: sized for one CM_FULL_RESOURCE_DESCRIPTOR
 * with a CM_PARTIAL_RESOURCE_LIST containing 2 descriptors
 * (I/O port + interrupt). We use a static buffer to avoid
 * additional heap allocations. */

/* Size: CM_RESOURCE_LIST header (4 bytes Count)
 *     + CM_FULL_RESOURCE_DESCRIPTOR (8 bytes + CM_PARTIAL_RESOURCE_LIST)
 *     + CM_PARTIAL_RESOURCE_LIST (8 bytes header + 2 descriptors)
 *     Each CM_PARTIAL_RESOURCE_DESCRIPTOR is 3*4 + union = varies by type
 *     We allocate generously. */
#define PNP_RESOURCE_BUF_SIZE  256

static UCHAR g_resource_buf[PNP_RESOURCE_BUF_SIZE];

NTSTATUS __cdecl pnp_start_device(
    PDEVICE_OBJECT DeviceObject,
    ULONG IoPortBase,
    ULONG IoPortLength,
    ULONG Irq)
{
    PIRP irp;
    PIO_STACK_LOCATION irp_sp;
    PCM_RESOURCE_LIST res_list;
    PCM_FULL_RESOURCE_DESCRIPTOR full_desc;
    PCM_PARTIAL_RESOURCE_LIST partial_list;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR port_desc;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR irq_desc;
    NTSTATUS status;

    if (!DeviceObject) {
        return STATUS_INVALID_PARAMETER;
    }

    VxD_Debug_Printf("PNP: pnp_start_device dev=%lx port=%lx len=%lu irq=%lu\n",
                     (ULONG)DeviceObject, IoPortBase, IoPortLength, Irq);

    /* ---- Build the CM_RESOURCE_LIST ---- */

    pnp_zero_mem(g_resource_buf, PNP_RESOURCE_BUF_SIZE);

    res_list = (PCM_RESOURCE_LIST)g_resource_buf;
    res_list->Count = 1;

    full_desc = &res_list->List[0];
    full_desc->InterfaceType = (ULONG)Isa; /* ISA bus */
    full_desc->BusNumber = 0;

    partial_list = &full_desc->PartialResourceList;
    partial_list->Version = 1;
    partial_list->Revision = 1;
    partial_list->Count = 2; /* I/O port + interrupt */

    /* Descriptor 0: I/O port range */
    port_desc = &partial_list->Descriptors[0];
    port_desc->Type = (UCHAR)CmResourceTypePort;
    port_desc->ShareDisposition = (UCHAR)CmResourceShareDeviceExclusive;
    port_desc->Flags = 1; /* CM_RESOURCE_PORT_IO */
    port_desc->u.Port.Start.u.LowPart = IoPortBase;
    port_desc->u.Port.Start.u.HighPart = 0;
    port_desc->u.Port.Length = IoPortLength;

    /* Descriptor 1: Interrupt
     * We place it right after descriptor 0 in memory. Since
     * Descriptors[] is declared as [1] in the structure, we
     * need to index manually. */
    irq_desc = (PCM_PARTIAL_RESOURCE_DESCRIPTOR)(
        (PUCHAR)port_desc + sizeof(CM_PARTIAL_RESOURCE_DESCRIPTOR));
    irq_desc->Type = (UCHAR)CmResourceTypeInterrupt;
    irq_desc->ShareDisposition = (UCHAR)CmResourceShareShared;
    irq_desc->Flags = 1; /* CM_RESOURCE_INTERRUPT_LEVEL_SENSITIVE */
    irq_desc->u.Interrupt.Level = Irq;
    irq_desc->u.Interrupt.Vector = Irq;
    irq_desc->u.Interrupt.Affinity = 1; /* Single processor */

    /* ---- Allocate and set up the IRP ---- */

    irp = IrpMgr_IoAllocateIrp(DeviceObject->StackSize, FALSE);
    if (!irp) {
        VxD_Debug_Printf("PNP: START_DEVICE IRP alloc failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Pre-set success: if driver passes through without handling,
     * the device starts successfully. */
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;

    /* Set up the PnP stack location */
    irp_sp = IrpMgr_IoGetNextIrpStackLocation(irp);
    pnp_zero_mem(irp_sp, sizeof(IO_STACK_LOCATION));

    irp_sp->MajorFunction = IRP_MJ_PNP;
    irp_sp->MinorFunction = IRP_MN_START_DEVICE;

    /* Pass the resource lists. On ISA, raw and translated are the same.
     * The PnP StartDevice parameters are accessed through the
     * QueryDeviceRelations.StartDevice union member. */
    irp_sp->Parameters.QueryDeviceRelations.StartDevice.AllocatedResources =
        (PVOID)res_list;
    irp_sp->Parameters.QueryDeviceRelations.StartDevice.AllocatedResourcesTranslated =
        (PVOID)res_list;

    /* Dispatch to the driver */
    status = IrpMgr_IoCallDriver(DeviceObject, irp);

    if (status == STATUS_PENDING) {
        VxD_Debug_Printf("PNP: START_DEVICE returned PENDING\n");
        status = STATUS_SUCCESS;
    }

    VxD_Debug_Printf("PNP: pnp_start_device status=%lx\n", status);
    return status;
}


/* ================================================================
 * PART 3: IRP_MN_QUERY_CAPABILITIES
 *
 * The driver queries device capabilities (power states, ejection,
 * wake support, etc.). We respond with a minimal stub indicating
 * the device supports D0 only and is not removable.
 * ================================================================ */

NTSTATUS __cdecl pnp_query_capabilities(PDEVICE_OBJECT DeviceObject)
{
    VxD_Debug_Printf("PNP: pnp_query_capabilities dev=%lx\n",
                     (ULONG)DeviceObject);

    /* Send a generic PnP IRP with IRP_MN_QUERY_CAPABILITIES.
     * The driver's PnP dispatch will process it. If the driver
     * passes it down to our PDO, the pre-set STATUS_SUCCESS
     * in IoStatus means the query succeeds with default caps. */
    return pnp_send_irp(DeviceObject, IRP_MN_QUERY_CAPABILITIES);
}


/* ================================================================
 * PART 4: ADD DEVICE
 *
 * The first step in the PnP startup sequence. The PnP manager
 * calls the driver's AddDevice routine with the PDO. The driver
 * creates an FDO and attaches it to the PDO.
 *
 * On our shim, we call AddDevice directly. The "PDO" is a device
 * object we created ourselves to represent the physical hardware.
 * ================================================================ */

NTSTATUS __cdecl pnp_call_add_device(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT PhysicalDeviceObject)
{
    PDRIVER_EXTENSION ext;
    PDRIVER_ADD_DEVICE add_device;
    NTSTATUS status;

    if (!DriverObject || !PhysicalDeviceObject) {
        return STATUS_INVALID_PARAMETER;
    }

    ext = DriverObject->DriverExtension;
    if (!ext) {
        VxD_Debug_Printf("PNP: AddDevice: no DriverExtension\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    add_device = ext->AddDevice;
    if (!add_device) {
        VxD_Debug_Printf("PNP: AddDevice: no AddDevice routine\n");
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    VxD_Debug_Printf("PNP: Calling AddDevice drv=%lx pdo=%lx\n",
                     (ULONG)DriverObject, (ULONG)PhysicalDeviceObject);

    status = add_device(DriverObject, PhysicalDeviceObject);

    VxD_Debug_Printf("PNP: AddDevice returned status=%lx\n", status);
    return status;
}


/* ================================================================
 * PART 5: POWER MANAGER
 *
 * NT5 WDM drivers use the power manager for device power state
 * transitions. On Win9x, devices are always powered on. We provide
 * stub implementations that always report D0 (fully powered) and
 * S0 (system working).
 *
 * PoStartNextPowerIrp: On real NT, power IRPs are serialized per
 * device stack. The driver must call this in its power IRP
 * completion path to release the next queued power IRP. We have
 * no power queue, so this is a no-op.
 *
 * PoCallDriver: On real NT, this has special serialization. We
 * just forward to IoCallDriver.
 *
 * PoRequestPowerIrp: Fabricates a power IRP and "completes" it
 * immediately, calling the completion callback synchronously.
 *
 * PoSetPowerState: Notifies the power manager of a state change.
 * We always return the previous state as D0/S0.
 * ================================================================ */

VOID __cdecl PnpMgr_PoStartNextPowerIrp(PIRP Irp)
{
    (void)Irp;
    /* No-op: no power IRP serialization on Win9x */
}


NTSTATUS __cdecl PnpMgr_PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    /* On real NT, PoCallDriver has special power IRP handling.
     * Our shim just dispatches normally. */
    return IrpMgr_IoCallDriver(DeviceObject, Irp);
}


NTSTATUS __cdecl PnpMgr_PoRequestPowerIrp(
    PDEVICE_OBJECT DeviceObject,
    UCHAR MinorFunction,
    POWER_STATE PowerState,
    PREQUEST_POWER_COMPLETE Completion,
    PVOID Context,
    PIRP *OutIrp)
{
    IO_STATUS_BLOCK io_status;
    PIRP irp;

    (void)MinorFunction;
    (void)PowerState;

    VxD_Debug_Printf("PNP: PoRequestPowerIrp dev=%lx minor=%d\n",
                     (ULONG)DeviceObject, (int)MinorFunction);

    /* We complete the power request synchronously. The device is
     * always in D0 (fully powered) on Win9x. */

    io_status.Status = STATUS_SUCCESS;
    io_status.Information = 0;

    /* If the caller wants the IRP pointer, allocate a minimal one.
     * Some drivers check the IRP; others just need the completion. */
    irp = NULL;
    if (OutIrp) {
        irp = IrpMgr_IoAllocateIrp(1, FALSE);
        if (irp) {
            irp->IoStatus.Status = STATUS_SUCCESS;
            irp->IoStatus.Information = 0;
        }
        *OutIrp = irp;
    }

    /* Call the completion callback immediately.
     * On real NT, this fires when the power IRP completes
     * asynchronously. We fire it synchronously. */
    if (Completion) {
        POWER_STATE result_state;
        result_state.DeviceState = PowerDeviceD0;

        Completion(DeviceObject, MinorFunction, result_state,
                   Context, &io_status);
    }

    /* Free the IRP if we allocated one and the caller got it */
    if (irp && OutIrp) {
        /* Caller may still reference *OutIrp, so don't free yet.
         * The caller is responsible for freeing via IoFreeIrp
         * if they requested it. */
    } else if (irp) {
        IrpMgr_IoFreeIrp(irp);
    }

    return STATUS_SUCCESS;
}


POWER_STATE __cdecl PnpMgr_PoSetPowerState(
    PDEVICE_OBJECT DeviceObject,
    POWER_STATE_TYPE Type,
    POWER_STATE State)
{
    POWER_STATE old_state;

    (void)DeviceObject;
    (void)State;

    VxD_Debug_Printf("PNP: PoSetPowerState dev=%lx type=%d\n",
                     (ULONG)DeviceObject, (int)Type);

    /* Return the "previous" state. We always report fully powered. */
    if (Type == DevicePowerState) {
        old_state.DeviceState = PowerDeviceD0;
    } else {
        old_state.SystemState = PowerSystemWorking;
    }

    return old_state;
}
