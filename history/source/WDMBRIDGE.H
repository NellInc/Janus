/*
 * WDMBRIDGE.H - WDM Bridge: NT5 Device Stack <-> Win9x IOS
 *
 * Bridges the NT5 WDM driver stack (pciidex.sys + pciide.sys + atapi.sys)
 * to the Win9x I/O Supervisor. Translates IOS I/O Requests (IORs) into
 * NT IRPs targeting the WDM device stack, and translates IRP completions
 * back to IOR completions.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#ifndef WDMBRIDGE_H
#define WDMBRIDGE_H

#include "NTKSHIM.H"
#include "PCIBUS.H"
#include "W9XDDK.H"

/* ================================================================
 * WDM_BRIDGE_CONTEXT
 *
 * One per detected storage device (disk or CD-ROM). Links the NT5
 * device stack top (where we send IRPs) to the Win9x DCB and IOS
 * calldown chain.
 * ================================================================ */

typedef struct _WDM_BRIDGE_CONTEXT {
    PDEVICE_OBJECT  StackTop;       /* Top of NT5 device stack (send IRPs here) */
    PDCB            Dcb;            /* Win9x Device Control Block */
    ULONG           SectorSize;     /* Device sector size in bytes */
    UCHAR           DeviceType;     /* DCB_TYPE_DISK, DCB_TYPE_CDROM, etc. */
    UCHAR           Channel;        /* IDE channel (0=primary, 1=secondary) */
    UCHAR           TargetId;       /* Device on channel (0=master, 1=slave) */
    UCHAR           Active;         /* TRUE if this context is in use */
} WDM_BRIDGE_CONTEXT, *PWDM_BRIDGE_CONTEXT;

/* Maximum devices across all channels/controllers */
#define WDM_MAX_DEVICES     8

/* ================================================================
 * Function Prototypes: IOR to IRP Translation
 * ================================================================ */

/*
 * wdm_ior_to_irp - Translate an IOS I/O Request into an NT IRP
 *
 * Creates an IRP targeting the NT5 device stack. The IOR function code
 * is mapped to the appropriate SCSI CDB (READ/WRITE/VERIFY/TUR) and
 * dispatched via IRP_MJ_SCSI (IRP_MJ_INTERNAL_DEVICE_CONTROL).
 *
 * Completion is asynchronous via wdm_irp_completion callback.
 */
NTSTATUS wdm_ior_to_irp(PIOR ior, PDEVICE_OBJECT top_device);

/*
 * wdm_irp_completion - IRP completion routine
 *
 * Called when the NT5 stack completes our IRP. Translates SRB status
 * back to IOR status and calls IOS_BD_Command_Complete.
 */
NTSTATUS NTAPI wdm_irp_completion(PDEVICE_OBJECT DeviceObject,
                                   PIRP Irp, PVOID Context);

/* ================================================================
 * Function Prototypes: Stack Assembly
 * ================================================================ */

/*
 * wdm_load_nt5_ide_stack - Load and assemble the complete NT5 IDE stack
 *
 * 1. PE-loads pciidex.sys, pciide.sys, atapi.sys
 * 2. Creates PDOs for each PCI IDE controller
 * 3. Calls DriverEntry, AddDevice, START_DEVICE for each
 * 4. Registers with IOS and creates DCBs
 * 5. Installs calldown handler for IOR routing
 *
 * Returns: 0 on success, negative on failure
 */
int wdm_load_nt5_ide_stack(PCI_IDE_DEVICE pci_devices[], ULONG count);

/*
 * wdm_calldown_handler - IOS calldown entry point
 *
 * Installed in each DCB's calldown chain. Called by IOS when an IOR
 * arrives for one of our devices. Looks up the WDM_BRIDGE_CONTEXT
 * and dispatches through wdm_ior_to_irp.
 */
void __cdecl wdm_calldown_handler(PIOR ior);

/* ================================================================
 * Function Prototypes: PE Loading Helpers
 * ================================================================ */

/*
 * wdm_load_driver - Load an NT5 .sys driver image
 *
 * Wrapper around pe_load_image() from PELOAD.C. Resolves imports
 * against our ntoskrnl shim export table.
 *
 * NOTE: The current PELOAD.C only resolves imports from a single DLL
 * (SCSIPORT.SYS). Phase 3 requires multi-DLL resolution (ntoskrnl.exe,
 * HAL.dll, pciidex.sys). PELOAD.C will need modification to support
 * this. See the TODO comment in the implementation.
 *
 * Returns: DriverEntry address, or NULL on failure
 */
PVOID wdm_load_driver(const char *filename,
                       PVOID exports[], ULONG export_count);

#endif /* WDMBRIDGE_H */
