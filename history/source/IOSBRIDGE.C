/*
 * IOSBRIDGE.C - IOS Bridge: Win9x IOR <-> NT4 SRB Translation Layer
 *
 * This file implements the bridge between the Windows 9x I/O Supervisor
 * (IOS) and NT4 SCSI miniport drivers. It registers as a Win9x port
 * driver (.PDR), receives IOS I/O Requests (IORs), translates them
 * into NT4 SCSI Request Blocks (SRBs), and dispatches them to the
 * miniport's HwStartIo routine.
 *
 * === How Win9x IOS Works ===
 *
 * Win9x storage I/O uses a layered driver model managed by the I/O
 * Supervisor (IOS). The layers are, top to bottom:
 *
 *   1. File System Driver (FSD)   - VFAT, CDFS, UDF, etc.
 *   2. Volume Tracker (VTD)       - Monitors volume changes
 *   3. Type-Specific Driver (TSD) - Translates FS requests to block I/O
 *   4. Vendor-Supplied Driver     - Optional (filters, encryption, etc.)
 *   5. Port Driver (PDR)          - Talks to hardware  <-- THIS IS US
 *
 * Communication between layers uses:
 *   - AEP (Async Event Packets): System events (init, shutdown, config)
 *   - IOR (I/O Requests):        Actual data transfer requests
 *   - DCB (Device Control Blocks): Represent detected devices
 *   - DDB (Device Descriptor Blocks): Identify loaded drivers
 *
 * The port driver registers with IOS by providing an AEP handler. IOS
 * calls this handler for system events. For I/O, IOS routes IORs down
 * the "calldown chain" to our handler.
 *
 * === What This Bridge Does ===
 *
 * 1. Registers with IOS as a port driver (AEP_INITIALIZE)
 * 2. Detects ATAPI devices via the NT miniport (AEP_CONFIG_DCB)
 * 3. Receives IORs from IOS (AEP_IOR / calldown chain)
 * 4. Translates IORs to SRBs (SCSI READ/WRITE/INQUIRY/TEST UNIT READY)
 * 5. Dispatches SRBs to the miniport's HwStartIo
 * 6. On SRB completion, translates status back and completes the IOR
 * 7. Queues IORs when the miniport is busy (StartIo model = 1 at a time)
 *
 * === Relationship to NTMINI.C ===
 *
 * NTMINI.C provides:
 *   - The ScsiPort API shim (22 functions)
 *   - The PE loader for the NT4 .sys file
 *   - Global state including miniport callbacks
 *
 * This file (IOSBRIDGE.C) provides:
 *   - The IOS-facing port driver interface
 *   - IOR-to-SRB translation
 *   - SRB completion handling
 *   - IOR queuing
 *   - DCB creation for detected devices
 *
 * They are compiled together into a single .PDR binary.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License.
 */

#include "W9XDDK.H"
#include "PORTIO.H"

/* ================================================================
 * NT Miniport Structures (shared with NTMINI.C)
 *
 * We need the SRB structure here for the translation layer.
 * These definitions must match NTMINI.C exactly. In a real build,
 * both files would include a shared header.
 * ================================================================ */

/* Physical address (NT style) */
typedef union {
    struct {
        ULONG LowPart;
        LONG  HighPart;
    };
    long long QuadPart;
} SCSI_PHYSICAL_ADDRESS;

/* SRB Function codes */
#define SRB_FUNCTION_EXECUTE_SCSI   0x00
#define SRB_FUNCTION_ABORT_COMMAND  0x10
#define SRB_FUNCTION_IO_CONTROL     0x02
#define SRB_FUNCTION_RESET_BUS      0x12
#define SRB_FUNCTION_RESET_DEVICE   0x13
#define SRB_FUNCTION_FLUSH          0x08
#define SRB_FUNCTION_SHUTDOWN       0x07

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
#define SRB_FLAGS_DISABLE_AUTOSENSE 0x00000040
#define SRB_FLAGS_DISABLE_DISCONNECT 0x00000080

/* SCSI Status codes */
#define SCSI_STATUS_GOOD            0x00
#define SCSI_STATUS_CHECK_CONDITION 0x02
#define SCSI_STATUS_BUSY            0x08

/* SCSI CDB opcodes we use */
#define SCSI_OP_TEST_UNIT_READY     0x00
#define SCSI_OP_REQUEST_SENSE       0x03
#define SCSI_OP_INQUIRY             0x12
#define SCSI_OP_START_STOP_UNIT     0x1B
#define SCSI_OP_PREVENT_ALLOW       0x1E
#define SCSI_OP_READ_CAPACITY       0x25
#define SCSI_OP_READ10              0x28
#define SCSI_OP_WRITE10             0x2A
#define SCSI_OP_VERIFY10            0x2F
#define SCSI_OP_READ_TOC            0x43

/* SCSI_REQUEST_BLOCK (must match NTMINI.C definition exactly) */
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

/* Sense data buffer */
typedef struct _SENSE_DATA {
    UCHAR   ErrorCode;          /* 0x70 = current, 0x71 = deferred */
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

/* Sense key values */
#define SENSE_NO_SENSE          0x00
#define SENSE_NOT_READY         0x02
#define SENSE_MEDIUM_ERROR      0x03
#define SENSE_ILLEGAL_REQUEST   0x05
#define SENSE_UNIT_ATTENTION    0x06


/* ================================================================
 * EXTERN: Miniport interface (provided by NTMINI.C global state)
 *
 * These are the miniport callbacks we dispatch SRBs to. NTMINI.C
 * fills these in when ScsiPortInitialize is called from the
 * miniport's DriverEntry.
 * ================================================================ */

/* Miniport's HwStartIo: accepts one SRB, returns TRUE if started */
extern BOOLEAN (*g_HwStartIo)(PVOID DevExt, PSCSI_REQUEST_BLOCK Srb);

/* Miniport's HwResetBus: resets the SCSI bus */
extern BOOLEAN (*g_HwResetBus)(PVOID DevExt, ULONG PathId);

/* Miniport's HwInterrupt: called from ISR to service interrupt */
extern BOOLEAN (*g_HwInterrupt)(PVOID DevExt);

/* Device extension: miniport's private context */
extern PVOID g_DeviceExtension;

/* SRB extension pool (miniport may need per-SRB workspace) */
extern ULONG g_SrbExtensionSize;

/* Flag: set by ScsiPortNotification(RequestComplete) in NTMINI.C */
extern volatile BOOLEAN g_SrbCompleted;

/* Flag: set by ScsiPortNotification(NextRequest) in NTMINI.C */
extern volatile BOOLEAN g_ReadyForNext;


/* ================================================================
 * BRIDGE GLOBAL STATE
 * ================================================================ */

/* Device tracking */
typedef struct _BRIDGE_DEVICE {
    PDCB    dcb;                /* Win9x DCB for this device */
    UCHAR   target_id;          /* SCSI/ATAPI target ID (0 or 1 for IDE) */
    UCHAR   lun;                /* Logical unit number (usually 0) */
    UCHAR   device_type;        /* DCB_TYPE_* */
    UCHAR   is_atapi;           /* TRUE if ATAPI (packet) device */
    ULONG   sector_size;        /* Sector size in bytes */
    ULONG   total_sectors;      /* Total addressable sectors */
} BRIDGE_DEVICE, *PBRIDGE_DEVICE;

/* IOR queue entry (for queueing when miniport is busy) */
typedef struct _IOR_QUEUE_ENTRY {
    PIOR    ior;                /* The queued IOR */
    PDCB    dcb;                /* Target DCB */
    struct _IOR_QUEUE_ENTRY *next; /* Next in queue */
} IOR_QUEUE_ENTRY, *PIOR_QUEUE_ENTRY;

/* Bridge global state */
static struct {
    /* Our identity */
    DDB         ddb;                    /* Device Descriptor Block */
    CALLDOWN    calldown;               /* Our calldown chain entry */
    BOOLEAN     initialized;            /* Driver is ready */
    BOOLEAN     boot_complete;          /* Boot sequence finished */

    /* Device table */
    BRIDGE_DEVICE devices[MAX_DEVICES]; /* Tracked devices */
    ULONG       num_devices;            /* Number of detected devices */

    /* IOR queue (for flow control to the miniport) */
    PIOR_QUEUE_ENTRY queue_head;        /* Head of pending IOR queue */
    PIOR_QUEUE_ENTRY queue_tail;        /* Tail of pending IOR queue */
    ULONG       queue_depth;            /* Number of queued IORs */

    /* Current in-flight I/O */
    PIOR        active_ior;             /* IOR currently being processed */
    SCSI_REQUEST_BLOCK active_srb;      /* SRB for the active IOR */
    SENSE_DATA  sense_buffer;           /* Auto-sense data buffer */
    UCHAR       srb_extension[256];     /* SRB extension workspace */
    BOOLEAN     busy;                   /* Miniport has an active SRB */

    /* IOS context (saved during initialization) */
    ULONG       ios_ref;                /* IOS reference from AEP_INITIALIZE */

    /* Interrupt handle */
    ULONG       irq_handle;            /* VPICD IRQ handle */

} g_bridge;


/* ================================================================
 * FORWARD DECLARATIONS
 * ================================================================ */

/* AEP handler (entry point from IOS) */
void __cdecl aep_handler(PAEP aep);

/* AEP sub-handlers */
static void aep_initialize(PAEP aep);
static void aep_boot_complete(PAEP aep);
static void aep_config_dcb(PAEP aep);
static void aep_unconfig_dcb(PAEP aep);
static void aep_process_ior(PAEP aep);
static void aep_system_shutdown(PAEP aep);
static void aep_uninitialize(PAEP aep);

/* IOR handler and SRB translation */
static void ior_handler(PIOR ior, PDCB dcb);
static void build_srb_from_ior(PIOR ior, PDCB dcb,
                                PSCSI_REQUEST_BLOCK srb);
static void build_read_write_cdb(PSCSI_REQUEST_BLOCK srb,
                                  ULONG lba, USHORT block_count,
                                  BOOLEAN is_write);
static void build_test_unit_ready_cdb(PSCSI_REQUEST_BLOCK srb);
static void build_passthrough_srb(PIOR ior, PSCSI_REQUEST_BLOCK srb);

/* SRB completion */
void srb_complete(PSCSI_REQUEST_BLOCK srb);
static USHORT srb_status_to_ior_status(UCHAR srb_status, UCHAR scsi_status);

/* IOR queue management */
static void ior_queue_enqueue(PIOR ior, PDCB dcb);
static BOOLEAN ior_queue_dequeue(PIOR *out_ior, PDCB *out_dcb);
static void ior_queue_drain(USHORT status);

/* DCB helpers */
static PBRIDGE_DEVICE find_device_for_dcb(PDCB dcb);
static PBRIDGE_DEVICE find_device_by_target(UCHAR target_id, UCHAR lun);

/* Utility */
static void zero_mem(PVOID dst, ULONG size);
static void copy_mem(PVOID dst, PVOID src, ULONG size);
static void complete_ior(PIOR ior, USHORT status);


/* ================================================================
 * PART 1: PORT DRIVER REGISTRATION
 *
 * Called once at load time to register with IOS. This sets up our
 * DDB and tells IOS where our AEP handler lives.
 *
 * In the real PDR, this is called from the VxD control procedure's
 * Sys_Dynamic_Device_Init handler (in NTMINI.ASM).
 * ================================================================ */

int ios_register_port_driver(void)
{
    ULONG result;

    DBGPRINT("NTMINI: ios_register_port_driver()\n");

    /* Zero out bridge state */
    zero_mem(&g_bridge, sizeof(g_bridge));

    /* Fill in our Device Descriptor Block.
     * This tells IOS:
     *   - We are a port driver (DDB_CLASS_PORT)
     *   - Our name is "NTMINI" (visible in device manager)
     *   - Our AEP handler function pointer
     *   - Our merit (priority in the driver stack) */
    g_bridge.ddb.DDB_size  = sizeof(DDB);
    g_bridge.ddb.DDB_class = DDB_CLASS_PORT;
    g_bridge.ddb.DDB_flags = 0;
    g_bridge.ddb.DDB_merit = DDB_MERIT_PORT_NORMAL;

    /* Driver name: padded to 16 chars, null-terminated */
    g_bridge.ddb.DDB_name[0]  = 'N';
    g_bridge.ddb.DDB_name[1]  = 'T';
    g_bridge.ddb.DDB_name[2]  = 'M';
    g_bridge.ddb.DDB_name[3]  = 'I';
    g_bridge.ddb.DDB_name[4]  = 'N';
    g_bridge.ddb.DDB_name[5]  = 'I';
    g_bridge.ddb.DDB_name[6]  = '\0';

    /* The AEP handler: IOS calls this for all events */
    g_bridge.ddb.DDB_aep_handler = (PVOID)aep_handler;
    g_bridge.ddb.DDB_lgn         = 0;
    g_bridge.ddb.DDB_expansion   = NULL;

    /* Register with IOS. This call is synchronous. On return, IOS
     * has recorded our DDB and will begin sending us AEP events.
     * The first event will be AEP_INITIALIZE. */
    result = IOS_Register(&g_bridge.ddb);

    if (result != 0) {
        DBGPRINT("NTMINI: IOS_Register FAILED (result=%lx)\n", result);
        return -1;
    }

    DBGPRINT("NTMINI: IOS_Register succeeded\n");
    return 0;
}


/* ================================================================
 * PART 2: AEP HANDLER (Main IOS Dispatch)
 *
 * This is the single entry point IOS uses to communicate with us.
 * It receives an AEP with a function code and dispatches to the
 * appropriate sub-handler.
 *
 * The calling convention is __cdecl (Win9x VxD standard).
 * The AEP pointer is valid only for the duration of the call.
 *
 * IOS calls this from ring 0. We are NOT reentrant: IOS serializes
 * AEP calls to a given port driver.
 * ================================================================ */

void __cdecl aep_handler(PAEP aep)
{
    switch (aep->AEP_func) {

    case AEP_INITIALIZE:
        /* IOS is initializing us. Save context, report capabilities.
         * This is the first AEP we receive after IOS_Register. */
        aep_initialize(aep);
        break;

    case AEP_BOOT_COMPLETE:
        /* System boot sequence is finished. All devices detected,
         * file systems mounted. Safe to do deferred initialization. */
        aep_boot_complete(aep);
        break;

    case AEP_CONFIG_DCB:
        /* IOS is telling us about a device. For a port driver, this
         * is where we claim devices we can handle. We insert our
         * calldown handler into the DCB's calldown chain. */
        aep_config_dcb(aep);
        break;

    case AEP_UNCONFIG_DCB:
        /* A device is being removed. Clean up our state for it. */
        aep_unconfig_dcb(aep);
        break;

    case AEP_IOR:
        /* An I/O request has arrived for one of our devices.
         * This is the hot path: translate IOR to SRB and dispatch. */
        aep_process_ior(aep);
        break;

    case AEP_SYSTEM_SHUTDOWN:
        /* System is shutting down gracefully. Flush caches, etc. */
        aep_system_shutdown(aep);
        break;

    case AEP_SYSTEM_CRIT_SHUTDOWN:
        /* Power failure or critical shutdown. No time for cleanup. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_UNINITIALIZE:
        /* Driver is being unloaded. Free all resources. */
        aep_uninitialize(aep);
        break;

    case AEP_ASSOCIATE_DCB:
        /* IOS is associating a DCB with us. Accept it. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_DISASSOCIATE_DCB:
        /* IOS is removing our association with a DCB. Accept it. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_REAL_MODE_HANDOFF:
        /* Transitioning from real-mode to protected-mode driver.
         * We don't have a real-mode component, so nothing to do. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_DCB_LOCK:
    case AEP_MOUNT_NOTIFY:
    case AEP_CREATE_VRP:
    case AEP_DESTROY_VRP:
    case AEP_REFRESH_DRIVE:
        /* These events are handled by upper layers (VTD, TSD).
         * Port drivers can ignore them. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    default:
        /* Unknown AEP function code. Return "not supported" so IOS
         * knows we didn't handle it and can try the next driver. */
        aep->AEP_result = AEP_NO_SUPPORT;
        break;
    }
}


/* ================================================================
 * PART 2a: AEP Sub-Handlers
 * ================================================================ */

/*
 * aep_initialize - Handle AEP_INITIALIZE
 *
 * This is called once after IOS_Register succeeds. IOS passes us
 * context information we may need later. We save it and report
 * that initialization succeeded.
 *
 * At this point, the NT miniport has already been loaded and
 * initialized by NTMINI.C (ScsiPortInitialize was called from
 * the miniport's DriverEntry). So we know what devices exist.
 */
static void aep_initialize(PAEP aep)
{
    DBGPRINT("NTMINI: AEP_INITIALIZE\n");

    /* Save IOS reference data. This is used in future IOS calls. */
    g_bridge.ios_ref = aep->AEP_BI_REFERENCE;

    /* Mark ourselves as initialized */
    g_bridge.initialized = TRUE;

    /* Success */
    aep->AEP_result = AEP_SUCCESS;
}


/*
 * aep_boot_complete - Handle AEP_BOOT_COMPLETE
 *
 * By this point, all devices are detected and configured. The
 * file system can now access our devices. This is a good place
 * to do any deferred initialization.
 */
static void aep_boot_complete(PAEP aep)
{
    DBGPRINT("NTMINI: AEP_BOOT_COMPLETE (devices=%lu)\n",
             g_bridge.num_devices);

    g_bridge.boot_complete = TRUE;
    aep->AEP_result = AEP_SUCCESS;
}


/*
 * aep_config_dcb - Handle AEP_CONFIG_DCB
 *
 * IOS calls this when it discovers a device (or when a device
 * is being configured for our port driver). We need to:
 *
 * 1. Check if the DCB represents a device we handle
 * 2. Fill in device-specific DCB fields
 * 3. Insert our calldown handler into the DCB's chain
 * 4. Create a BRIDGE_DEVICE entry for tracking
 *
 * For our use case, we handle ATAPI CD-ROM/DVD drives that
 * the NT miniport detected during HwFindAdapter/HwInitialize.
 *
 * In a more complete implementation, we would create the DCBs
 * ourselves (since we ARE the port driver). But IOS may also
 * present us with DCBs created by the real-mode mapper or
 * a previous driver that we're replacing.
 */
static void aep_config_dcb(PAEP aep)
{
    PDCB dcb;
    PBRIDGE_DEVICE dev;
    ULONG i;

    dcb = aep->AEP_CD_DCB;
    if (!dcb) {
        aep->AEP_result = AEP_FAILURE;
        return;
    }

    DBGPRINT("NTMINI: AEP_CONFIG_DCB type=%d target=%d\n",
             dcb->DCB_device_type, dcb->DCB_target_id);

    /* Check if we already track this device */
    for (i = 0; i < g_bridge.num_devices; i++) {
        if (g_bridge.devices[i].dcb == dcb) {
            /* Already configured. Update and accept. */
            aep->AEP_result = AEP_SUCCESS;
            return;
        }
    }

    /* Only claim devices we can handle.
     * We handle: CD-ROM, DVD (ATAPI devices on the IDE bus).
     * We could also handle hard disks, but Win98's built-in
     * ESDI_506.PDR already handles IDE hard disks well.
     * Our value-add is ATAPI/SCSI devices that need the
     * NT miniport's superior command handling. */
    if (dcb->DCB_device_type != DCB_TYPE_CDROM &&
        !(dcb->DCB_dmd_flags & DCB_DEV_ATAPI)) {
        /* Not our device. Let another port driver handle it. */
        aep->AEP_result = AEP_NO_SUPPORT;
        return;
    }

    /* Bounds check */
    if (g_bridge.num_devices >= MAX_DEVICES) {
        DBGPRINT("NTMINI: Too many devices, ignoring DCB\n");
        aep->AEP_result = AEP_FAILURE;
        return;
    }

    /* Create our tracking entry */
    dev = &g_bridge.devices[g_bridge.num_devices];
    zero_mem(dev, sizeof(BRIDGE_DEVICE));

    dev->dcb        = dcb;
    dev->target_id  = dcb->DCB_target_id;
    dev->lun        = dcb->DCB_lun;
    dev->device_type = dcb->DCB_device_type;
    dev->is_atapi   = (dcb->DCB_dmd_flags & DCB_DEV_ATAPI) ? TRUE : FALSE;

    /* Set sector size based on device type */
    if (dcb->DCB_device_type == DCB_TYPE_CDROM) {
        dev->sector_size = CDROM_SECTOR_SIZE;
        dcb->DCB_apparent_blk_size  = CDROM_SECTOR_SIZE;
        dcb->DCB_apparent_blk_shift = 11; /* log2(2048) = 11 */
    } else {
        dev->sector_size = DISK_SECTOR_SIZE;
        dcb->DCB_apparent_blk_size  = DISK_SECTOR_SIZE;
        dcb->DCB_apparent_blk_shift = 9;  /* log2(512) = 9 */
    }

    /* Fill in DCB device flags */
    dcb->DCB_dmd_flags |= DCB_DEV_PHYSICAL | DCB_DEV_REMOVABLE;
    if (dev->is_atapi) {
        dcb->DCB_dmd_flags |= DCB_DEV_ATAPI;
    }
    if (dcb->DCB_device_type == DCB_TYPE_CDROM) {
        dcb->DCB_dmd_flags |= DCB_DEV_CDROM;
    }

    /* Set max transfer length.
     * For PIO ATAPI, we can do 64KB per transfer (limited by the
     * byte count registers in the ATAPI protocol). */
    dcb->DCB_max_xfer_len = 0x10000; /* 64KB */

    /* Insert our calldown handler into the DCB's calldown chain.
     * This tells IOS "send I/O for this device to my handler."
     * We insert at the bottom because we ARE the port driver. */
    g_bridge.calldown.CD_func  = (CALLDOWN_FUNC)ior_handler;
    g_bridge.calldown.CD_ddb   = &g_bridge.ddb;
    g_bridge.calldown.CD_next  = NULL;
    g_bridge.calldown.CD_flags = 0;

    ISP_INSERT_CALLDOWN(dcb, &g_bridge.calldown, &g_bridge.ddb,
                         ISPCDF_BOTTOM | ISPCDF_PORT_DRIVER);

    g_bridge.num_devices++;

    DBGPRINT("NTMINI: Claimed device target=%d type=%d (now %lu devices)\n",
             dev->target_id, dev->device_type, g_bridge.num_devices);

    aep->AEP_result = AEP_SUCCESS;
}


/*
 * aep_unconfig_dcb - Handle AEP_UNCONFIG_DCB
 *
 * A device is being removed. Remove our tracking entry and clean up.
 */
static void aep_unconfig_dcb(PAEP aep)
{
    PDCB dcb;
    ULONG i;

    dcb = aep->AEP_UD_DCB;
    if (!dcb) {
        aep->AEP_result = AEP_SUCCESS;
        return;
    }

    DBGPRINT("NTMINI: AEP_UNCONFIG_DCB target=%d\n", dcb->DCB_target_id);

    /* Find and remove our tracking entry */
    for (i = 0; i < g_bridge.num_devices; i++) {
        if (g_bridge.devices[i].dcb == dcb) {
            /* Shift remaining entries down */
            ULONG j;
            for (j = i; j < g_bridge.num_devices - 1; j++) {
                copy_mem(&g_bridge.devices[j],
                         &g_bridge.devices[j + 1],
                         sizeof(BRIDGE_DEVICE));
            }
            g_bridge.num_devices--;
            break;
        }
    }

    aep->AEP_result = AEP_SUCCESS;
}


/*
 * aep_process_ior - Handle AEP_IOR
 *
 * This is the IOS path for routing an IOR to us. Some IOS versions
 * use this instead of the calldown chain for certain operations.
 * We extract the IOR and DCB from the AEP and call our handler.
 */
static void aep_process_ior(PAEP aep)
{
    PIOR ior;
    PDCB dcb;

    ior = aep->AEP_IOR_IOR;
    dcb = aep->AEP_IOR_DCB;

    if (!ior || !dcb) {
        aep->AEP_result = AEP_FAILURE;
        return;
    }

    ior_handler(ior, dcb);
    aep->AEP_result = AEP_SUCCESS;
}


/*
 * aep_system_shutdown - Handle AEP_SYSTEM_SHUTDOWN
 *
 * System is shutting down. Drain any pending I/O, send FLUSH
 * commands to devices if needed.
 */
static void aep_system_shutdown(PAEP aep)
{
    DBGPRINT("NTMINI: AEP_SYSTEM_SHUTDOWN\n");

    /* Drain the IOR queue, failing any pending requests */
    ior_queue_drain(IORS_REQUEST_ABORTED);

    /* If the miniport has a pending SRB, we can't do much about it.
     * The hardware is about to lose power anyway. */

    aep->AEP_result = AEP_SUCCESS;
}


/*
 * aep_uninitialize - Handle AEP_UNINITIALIZE
 *
 * Driver is being unloaded. Free all allocated resources.
 */
static void aep_uninitialize(PAEP aep)
{
    DBGPRINT("NTMINI: AEP_UNINITIALIZE\n");

    /* Drain pending I/O */
    ior_queue_drain(IORS_REQUEST_ABORTED);

    /* Clear device table */
    g_bridge.num_devices = 0;
    g_bridge.initialized = FALSE;

    aep->AEP_result = AEP_SUCCESS;
}


/* ================================================================
 * PART 3: IOR-TO-SRB TRANSLATOR
 *
 * This is the heart of the bridge. When IOS routes an IOR to us
 * (either via the calldown chain or AEP_IOR), we:
 *
 * 1. Check if the miniport is busy (one-SRB-at-a-time model)
 * 2. If busy, queue the IOR for later
 * 3. If free, build an SRB from the IOR and dispatch
 *
 * The translation maps Win9x IOR operations to SCSI CDBs:
 *   IOR_READ              -> SCSI READ(10)  [opcode 0x28]
 *   IOR_WRITE             -> SCSI WRITE(10) [opcode 0x2A]
 *   IOR_WRITEV            -> SCSI WRITE(10) + VERIFY(10)
 *   IOR_VERIFY            -> SCSI VERIFY(10) [opcode 0x2F]
 *   IOR_MEDIA_CHECK       -> SCSI TEST UNIT READY [opcode 0x00]
 *   IOR_MEDIA_CHECK_RESET -> SCSI TEST UNIT READY
 *   IOR_SCSI_PASS_THROUGH -> Raw CDB from IOR
 *   IOR_DOS_RESET         -> SRB_FUNCTION_RESET_BUS
 *   IOR_CANCEL             -> Abort current SRB
 *   IOR_EJECT_MEDIA        -> SCSI START/STOP UNIT [opcode 0x1B]
 *   IOR_LOCK_MEDIA         -> SCSI PREVENT/ALLOW [opcode 0x1E]
 *   IOR_UNLOCK_MEDIA       -> SCSI PREVENT/ALLOW [opcode 0x1E]
 *
 * For READ/WRITE, the IOR contains:
 *   IOR_start_addr[0]     - Starting sector (LBA, low 32 bits)
 *   IOR_start_addr[1]     - Starting sector (LBA, high 32 bits, usually 0)
 *   IOR_xfer_count        - Transfer size in BYTES
 *   IOR_buffer_ptr        - Linear address of data buffer
 *
 * We convert sectors + byte count to a SCSI READ/WRITE(10) CDB:
 *   CDB[0] = opcode (0x28 read, 0x2A write)
 *   CDB[1] = 0 (reserved)
 *   CDB[2-5] = LBA (big-endian 32-bit)
 *   CDB[6] = 0 (reserved)
 *   CDB[7-8] = block count (big-endian 16-bit)
 *   CDB[9] = 0 (control)
 * ================================================================ */

static void ior_handler(PIOR ior, PDCB dcb)
{
    PBRIDGE_DEVICE dev;

    if (!ior) {
        return;
    }

    /* Find our device tracking entry for this DCB */
    dev = find_device_for_dcb(dcb);
    if (!dev) {
        /* We don't own this device. This shouldn't happen if IOS
         * routed correctly, but be defensive. */
        DBGPRINT("NTMINI: IOR for unknown DCB, failing\n");
        complete_ior(ior, IORS_ERROR_DESIGNTR);
        return;
    }

    /* If the miniport is busy processing another SRB, queue this IOR.
     * NT4 miniports using the StartIo model process one SRB at a time.
     * The miniport signals readiness for the next SRB via
     * ScsiPortNotification(NextRequest). */
    if (g_bridge.busy) {
        DBGPRINT("NTMINI: Miniport busy, queueing IOR func=%04x\n",
                 ior->IOR_func);
        ior_queue_enqueue(ior, dcb);
        return;
    }

    /* Build an SRB from this IOR and dispatch to the miniport */
    build_srb_from_ior(ior, dcb, &g_bridge.active_srb);

    /* Record active state */
    g_bridge.active_ior = ior;
    g_bridge.busy       = TRUE;
    g_SrbCompleted      = FALSE;
    g_ReadyForNext      = FALSE;

    /* Dispatch to the miniport's HwStartIo.
     *
     * This is a synchronous call. The miniport either:
     *   a) Completes the SRB immediately (PIO transfer done in-line)
     *   b) Starts the hardware and returns TRUE (interrupt will complete)
     *   c) Returns FALSE (can't handle this SRB right now)
     *
     * For case (b), the interrupt handler (in NTMINI.C) calls
     * g_HwInterrupt, which eventually calls ScsiPortNotification
     * (RequestComplete), which sets g_SrbCompleted = TRUE and
     * calls our srb_complete(). */

    DBGPRINT("NTMINI: HwStartIo SRB func=%02x CDB[0]=%02x target=%d\n",
             g_bridge.active_srb.Function,
             g_bridge.active_srb.Cdb[0],
             g_bridge.active_srb.TargetId);

    if (!g_HwStartIo(g_DeviceExtension, &g_bridge.active_srb)) {
        /* Miniport rejected the SRB. This is unusual but can happen
         * if the device is in an error state. */
        DBGPRINT("NTMINI: HwStartIo returned FALSE\n");
        g_bridge.busy = FALSE;
        g_bridge.active_ior = NULL;
        complete_ior(ior, IORS_CMD_FAILED);
        return;
    }

    /* If the SRB was completed synchronously (PIO mode often does this),
     * the completion callback has already fired. Check. */
    if (g_SrbCompleted) {
        /* SRB was completed inline during HwStartIo.
         * srb_complete() has already been called. Nothing more to do. */
    }
    /* Otherwise, we wait for the interrupt to complete the SRB.
     * The ISR in NTMINI.C will call g_HwInterrupt, which will call
     * ScsiPortNotification(RequestComplete), which will call
     * srb_complete(). All of this happens at interrupt time. */
}


/*
 * build_srb_from_ior - Translate an IOR into an SRB
 *
 * This is the core translation function. It examines the IOR's
 * function code and builds the appropriate SCSI CDB.
 */
static void build_srb_from_ior(PIOR ior, PDCB dcb,
                                PSCSI_REQUEST_BLOCK srb)
{
    PBRIDGE_DEVICE dev;
    ULONG lba;
    ULONG byte_count;
    USHORT block_count;

    dev = find_device_for_dcb(dcb);

    /* Zero the SRB */
    zero_mem(srb, sizeof(SCSI_REQUEST_BLOCK));

    /* Common SRB fields */
    srb->Length               = sizeof(SCSI_REQUEST_BLOCK);
    srb->Function             = SRB_FUNCTION_EXECUTE_SCSI;
    srb->PathId               = 0;
    srb->TargetId             = dev ? dev->target_id : dcb->DCB_target_id;
    srb->Lun                  = dev ? dev->lun : dcb->DCB_lun;
    srb->SrbStatus            = SRB_STATUS_PENDING;
    srb->TimeOutValue         = 10; /* 10 seconds default */
    srb->SenseInfoBuffer      = &g_bridge.sense_buffer;
    srb->SenseInfoBufferLength = sizeof(SENSE_DATA);
    srb->OriginalRequest      = (PVOID)ior;

    /* Set up SRB extension if the miniport needs one */
    if (g_SrbExtensionSize > 0 && g_SrbExtensionSize <= sizeof(g_bridge.srb_extension)) {
        srb->SrbExtension = g_bridge.srb_extension;
        zero_mem(srb->SrbExtension, g_SrbExtensionSize);
    }

    switch (ior->IOR_func) {

    case IOR_READ:
        /*
         * READ: Translate sector address + byte count to SCSI READ(10).
         *
         * IOR_start_addr[0] = starting sector (LBA)
         * IOR_xfer_count    = number of bytes to read
         * IOR_buffer_ptr    = destination buffer (linear address)
         *
         * SCSI READ(10) CDB:
         *   Byte 0: 0x28 (opcode)
         *   Byte 1: 0x00 (reserved)
         *   Bytes 2-5: LBA (big-endian 32-bit)
         *   Byte 6: 0x00 (reserved/group)
         *   Bytes 7-8: Transfer length in blocks (big-endian 16-bit)
         *   Byte 9: 0x00 (control)
         */
        lba        = ior->IOR_start_addr[0];
        byte_count = ior->IOR_xfer_count;
        block_count = (USHORT)(byte_count / (dev ? dev->sector_size : CDROM_SECTOR_SIZE));
        if (block_count == 0) block_count = 1;

        srb->DataBuffer         = (PVOID)ior->IOR_buffer_ptr;
        srb->DataTransferLength = byte_count;
        srb->SrbFlags           = SRB_FLAGS_DATA_IN | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;

        build_read_write_cdb(srb, lba, block_count, FALSE);

        DBGPRINT("NTMINI: READ LBA=%lu count=%u bytes=%lu\n",
                 lba, block_count, byte_count);
        break;

    case IOR_WRITE:
    case IOR_WRITEV:
        /*
         * WRITE: Same as READ but opcode 0x2A and data direction is OUT.
         * IOR_WRITEV is write-with-verify: we send WRITE(10) and let
         * the drive handle verification (most modern drives verify
         * internally anyway).
         */
        lba        = ior->IOR_start_addr[0];
        byte_count = ior->IOR_xfer_count;
        block_count = (USHORT)(byte_count / (dev ? dev->sector_size : CDROM_SECTOR_SIZE));
        if (block_count == 0) block_count = 1;

        srb->DataBuffer         = (PVOID)ior->IOR_buffer_ptr;
        srb->DataTransferLength = byte_count;
        srb->SrbFlags           = SRB_FLAGS_DATA_OUT | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;

        build_read_write_cdb(srb, lba, block_count, TRUE);

        DBGPRINT("NTMINI: WRITE LBA=%lu count=%u bytes=%lu\n",
                 lba, block_count, byte_count);
        break;

    case IOR_VERIFY:
        /*
         * VERIFY: Verify sectors without data transfer.
         * SCSI VERIFY(10) opcode 0x2F. Same CDB layout as READ(10)
         * but no data phase.
         */
        lba        = ior->IOR_start_addr[0];
        byte_count = ior->IOR_xfer_count;
        block_count = (USHORT)(byte_count / (dev ? dev->sector_size : CDROM_SECTOR_SIZE));
        if (block_count == 0) block_count = 1;

        srb->DataBuffer         = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags           = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength          = 10;

        srb->Cdb[0] = SCSI_OP_VERIFY10;
        srb->Cdb[1] = 0x00;
        /* LBA in bytes 2-5 (big-endian) */
        srb->Cdb[2] = (UCHAR)((lba >> 24) & 0xFF);
        srb->Cdb[3] = (UCHAR)((lba >> 16) & 0xFF);
        srb->Cdb[4] = (UCHAR)((lba >>  8) & 0xFF);
        srb->Cdb[5] = (UCHAR)((lba >>  0) & 0xFF);
        srb->Cdb[6] = 0x00;
        /* Block count in bytes 7-8 (big-endian) */
        srb->Cdb[7] = (UCHAR)((block_count >> 8) & 0xFF);
        srb->Cdb[8] = (UCHAR)((block_count >> 0) & 0xFF);
        srb->Cdb[9] = 0x00;

        DBGPRINT("NTMINI: VERIFY LBA=%lu count=%u\n", lba, block_count);
        break;

    case IOR_SCSI_PASS_THROUGH:
        /*
         * SCSI PASS-THROUGH: The caller has a raw SCSI CDB they want
         * sent to the device. This is used by CD-ROM file systems for
         * READ TOC, MODE SENSE, GET CONFIGURATION, etc.
         *
         * The CDB is in the IOR's SCSI passthrough sub-structure.
         */
        build_passthrough_srb(ior, srb);

        DBGPRINT("NTMINI: PASSTHROUGH CDB[0]=%02x len=%d\n",
                 srb->Cdb[0], srb->CdbLength);
        break;

    case IOR_MEDIA_CHECK:
    case IOR_MEDIA_CHECK_RESET:
        /*
         * MEDIA CHECK: Is media present? Has it changed?
         * Use SCSI TEST UNIT READY (opcode 0x00), a 6-byte CDB
         * with all zeros. No data transfer.
         *
         * If the device returns CHECK CONDITION with sense key
         * UNIT ATTENTION (0x06), media has changed.
         */
        build_test_unit_ready_cdb(srb);

        DBGPRINT("NTMINI: MEDIA_CHECK\n");
        break;

    case IOR_DOS_RESET:
        /*
         * DOS RESET: Reset the device/bus.
         * Use SRB_FUNCTION_RESET_BUS, which calls the miniport's
         * HwResetBus callback.
         */
        srb->Function            = SRB_FUNCTION_RESET_BUS;
        srb->DataBuffer          = NULL;
        srb->DataTransferLength  = 0;
        srb->SrbFlags            = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength           = 0;

        DBGPRINT("NTMINI: DOS_RESET\n");
        break;

    case IOR_LOCK_MEDIA:
        /*
         * LOCK MEDIA: Prevent media removal (e.g. CD tray).
         * SCSI PREVENT ALLOW MEDIUM REMOVAL (0x1E):
         *   CDB[4] bit 0: 1 = prevent, 0 = allow
         */
        srb->DataBuffer         = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags           = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength          = 6;
        srb->Cdb[0] = SCSI_OP_PREVENT_ALLOW;
        srb->Cdb[4] = 0x01; /* Prevent removal */
        srb->TimeOutValue       = 5;

        DBGPRINT("NTMINI: LOCK_MEDIA\n");
        break;

    case IOR_UNLOCK_MEDIA:
        /*
         * UNLOCK MEDIA: Allow media removal.
         */
        srb->DataBuffer         = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags           = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength          = 6;
        srb->Cdb[0] = SCSI_OP_PREVENT_ALLOW;
        srb->Cdb[4] = 0x00; /* Allow removal */
        srb->TimeOutValue       = 5;

        DBGPRINT("NTMINI: UNLOCK_MEDIA\n");
        break;

    case IOR_EJECT_MEDIA:
        /*
         * EJECT MEDIA: Open the CD tray.
         * SCSI START STOP UNIT (0x1B):
         *   CDB[4] bit 1: LoEj = 1 (load/eject)
         *   CDB[4] bit 0: Start = 0 (eject) or 1 (load)
         */
        srb->DataBuffer         = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags           = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength          = 6;
        srb->Cdb[0] = SCSI_OP_START_STOP_UNIT;
        srb->Cdb[4] = 0x02; /* LoEj=1, Start=0 = eject */
        srb->TimeOutValue       = 10;

        DBGPRINT("NTMINI: EJECT_MEDIA\n");
        break;

    case IOR_LOAD_MEDIA:
        /*
         * LOAD MEDIA: Close the CD tray.
         */
        srb->DataBuffer         = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags           = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength          = 6;
        srb->Cdb[0] = SCSI_OP_START_STOP_UNIT;
        srb->Cdb[4] = 0x03; /* LoEj=1, Start=1 = load */
        srb->TimeOutValue       = 10;

        DBGPRINT("NTMINI: LOAD_MEDIA\n");
        break;

    case IOR_CANCEL:
        /*
         * CANCEL: Abort the current operation.
         * If we have a pending SRB, we can try to abort it, but
         * most IDE/ATAPI devices don't support ABORT well.
         * Best we can do is reset the bus.
         */
        if (g_bridge.busy && g_HwResetBus) {
            g_HwResetBus(g_DeviceExtension, 0);
        }
        srb->Function = SRB_FUNCTION_ABORT_COMMAND;
        srb->DataBuffer = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength = 0;

        DBGPRINT("NTMINI: CANCEL\n");
        break;

    case IOR_CLEAR_QUEUE:
        /*
         * CLEAR QUEUE: Discard all pending requests.
         * Drain our IOR queue and complete this one.
         */
        ior_queue_drain(IORS_REQUEST_ABORTED);
        srb->Function = SRB_FUNCTION_RESET_DEVICE;
        srb->DataBuffer = NULL;
        srb->DataTransferLength = 0;
        srb->SrbFlags = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->CdbLength = 0;

        DBGPRINT("NTMINI: CLEAR_QUEUE\n");
        break;

    default:
        /*
         * Unknown IOR function. Fail it gracefully.
         */
        DBGPRINT("NTMINI: Unknown IOR func=%04x, failing\n", ior->IOR_func);
        g_bridge.busy = FALSE;
        g_bridge.active_ior = NULL;
        complete_ior(ior, IORS_NOT_SUPPORTED);
        return;
    }
}


/*
 * build_read_write_cdb - Build a SCSI READ(10) or WRITE(10) CDB
 *
 * SCSI READ(10) / WRITE(10) CDB format (10 bytes):
 *   Byte 0:   Operation code (0x28 = READ, 0x2A = WRITE)
 *   Byte 1:   Flags (FUA, etc.) - we leave at 0
 *   Bytes 2-5: Logical Block Address (big-endian 32-bit)
 *   Byte 6:   Group number / reserved
 *   Bytes 7-8: Transfer length in blocks (big-endian 16-bit)
 *   Byte 9:   Control byte
 */
static void build_read_write_cdb(PSCSI_REQUEST_BLOCK srb,
                                  ULONG lba, USHORT block_count,
                                  BOOLEAN is_write)
{
    srb->CdbLength = 10;

    /* Opcode */
    srb->Cdb[0] = is_write ? SCSI_OP_WRITE10 : SCSI_OP_READ10;

    /* Flags byte (byte 1): 0 for normal operation */
    srb->Cdb[1] = 0x00;

    /* LBA in bytes 2-5 (big-endian, MSB first) */
    srb->Cdb[2] = (UCHAR)((lba >> 24) & 0xFF);
    srb->Cdb[3] = (UCHAR)((lba >> 16) & 0xFF);
    srb->Cdb[4] = (UCHAR)((lba >>  8) & 0xFF);
    srb->Cdb[5] = (UCHAR)((lba >>  0) & 0xFF);

    /* Reserved / group number (byte 6) */
    srb->Cdb[6] = 0x00;

    /* Transfer length in blocks, bytes 7-8 (big-endian) */
    srb->Cdb[7] = (UCHAR)((block_count >> 8) & 0xFF);
    srb->Cdb[8] = (UCHAR)((block_count >> 0) & 0xFF);

    /* Control byte (byte 9) */
    srb->Cdb[9] = 0x00;
}


/*
 * build_test_unit_ready_cdb - Build a SCSI TEST UNIT READY CDB
 *
 * TEST UNIT READY is a 6-byte CDB that's all zeros.
 * It checks if the device is ready to accept commands.
 * No data transfer occurs.
 *
 * For CD-ROM drives, this also checks for media presence.
 * If media changed, the drive returns CHECK CONDITION with
 * sense key UNIT ATTENTION.
 */
static void build_test_unit_ready_cdb(PSCSI_REQUEST_BLOCK srb)
{
    srb->CdbLength           = 6;
    srb->DataBuffer          = NULL;
    srb->DataTransferLength  = 0;
    srb->SrbFlags            = SRB_FLAGS_NO_DATA_TRANSFER;

    /* All six CDB bytes are zero */
    srb->Cdb[0] = SCSI_OP_TEST_UNIT_READY;
    srb->Cdb[1] = 0x00;
    srb->Cdb[2] = 0x00;
    srb->Cdb[3] = 0x00;
    srb->Cdb[4] = 0x00;
    srb->Cdb[5] = 0x00;
}


/*
 * build_passthrough_srb - Build an SRB from a SCSI pass-through IOR
 *
 * For IOR_SCSI_PASS_THROUGH, the caller provides a raw SCSI CDB.
 * We copy it into the SRB and set up the data transfer based on
 * the direction flags.
 *
 * This is used by the CD-ROM file system (CDFS) for commands like
 * READ TOC, MODE SENSE, GET CONFIGURATION, etc. that don't map to
 * simple read/write operations.
 */
static void build_passthrough_srb(PIOR ior, PSCSI_REQUEST_BLOCK srb)
{
    PIOR_SCSI_PASSTHROUGH sp;
    ULONG i;

    sp = &ior->IOR_scsi_pass;

    /* Copy the CDB from the passthrough structure */
    srb->CdbLength = sp->SP_CDBLength;
    if (srb->CdbLength > MAX_CDB_LENGTH) {
        srb->CdbLength = MAX_CDB_LENGTH;
    }
    for (i = 0; i < srb->CdbLength; i++) {
        srb->Cdb[i] = sp->SP_CDB[i];
    }

    /* Set up data transfer based on direction flags */
    if (sp->SP_Flags & SP_DATA_IN) {
        srb->SrbFlags            = SRB_FLAGS_DATA_IN | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
        srb->DataBuffer          = (PVOID)ior->IOR_buffer_ptr;
        srb->DataTransferLength  = sp->SP_DataLength;
    } else if (sp->SP_Flags & SP_DATA_OUT) {
        srb->SrbFlags            = SRB_FLAGS_DATA_OUT | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
        srb->DataBuffer          = (PVOID)ior->IOR_buffer_ptr;
        srb->DataTransferLength  = sp->SP_DataLength;
    } else {
        srb->SrbFlags            = SRB_FLAGS_NO_DATA_TRANSFER;
        srb->DataBuffer          = NULL;
        srb->DataTransferLength  = 0;
    }

    /* Passthrough commands may take longer (e.g. FORMAT UNIT) */
    srb->TimeOutValue = 30;
}


/* ================================================================
 * PART 4: SRB COMPLETION CALLBACK
 *
 * Called when ScsiPortNotification(RequestComplete, ...) fires
 * in NTMINI.C. This is the return path: we translate the SRB
 * completion status back to IOR status and notify IOS.
 *
 * This function may be called:
 *   a) Inline during HwStartIo (synchronous PIO completion)
 *   b) From the interrupt handler (asynchronous completion)
 *
 * Either way, we must:
 *   1. Translate SRB status to IOR status
 *   2. Set IOR_status and IOR_xfer_count
 *   3. Call the IOR's completion callback
 *   4. Mark the miniport as no longer busy
 *   5. Dequeue and dispatch the next IOR if one is pending
 * ================================================================ */

void srb_complete(PSCSI_REQUEST_BLOCK srb)
{
    PIOR ior;
    USHORT ior_status;
    PIOR next_ior;
    PDCB next_dcb;

    /* The IOR that originated this SRB is stored in OriginalRequest */
    ior = (PIOR)srb->OriginalRequest;

    if (!ior) {
        /* Orphaned SRB completion. This can happen if the IOR was
         * cancelled while the SRB was in flight. Just clean up. */
        DBGPRINT("NTMINI: srb_complete with no IOR (orphaned)\n");
        g_bridge.busy       = FALSE;
        g_bridge.active_ior = NULL;
        return;
    }

    /* Translate SRB status to IOR status */
    ior_status = srb_status_to_ior_status(srb->SrbStatus, srb->ScsiStatus);

    DBGPRINT("NTMINI: srb_complete SrbStatus=%02x ScsiStatus=%02x -> IORS=%04x\n",
             srb->SrbStatus, srb->ScsiStatus, ior_status);

    /* For media check operations, translate UNIT ATTENTION to
     * IORS_UNCERTAIN_MEDIA (which tells IOS to re-read the media) */
    if ((ior->IOR_func == IOR_MEDIA_CHECK ||
         ior->IOR_func == IOR_MEDIA_CHECK_RESET) &&
        srb->ScsiStatus == SCSI_STATUS_CHECK_CONDITION) {
        PSENSE_DATA sense = (PSENSE_DATA)srb->SenseInfoBuffer;
        if (sense && (sense->SenseKey == SENSE_UNIT_ATTENTION)) {
            ior_status = IORS_UNCERTAIN_MEDIA;
        }
    }

    /* Update the IOR with results */
    ior->IOR_status = ior_status;

    /* Set actual transfer count. For successful transfers, this
     * should match the requested count. For errors, it reflects
     * how much was actually transferred before the error. */
    if (ior_status == IORS_SUCCESS) {
        /* Leave IOR_xfer_count as-is (requested amount was transferred) */
    } else {
        /* On error, report how much was actually transferred.
         * The SRB's DataTransferLength may have been updated by
         * the miniport to reflect the actual amount. */
        if (srb->DataTransferLength < ior->IOR_xfer_count) {
            ior->IOR_xfer_count = srb->DataTransferLength;
        }
    }

    /* Copy sense data back for SCSI pass-through commands */
    if (ior->IOR_func == IOR_SCSI_PASS_THROUGH &&
        srb->SenseInfoBufferLength > 0 &&
        srb->SenseInfoBuffer != NULL) {
        ULONG sense_len = srb->SenseInfoBufferLength;
        if (sense_len > sizeof(ior->IOR_scsi_pass.SP_SenseData)) {
            sense_len = sizeof(ior->IOR_scsi_pass.SP_SenseData);
        }
        copy_mem(ior->IOR_scsi_pass.SP_SenseData,
                 srb->SenseInfoBuffer, sense_len);
        ior->IOR_scsi_pass.SP_SenseLength = (UCHAR)sense_len;
    }

    /* Clear active state BEFORE completing the IOR, because the
     * completion callback may trigger another IOR submission. */
    g_bridge.active_ior = NULL;
    g_bridge.busy       = FALSE;

    /* Complete the IOR (calls IOR_callback and notifies IOS) */
    complete_ior(ior, ior_status);

    /* Check if there's a queued IOR waiting to be dispatched.
     * The miniport signalled NextRequest (via g_ReadyForNext),
     * so it's ready for another SRB. */
    if (ior_queue_dequeue(&next_ior, &next_dcb)) {
        DBGPRINT("NTMINI: Dequeueing next IOR func=%04x\n",
                 next_ior->IOR_func);
        ior_handler(next_ior, next_dcb);
    }
}


/*
 * srb_status_to_ior_status - Map SRB completion status to IOR status
 *
 * The NT miniport sets SrbStatus and ScsiStatus. We map these
 * to the Win9x IOR status codes that IOS and upper layers expect.
 */
static USHORT srb_status_to_ior_status(UCHAR srb_status, UCHAR scsi_status)
{
    /* Strip the auto-sense flag from SrbStatus (bit 7) */
    UCHAR status = srb_status & 0x3F;

    switch (status) {

    case SRB_STATUS_SUCCESS:
        /* Command completed without error */
        return IORS_SUCCESS;

    case SRB_STATUS_PENDING:
        /* Still in progress. Shouldn't happen in completion. */
        return IORS_SUCCESS;

    case SRB_STATUS_ABORTED:
        /* Command was aborted */
        return IORS_REQUEST_ABORTED;

    case SRB_STATUS_ERROR:
        /* Generic error. Check SCSI status for more detail. */
        if (scsi_status == SCSI_STATUS_CHECK_CONDITION) {
            /* Sense data has been filled in (if auto-sense is on).
             * We could parse it for a more specific error, but
             * for now, map common sense keys. */
            return IORS_CMD_FAILED;
        }
        if (scsi_status == SCSI_STATUS_BUSY) {
            return IORS_DRIVENOTREADY;
        }
        return IORS_DEVICE_ERROR;

    case SRB_STATUS_INVALID_REQUEST:
        /* The miniport doesn't understand this SRB */
        return IORS_CMD_INVALID;

    case SRB_STATUS_NO_DEVICE:
        /* Target device doesn't exist */
        return IORS_ERROR_DESIGNTR;

    case SRB_STATUS_TIMEOUT:
        /* Command timed out */
        return IORS_TIME_OUT;

    case SRB_STATUS_SELECTION_TIMEOUT:
        /* Device didn't respond to selection */
        return IORS_DRIVENOTREADY;

    case SRB_STATUS_BUS_RESET:
        /* Bus was reset, command lost */
        return IORS_UNCERTAIN_MEDIA;

    case SRB_STATUS_DATA_OVERRUN:
        /* More data than expected. This can be OK for some commands. */
        return IORS_SUCCESS;

    default:
        /* Unknown SRB status */
        DBGPRINT("NTMINI: Unknown SRB status %02x\n", status);
        return IORS_DEVICE_ERROR;
    }
}


/* ================================================================
 * PART 5: IOR QUEUE
 *
 * NT4 miniports using the StartIo model process one SRB at a time.
 * When the miniport is busy, we queue incoming IORs in a simple
 * linked list. When the miniport signals NextRequest (via
 * ScsiPortNotification), we dequeue and dispatch the next IOR.
 *
 * Queue entries are allocated from the VxD heap. In a real
 * implementation, we'd use a fixed-size pool to avoid heap
 * fragmentation and allocation failures under load.
 *
 * The queue is FIFO to preserve ordering. This is important for
 * CD-ROM read-ahead patterns and for correctness of write ordering.
 * ================================================================ */

static void ior_queue_enqueue(PIOR ior, PDCB dcb)
{
    PIOR_QUEUE_ENTRY entry;

    /* Allocate a queue entry from the VxD heap */
    entry = (PIOR_QUEUE_ENTRY)VxD_HeapAllocate(
        sizeof(IOR_QUEUE_ENTRY), HEAPF_ZEROINIT);

    if (!entry) {
        /* Can't allocate. Fail the IOR immediately. */
        DBGPRINT("NTMINI: Queue alloc failed, failing IOR\n");
        complete_ior(ior, IORS_MEMORY_PROBLEM);
        return;
    }

    entry->ior  = ior;
    entry->dcb  = dcb;
    entry->next = NULL;

    /* Append to tail of queue */
    if (g_bridge.queue_tail) {
        g_bridge.queue_tail->next = entry;
    } else {
        g_bridge.queue_head = entry;
    }
    g_bridge.queue_tail = entry;
    g_bridge.queue_depth++;

    DBGPRINT("NTMINI: Queue depth now %lu\n", g_bridge.queue_depth);
}

static BOOLEAN ior_queue_dequeue(PIOR *out_ior, PDCB *out_dcb)
{
    PIOR_QUEUE_ENTRY entry;

    if (!g_bridge.queue_head) {
        return FALSE;
    }

    entry = g_bridge.queue_head;
    g_bridge.queue_head = entry->next;
    if (!g_bridge.queue_head) {
        g_bridge.queue_tail = NULL;
    }
    g_bridge.queue_depth--;

    *out_ior = entry->ior;
    *out_dcb = entry->dcb;

    /* Free the queue entry */
    VxD_HeapFree(entry, 0);

    return TRUE;
}

/*
 * ior_queue_drain - Complete all queued IORs with the given status
 *
 * Used during shutdown and error recovery to empty the queue.
 */
static void ior_queue_drain(USHORT status)
{
    PIOR ior;
    PDCB dcb;

    while (ior_queue_dequeue(&ior, &dcb)) {
        DBGPRINT("NTMINI: Draining IOR func=%04x with status=%04x\n",
                 ior->IOR_func, status);
        complete_ior(ior, status);
    }
}


/* ================================================================
 * PART 6: DCB SETUP FOR ATAPI/CD-ROM
 *
 * When the NT miniport's HwFindAdapter and IDENTIFY succeed (done
 * in NTMINI.C via ScsiPortInitialize), we need to create Win9x
 * DCBs so IOS knows about our devices.
 *
 * This function is called from NTMINI.C after the miniport is
 * fully initialized. It creates a DCB for each detected device
 * and registers it with IOS.
 *
 * For ATAPI devices (CD-ROM, DVD):
 *   - Device type = DCB_TYPE_CDROM
 *   - Sector size = 2048
 *   - Removable media flag set
 *   - ATAPI flag set
 *
 * For ATA devices (hard disk):
 *   - We don't handle these (ESDI_506.PDR does)
 *   - But we include the code path for completeness
 * ================================================================ */

/*
 * bridge_create_dcb - Create a Win9x DCB for a detected device
 *
 * Parameters:
 *   target_id   - SCSI/ATAPI target ID (0 = master, 1 = slave)
 *   lun         - Logical unit number (usually 0)
 *   is_atapi    - TRUE for ATAPI (packet) devices
 *   device_type - DCB_TYPE_* constant
 *   vendor_id   - 8-char vendor string from IDENTIFY data
 *   product_id  - 16-char product string from IDENTIFY data
 *   rev_level   - 4-char revision string from IDENTIFY data
 *
 * Returns:
 *   Pointer to the created DCB, or NULL on failure
 */
PDCB bridge_create_dcb(
    UCHAR target_id,
    UCHAR lun,
    BOOLEAN is_atapi,
    UCHAR device_type,
    const char *vendor_id,
    const char *product_id,
    const char *rev_level)
{
    PDCB dcb;
    ULONG i;

    DBGPRINT("NTMINI: Creating DCB for target=%d type=%d atapi=%d\n",
             target_id, device_type, is_atapi);

    /* Allocate a DCB from the VxD heap.
     * In the real Win98 DDK, IOS provides an allocation function
     * (IOS_Requestor_Service with appropriate service code).
     * We use heap allocation as a stand-in. */
    dcb = (PDCB)VxD_HeapAllocate(sizeof(DCB), HEAPF_ZEROINIT);
    if (!dcb) {
        DBGPRINT("NTMINI: DCB allocation failed\n");
        return NULL;
    }

    /* Fill in DCB fields */
    dcb->DCB_cmn_size       = sizeof(DCB);
    dcb->DCB_next           = NULL;
    dcb->DCB_next_logical   = NULL;
    dcb->DCB_ddb            = &g_bridge.ddb;

    dcb->DCB_device_type    = device_type;
    dcb->DCB_bus_type       = DCB_BUS_ESDI; /* IDE is ESDI-class in Win9x */
    dcb->DCB_bus_number     = 0;
    dcb->DCB_target_id      = target_id;
    dcb->DCB_lun            = lun;

    /* Device flags */
    dcb->DCB_dmd_flags      = DCB_DEV_PHYSICAL;
    if (is_atapi) {
        dcb->DCB_dmd_flags |= DCB_DEV_ATAPI;
    }
    if (device_type == DCB_TYPE_CDROM) {
        dcb->DCB_dmd_flags |= DCB_DEV_CDROM | DCB_DEV_REMOVABLE | DCB_DEV_EJECTABLE;
    }

    /* Geometry: for CD-ROM, sector size is 2048, geometry is meaningless */
    if (device_type == DCB_TYPE_CDROM) {
        dcb->DCB_apparent_blk_size  = CDROM_SECTOR_SIZE;
        dcb->DCB_apparent_blk_shift = 11;
        dcb->DCB_apparent_head_count = 0;
        dcb->DCB_apparent_cyl_count  = 0;
        dcb->DCB_apparent_spt        = 0;
        dcb->DCB_apparent_total_sectors = 0; /* Unknown until media inserted */
    } else {
        dcb->DCB_apparent_blk_size  = DISK_SECTOR_SIZE;
        dcb->DCB_apparent_blk_shift = 9;
    }

    /* Max transfer length */
    dcb->DCB_max_xfer_len = 0x10000; /* 64KB */

    /* Copy identification strings */
    if (vendor_id) {
        for (i = 0; i < 8 && vendor_id[i]; i++) {
            dcb->DCB_vendor_id[i] = vendor_id[i];
        }
    }
    if (product_id) {
        for (i = 0; i < 16 && product_id[i]; i++) {
            dcb->DCB_product_id[i] = product_id[i];
        }
    }
    if (rev_level) {
        for (i = 0; i < 4 && rev_level[i]; i++) {
            dcb->DCB_rev_level[i] = rev_level[i];
        }
    }

    /* The DCB is allocated. Now IOS needs to know about it.
     * IOS_SendCommand or a special IOS service would register
     * the DCB. For now, we rely on AEP_CONFIG_DCB being called
     * by IOS when it processes our device enumeration. */

    return dcb;
}


/*
 * bridge_enumerate_devices - Called after NT miniport init to
 *                            enumerate detected devices
 *
 * This sends INQUIRY commands to all possible targets to discover
 * what's on the bus. For each responding device, we create a DCB.
 *
 * On a typical secondary IDE channel:
 *   Target 0 (master): CD-ROM or DVD drive
 *   Target 1 (slave):  CD-ROM, DVD, or empty
 *
 * We send SCSI INQUIRY to each target. ATAPI devices respond
 * with device type in byte 0 and identification in bytes 8-35.
 */
void bridge_enumerate_devices(void)
{
    SCSI_REQUEST_BLOCK srb;
    UCHAR inquiry_buf[36]; /* Standard INQUIRY response length */
    UCHAR target;
    UCHAR device_type;
    BOOLEAN is_atapi;
    char vendor_id[9];
    char product_id[17];
    char rev_level[5];
    ULONG i;

    DBGPRINT("NTMINI: Enumerating devices on bus\n");

    for (target = 0; target < 2; target++) {
        /* Build an INQUIRY SRB */
        zero_mem(&srb, sizeof(srb));
        zero_mem(inquiry_buf, sizeof(inquiry_buf));

        srb.Length               = sizeof(SCSI_REQUEST_BLOCK);
        srb.Function             = SRB_FUNCTION_EXECUTE_SCSI;
        srb.PathId               = 0;
        srb.TargetId             = target;
        srb.Lun                  = 0;
        srb.SrbStatus            = SRB_STATUS_PENDING;
        srb.CdbLength            = 6;
        srb.DataBuffer           = inquiry_buf;
        srb.DataTransferLength   = sizeof(inquiry_buf);
        srb.SrbFlags             = SRB_FLAGS_DATA_IN | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
        srb.TimeOutValue         = 5;
        srb.SenseInfoBuffer      = &g_bridge.sense_buffer;
        srb.SenseInfoBufferLength = sizeof(SENSE_DATA);

        /* INQUIRY CDB (6 bytes):
         *   Byte 0: 0x12 (INQUIRY opcode)
         *   Byte 1: 0x00
         *   Byte 2: 0x00 (page code)
         *   Byte 3: 0x00
         *   Byte 4: 36   (allocation length)
         *   Byte 5: 0x00 (control) */
        srb.Cdb[0] = SCSI_OP_INQUIRY;
        srb.Cdb[4] = 36;

        /* Set up SRB extension if needed */
        if (g_SrbExtensionSize > 0 && g_SrbExtensionSize <= sizeof(g_bridge.srb_extension)) {
            srb.SrbExtension = g_bridge.srb_extension;
            zero_mem(srb.SrbExtension, g_SrbExtensionSize);
        }

        /* Dispatch to miniport */
        g_SrbCompleted = FALSE;
        if (!g_HwStartIo(g_DeviceExtension, &srb)) {
            DBGPRINT("NTMINI: INQUIRY target %d: HwStartIo rejected\n", target);
            continue;
        }

        /* Wait for completion (poll for synchronous miniport behavior).
         * In a real implementation, this would be interrupt-driven.
         * During enumeration (before interrupts are fully set up),
         * the miniport may complete PIO transfers synchronously. */
        {
            ULONG timeout = 500000; /* ~500ms in stall iterations */
            while (!g_SrbCompleted && timeout > 0) {
                /* Check if the miniport's interrupt handler fires */
                if (g_HwInterrupt) {
                    g_HwInterrupt(g_DeviceExtension);
                }
                /* Small delay */
                {
                    PORT_STALL_ONE();
                }
                timeout--;
            }
        }

        /* Check result */
        if ((srb.SrbStatus & 0x3F) != SRB_STATUS_SUCCESS) {
            DBGPRINT("NTMINI: INQUIRY target %d: SrbStatus=%02x (no device)\n",
                     target, srb.SrbStatus);
            continue;
        }

        /* Parse INQUIRY response:
         *   Byte 0 [4:0]: Peripheral device type
         *   Byte 0 [7:5]: Peripheral qualifier
         *   Bytes 8-15:   Vendor ID (ASCII)
         *   Bytes 16-31:  Product ID (ASCII)
         *   Bytes 32-35:  Revision level (ASCII) */
        device_type = inquiry_buf[0] & 0x1F;
        is_atapi    = TRUE; /* We're on IDE, so if it responds, it's ATAPI */

        /* Extract strings */
        zero_mem(vendor_id, sizeof(vendor_id));
        zero_mem(product_id, sizeof(product_id));
        zero_mem(rev_level, sizeof(rev_level));

        for (i = 0; i < 8; i++)  vendor_id[i]  = (char)inquiry_buf[8 + i];
        for (i = 0; i < 16; i++) product_id[i] = (char)inquiry_buf[16 + i];
        for (i = 0; i < 4; i++)  rev_level[i]  = (char)inquiry_buf[32 + i];

        DBGPRINT("NTMINI: Found target %d: type=%d vendor='%.8s' product='%.16s'\n",
                 target, device_type, vendor_id, product_id);

        /* Map SCSI peripheral device type to Win9x DCB type */
        {
            UCHAR dcb_type;
            switch (device_type) {
            case 0x00: dcb_type = DCB_TYPE_DISK;    break;
            case 0x01: dcb_type = DCB_TYPE_TAPE;    break;
            case 0x05: dcb_type = DCB_TYPE_CDROM;   break;
            case 0x07: dcb_type = DCB_TYPE_OPTICAL_DISK; break;
            default:   dcb_type = DCB_TYPE_CDROM;   break; /* Default to CD-ROM for ATAPI */
            }

            /* Create the DCB */
            bridge_create_dcb(target, 0, is_atapi, dcb_type,
                              vendor_id, product_id, rev_level);
        }
    }

    DBGPRINT("NTMINI: Enumeration complete, %lu devices found\n",
             g_bridge.num_devices);
}


/* ================================================================
 * PART 7: UTILITY FUNCTIONS
 * ================================================================ */

/*
 * find_device_for_dcb - Look up our BRIDGE_DEVICE by DCB pointer
 */
static PBRIDGE_DEVICE find_device_for_dcb(PDCB dcb)
{
    ULONG i;
    for (i = 0; i < g_bridge.num_devices; i++) {
        if (g_bridge.devices[i].dcb == dcb) {
            return &g_bridge.devices[i];
        }
    }
    return NULL;
}

/*
 * find_device_by_target - Look up our BRIDGE_DEVICE by SCSI target/LUN
 */
static PBRIDGE_DEVICE find_device_by_target(UCHAR target_id, UCHAR lun)
{
    ULONG i;
    for (i = 0; i < g_bridge.num_devices; i++) {
        if (g_bridge.devices[i].target_id == target_id &&
            g_bridge.devices[i].lun == lun) {
            return &g_bridge.devices[i];
        }
    }
    return NULL;
}

/*
 * complete_ior - Set IOR status and invoke its completion callback
 *
 * This is the IOS-facing completion path. We set the status code
 * in the IOR, then call IOS_BD_Command_Complete to notify IOS
 * that the request is done. IOS handles calling the IOR's callback
 * and returning it to the requesting layer.
 */
static void complete_ior(PIOR ior, USHORT status)
{
    ior->IOR_status = status;

    /* Notify IOS that this IOR is complete.
     * IOS_BD_Command_Complete calls the IOR's callback chain
     * (which includes the originating layer's completion handler)
     * and performs any necessary post-processing. */
    IOS_BD_Command_Complete(ior);
}

/*
 * zero_mem - Zero-fill a memory region
 *
 * We provide our own to avoid C library dependencies (VxDs don't
 * link against the CRT).
 */
static void zero_mem(PVOID dst, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = 0;
    }
}

/*
 * copy_mem - Copy bytes from src to dst
 */
static void copy_mem(PVOID dst, PVOID src, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    PUCHAR s = (PUCHAR)src;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = s[i];
    }
}


/* ================================================================
 * PART 8: INTERRUPT BRIDGE
 *
 * This function is called from the VPICD interrupt handler (set
 * up in NTMINI.ASM). When the IDE hardware fires an interrupt:
 *
 * 1. The assembly ISR calls bridge_isr()
 * 2. We call the miniport's HwInterrupt
 * 3. If the miniport handled the interrupt, it calls
 *    ScsiPortNotification(RequestComplete) which calls
 *    srb_complete()
 * 4. We EOI the interrupt
 *
 * This function runs at interrupt time (ring 0, interrupts
 * disabled on the current processor). Keep it fast.
 * ================================================================ */

/*
 * bridge_isr - Called from the VPICD interrupt trampoline
 *
 * Returns TRUE if we handled the interrupt, FALSE if it wasn't ours.
 */
BOOLEAN bridge_isr(void)
{
    BOOLEAN handled;

    if (!g_bridge.initialized || !g_HwInterrupt) {
        return FALSE;
    }

    /* Call the miniport's interrupt handler.
     * For IDE, this reads the status register (which clears the
     * interrupt), checks if there's a pending SRB, and if so,
     * processes the data phase (PIO read/write) and completes. */
    handled = g_HwInterrupt(g_DeviceExtension);

    if (handled) {
        /* The miniport handled it. If it called
         * ScsiPortNotification(RequestComplete), the SRB is done
         * and srb_complete() has already fired. */

        /* EOI the interrupt through VPICD */
        if (g_bridge.irq_handle) {
            VxD_VPICD_Phys_EOI(g_bridge.irq_handle);
        }
    }

    return handled;
}


/*
 * bridge_setup_interrupt - Hook the IDE IRQ through VPICD
 *
 * Parameters:
 *   irq_number  - IRQ number to hook (15 for secondary IDE)
 *   isr_proc    - Assembly trampoline that calls bridge_isr
 *
 * Returns:
 *   0 on success, -1 on failure
 *
 * This is called from NTMINI.C during initialization, after the
 * miniport has been loaded and configured. The isr_proc parameter
 * is a pointer to an assembly stub that saves registers, calls
 * bridge_isr(), and handles the IRET.
 */
int bridge_setup_interrupt(USHORT irq_number, PVOID isr_proc)
{
    VPICD_IRQ_DESCRIPTOR desc;

    DBGPRINT("NTMINI: Hooking IRQ %d\n", irq_number);

    zero_mem(&desc, sizeof(desc));
    desc.VID_IRQ_Number   = irq_number;
    desc.VID_Options      = VPICD_OPT_CAN_SHARE;
    desc.VID_Hw_Int_Proc  = isr_proc;
    desc.VID_Hw_Int_Ref   = NULL;

    g_bridge.irq_handle = VxD_VPICD_Virtualize_IRQ(&desc);

    if (!g_bridge.irq_handle) {
        DBGPRINT("NTMINI: VPICD_Virtualize_IRQ FAILED for IRQ %d\n",
                 irq_number);
        return -1;
    }

    DBGPRINT("NTMINI: IRQ %d hooked (handle=%lx)\n",
             irq_number, g_bridge.irq_handle);
    return 0;
}
