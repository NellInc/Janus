/*
 * PNPMGR.H - Minimal PnP and Power Manager for Win9x Shim Layer
 *
 * Defines the PnP minor function codes, power types, resource
 * descriptors, and function prototypes needed to bootstrap an NT5
 * WDM driver stack without real Plug and Play support.
 *
 * On real NT, PnP is a complex subsystem involving device
 * enumeration, resource arbitration, and driver loading. Here, we
 * implement just enough to send the critical IRP_MN_START_DEVICE
 * and IRP_MN_QUERY_CAPABILITIES to the driver, and to fabricate
 * resource lists from known I/O port and IRQ values.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#ifndef _PNPMGR_H_
#define _PNPMGR_H_

#include "NTKRNL.H"
#include "IRPMGR.H"

/* ================================================================
 * PNP MINOR FUNCTION CODES
 *
 * These are the MinorFunction values used with IRP_MJ_PNP.
 * Already defined in NTKRNL.H. We re-verify the critical ones
 * here with fallback defines.
 * ================================================================ */

#ifndef IRP_MN_START_DEVICE
#define IRP_MN_START_DEVICE                 0x00
#endif
#ifndef IRP_MN_QUERY_REMOVE_DEVICE
#define IRP_MN_QUERY_REMOVE_DEVICE          0x01
#endif
#ifndef IRP_MN_REMOVE_DEVICE
#define IRP_MN_REMOVE_DEVICE                0x02
#endif
#ifndef IRP_MN_CANCEL_REMOVE_DEVICE
#define IRP_MN_CANCEL_REMOVE_DEVICE         0x03
#endif
#ifndef IRP_MN_STOP_DEVICE
#define IRP_MN_STOP_DEVICE                  0x04
#endif
#ifndef IRP_MN_QUERY_STOP_DEVICE
#define IRP_MN_QUERY_STOP_DEVICE            0x05
#endif
#ifndef IRP_MN_CANCEL_STOP_DEVICE
#define IRP_MN_CANCEL_STOP_DEVICE           0x06
#endif
#ifndef IRP_MN_QUERY_DEVICE_RELATIONS
#define IRP_MN_QUERY_DEVICE_RELATIONS       0x07
#endif
#ifndef IRP_MN_QUERY_CAPABILITIES
#define IRP_MN_QUERY_CAPABILITIES           0x09
#endif
#ifndef IRP_MN_SURPRISE_REMOVAL
#define IRP_MN_SURPRISE_REMOVAL             0x17
#endif

/* ================================================================
 * POWER TYPES
 *
 * Already defined in NTKRNL.H:
 *   DEVICE_POWER_STATE, SYSTEM_POWER_STATE, POWER_STATE,
 *   POWER_STATE_TYPE, POWER_ACTION
 * ================================================================ */

/* ================================================================
 * CM_RESOURCE_LIST
 *
 * Resource descriptors describe the hardware resources (I/O ports,
 * IRQs, memory ranges) assigned to a device. We fabricate these
 * from known values (e.g., secondary IDE at 0x170/0x376, IRQ 15).
 *
 * Already defined in NTKRNL.H:
 *   CM_PARTIAL_RESOURCE_DESCRIPTOR, CM_PARTIAL_RESOURCE_LIST,
 *   CM_FULL_RESOURCE_DESCRIPTOR, CM_RESOURCE_LIST
 * ================================================================ */

/* ================================================================
 * PNP MANAGER FUNCTION PROTOTYPES
 * ================================================================ */

/*
 * pnp_start_device - Send IRP_MN_START_DEVICE to a device
 *
 * Fabricates a CM_RESOURCE_LIST with the given I/O port range
 * and IRQ, then sends IRP_MN_START_DEVICE to the device's driver.
 * This is the critical PnP IRP that tells a WDM driver "your
 * hardware resources are assigned, initialize the device."
 *
 * Parameters:
 *   DeviceObject   - Target device (FDO or filter DO)
 *   IoPortBase     - Base I/O port address (e.g. 0x170)
 *   IoPortLength   - Length of I/O port range (e.g. 8)
 *   Irq            - IRQ number (e.g. 15)
 *
 * Returns:
 *   NTSTATUS from the driver's PnP dispatch routine
 */
NTSTATUS __cdecl pnp_start_device(
    PDEVICE_OBJECT DeviceObject,
    ULONG IoPortBase,
    ULONG IoPortLength,
    ULONG Irq);

/*
 * pnp_send_irp - Send a generic PnP IRP to a device
 *
 * Allocates an IRP, sets up the PnP stack location with the
 * given minor function code, calls IoCallDriver, and waits
 * for synchronous completion.
 *
 * Parameters:
 *   DeviceObject   - Target device
 *   MinorFunction  - PnP minor function code (IRP_MN_xxx)
 *
 * Returns:
 *   NTSTATUS from the driver's PnP dispatch routine
 */
NTSTATUS __cdecl pnp_send_irp(
    PDEVICE_OBJECT DeviceObject,
    UCHAR MinorFunction);

/*
 * pnp_query_capabilities - Send IRP_MN_QUERY_CAPABILITIES
 *
 * Sends a capabilities query to the device. The response is
 * a stub: we report D0 (fully powered) support only.
 *
 * Parameters:
 *   DeviceObject - Target device
 *
 * Returns:
 *   NTSTATUS from the driver's PnP dispatch routine
 */
NTSTATUS __cdecl pnp_query_capabilities(PDEVICE_OBJECT DeviceObject);

/*
 * pnp_call_add_device - Call a driver's AddDevice routine
 *
 * Calls DriverExtension->AddDevice to create the FDO and
 * attach it to the device stack. This is the first step in
 * the PnP startup sequence.
 *
 * Parameters:
 *   DriverObject         - The driver to call
 *   PhysicalDeviceObject - The PDO to attach to
 *
 * Returns:
 *   NTSTATUS from the AddDevice routine
 */
NTSTATUS __cdecl pnp_call_add_device(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT PhysicalDeviceObject);

/* ================================================================
 * POWER MANAGER FUNCTION PROTOTYPES
 * ================================================================ */

/*
 * PoStartNextPowerIrp - Signal readiness for next power IRP
 *
 * On real NT, this is required in power IRP completion paths.
 * Our shim is single-threaded with no power queue, so this is
 * a no-op.
 *
 * Parameters:
 *   Irp - The power IRP being completed
 */
VOID __cdecl PnpMgr_PoStartNextPowerIrp(PIRP Irp);

/*
 * PoCallDriver - Forward a power IRP to the next driver
 *
 * On real NT, power IRPs have special serialization requirements.
 * Our shim just calls IoCallDriver.
 *
 * Parameters:
 *   DeviceObject - Target device
 *   Irp          - Power IRP
 *
 * Returns:
 *   NTSTATUS from the driver's dispatch routine
 */
NTSTATUS __cdecl PnpMgr_PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/*
 * PoRequestPowerIrp - Request a power state change
 *
 * Fabricates a power IRP and completes it synchronously with
 * D0/S0 (fully powered). The completion callback is invoked
 * immediately.
 *
 * Parameters:
 *   DeviceObject  - Target device
 *   MinorFunction - IRP_MN_SET_POWER or IRP_MN_QUERY_POWER
 *   PowerState    - Requested power state
 *   Completion    - Completion callback (called synchronously)
 *   Context       - Context for completion callback
 *   Irp           - Receives pointer to the fabricated IRP (optional)
 *
 * Returns:
 *   STATUS_SUCCESS
 */
NTSTATUS __cdecl PnpMgr_PoRequestPowerIrp(
    PDEVICE_OBJECT DeviceObject,
    UCHAR MinorFunction,
    POWER_STATE PowerState,
    PREQUEST_POWER_COMPLETE Completion,
    PVOID Context,
    PIRP *Irp);

/*
 * PoSetPowerState - Notify the power manager of a state change
 *
 * No-op on Win9x. Returns the previous state (always D0/S0).
 *
 * Parameters:
 *   DeviceObject - Device changing state
 *   Type         - SystemPowerState or DevicePowerState
 *   State        - New power state
 *
 * Returns:
 *   Previous power state (always D0 or S0)
 */
POWER_STATE __cdecl PnpMgr_PoSetPowerState(
    PDEVICE_OBJECT DeviceObject,
    POWER_STATE_TYPE Type,
    POWER_STATE State);

#endif /* _PNPMGR_H_ */
