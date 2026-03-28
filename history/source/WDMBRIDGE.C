/*
 * WDMBRIDGE.C - WDM Bridge: NT5 Device Stack <-> Win9x IOS
 *
 * Bridges the Windows 2000/XP WDM IDE driver stack (pciidex.sys +
 * pciide.sys + atapi.sys) to the Windows 9x I/O Supervisor (IOS).
 *
 * === Architecture ===
 *
 * Win9x side:                       NT5 side (shimmed):
 *
 *   FSD (VFAT, CDFS)                 atapi.sys (miniport)
 *       |                                |
 *   TSD (Type-Specific)              pciidex.sys (bus driver)
 *       |                                |
 *   [our calldown] ---> WDMBRIDGE ---> [IRP_MJ_SCSI]
 *       |                                |
 *   Port driver layer              pciide.sys (vendor)
 *
 * IORs arrive from IOS via the calldown chain. We translate them
 * to IRPs with SCSI SRBs and dispatch them to the NT5 device stack.
 * When the NT5 stack completes the IRP, we translate back to IOR
 * status and call IOS_BD_Command_Complete.
 *
 * === Relationship to Other Files ===
 *
 * PCIBUS.C:   Provides PCI scan, PDO creation, BUS_INTERFACE_STANDARD
 * NTKSHIM.C:  Provides ntoskrnl.exe API shim (IoCreateDevice, etc.)
 * IOSBRIDGE.C: The v4 bridge (IOR<->SRB for NT4 miniports). This file
 *              is the v5 bridge (IOR<->IRP for NT5 WDM stacks).
 * PELOAD.C:   PE image loader for .sys files
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#include "W9XDDK.H"
#include "PORTIO.H"
#include "NTKSHIM.H"
#include "PCIBUS.H"
#include "WDMBRIDGE.H"

/* ================================================================
 * NT5 SCSI Structures (needed for IRP_MJ_SCSI dispatch)
 *
 * These match the definitions in IOSBRIDGE.C but are needed here
 * for building SRBs inside IRPs. In a real build, both files would
 * share a common SCSI header.
 * ================================================================ */

/* SRB Function codes */
#define SRB_FUNCTION_EXECUTE_SCSI   0x00
#define SRB_FUNCTION_RESET_BUS      0x12

/* SRB Status codes */
#define SRB_STATUS_PENDING          0x00
#define SRB_STATUS_SUCCESS          0x01
#define SRB_STATUS_ABORTED          0x02
#define SRB_STATUS_ERROR            0x04
#define SRB_STATUS_INVALID_REQUEST  0x06
#define SRB_STATUS_NO_DEVICE        0x08
#define SRB_STATUS_TIMEOUT          0x09
#define SRB_STATUS_SELECTION_TIMEOUT 0x0A
#define SRB_STATUS_BUS_RESET        0x0E
#define SRB_STATUS_DATA_OVERRUN     0x12

/* SRB Flags */
#define SRB_FLAGS_DATA_IN           0x00000008
#define SRB_FLAGS_DATA_OUT          0x00000010
#define SRB_FLAGS_NO_DATA_TRANSFER  0x00000000
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER 0x00000020

/* SCSI CDB opcodes */
#define SCSI_OP_TEST_UNIT_READY     0x00
#define SCSI_OP_READ10              0x28
#define SCSI_OP_WRITE10             0x2A
#define SCSI_OP_VERIFY10            0x2F

/* SCSI_REQUEST_BLOCK (must match IOSBRIDGE.C / NT definition) */
typedef struct _SCSI_REQUEST_BLOCK {
    USHORT  Length;
    UCHAR   Function;
    UCHAR   SrbStatus;
    UCHAR   ScsiStatus;
    UCHAR   PathId;
    UCHAR   TargetId;
    UCHAR   Lun;
    UCHAR   QueueTag;
    UCHAR   QueueAction;
    UCHAR   CdbLength;
    UCHAR   SenseInfoBufferLength;
    ULONG   SrbFlags;
    ULONG   DataTransferLength;
    ULONG   TimeOutValue;
    PVOID   DataBuffer;
    PVOID   SenseInfoBuffer;
    struct _SCSI_REQUEST_BLOCK *NextSrb;
    PVOID   OriginalRequest;
    PVOID   SrbExtension;
    ULONG   InternalStatus;
    UCHAR   Cdb[16];
} SCSI_REQUEST_BLOCK, *PSCSI_REQUEST_BLOCK;

/* Sense data */
typedef struct _SENSE_DATA {
    UCHAR   ErrorCode;
    UCHAR   SegmentNumber;
    UCHAR   SenseKey;
    UCHAR   Information[4];
    UCHAR   AdditionalSenseLength;
    UCHAR   CommandSpecificInfo[4];
    UCHAR   AdditionalSenseCode;
    UCHAR   AdditionalSenseQualifier;
    UCHAR   FieldReplaceable;
    UCHAR   SenseKeySpecific[3];
} SENSE_DATA, *PSENSE_DATA;

/* IRP_MJ_SCSI is the same as IRP_MJ_INTERNAL_DEVICE_CONTROL */
#define IRP_MJ_SCSI  IRP_MJ_INTERNAL_DEVICE_CONTROL

/* Sense key for unit attention (media changed) */
#define SENSE_UNIT_ATTENTION    0x06


/* ================================================================
 * FORWARD DECLARATIONS
 * ================================================================ */

static void wdm_zero_mem(PVOID dst, ULONG size);
static void wdm_copy_mem(PVOID dst, PVOID src, ULONG size);
static USHORT wdm_srb_status_to_ior_status(UCHAR srb_status,
                                             UCHAR scsi_status);
static void wdm_complete_ior(PIOR ior, USHORT status);
static PWDM_BRIDGE_CONTEXT wdm_find_context_for_dcb(PDCB dcb);


/* ================================================================
 * GLOBAL STATE
 * ================================================================ */

static struct {
    /* Bridge contexts: one per detected storage device */
    WDM_BRIDGE_CONTEXT  devices[WDM_MAX_DEVICES];
    ULONG               num_devices;

    /* IOS identity */
    DDB                 ddb;
    CALLDOWN            calldown;
    BOOLEAN             initialized;

    /* Driver objects for loaded NT5 drivers */
    PDRIVER_OBJECT      pciidex_driver;
    PDRIVER_OBJECT      pciide_driver;
    PDRIVER_OBJECT      atapi_driver;

    /* Sense data buffer for completions */
    SENSE_DATA          sense_buffer;

} g_wdm;


/* ================================================================
 * PART 1: IOR TO IRP TRANSLATION
 *
 * The core bridge function. Receives a Win9x IOR (I/O Request) and
 * constructs an NT IRP containing a SCSI Request Block (SRB) with
 * the appropriate CDB. The IRP is dispatched to the top of the NT5
 * device stack via IoCallDriver.
 *
 * Mapping:
 *   IOR_READ              -> IRP_MJ_SCSI + SRB with READ(10) CDB
 *   IOR_WRITE/IOR_WRITEV  -> IRP_MJ_SCSI + SRB with WRITE(10) CDB
 *   IOR_VERIFY            -> IRP_MJ_SCSI + SRB with VERIFY(10) CDB
 *   IOR_MEDIA_CHECK       -> IRP_MJ_SCSI + SRB with TEST UNIT READY CDB
 *   IOR_SCSI_PASS_THROUGH -> IRP_MJ_SCSI + SRB with raw CDB from IOR
 *
 * Completion is asynchronous: IoSetCompletionRoutine installs
 * wdm_irp_completion which fires when the NT5 stack finishes.
 * ================================================================ */

NTSTATUS wdm_ior_to_irp(PIOR ior, PDEVICE_OBJECT top_device)
{
    PIRP irp;
    PIO_STACK_LOCATION irpSp;
    PSCSI_REQUEST_BLOCK srb;
    ULONG lba, byte_count, sector_size;
    USHORT block_count;

    if (!ior || !top_device) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Allocate an IRP with enough stack locations for the device stack.
     * StackSize comes from the target device object. */
    irp = IoAllocateIrp(top_device->StackSize, FALSE);
    if (!irp) {
        VxD_Debug_Printf("WDM: IoAllocateIrp failed\n");
        wdm_complete_ior(ior, IORS_MEMORY_PROBLEM);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Allocate an SRB from heap (will be freed in completion) */
    srb = (PSCSI_REQUEST_BLOCK)VxD_HeapAllocate(
        sizeof(SCSI_REQUEST_BLOCK), HEAPF_ZEROINIT);
    if (!srb) {
        VxD_Debug_Printf("WDM: SRB allocation failed\n");
        IoFreeIrp(irp);
        wdm_complete_ior(ior, IORS_MEMORY_PROBLEM);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Common SRB setup */
    srb->Length              = sizeof(SCSI_REQUEST_BLOCK);
    srb->Function            = SRB_FUNCTION_EXECUTE_SCSI;
    srb->SrbStatus           = SRB_STATUS_PENDING;
    srb->PathId              = 0;
    srb->TargetId            = 0;
    srb->Lun                 = 0;
    srb->TimeOutValue        = 10;
    srb->SenseInfoBuffer     = &g_wdm.sense_buffer;
    srb->SenseInfoBufferLength = sizeof(SENSE_DATA);
    srb->OriginalRequest     = irp;

    /* Determine sector size (default to 512 for safety) */
    sector_size = DISK_SECTOR_SIZE;
    {
        PWDM_BRIDGE_CONTEXT ctx = (PWDM_BRIDGE_CONTEXT)ior->IOR_private_port;
        if (ctx) {
            sector_size = ctx->SectorSize;
            srb->TargetId = ctx->TargetId;
        }
    }

    switch (ior->IOR_func) {

    case IOR_READ:
        /*
         * READ: Build SCSI READ(10) CDB
         */
        lba        = ior->IOR_start_addr[0];
        byte_count = ior->IOR_xfer_count;
        block_count = (USHORT)(byte_count / sector_size);
        if (block_count == 0) block_count = 1;

        srb->DataBuffer         = (PVOID)ior->IOR_buffer_ptr;
        srb->DataTransferLength = byte_count;
        srb->SrbFlags           = SRB_FLAGS_DATA_IN |
                                  SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
        srb->CdbLength          = 10;
        srb->Cdb[0] = SCSI_OP_READ10;
        srb->Cdb[2] = (UCHAR)((lba >> 24) & 0xFF);
        srb->Cdb[3] = (UCHAR)((lba >> 16) & 0xFF);
        srb->Cdb[4] = (UCHAR)((lba >>  8) & 0xFF);
        srb->Cdb[5] = (UCHAR)((lba >>  0) & 0xFF);
        srb->Cdb[7] = (UCHAR)((block_count >> 8) & 0xFF);
        srb->Cdb[8] = (UCHAR)((block_count >> 0) & 0xFF);

        VxD_Debug_Printf("WDM: READ LBA=%lu count=%u bytes=%lu\n",
                         lba, block_count, byte_count);
        break;

    case IOR_WRITE:
    case IOR_WRITEV:
        /*
         * WRITE: Build SCSI WRITE(10) CDB
         */
        lba        = ior->IOR_start_addr[0];
        byte_count = ior->IOR_xfer_count;
        block_count = (USHORT)(byte_count / sector_size);
        if (block_count == 0) block_count = 1;

        srb->DataBuffer         = (PVOID)ior->IOR_buffer_ptr;
        srb->DataTransferLength = byte_count;
        srb->SrbFlags           = SRB_FLAGS_DATA_OUT |
                                  SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
        srb->CdbLength          = 10;
        srb->Cdb[0] = SCSI_OP_WRITE10;
        srb->Cdb[2] = (UCHAR)((lba >> 24) & 0xFF);
        srb->Cdb[3] = (UCHAR)((lba >> 16) & 0xFF);
        srb->Cdb[4] = (UCHAR)((lba >>  8) & 0xFF);
        srb->Cdb[5] = (UCHAR)((lba >>  0) & 0xFF);
        srb->Cdb[7] = (UCHAR)((block_count >> 8) & 0xFF);
        srb->Cdb[8] = (UCHAR)((block_count >> 0) & 0xFF);

        VxD_Debug_Printf("WDM: WRITE LBA=%lu count=%u bytes=%lu\n",
                         lba, block_count, byte_count);
        break;

    case IOR_VERIFY:
        /*
         * VERIFY: Build SCSI VERIFY(10) CDB, no data transfer
         */
        lba        = ior->IOR_start_addr[0];
        byte_count = ior->IOR_xfer_count;
        block_count = (USHORT)(byte_count / sector_size);
        if (block_count == 0) block_count = 1;

        srb->DataBuffer         = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags           = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength          = 10;
        srb->Cdb[0] = SCSI_OP_VERIFY10;
        srb->Cdb[2] = (UCHAR)((lba >> 24) & 0xFF);
        srb->Cdb[3] = (UCHAR)((lba >> 16) & 0xFF);
        srb->Cdb[4] = (UCHAR)((lba >>  8) & 0xFF);
        srb->Cdb[5] = (UCHAR)((lba >>  0) & 0xFF);
        srb->Cdb[7] = (UCHAR)((block_count >> 8) & 0xFF);
        srb->Cdb[8] = (UCHAR)((block_count >> 0) & 0xFF);

        VxD_Debug_Printf("WDM: VERIFY LBA=%lu count=%u\n",
                         lba, block_count);
        break;

    case IOR_MEDIA_CHECK:
    case IOR_MEDIA_CHECK_RESET:
        /*
         * MEDIA CHECK: Build SCSI TEST UNIT READY CDB (6 bytes, all zero)
         */
        srb->DataBuffer         = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags           = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength          = 6;
        srb->Cdb[0] = SCSI_OP_TEST_UNIT_READY;

        VxD_Debug_Printf("WDM: MEDIA_CHECK\n");
        break;

    case IOR_SCSI_PASS_THROUGH:
        /*
         * SCSI PASS-THROUGH: Copy raw CDB from IOR
         */
        {
            PIOR_SCSI_PASSTHROUGH sp = &ior->IOR_scsi_pass;
            ULONG i;

            srb->CdbLength = sp->SP_CDBLength;
            if (srb->CdbLength > 16) {
                srb->CdbLength = 16;
            }
            for (i = 0; i < srb->CdbLength; i++) {
                srb->Cdb[i] = sp->SP_CDB[i];
            }

            if (sp->SP_Flags & SP_DATA_IN) {
                srb->SrbFlags           = SRB_FLAGS_DATA_IN |
                                          SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
                srb->DataBuffer         = (PVOID)ior->IOR_buffer_ptr;
                srb->DataTransferLength = sp->SP_DataLength;
            } else if (sp->SP_Flags & SP_DATA_OUT) {
                srb->SrbFlags           = SRB_FLAGS_DATA_OUT |
                                          SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
                srb->DataBuffer         = (PVOID)ior->IOR_buffer_ptr;
                srb->DataTransferLength = sp->SP_DataLength;
            } else {
                srb->SrbFlags           = SRB_FLAGS_NO_DATA_TRANSFER;
                srb->DataBuffer         = NULL;
                srb->DataTransferLength = 0;
            }

            srb->TimeOutValue = 30;
        }

        VxD_Debug_Printf("WDM: PASSTHROUGH CDB[0]=%02x len=%d\n",
                         srb->Cdb[0], srb->CdbLength);
        break;

    default:
        VxD_Debug_Printf("WDM: Unsupported IOR func=%04x\n", ior->IOR_func);
        VxD_HeapFree(srb, 0);
        IoFreeIrp(irp);
        wdm_complete_ior(ior, IORS_NOT_SUPPORTED);
        return STATUS_NOT_SUPPORTED;
    }

    /* Set up the IRP stack location for IRP_MJ_SCSI.
     * This is how NT5 storage drivers receive SCSI requests:
     * IRP_MJ_INTERNAL_DEVICE_CONTROL with the SRB pointer in
     * Parameters.Scsi.Srb. */
    irpSp = IoGetNextIrpStackLocation(irp);
    irpSp->MajorFunction = IRP_MJ_SCSI;
    irpSp->Parameters.Scsi.Srb = srb;

    /* Stash the IOR pointer in the IRP's DriverContext so the
     * completion routine can find it */
    irp->Tail.Overlay.DriverContext[0] = ior;
    irp->Tail.Overlay.DriverContext[1] = srb;

    /* Set completion routine: fires on success, error, and cancel */
    IoSetCompletionRoutine(irp, wdm_irp_completion, ior,
                            TRUE, TRUE, TRUE);

    /* Dispatch the IRP down the NT5 device stack */
    VxD_Debug_Printf("WDM: Dispatching IRP %08lx to device %08lx\n",
                     (ULONG)irp, (ULONG)top_device);

    return IofCallDriver(top_device, irp);
}


/* ================================================================
 * PART 2: IRP COMPLETION CALLBACK
 *
 * Called when the NT5 device stack completes our IRP. This is the
 * return path: we extract the SRB status, map it to an IOR status
 * code (reusing the srb_status_to_ior_status pattern from
 * IOSBRIDGE.C), set IOR_status, and call IOS_BD_Command_Complete.
 *
 * This routine runs at DISPATCH_LEVEL (or higher). Keep it fast
 * and non-blocking.
 * ================================================================ */

NTSTATUS NTAPI wdm_irp_completion(PDEVICE_OBJECT DeviceObject,
                                   PIRP Irp, PVOID Context)
{
    PIOR ior;
    PSCSI_REQUEST_BLOCK srb;
    USHORT ior_status;

    /* Recover the IOR and SRB from the IRP's DriverContext */
    ior = (PIOR)Irp->Tail.Overlay.DriverContext[0];
    srb = (PSCSI_REQUEST_BLOCK)Irp->Tail.Overlay.DriverContext[1];

    if (!ior) {
        VxD_Debug_Printf("WDM: Completion with no IOR (orphaned)\n");
        if (srb) VxD_HeapFree(srb, 0);
        IoFreeIrp(Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (!srb) {
        VxD_Debug_Printf("WDM: Completion with no SRB\n");
        wdm_complete_ior(ior, IORS_DEVICE_ERROR);
        IoFreeIrp(Irp);
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    /* Map SRB status to IOR status */
    ior_status = wdm_srb_status_to_ior_status(srb->SrbStatus,
                                                srb->ScsiStatus);

    VxD_Debug_Printf("WDM: Completion SrbStatus=%02x ScsiStatus=%02x "
                     "-> IORS=%04x\n",
                     srb->SrbStatus, srb->ScsiStatus, ior_status);

    /* For media check operations, detect UNIT ATTENTION (media changed) */
    if ((ior->IOR_func == IOR_MEDIA_CHECK ||
         ior->IOR_func == IOR_MEDIA_CHECK_RESET) &&
        srb->ScsiStatus == 0x02) { /* CHECK_CONDITION */
        PSENSE_DATA sense = (PSENSE_DATA)srb->SenseInfoBuffer;
        if (sense && (sense->SenseKey == SENSE_UNIT_ATTENTION)) {
            ior_status = IORS_UNCERTAIN_MEDIA;
        }
    }

    /* Copy sense data back for SCSI pass-through */
    if (ior->IOR_func == IOR_SCSI_PASS_THROUGH &&
        srb->SenseInfoBufferLength > 0 &&
        srb->SenseInfoBuffer != NULL) {
        ULONG sense_len = srb->SenseInfoBufferLength;
        if (sense_len > sizeof(ior->IOR_scsi_pass.SP_SenseData)) {
            sense_len = sizeof(ior->IOR_scsi_pass.SP_SenseData);
        }
        wdm_copy_mem(ior->IOR_scsi_pass.SP_SenseData,
                     srb->SenseInfoBuffer, sense_len);
        ior->IOR_scsi_pass.SP_SenseLength = (UCHAR)sense_len;
    }

    /* Update IOR transfer count on error */
    if (ior_status != IORS_SUCCESS) {
        if (srb->DataTransferLength < ior->IOR_xfer_count) {
            ior->IOR_xfer_count = srb->DataTransferLength;
        }
    }

    /* Set IOR status and complete back to IOS */
    ior->IOR_status = ior_status;
    IOS_BD_Command_Complete(ior);

    /* Free the SRB and IRP.
     * We return STATUS_MORE_PROCESSING_REQUIRED to tell the I/O
     * manager we've taken ownership of the IRP and freed it ourselves.
     * This prevents double-free. */
    VxD_HeapFree(srb, 0);
    IoFreeIrp(Irp);

    return STATUS_MORE_PROCESSING_REQUIRED;
}


/* ================================================================
 * PART 3: STACK ASSEMBLY
 *
 * This is the master function that loads the NT5 IDE driver stack
 * and wires it into the Win9x IOS. It performs these steps:
 *
 * 1. PE-load pciidex.sys (bus driver), resolve against NTKSHIM exports
 * 2. PE-load pciide.sys (vendor driver), resolve against pciidex exports
 * 3. PE-load atapi.sys (miniport), resolve against NTKSHIM exports
 * 4. For each IDE controller from PCI scan:
 *    a. Create PDO via pci_create_pdo()
 *    b. Call pciidex DriverEntry
 *    c. Trigger channel enumeration
 *    d. Call atapi AddDevice for each channel
 *    e. Send IRP_MN_START_DEVICE to each device
 * 5. Register with IOS (DRP pattern from IOSBRIDGE.C)
 * 6. Create DCBs for detected devices
 * 7. Install calldown handler
 * ================================================================ */

int wdm_load_nt5_ide_stack(PCI_IDE_DEVICE pci_devices[], ULONG count)
{
    ULONG i;

    VxD_Debug_Printf("WDM: Loading NT5 IDE stack for %lu controller(s)\n",
                     count);

    /* Initialize bridge state */
    wdm_zero_mem(&g_wdm, sizeof(g_wdm));

    /* ----- Step 1: Load pciidex.sys (IDE bus extender) ----- */

    /*
     * TODO: PE-load pciidex.sys and resolve imports against NTKSHIM exports.
     *
     * NOTE: The current PE loader in PELOAD.C only resolves imports from
     * a single DLL (SCSIPORT.SYS). For Phase 3, we need multi-DLL import
     * resolution: pciidex.sys imports from ntoskrnl.exe and HAL.dll,
     * pciide.sys imports from ntoskrnl.exe and pciidex.sys, atapi.sys
     * imports from ntoskrnl.exe, HAL.dll, and SCSIPORT.SYS (via pciidex).
     * PELOAD.C will need modification to iterate the import directory and
     * match each DLL name to the correct export table.
     *
     * When PELOAD.C is extended, the loading sequence will be:
     *
     * g_wdm.pciidex_driver = wdm_load_driver("pciidex.sys",
     *     ntkshim_export_table, ntkshim_export_count);
     * g_wdm.pciide_driver = wdm_load_driver("pciide.sys",
     *     pciidex_export_table, pciidex_export_count);
     * g_wdm.atapi_driver = wdm_load_driver("atapi.sys",
     *     ntkshim_export_table, ntkshim_export_count);
     */

    VxD_Debug_Printf("WDM: TODO: PE-load pciidex.sys, pciide.sys, atapi.sys\n");
    VxD_Debug_Printf("WDM: PELOAD.C needs multi-DLL import resolution\n");

    /* ----- Step 2: Create PDOs and call DriverEntry ----- */

    for (i = 0; i < count; i++) {
        PDEVICE_OBJECT pdo;

        VxD_Debug_Printf("WDM: Processing controller %lu: %04x:%04x\n",
                         i, (ULONG)pci_devices[i].VendorId,
                         (ULONG)pci_devices[i].DeviceId);

        /* Create a PDO representing this PCI IDE controller */
        pdo = pci_create_pdo(&pci_devices[i]);
        if (!pdo) {
            VxD_Debug_Printf("WDM: Failed to create PDO for controller %lu\n",
                             i);
            continue;
        }

        /*
         * TODO: When PE loader is extended:
         *
         * Step 2a: Call pciidex DriverEntry. This registers pciidex as
         *          a bus driver. DriverEntry takes (DriverObject, RegistryPath).
         *          We pass our shimmed DriverObject.
         *
         * Step 2b: Call pciidex AddDevice(DriverObject, PDO). This creates
         *          an FDO and attaches it to our PDO. pciidex then enumerates
         *          IDE channels (primary/secondary) and creates channel PDOs.
         *
         * Step 2c: For each channel PDO that pciidex creates:
         *          - Call atapi AddDevice(AtapiDriverObject, ChannelPdo)
         *          - This creates atapi's FDO on top of the channel PDO
         *
         * Step 2d: Send IRP_MN_START_DEVICE down each device stack:
         *          - Build resource lists (I/O ports, IRQ) from PCI BARs
         *          - The standard IDE primary channel uses 0x1F0-0x1F7, IRQ 14
         *          - Secondary channel uses 0x170-0x177, IRQ 15
         *          - For native mode (ProgIf bit 0/2), read BARs from PCI config
         *
         * Step 2e: After START_DEVICE completes, the atapi miniport has
         *          initialized and can accept SCSI commands via IRP_MJ_SCSI.
         */

        VxD_Debug_Printf("WDM: TODO: DriverEntry/AddDevice/StartDevice "
                         "for controller %lu\n", i);
    }

    /* ----- Step 3: Register with IOS ----- */

    /* Fill in DDB (same pattern as ios_register_port_driver in IOSBRIDGE.C) */
    g_wdm.ddb.DDB_size  = sizeof(DDB);
    g_wdm.ddb.DDB_class = DDB_CLASS_PORT;
    g_wdm.ddb.DDB_flags = 0;
    g_wdm.ddb.DDB_merit = DDB_MERIT_PORT_NORMAL;

    g_wdm.ddb.DDB_name[0]  = 'N';
    g_wdm.ddb.DDB_name[1]  = 'T';
    g_wdm.ddb.DDB_name[2]  = '5';
    g_wdm.ddb.DDB_name[3]  = 'I';
    g_wdm.ddb.DDB_name[4]  = 'D';
    g_wdm.ddb.DDB_name[5]  = 'E';
    g_wdm.ddb.DDB_name[6]  = '\0';

    g_wdm.ddb.DDB_aep_handler = (PVOID)wdm_calldown_handler;
    g_wdm.ddb.DDB_lgn         = 0;
    g_wdm.ddb.DDB_expansion   = NULL;

    {
        ULONG result = IOS_Register(&g_wdm.ddb);
        if (result != 0) {
            VxD_Debug_Printf("WDM: IOS_Register FAILED (result=%lx)\n",
                             result);
            return -1;
        }
    }

    VxD_Debug_Printf("WDM: IOS_Register succeeded\n");

    /* ----- Step 4: Create DCBs for detected devices ----- */

    /*
     * TODO: After the NT5 stack is fully initialized, enumerate the
     * devices that atapi.sys detected. For each device:
     *
     * 1. Send SCSI INQUIRY to determine device type
     * 2. Create a DCB via bridge_create_dcb() (reuse from IOSBRIDGE.C)
     * 3. Create a WDM_BRIDGE_CONTEXT linking the DCB to the NT5 stack top
     * 4. Insert our calldown handler into the DCB's chain
     *
     * For now, this is a placeholder. The actual DCB creation depends on
     * the NT5 stack being fully operational.
     */

    VxD_Debug_Printf("WDM: TODO: DCB creation after NT5 stack init\n");

    /* ----- Step 5: Mark initialized ----- */

    g_wdm.initialized = TRUE;

    VxD_Debug_Printf("WDM: NT5 IDE stack assembly complete "
                     "(%lu controllers)\n", count);

    return 0;
}


/* ================================================================
 * PART 4: IOS CALLDOWN HANDLER
 *
 * Installed in each DCB's calldown chain (bottom of chain, as the
 * port driver). When IOS routes an IOR to us, we look up the
 * WDM_BRIDGE_CONTEXT for the target DCB and dispatch via
 * wdm_ior_to_irp.
 * ================================================================ */

void __cdecl wdm_calldown_handler(PIOR ior)
{
    PWDM_BRIDGE_CONTEXT ctx;
    PDCB dcb;

    if (!ior) {
        return;
    }

    if (!g_wdm.initialized) {
        VxD_Debug_Printf("WDM: Calldown before init, failing IOR\n");
        wdm_complete_ior(ior, IORS_DEVICE_ERROR);
        return;
    }

    /*
     * Recover the DCB from the IOR. In a real implementation, IOS
     * passes the DCB through the calldown chain context. We stored
     * a pointer to our bridge context in IOR_private_port during
     * the calldown insertion.
     */
    ctx = (PWDM_BRIDGE_CONTEXT)ior->IOR_private_port;
    if (!ctx || !ctx->Active) {
        /* Try to find context by walking our device table.
         * This fallback handles cases where IOR_private_port
         * wasn't set (e.g. direct AEP_IOR dispatch). */
        ULONG i;
        ctx = NULL;
        for (i = 0; i < g_wdm.num_devices; i++) {
            if (g_wdm.devices[i].Active) {
                /* If only one device, use it. Otherwise we need
                 * the DCB to disambiguate. */
                ctx = &g_wdm.devices[i];
                break;
            }
        }
    }

    if (!ctx || !ctx->StackTop) {
        VxD_Debug_Printf("WDM: No bridge context for IOR, failing\n");
        wdm_complete_ior(ior, IORS_ERROR_DESIGNTR);
        return;
    }

    VxD_Debug_Printf("WDM: Calldown func=%04x -> IRP dispatch\n",
                     ior->IOR_func);

    /* Dispatch: translate IOR to IRP and send down the NT5 stack.
     * If the miniport completes synchronously, wdm_irp_completion
     * will fire before wdm_ior_to_irp returns. Otherwise, completion
     * happens asynchronously (interrupt-driven). */
    wdm_ior_to_irp(ior, ctx->StackTop);
}


/* ================================================================
 * PART 5: PE LOADING HELPERS
 *
 * Wrappers around PELOAD.C's pe_load_image() for loading NT5
 * kernel-mode drivers. These build the appropriate export table
 * (ntoskrnl.exe + HAL.dll shim functions) and pass it to the
 * PE loader for import resolution.
 * ================================================================ */

/*
 * wdm_load_driver - Load an NT5 .sys driver image
 *
 * Currently a placeholder. When PELOAD.C is extended to support
 * multi-DLL import resolution, this function will:
 *
 * 1. Read the .sys file from disk (or embedded resource)
 * 2. Call pe_load_image() with an array of export tables:
 *    - ntoskrnl.exe: all NTKSHIM functions
 *    - HAL.dll: HalGetInterruptVector, HalTranslateBusAddress, etc.
 *    - SCSIPORT.SYS / pciidex.sys: for cross-driver imports
 * 3. Apply relocations
 * 4. Return the DriverEntry address
 *
 * NOTE: PELOAD.C currently walks a single import DLL. It needs to
 * be modified to iterate IMAGE_IMPORT_DESCRIPTOR entries and match
 * each DLL name to the correct export table. This is a key
 * modification for Phase 3.
 */
PVOID wdm_load_driver(const char *filename,
                       PVOID exports[], ULONG export_count)
{
    VxD_Debug_Printf("WDM: wdm_load_driver('%s') - "
                     "STUB: needs multi-DLL PELOAD.C\n", filename);

    /*
     * TODO: Implementation when PELOAD.C supports multi-DLL resolution:
     *
     * UCHAR *file_buf;
     * ULONG file_size;
     * PVOID entry;
     *
     * file_buf = read_file_from_disk(filename, &file_size);
     * if (!file_buf) return NULL;
     *
     * entry = pe_load_image(file_buf, file_size,
     *     multi_dll_exports, multi_dll_count);
     *
     * VxD_HeapFree(file_buf, 0);
     * return entry;
     */

    return NULL;
}


/* ================================================================
 * PART 6: STATUS TRANSLATION
 *
 * Maps SRB completion status to IOR status codes. This is the same
 * logic as srb_status_to_ior_status in IOSBRIDGE.C, kept here to
 * avoid a cross-file dependency (the two bridge files may not be
 * linked together).
 * ================================================================ */

static USHORT wdm_srb_status_to_ior_status(UCHAR srb_status,
                                             UCHAR scsi_status)
{
    /* Strip auto-sense flag (bit 7) */
    UCHAR status = srb_status & 0x3F;

    switch (status) {

    case SRB_STATUS_SUCCESS:
        return IORS_SUCCESS;

    case SRB_STATUS_PENDING:
        return IORS_SUCCESS;

    case SRB_STATUS_ABORTED:
        return IORS_REQUEST_ABORTED;

    case SRB_STATUS_ERROR:
        if (scsi_status == 0x02) { /* CHECK_CONDITION */
            return IORS_CMD_FAILED;
        }
        if (scsi_status == 0x08) { /* BUSY */
            return IORS_DRIVENOTREADY;
        }
        return IORS_DEVICE_ERROR;

    case SRB_STATUS_INVALID_REQUEST:
        return IORS_CMD_INVALID;

    case SRB_STATUS_NO_DEVICE:
        return IORS_ERROR_DESIGNTR;

    case SRB_STATUS_TIMEOUT:
        return IORS_TIME_OUT;

    case SRB_STATUS_SELECTION_TIMEOUT:
        return IORS_DRIVENOTREADY;

    case SRB_STATUS_BUS_RESET:
        return IORS_UNCERTAIN_MEDIA;

    case SRB_STATUS_DATA_OVERRUN:
        return IORS_SUCCESS;

    default:
        VxD_Debug_Printf("WDM: Unknown SRB status %02x\n", status);
        return IORS_DEVICE_ERROR;
    }
}


/* ================================================================
 * PART 7: UTILITY FUNCTIONS
 * ================================================================ */

static void wdm_complete_ior(PIOR ior, USHORT status)
{
    ior->IOR_status = status;
    IOS_BD_Command_Complete(ior);
}

static void wdm_zero_mem(PVOID dst, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = 0;
    }
}

static void wdm_copy_mem(PVOID dst, PVOID src, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    PUCHAR s = (PUCHAR)src;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = s[i];
    }
}

static PWDM_BRIDGE_CONTEXT wdm_find_context_for_dcb(PDCB dcb)
{
    ULONG i;
    for (i = 0; i < g_wdm.num_devices; i++) {
        if (g_wdm.devices[i].Active && g_wdm.devices[i].Dcb == dcb) {
            return &g_wdm.devices[i];
        }
    }
    return NULL;
}
