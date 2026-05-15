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
 * 3. Receives I/O through IOS calldown routing
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
#include "WDMBRIDGE.H"
#include "PORTIO.H"

#pragma pack(push,1)
typedef struct _IOS_DRP {
    UCHAR   eyecatch[8];
    ULONG   lgn;
    ULONG   aer;
    ULONG   ilb;
    UCHAR   ascii_name[16];
    UCHAR   revision;
    ULONG   feature_code;
    USHORT  if_requirements;
    UCHAR   bus_type;
    USHORT  reg_result;
    ULONG   reference_data;
    UCHAR   reserved1[2];
    ULONG   reserved2;
} IOS_DRP, *PIOS_DRP;

typedef struct _IOS_ILB {
    ULONG   service_rtn;
    ULONG   dprintf_rtn;
    ULONG   wait_10th_sec;
    ULONG   internal_request;
    ULONG   io_criteria_rtn;
    ULONG   int_io_criteria_rtn;
    ULONG   dvt;
    ULONG   ios_mem_virt;
    ULONG   enqueue_iop;
    ULONG   dequeue_iop;
} IOS_ILB, *PIOS_ILB;

typedef struct _ISP_CREATE_DCB_PKT {
    USHORT  func;
    USHORT  result;
    USHORT  dcb_size;
    ULONG   dcb_ptr;
    UCHAR   pad[2];
} ISP_CREATE_DCB_PKT;

typedef struct _ISP_DCB_DESTROY_PKT {
    USHORT  func;
    USHORT  result;
    ULONG   dcb;
} ISP_DCB_DESTROY_PKT;

typedef struct _ISP_INSERT_CD_PKT {
    USHORT  func;
    USHORT  result;
    ULONG   dcb;
    ULONG   req;
    ULONG   ddb;
    USHORT  expan_len;
    ULONG   flags;
    UCHAR   lgn;
    UCHAR   pad;
} ISP_INSERT_CD_PKT;

typedef struct _ISP_ASSOC_DCB_PKT {
    USHORT  func;
    USHORT  result;
    ULONG   dcb;
    UCHAR   drive;
    UCHAR   flags;
    UCHAR   pad[2];
} ISP_ASSOC_DCB_PKT;

typedef struct _ISP_PICK_DRIVE_LETTER_PKT {
    USHORT  func;
    USHORT  result;
    ULONG   dcb;
    UCHAR   letter[2];
    UCHAR   flags;
    UCHAR   pad;
} ISP_PICK_DRIVE_LETTER_PKT;

typedef struct _ISP_GET_DCB_PKT {
    USHORT  func;
    USHORT  result;
    ULONG   dcb;
    ULONG   drive;
} ISP_GET_DCB_PKT;

typedef struct _ISP_QUERY_MATCH_PKT {
    USHORT  func;
    USHORT  result;
    ULONG   dcb;
    ULONG   drives;
} ISP_QUERY_MATCH_PKT;

typedef struct _ISP_BCAST_AEP_PKT {
    USHORT  func;
    USHORT  result;
    ULONG   paep;
} ISP_BCAST_AEP_PKT;

typedef struct _ISP_DEV_ARRIVED_PKT {
    USHORT  func;
    USHORT  result;
    ULONG   dcb;
    ULONG   flags;
} ISP_DEV_ARRIVED_PKT;

typedef struct _REAL_CALLDOWN_NODE {
    ULONG handler;
    ULONG flags;
    ULONG ddb;
    ULONG next;
} REAL_CALLDOWN_NODE;
#pragma pack(pop)

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
#define SRB_FLAGS_DATA_IN           0x00000040
#define SRB_FLAGS_DATA_OUT          0x00000080
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
    IOS_DRP     drp;                    /* IOS Driver Registration Packet */
    ULONG       ios_ddb;                /* IOS DDB id/handle from AEP_INITIALIZE */
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
static void __cdecl ios_ior_entry(PVOID iop_ptr);
void __cdecl ios_wdm_ior_entry(PVOID iop_ptr);
void __cdecl ios_wdm_log_iop(PVOID iop_ptr);
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
static void ios_dbg_hex8(UCHAR value);
static void ios_dbg_hex16(USHORT value);
static void ios_dbg_hex32(ULONG value);
static int is_ring0_ptr(ULONG ptr);
static PIOS_ILB find_existing_ilb(void);
static int ios_late_create_device(void);
int ios_late_destroy_device(void);
void __cdecl ios_late_cdrom_attach_probe(void);
void __cdecl ios_schedule_late_cdrom_attach_probe(void);

extern ULONG __cdecl VMM_Get_DDB_Wrapper(USHORT device_id);
extern void __cdecl Call_ILB_Service(ULONG service_rtn, PVOID isp_packet);
extern void __cdecl IFSMgr_NotifyVolumeArrival_Wrapper(ULONG drive);
extern ULONG __cdecl IFSMgr_CDROM_Attach_Wrapper(ULONG drive, PULONG out_vrp);
extern ULONG __cdecl IFSMgr_RegisterMount_Wrapper(ULONG pfnMount, ULONG version, ULONG is_default);
extern ULONG __cdecl IFSMgr_InstallFSHook_Wrapper(ULONG pfnHook);
extern ULONG __cdecl IFSMgr_FSDAttachSFT_Wrapper(ULONG pir);
extern ULONG __cdecl VWIN32_CopyMem_Wrapper(ULONG dst, ULONG src, ULONG cb);

static UCHAR g_iso_read_bounce[2048];
extern int __cdecl IFSMgr_CallPrevHook_Far16_32(ULONG packedPrev, int fn, int drive, int resType, int cpid, ULONG pir);
extern int __cdecl IFSMgr_CallPrevFSHook(ULONG ppPrevHook, ULONG fsdFnAddr, int fn, int drive, int resType, int cpid, ULONG pir);
extern int __cdecl IFSMgr_CallPrevFSHook5(ULONG ppPrevHook, int fn, int drive, int resType, int cpid, ULONG pir);
extern ULONG __cdecl VxD_SetTimer(ULONG milliseconds, PVOID callback, PVOID refdata);
extern void __cdecl IOS_BD_Complete_IOP(PVOID iop);
extern void ios_ior_stub(void);
extern int ntmini_search_stub_asm(void);
extern int ntmini_netdir_stub_asm(void);
extern int ntmini_shutdown_stub_asm(void);
extern void ios_aep_bridge(void);
extern void ios_wdm_passthru_stub(void);
extern void ios_wdm_preserve_stub(void);
extern int nt5_read_lba(ULONG lba, UCHAR *data_buf);
extern int nt5_get_iso_root_cache(UCHAR *pvd_buf, UCHAR *root_buf,
                                  ULONG *root_lba, ULONG *root_size);
extern int nt5_get_iso_pvd_info(ULONG *root_lba, ULONG *root_size);
extern ULONG NTMINI_DDB;


/* ================================================================
 * PART 1: PORT DRIVER REGISTRATION
 *
 * Called once at load time to register with IOS. This sets up our
 * DDB and tells IOS where our AEP handler lives.
 *
 * In the real PDR, this is called from the VxD control procedure's
 * Sys_Dynamic_Device_Init handler (in NTMINI.ASM).
 * ================================================================ */

/* NT5 loader entry point (from NT5LOADER.C) */
extern int nt5_init(int use_primary);

extern void dbg_mark(char c);

static int g_nt5_init_done = 0;
static int g_ios_registered = 0;
static ULONG g_late_dcb_ptr = 0;
static ULONG g_late_delta_to_ior = 0;
static int g_late_dcb_destroyed = 0;
static ULONG g_existing_dcb_ptr = 0;
static int g_wdm_iop_dump_count = 0;
ULONG g_late_next_handler = 0;
static int g_fsd_registered = 0;
static ULONG g_fsd_provider_id = 0;
static ULONG g_fsd_prev_hook_ptr = 0;
static ULONG g_fsd_table[17];
static int g_fsd_table_inited = 0;
static ULONG g_fsd_volume_data[8];
static int g_fsd_search_emitted = 0;
static int g_fsd_hook_installed = 0;
static int g_fsd_hook_pending = 0;
static int g_fsd_log_count = 0;
static int g_ifs_hook_all_log_count = 0;
static ULONG g_fsd_search_ctx_last = 0;
static ULONG g_fsd_search_ctx_alloc_count = 0;
static REAL_CALLDOWN_NODE g_vrp_calldown;
static UCHAR g_dummy_vrp[64];
static int g_late_attach_done = 0;
static int g_late_attach_scheduled = 0;

#define IFSFN_READ       0
#define IFSFN_CLOSE      11
#define IFSFN_OPEN       36
#define MAX_ISO_FILES    8

typedef struct _ISO_FILE_ENTRY {
    int in_use;
    ULONG file_lba;
    ULONG file_size;
    ULONG file_pos;
    ULONG handle;
} ISO_FILE_ENTRY;

static ISO_FILE_ENTRY g_iso_files[MAX_ISO_FILES];
static int g_last_opened_slot = -1;
static UCHAR g_iso_sector_buf[2048];
static UCHAR g_iso_enum_sector_buf[2048];
static int g_iso_pvd_valid = 0;
static ULONG g_iso_root_lba = 0;
static ULONG g_iso_root_size = 0;
static int g_iso_root_cached = 0;
static ULONG g_search_dir_offset = 0;
static int g_search_active = 0;
static ULONG g_fsd_find_misc[9];

typedef int (__cdecl *PFN_IFS_FUNC)(int fn, int drive, int resType, int cpid, ULONG pir);

static int __cdecl ntmini_fsd_mount_probe(ULONG pir);
static int __cdecl ntmini_fsd_stub_common(char slot, int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_stub_1(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_stub_2(ULONG pir);
static int __cdecl ntmini_fsd_file_attributes(ULONG arg0, int drive, int resType, int cpid, ULONG arg4);
static int __cdecl ntmini_fsd_stub_3(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_fileinfo_diag(ULONG pir);
static int __cdecl ntmini_fsd_stub_4(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_stub_5(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_stub_6(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_stub_7(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_stub_8(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_stub_9(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_stub_A(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_getdiskinfo(int fn, int drive, int resType, int cpid, ULONG pir);
int __cdecl ntmini_gdi31_callback(ULONG pir);
int __cdecl ntmini_gdi_error5_callback(ULONG pir);
static volatile ULONG g_gdi_callback_keepalive[2] = {
    (ULONG)ntmini_gdi31_callback,
    (ULONG)ntmini_gdi_error5_callback
};
static int __cdecl ntmini_fsd_open(int fn, int drive, int resType, int cpid, ULONG pir);
static int __cdecl ntmini_fsd_open_onearg(ULONG pir);
static int __cdecl ntmini_fsd_slot15_search_fail(ULONG pir);
static int __cdecl ntmini_fsd_handle_fail(ULONG pir);
static int __cdecl ntmini_fsd_findclose(ULONG pir);
static int __cdecl ntmini_fsd_search(ULONG pir);
static int __cdecl ntmini_fsd_shutdown(ULONG pir);
int __cdecl ntmini_ifs_hook(ULONG pfnPrev, int fn, int drive, int resType, int cpid, ULONG pir);
void __cdecl ntmini_install_fsd_hook_timer(void);
void __cdecl ios_schedule_late_cdrom_attach_probe(void);
static int iso_namecmp(const char *a, ULONG alen, const char *b, ULONG blen);
static int iso9660_read_pvd(void);
static int iso9660_prefetch_root(void);
static int iso9660_find_file(ULONG dir_lba, ULONG dir_size, const char *name,
                             ULONG name_len, ULONG *out_lba, ULONG *out_size);
static int iso9660_enum_dir(ULONG dir_lba, ULONG dir_size, int index,
                            char *out_name, ULONG *out_name_len,
                            ULONG *out_lba, ULONG *out_size, UCHAR *out_flags);
static int iso9660_read_file(ULONG file_lba, ULONG file_size, ULONG offset,
                             UCHAR *buffer, ULONG count);

static int __cdecl ntmini_fsd_stub_common(char slot, int fn, int drive, int resType, int cpid, ULONG pir)
{
    (void)resType;
    (void)cpid;
    dbg_mark('d');
    dbg_mark(slot);
    dbg_mark('<');
    ios_dbg_hex32((ULONG)fn);
    ios_dbg_hex8((UCHAR)drive);
    ios_dbg_hex32(pir);
    dbg_mark('>');
    return -1;
}

#define DEFINE_FSD_STUB(name, marker) \
static int __cdecl ntmini_fsd_stub_##name(int fn, int drive, int resType, int cpid, ULONG pir) \
{ \
    return ntmini_fsd_stub_common(marker, fn, drive, resType, cpid, pir); \
}

DEFINE_FSD_STUB(1, '1')
static int __cdecl ntmini_fsd_stub_2(ULONG pir)
{
    ULONG i;
    ULONG *pi;

    dbg_mark('D');
    dbg_mark('2');
    dbg_mark('E');
    if (!is_ring0_ptr(pir)) {
        dbg_mark('!');
        return -1;
    }

    pi = (ULONG *)pir;
    dbg_mark('[');
    for (i = 0; i < 16; i++) {
        ios_dbg_hex32(pi[i]);
    }
    dbg_mark(']');
    if (((UCHAR *)pir)[4] == 2 && pi[4] == (ULONG)(g_fsd_table + 1)) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
        dbg_mark('R');
        return 0;
    }
    *(USHORT *)((UCHAR *)pir + 0x1A) = 5;
    g_search_dir_offset = 0;
    dbg_mark('<');
    ios_dbg_hex32(pir);
    dbg_mark('>');
    dbg_mark('e');
    return 5;
}
DEFINE_FSD_STUB(3, '3')
static int __cdecl ntmini_fsd_fileinfo_diag(ULONG pir)
{
    ULONG *pi;
    ULONG ppath_addr;
    char filename[64];
    int nc;
    int fi;

    dbg_mark('V');
    dbg_mark('4');
    if (!is_ring0_ptr(pir)) {
        dbg_mark('!');
        return 5;
    }

    pi = (ULONG *)pir;
    dbg_mark('<');
    ios_dbg_hex32(pir);
    ppath_addr = pi[3];
    ios_dbg_hex32(ppath_addr);

    nc = 0;
    if (is_ring0_ptr(ppath_addr)) {
        UCHAR *pp;
        USHORT elem_len;

        pp = (UCHAR *)ppath_addr;
        elem_len = *(USHORT *)(pp + 4);
        if (elem_len > 0 && elem_len <= 126) {
            USHORT *uc;

            uc = (USHORT *)(pp + 6);
            nc = elem_len / 2;
            if (nc > 63) {
                nc = 63;
            }
            for (fi = 0; fi < nc; fi++) {
                filename[fi] = (char)(uc[fi] & 0xFF);
            }
            filename[nc] = 0;
            while (nc > 0 && filename[nc - 1] == 0) {
                nc--;
            }
        } else {
            nc = 0;
        }
    }

    ios_dbg_hex8((UCHAR)nc);
    dbg_mark('>');

    if (nc != 0 && (iso_namecmp(filename, (ULONG)nc, "README.TXT", 10) ||
                    iso_namecmp(filename, (ULONG)nc, "README", 6))) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
        dbg_mark('M');
        ios_dbg_hex8((UCHAR)nc);
        dbg_mark('v');
        return 0;
    }

    *(USHORT *)((UCHAR *)pir + 0x1A) = 5;
    dbg_mark('N');
    return 5;
}

static int __cdecl ntmini_fsd_stub_4(int fn, int drive, int resType, int cpid, ULONG pir)
{
    (void)resType;
    (void)cpid;
    dbg_mark('d');
    dbg_mark('4');
    dbg_mark('<');
    ios_dbg_hex32((ULONG)fn);
    ios_dbg_hex8((UCHAR)drive);
    ios_dbg_hex32(pir);
    dbg_mark('>');
    if (is_ring0_ptr(pir)) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
    }
    return 0;
}
DEFINE_FSD_STUB(5, '5')
DEFINE_FSD_STUB(6, '6')
DEFINE_FSD_STUB(7, '7')
static int __cdecl ntmini_fsd_stub_8(int arg0, int drive, int resType, int cpid, ULONG arg4)
{
    ULONG pir;

    (void)drive;
    (void)resType;
    (void)cpid;

    pir = is_ring0_ptr((ULONG)arg0) ? (ULONG)arg0 : arg4;
    dbg_mark('d');
    dbg_mark('8');
    dbg_mark('<');
    ios_dbg_hex32((ULONG)arg0);
    ios_dbg_hex32(pir);
    dbg_mark('>');
    return 5;
}
DEFINE_FSD_STUB(9, '9')
static int __cdecl ntmini_fsd_shutdown(ULONG pir)
{
    dbg_mark('K');
    if (is_ring0_ptr(pir)) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
    }
    return 0;
}

static int __cdecl ntmini_fsd_extra_ok(int fn, int drive, int resType, int cpid, ULONG pir)
{
    dbg_mark('X');
    dbg_mark('(');
    ios_dbg_hex32((ULONG)fn);
    ios_dbg_hex32((ULONG)drive);
    ios_dbg_hex32((ULONG)resType);
    ios_dbg_hex32((ULONG)cpid);
    ios_dbg_hex32(pir);
    dbg_mark(')');
    if (is_ring0_ptr(pir)) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 5;
    }
    dbg_mark('x');
    return 5;
}

static int __cdecl ntmini_fsd_extra12_onearg(ULONG pir)
{
    USHORT status;

    dbg_mark('Y');
    dbg_mark('2');
    dbg_mark('<');
    ios_dbg_hex32(pir);
    dbg_mark('>');
    if (is_ring0_ptr(pir)) {
        if (*(USHORT *)((UCHAR *)pir + 0x18) == 2) {
            *(USHORT *)((UCHAR *)pir + 0x1A) = 0x12;
            dbg_mark('S');
            return 0x12;
        }
        status = *(USHORT *)((UCHAR *)pir + 0x1A);
        ios_dbg_hex16(status);
        return (int)status;
    }
    return 0;
}

static int __cdecl ntmini_fsd_extra14_onearg(ULONG pir)
{
    USHORT status;

    dbg_mark('Y');
    dbg_mark('4');
    dbg_mark('<');
    ios_dbg_hex32(pir);
    dbg_mark('>');
    if (is_ring0_ptr(pir)) {
        status = *(USHORT *)((UCHAR *)pir + 0x1A);
        ios_dbg_hex16(status);
        return (int)status;
    }
    return 0;
}

static int __cdecl ntmini_fsd_stub_A(int arg0, int drive, int resType, int cpid, ULONG arg4)
{
    ULONG pir;

    (void)drive;
    (void)resType;
    (void)cpid;

    dbg_mark('E');
    dbg_mark('A');
    pir = is_ring0_ptr((ULONG)arg0) ? (ULONG)arg0 : arg4;
    if (is_ring0_ptr(pir)) {
        ULONG *pi;
        ULONG i;
        ULONG candidates[5];
        UCHAR labels[5];

        pi = (ULONG *)pir;
        dbg_mark('<');
        ios_dbg_hex32(pir);
        for (i = 0; i < 16; i++) {
            ios_dbg_hex32(pi[i]);
        }
        dbg_mark('>');
        candidates[0] = pi[3];  labels[0] = '3';
        candidates[1] = pi[4];  labels[1] = '4';
        candidates[2] = pi[5];  labels[2] = '5';
        candidates[3] = pi[10]; labels[3] = 'A';
        candidates[4] = pi[11]; labels[4] = 'B';
        for (i = 0; i < 5; i++) {
            if (is_ring0_ptr(candidates[i])) {
                ULONG j;
                UCHAR *pb;

                pb = (UCHAR *)candidates[i];
                dbg_mark('|');
                dbg_mark((char)labels[i]);
                dbg_mark('=');
                ios_dbg_hex32(candidates[i]);
                dbg_mark(':');
                for (j = 0; j < 32; j++) {
                    ios_dbg_hex8(pb[j]);
                }
                dbg_mark(';');
            }
        }
        pi[4] = 0x12;
        dbg_mark('N');
        return 0x12;  /* ERROR_NO_MORE_FILES */
        if (!g_fsd_search_emitted && is_ring0_ptr(pi[5])) {
            UCHAR *ob;
            ULONG z;

            ob = (UCHAR *)pi[5];
            for (z = 0; z < 0x130; z++) {
                ob[z] = 0;
            }
            *(ULONG *)(ob + 0x00) = 0x00000001UL;  /* FILE_ATTRIBUTE_READONLY */
            *(ULONG *)(ob + 0x1C) = 0;
            *(ULONG *)(ob + 0x20) = 19;
            ob[0x2C] = 'R';
            ob[0x2D] = 'E';
            ob[0x2E] = 'A';
            ob[0x2F] = 'D';
            ob[0x30] = 'M';
            ob[0x31] = 'E';
            ob[0x32] = '.';
            ob[0x33] = 'T';
            ob[0x34] = 'X';
            ob[0x35] = 'T';
            ob[0x36] = 0;
            if (is_ring0_ptr(pi[4])) {
                UCHAR *dta;

                dta = (UCHAR *)pi[4];
                dta[21] = 0x01;  /* FILE_ATTRIBUTE_READONLY */
                dta[22] = 0x00;  /* DOS time */
                dta[23] = 0x00;
                dta[24] = 0x21;  /* DOS date: 1980-01-01 */
                dta[25] = 0x00;
                dta[26] = 19;
                dta[27] = 0;
                dta[28] = 0;
                dta[29] = 0;
                dta[30] = 'R';
                dta[31] = 'E';
                dta[32] = 'A';
                dta[33] = 'D';
                dta[34] = 'M';
                dta[35] = 'E';
                dta[36] = '.';
                dta[37] = 'T';
                dta[38] = 'X';
                dta[39] = 'T';
                dta[40] = 0;
                dbg_mark('4');
            }
            g_fsd_search_emitted = 1;
            dbg_mark('R');
            return 0;
        }
        dbg_mark('N');
        return 0x12;  /* ERROR_NO_MORE_FILES */
        if (is_ring0_ptr(pi[4])) {
            UCHAR *dta;

            dbg_mark('x');
            return -1;

            dta = (UCHAR *)pi[4];
            dta[21] = 0x01;  /* read-only */
            dta[22] = 0;
            dta[23] = 0;
            dta[24] = 0;
            dta[25] = 0;
            dta[26] = 19;
            dta[27] = 0;
            dta[28] = 0;
            dta[29] = 0;
            dta[30] = 'R';
            dta[31] = 'E';
            dta[32] = 'A';
            dta[33] = 'D';
            dta[34] = 'M';
            dta[35] = 'E';
            dta[36] = '.';
            dta[37] = 'T';
            dta[38] = 'X';
            dta[39] = 'T';
            dta[40] = 0;
            g_fsd_search_emitted = 1;
            dbg_mark('r');
        } else {
            dbg_mark('n');
            return -1;
        }
    } else {
        dbg_mark('!');
        return -1;
    }
    return 0;
}

static int __cdecl ntmini_fsd_file_attributes(ULONG arg0, int drive, int resType, int cpid, ULONG arg4)
{
    ULONG pir;
    ULONG *pi;
    ULONG i;
    ULONG path_ptr;
    int len;
    int start;
    int nc;
    int fi;
    char filename[64];
    ULONG attr;

    (void)drive;
    (void)resType;
    (void)cpid;

    dbg_mark('A');
    pir = is_ring0_ptr(arg0) ? arg0 : arg4;
    if (!is_ring0_ptr(pir)) {
        dbg_mark('!');
        return -1;
    }

    pi = (ULONG *)pir;
    dbg_mark('<');
    ios_dbg_hex32(pir);
    for (i = 0; i < 16; i++) {
        ios_dbg_hex32(pi[i]);
    }
    dbg_mark('>');

    attr = 0x00000001UL;  /* FILE_ATTRIBUTE_READONLY */
    path_ptr = pi[11];
    nc = 0;
    if (is_ring0_ptr(path_ptr)) {
        USHORT *uc;

        uc = (USHORT *)path_ptr;
        len = 0;
        while (len < 63 && uc[len] != 0) {
            len++;
        }
        start = 0;
        if (len >= 3 && uc[1] == ':' && (uc[2] == '\\' || uc[2] == '/')) {
            start = 3;
        } else if (len >= 1 && (uc[0] == '\\' || uc[0] == '/')) {
            start = 1;
        }
        for (fi = start; fi < len && nc < 63; fi++) {
            filename[nc++] = (char)(uc[fi] & 0xFF);
        }
        while (nc > 0 && (filename[nc - 1] == '\\' || filename[nc - 1] == '/')) {
            nc--;
        }
    }
    filename[nc] = 0;
    if (nc == 0) {
        attr = 0x00000011UL;  /* FILE_ATTRIBUTE_READONLY | DIRECTORY */
    } else if (!(iso_namecmp(filename, (ULONG)nc, "README.TXT", 10) ||
                 iso_namecmp(filename, (ULONG)nc, "README", 6))) {
        ULONG file_lba;
        ULONG file_size;

        if (iso9660_find_file(g_iso_root_lba, g_iso_root_size,
                              filename, (ULONG)nc,
                              &file_lba, &file_size) != 0) {
            pi[0] = 0xFFFFFFFFUL;
            *(USHORT *)((UCHAR *)pir + 0x1A) = 2;
            dbg_mark('n');
            return 2;
        }
    }
    pi[0] = attr;
    *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
    dbg_mark('a');
    return 0;
}

static int iso_namecmp(const char *a, ULONG alen, const char *b, ULONG blen)
{
    ULONG i;

    if (alen != blen) {
        return 0;
    }

    for (i = 0; i < alen; i++) {
        char ca;
        char cb;

        ca = a[i];
        cb = b[i];
        if (ca >= 'a' && ca <= 'z') {
            ca = (char)(ca - 32);
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = (char)(cb - 32);
        }
        if (ca != cb) {
            return 0;
        }
    }

    return 1;
}

static int iso9660_read_pvd(void)
{
    int r;

    r = nt5_read_lba(16, g_iso_sector_buf);
    if (r != 0) {
        dbg_mark('o');
        dbg_mark('p');
        ios_dbg_hex32((ULONG)r);
        return -1;
    }

    if (g_iso_sector_buf[0] != 1 ||
        g_iso_sector_buf[1] != 'C' ||
        g_iso_sector_buf[2] != 'D' ||
        g_iso_sector_buf[3] != '0' ||
        g_iso_sector_buf[4] != '0' ||
        g_iso_sector_buf[5] != '1') {
        dbg_mark('o');
        dbg_mark('!');
        return -2;
    }

    g_iso_root_lba = *(ULONG *)(g_iso_sector_buf + 156 + 2);
    g_iso_root_size = *(ULONG *)(g_iso_sector_buf + 156 + 10);
    g_iso_pvd_valid = 1;
    dbg_mark('O');
    dbg_mark('{');
    ios_dbg_hex32(g_iso_root_lba);
    ios_dbg_hex32(g_iso_root_size);
    dbg_mark('}');
    return 0;
}

static int iso9660_prefetch_root(void)
{
    int r;

    r = nt5_get_iso_root_cache(g_iso_sector_buf, g_iso_enum_sector_buf,
                               &g_iso_root_lba, &g_iso_root_size);
    if (r != 0) {
        dbg_mark('o');
        dbg_mark('c');
        ios_dbg_hex32((ULONG)r);
        return -1;
    }

    g_iso_pvd_valid = 1;
    g_iso_root_cached = 1;
    dbg_mark('O');
    dbg_mark('C');
    return 0;
}

static int iso9660_find_file(ULONG dir_lba, ULONG dir_size, const char *name,
                             ULONG name_len, ULONG *out_lba, ULONG *out_size)
{
    ULONG sectors;
    ULONG s;

    sectors = (dir_size + 2047) / 2048;
    for (s = 0; s < sectors && s < 16; s++) {
        ULONG pos;
        int r;
        UCHAR *buf;

        if (dir_lba == g_iso_root_lba && g_iso_root_cached && s == 0) {
            buf = g_iso_enum_sector_buf;
        } else {
            buf = g_iso_sector_buf;
            r = nt5_read_lba(dir_lba + s, buf);
            if (r != 0) {
                return -1;
            }
        }

        pos = 0;
        while (pos + 33 < 2048) {
            UCHAR rec_len;
            UCHAR id_len;
            char *id;
            ULONG id_clean;

            rec_len = buf[pos];
            if (rec_len < 34) {
                break;
            }
            if (pos + rec_len > 2048) {
                break;
            }

            id_len = buf[pos + 32];
            if (pos + 33 + id_len > 2048) {
                break;
            }
            id = (char *)(buf + pos + 33);
            id_clean = id_len;
            if (id_clean > 2 && id[id_clean - 2] == ';') {
                id_clean -= 2;
            }
            if (id_clean > 1 && id[id_clean - 1] == '.') {
                id_clean -= 1;
            }

            if (iso_namecmp(name, name_len, id, id_clean)) {
                *out_lba = *(ULONG *)(buf + pos + 2);
                *out_size = *(ULONG *)(buf + pos + 10);
                return 0;
            }

            pos += rec_len;
        }
    }

    return -2;
}

static int iso9660_enum_dir(ULONG dir_lba, ULONG dir_size, int index,
                            char *out_name, ULONG *out_name_len,
                            ULONG *out_lba, ULONG *out_size, UCHAR *out_flags)
{
    ULONG sectors;
    ULONG s;
    int file_index;

    sectors = (dir_size + 2047) / 2048;
    file_index = 0;

    for (s = 0; s < sectors && s < 16; s++) {
        ULONG pos;
        int r;

        if (!(dir_lba == g_iso_root_lba && g_iso_root_cached && s == 0)) {
            r = nt5_read_lba(dir_lba + s, g_iso_enum_sector_buf);
            if (r != 0) {
                return -1;
            }
        }

        pos = 0;
        while (pos + 34 <= 2048) {
            UCHAR rec_len;
            UCHAR id_len;
            UCHAR flags;
            char *id;
            ULONG id_clean;

            rec_len = g_iso_enum_sector_buf[pos];
            if (rec_len < 34) {
                break;
            }
            if (pos + rec_len > 2048) {
                break;
            }
            id_len = g_iso_enum_sector_buf[pos + 32];
            if (pos + 33 + id_len > 2048) {
                break;
            }

            id = (char *)(g_iso_enum_sector_buf + pos + 33);
            flags = g_iso_enum_sector_buf[pos + 25];
            if (id_len == 1 && (id[0] == 0x00 || id[0] == 0x01)) {
                pos += rec_len;
                continue;
            }

            if (file_index == index) {
                ULONG k;

                id_clean = id_len;
                if (id_clean > 2 && id[id_clean - 2] == ';') {
                    id_clean -= 2;
                }
                if (id_clean > 1 && id[id_clean - 1] == '.') {
                    id_clean -= 1;
                }
                if (id_clean > 63) {
                    id_clean = 63;
                }
                for (k = 0; k < id_clean; k++) {
                    out_name[k] = id[k];
                }
                out_name[id_clean] = 0;
                *out_name_len = id_clean;
                *out_lba = *(ULONG *)(g_iso_enum_sector_buf + pos + 2);
                *out_size = *(ULONG *)(g_iso_enum_sector_buf + pos + 10);
                *out_flags = flags;
                return 0;
            }

            file_index++;
            pos += rec_len;
        }
    }

    return -2;
}

static int iso9660_read_file(ULONG file_lba, ULONG file_size, ULONG offset,
                             UCHAR *buffer, ULONG count)
{
    ULONG bytes_read;

    if (offset >= file_size) {
        return 0;
    }
    if (offset + count > file_size) {
        count = file_size - offset;
    }

    bytes_read = 0;
    while (count > 0) {
        ULONG sect;
        ULONG off_in_sect;
        ULONG chunk;
        ULONG j;
        int r;

        sect = file_lba + (offset / 2048);
        off_in_sect = offset % 2048;
        chunk = 2048 - off_in_sect;
        if (chunk > count) {
            chunk = count;
        }

        r = nt5_read_lba(sect, g_iso_sector_buf);
        if (r != 0) {
            return -1;
        }

        for (j = 0; j < chunk; j++) {
            buffer[bytes_read + j] = g_iso_sector_buf[off_in_sect + j];
        }

        bytes_read += chunk;
        offset += chunk;
        count -= chunk;
    }

    return (int)bytes_read;
}

static int __cdecl ntmini_fsd_getdiskinfo(int fn, int drive, int resType, int cpid, ULONG pir)
{
    (void)fn;
    (void)drive;
    (void)resType;
    (void)cpid;

    dbg_mark('G');
    return -1;
    if (is_ring0_ptr(pir)) {
        ULONG *pi;
        UCHAR subfn;

        pi = (ULONG *)pir;
        subfn = (UCHAR)(pi[6] & 0xFF);
        dbg_mark('Z');
        dbg_mark('Q');
        dbg_mark('<');
        ios_dbg_hex32(pir);
        ios_dbg_hex8(subfn);
        dbg_mark('>');
        if (subfn == 1) {
            dbg_mark('1');
            dbg_mark('T');
            if (is_ring0_ptr(pi[4])) {
                ULONG *out;
                out = (ULONG *)pi[4];
                out[0] = 0x000084C1UL;
                out[1] = 0x00008512UL;
                out[2] = 0x00000000UL;
                dbg_mark('1');
                dbg_mark('X');
                dbg_mark('x');
            }
            *((UCHAR *)pir + 0x04) = 0;
            *(USHORT *)((UCHAR *)pir + 0x18) = 1;
            *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
            *(ULONG *)pir = 1;
            *(ULONG *)((UCHAR *)pir + 0x24) = 0;
            *(ULONG *)((UCHAR *)pir + 0x20) = (ULONG)g_fsd_table;
            *(ULONG *)((UCHAR *)pir + 0x28) = 0;
            dbg_mark('1');
            dbg_mark('E');
            dbg_mark('e');
            dbg_mark('t');
            return 0;
        }
        if (subfn == 0 && is_ring0_ptr(pi[5])) {
            USHORT *dp;

            dp = (USHORT *)pi[5];
            dp[0] = 1;
            dp[1] = 2048;
            dp[2] = 0;
            dp[3] = 0xFFFF;
        }
    } else {
        dbg_mark('!');
    }

    return -1;
}

int __cdecl ntmini_gdi31_callback(ULONG pir)
{
    dbg_mark('Y');
    dbg_mark('1');
    dbg_mark('<');
    ios_dbg_hex32(pir);
    dbg_mark('>');
    if (is_ring0_ptr(pir)) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
    }
    return 0;
}

int __cdecl ntmini_gdi_error5_callback(ULONG pir)
{
    dbg_mark('Y');
    dbg_mark('5');
    dbg_mark('<');
    ios_dbg_hex32(pir);
    dbg_mark('>');
    if (is_ring0_ptr(pir)) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 5;
    }
    return 5;
}

static int __cdecl ntmini_fsd_open(int fn, int drive, int resType, int cpid, ULONG pir)
{
    ULONG *pi;
    ULONG ppath_addr;
    char filename[64];
    int nc;
    int fi;
    ULONG file_lba;
    ULONG file_size;
    ULONG sft_result;

    (void)fn;
    (void)drive;
    (void)resType;
    (void)cpid;

    dbg_mark('U');
    if (!is_ring0_ptr(pir)) {
        if (is_ring0_ptr((ULONG)fn)) {
            dbg_mark('~');
            pir = (ULONG)fn;
        } else {
            dbg_mark('!');
            dbg_mark('<');
            ios_dbg_hex32((ULONG)fn);
            ios_dbg_hex32((ULONG)drive);
            ios_dbg_hex32((ULONG)resType);
            ios_dbg_hex32((ULONG)cpid);
            ios_dbg_hex32(pir);
            dbg_mark('>');
            return 2;
        }
    }
    pi = (ULONG *)pir;

    ppath_addr = pi[3];
    if (!is_ring0_ptr(ppath_addr)) {
        pi[4] = 2;
        dbg_mark('?');
        dbg_mark('<');
        ios_dbg_hex32(pir);
        ios_dbg_hex32(ppath_addr);
        ios_dbg_hex32(pi[0]);
        ios_dbg_hex32(pi[1]);
        ios_dbg_hex32(pi[2]);
        dbg_mark('>');
        return 2;
    }

    {
        UCHAR *pp;
        USHORT elem_len;

        pp = (UCHAR *)ppath_addr;
        elem_len = *(USHORT *)(pp + 4);
        nc = 0;
        if (elem_len > 0 && elem_len <= 126) {
            USHORT *uc;

            uc = (USHORT *)(pp + 6);
            nc = elem_len / 2;
            if (nc > 63) {
                nc = 63;
            }
            for (fi = 0; fi < nc; fi++) {
                filename[fi] = (char)(uc[fi] & 0xFF);
            }
            filename[nc] = 0;
            while (nc > 0 && filename[nc - 1] == 0) {
                nc--;
            }
        } else {
            nc = 0;
        }
    }

    dbg_mark('<');
    ios_dbg_hex32(pir);
    ios_dbg_hex32(ppath_addr);
    ios_dbg_hex8((UCHAR)nc);
    dbg_mark('>');

    if (nc == 0 ||
               iso9660_find_file(g_iso_root_lba, g_iso_root_size,
                                 filename, (ULONG)nc, &file_lba, &file_size) != 0) {
        pi[4] = 2;
        dbg_mark('n');
        return 2;
    }

    {
        int slot;

        for (slot = 0; slot < MAX_ISO_FILES; slot++) {
            if (!g_iso_files[slot].in_use) {
                break;
            }
        }
        if (slot >= MAX_ISO_FILES) {
            slot = 0;
        }

        g_iso_files[slot].in_use = 1;
        g_iso_files[slot].file_lba = file_lba;
        g_iso_files[slot].file_size = file_size;
        g_iso_files[slot].file_pos = 0;
        g_iso_files[slot].handle = 0;
        g_last_opened_slot = slot;
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
        dbg_mark('F');
        sft_result = IFSMgr_FSDAttachSFT_Wrapper(pir);
        ios_dbg_hex32(sft_result);
        dbg_mark('f');
        if (sft_result != 0) {
            return (int)sft_result;
        }
        pi[4] = 0;
        dbg_mark('u');
        ios_dbg_hex8((UCHAR)slot);
        ios_dbg_hex32(file_lba);
        ios_dbg_hex32(file_size);
        dbg_mark('H');
        ios_dbg_hex32(0);
        if (!g_fsd_hook_installed) {
            ULONG prev_hook;

            dbg_mark('L');
            prev_hook = IFSMgr_InstallFSHook_Wrapper((ULONG)ntmini_ifs_hook);
            g_fsd_prev_hook_ptr = prev_hook;
            ios_dbg_hex32(prev_hook);
            g_fsd_hook_installed = 1;
            dbg_mark('l');
        }
    }

    return 0;
}

static int __cdecl ntmini_fsd_open_onearg(ULONG pir)
{
    dbg_mark('O');
    dbg_mark('1');
    return ntmini_fsd_open(0, 4, 0, 0, pir);
}

static int __cdecl ntmini_fsd_slot15_search_fail(ULONG pir)
{
    dbg_mark('S');
    dbg_mark('1');
    dbg_mark('5');
    dbg_mark('<');
    ios_dbg_hex32(pir);
    if (is_ring0_ptr(pir)) {
        ULONG *pi;

        pi = (ULONG *)pir;
        ios_dbg_hex32(pi[0]);
        ios_dbg_hex32(pi[1]);
        ios_dbg_hex32(pi[2]);
        ios_dbg_hex32(pi[3]);
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0x12;
    }
    dbg_mark('>');
    return 0x12;
}

static int __cdecl ntmini_fsd_handle_fail(ULONG pir)
{
    dbg_mark('H');
    dbg_mark('F');
    dbg_mark('<');
    ios_dbg_hex32(pir);
    if (is_ring0_ptr(pir)) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 5;
    }
    dbg_mark('>');
    return 5;
}

static int __cdecl ntmini_fsd_findclose(ULONG pir)
{
    dbg_mark('F');
    dbg_mark('C');
    dbg_mark('<');
    ios_dbg_hex32(pir);
    if (is_ring0_ptr(pir)) {
        ULONG *pi;
        ULONG k;

        pi = (ULONG *)pir;
        for (k = 0; k < 12; k++) {
            ios_dbg_hex32(pi[k]);
        }
        if (is_ring0_ptr(pi[8])) {
            UCHAR *ctx;

            ctx = (UCHAR *)pi[8];
            if (*(USHORT *)(ctx + 0x08) == 1 &&
                ctx[0x0B] == 2 &&
                ctx[0x3C] == 'R' &&
                ctx[0x3D] == 'E') {
                ULONG rh;

                dbg_mark('Q');
                ios_dbg_hex32((ULONG)ctx);
                rh = *(ULONG *)(ctx + 0x10);
                if (is_ring0_ptr(rh)) {
                    USHORT *ref_count;

                    ref_count = (USHORT *)((UCHAR *)rh + 0x1C);
                    if (*ref_count != 0) {
                        (*ref_count)--;
                    }
                    dbg_mark('N');
                    ios_dbg_hex16(*ref_count);
                }
                if (g_fsd_search_ctx_last == (ULONG)ctx) {
                    g_fsd_search_ctx_last = 0;
                }
                if (g_fsd_search_ctx_alloc_count != 0) {
                    g_fsd_search_ctx_alloc_count--;
                }
                VxD_HeapFree(ctx, 0);
            }
        }
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
    }
    dbg_mark('>');
    return 0;
}

static void ntmini_fill_find_output(UCHAR *out, const char *name,
                                    ULONG name_len, ULONG size, UCHAR flags)
{
    ULONG k;
    ULONG attr;

    if (name_len > 63) {
        name_len = 63;
    }

    for (k = 0; k < 320; k++) {
        out[k] = 0;
    }

    attr = (flags & 0x02) ? 0x00000010UL : 0x00000001UL;
    *(ULONG *)(out + 0x00) = attr;
    *(ULONG *)(out + 0x1C) = 0;
    *(ULONG *)(out + 0x20) = size;
    for (k = 0; k < name_len; k++) {
        ((USHORT *)(out + 0x2C))[k] = (USHORT)name[k];
        out[0x130 + k] = (UCHAR)name[k];
    }
    ((USHORT *)(out + 0x2C))[name_len] = 0;
    out[0x130 + name_len] = 0;
}

static int __cdecl ntmini_fsd_search(ULONG pir)
{
    ULONG req;
    ULONG dump_i;
    USHORT sub;
    ULONG *pi;

    dbg_mark('Z');
    if (is_ring0_ptr(pir)) {
        ULONG out_ptr;
        UCHAR *out;
        ULONG k;
        ULONG *zpi;
        ULONG search_attr;
        char pattern[64];
        int pattern_len;
        int path_len;
        int path_start;
        int fi;
        int wildcard;
        ULONG file_lba;
        ULONG file_size;
        char found_name[64];
        ULONG found_name_len;
        UCHAR found_flags;

        zpi = (ULONG *)pir;
        search_attr = zpi[0] & 0xFFUL;
        dbg_mark('[');
        ios_dbg_hex32(pir);
        for (k = 0; k < 16; k++) {
            ios_dbg_hex32(zpi[k]);
        }
        dbg_mark(']');
        if (is_ring0_ptr(zpi[7])) {
            ULONG *p7;

            p7 = (ULONG *)zpi[7];
            dbg_mark('7');
            dbg_mark('[');
            ios_dbg_hex32(zpi[7]);
            for (k = 0; k < 12; k++) {
                ios_dbg_hex32(p7[k]);
            }
            dbg_mark(']');
        }
        if (is_ring0_ptr(zpi[5])) {
            ULONG *p5;

            p5 = (ULONG *)zpi[5];
            dbg_mark('5');
            dbg_mark('[');
            ios_dbg_hex32(zpi[5]);
            for (k = 0; k < 12; k++) {
                ios_dbg_hex32(p5[k]);
            }
            dbg_mark(']');
        }
        if (is_ring0_ptr(zpi[4])) {
            ULONG *p4;

            p4 = (ULONG *)zpi[4];
            dbg_mark('4');
            dbg_mark('[');
            ios_dbg_hex32(zpi[4]);
            for (k = 0; k < 8; k++) {
                ios_dbg_hex32(p4[k]);
            }
            dbg_mark(']');
        }

        if (is_ring0_ptr(zpi[8])) {
            UCHAR *ctx;

            ctx = (UCHAR *)zpi[8];
            out_ptr = *(ULONG *)((UCHAR *)pir + 0x14);
            if (ctx[0x09] != 0 && is_ring0_ptr(out_ptr) &&
                iso9660_enum_dir(g_iso_root_lba, g_iso_root_size,
                                 (int)ctx[0x0A],
                                 found_name, &found_name_len,
                                 &file_lba, &file_size, &found_flags) == 0) {
                ntmini_fill_find_output((UCHAR *)out_ptr, found_name,
                                        found_name_len, file_size, found_flags);
                ctx[0x0A]++;
                *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
                dbg_mark('X');
                dbg_mark('Y');
                return 0;
            }
            *(USHORT *)((UCHAR *)pir + 0x1A) = 0x12;
            dbg_mark('X');
            dbg_mark('N');
            ios_dbg_hex32(zpi[8]);
            return 0x12;
        }

        sub = *(USHORT *)((UCHAR *)pir + 0x18);
        if (sub == 2) {
            *(USHORT *)((UCHAR *)pir + 0x1A) = 0x12;
            dbg_mark('L');
            dbg_mark('N');
            return 0x12;
        }

        pattern_len = 0;
        if (is_ring0_ptr(zpi[11])) {
            USHORT *uc;

            uc = (USHORT *)zpi[11];
            path_len = 0;
            while (path_len < 63 && uc[path_len] != 0) {
                path_len++;
            }
            path_start = 0;
            if (path_len >= 3 && uc[1] == ':' &&
                (uc[2] == '\\' || uc[2] == '/')) {
                path_start = 3;
            } else if (path_len >= 1 && (uc[0] == '\\' || uc[0] == '/')) {
                path_start = 1;
            }
            for (fi = path_start; fi < path_len && pattern_len < 63; fi++) {
                pattern[pattern_len++] = (char)(uc[fi] & 0xFF);
            }
            while (pattern_len > 0 &&
                   (pattern[pattern_len - 1] == '\\' ||
                    pattern[pattern_len - 1] == '/')) {
                pattern_len--;
            }
        }
        pattern[pattern_len] = 0;
        wildcard = (pattern_len == 0 ||
                    iso_namecmp(pattern, (ULONG)pattern_len, "*.*", 3) ||
                    iso_namecmp(pattern, (ULONG)pattern_len, "*", 1) ||
                    iso_namecmp(pattern, (ULONG)pattern_len, "*.TXT", 5));
        file_lba = 0;
        file_size = 0;
        if (search_attr != 0x08UL && !wildcard &&
            iso9660_find_file(g_iso_root_lba, g_iso_root_size,
                              pattern, (ULONG)pattern_len,
                              &file_lba, &file_size) != 0) {
            *(USHORT *)((UCHAR *)pir + 0x1A) = 2;
            dbg_mark('N');
            return 2;
        }

        out_ptr = *(ULONG *)((UCHAR *)pir + 0x14);
        dbg_mark('O');
        ios_dbg_hex32(out_ptr);
        if (is_ring0_ptr(out_ptr)) {
            out = (UCHAR *)out_ptr;
            for (k = 0; k < 320; k++) {
                out[k] = 0;
            }
            if (search_attr == 0x08UL) {
                static const char vol_label[11] = {
                    'N','T','M','I','N','I',' ','C','D',' ',' '
                };

                *(ULONG *)(out + 0x00) = 0x00000008UL;
                *(ULONG *)(out + 0x1C) = 0;
                *(ULONG *)(out + 0x20) = 0;
                for (k = 0; k < 11; k++) {
                    ((USHORT *)(out + 0x2C))[k] = (USHORT)vol_label[k];
                    out[0x130 + k] = (UCHAR)vol_label[k];
                }
                ((USHORT *)(out + 0x2C))[11] = 0;
                out[0x130 + 11] = 0;
                dbg_mark('A');
                dbg_mark('[');
                for (k = 0; k < 16; k++) {
                    ios_dbg_hex32(((ULONG *)out)[k]);
                }
                dbg_mark(']');
                *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
                g_search_dir_offset = 0;
                dbg_mark('V');
                return 0;
            }
            if (wildcard) {
                if (iso9660_enum_dir(g_iso_root_lba, g_iso_root_size, 0,
                                     found_name, &found_name_len,
                                     &file_lba, &file_size, &found_flags) != 0) {
                    *(USHORT *)((UCHAR *)pir + 0x1A) = 0x12;
                    dbg_mark('N');
                    return 0x12;
                }
            } else {
                found_name_len = (ULONG)pattern_len;
                for (k = 0; k < found_name_len; k++) {
                    found_name[k] = pattern[k];
                }
                found_name[found_name_len] = 0;
                found_flags = 0;
            }
            ntmini_fill_find_output(out, found_name, found_name_len,
                                    file_size, found_flags);
            dbg_mark('A');
            dbg_mark('[');
            for (k = 0; k < 16; k++) {
                ios_dbg_hex32(((ULONG *)out)[k]);
            }
            dbg_mark(']');
            {
                UCHAR *ctx;
                ULONG name_len;

                ctx = (UCHAR *)VxD_HeapAllocate(0x100, 1);
                dbg_mark('H');
                ios_dbg_hex32((ULONG)ctx);
                if (is_ring0_ptr((ULONG)ctx)) {
                    g_fsd_search_ctx_last = (ULONG)ctx;
                    g_fsd_search_ctx_alloc_count++;
                    *(ULONG *)(ctx + 0x04) = (ULONG)(ctx + 0x0C);
                    *(USHORT *)(ctx + 0x08) = 1;
                    ctx[0x09] = wildcard ? 1 : 0;
                    ctx[0x0A] = 1;
                    ctx[0x0B] = 2;
                    *(ULONG *)(ctx + 0x10) = zpi[7];
                    if (is_ring0_ptr(zpi[7])) {
                        USHORT *ref_count;

                        ref_count = (USHORT *)((UCHAR *)zpi[7] + 0x1C);
                        (*ref_count)++;
                        dbg_mark('n');
                        ios_dbg_hex16(*ref_count);
                    }
                    name_len = found_name_len;
                    *(USHORT *)(ctx + 0x3A) = (USHORT)name_len;
                    for (k = 0; k < name_len; k++) {
                        ctx[0x3C + k] = (UCHAR)found_name[k];
                    }
                    zpi[8] = (ULONG)ctx;
                    if (is_ring0_ptr(zpi[4])) {
                        ULONG *resume;

                        resume = (ULONG *)zpi[4];
                        resume[0] = (ULONG)ntmini_fsd_search;
                        resume[1] = (ULONG)ntmini_fsd_slot15_search_fail;
                        resume[2] = (ULONG)g_fsd_find_misc;
                        dbg_mark('r');
                        dbg_mark('[');
                        ios_dbg_hex32(zpi[4]);
                        ios_dbg_hex32(resume[0]);
                        ios_dbg_hex32(resume[1]);
                        ios_dbg_hex32(resume[2]);
                        dbg_mark(']');
                    }
                    dbg_mark('h');
                    dbg_mark('[');
                    for (k = 0; k < 16; k++) {
                        ios_dbg_hex32(((ULONG *)ctx)[k]);
                    }
                    dbg_mark(']');
                }
            }
            *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
            g_search_dir_offset = 0;
            dbg_mark('W');
            return 0;
        }
    }

    dbg_mark('N');
    if (is_ring0_ptr(pir)) {
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0x12;
    }
    return 0x12;

    dbg_mark('E');
    req = pir;
    if (!is_ring0_ptr(req)) {
        dbg_mark('!');
        ios_dbg_hex32(pir);
        ios_dbg_hex32(pir);
        return 0x12;
    }

    pi = (ULONG *)req;
    sub = *(USHORT *)((UCHAR *)req + 0x18);
    dbg_mark('<');
    ios_dbg_hex32(req);
    ios_dbg_hex16(sub);
    dbg_mark('>');
    dbg_mark('[');
    for (dump_i = 0; dump_i < 12; dump_i++) {
        ios_dbg_hex32(pi[dump_i]);
    }
    dbg_mark(']');

    if (g_search_dir_offset != 0) {
        *(USHORT *)((UCHAR *)req + 0x1A) = 0x12;
        g_search_active = 0;
        g_search_dir_offset = 0;
        dbg_mark('S');
        dbg_mark('2');
        return 0;
    }

    if (sub == 2) {
        dbg_mark('S');
        dbg_mark('T');
    } else {
        *(USHORT *)((UCHAR *)req + 0x1A) = 0x12;
        g_search_active = 0;
        g_search_dir_offset = 0;
        dbg_mark('S');
        dbg_mark('N');
        return 0;
    }

    if (!g_search_active) {
        g_search_dir_offset = 0;
        g_search_active = 1;
    }

    if (is_ring0_ptr(pi[5])) {
        UCHAR *ob;
        ULONG z;
        ULONG k;

        ob = (UCHAR *)pi[5];
        for (z = 0; z < 48; z++) {
            ob[z] = 0;
        }
        ob[0] = 0x01;
        ob[1] = 0x00;
        ob[2] = 0x00;
        ob[3] = 0x21;
        ob[4] = 0x00;
        ob[5] = 0x13;
        ob[6] = 0x00;
        ob[7] = 0x00;
        ob[8] = 0x00;
        for (k = 0; k < 10; k++) {
            ob[9 + k] = (UCHAR)"README.TXT"[k];
        }
        ob[19] = 0;
        dbg_mark('5');
    }

    if (0 && is_ring0_ptr(pi[4])) {
        UCHAR *dta;
        ULONG dta_ptr;
        ULONG k2;

        dta_ptr = *(ULONG *)((UCHAR *)pi[4] + 0x20);
        dbg_mark('4');
        ios_dbg_hex32(dta_ptr);
        if (!is_ring0_ptr(dta_ptr)) {
            dbg_mark('!');
        } else {
        dta = (UCHAR *)dta_ptr;
        dta[21] = 0x01;
        dta[22] = 0x00;
        dta[23] = 0x00;
        dta[24] = 0x21;
        dta[25] = 0x00;
        dta[26] = 0x13;
        dta[27] = 0x00;
        dta[28] = 0x00;
        dta[29] = 0x00;
        for (k2 = 0; k2 < 10; k2++) {
            dta[30 + k2] = (UCHAR)"README.TXT"[k2];
        }
        dta[30 + k2] = 0;
        dbg_mark('D');
        }
    }

    *(USHORT *)((UCHAR *)req + 0x1A) = 0;
    g_search_dir_offset++;
    dbg_mark('z');
    ios_dbg_hex32(0x00000019UL);
    ios_dbg_hex32(0x00000013UL);

    return 0;
}

void __cdecl ntmini_install_fsd_hook_timer(void)
{
    ULONG prev_hook;

    g_fsd_hook_pending = 0;
    if (g_fsd_hook_installed) {
        return;
    }

    dbg_mark('L');
    prev_hook = IFSMgr_InstallFSHook_Wrapper((ULONG)ntmini_ifs_hook);
    g_fsd_prev_hook_ptr = prev_hook;
    ios_dbg_hex32(prev_hook);
    g_fsd_hook_installed = 1;
    dbg_mark('l');
}

int __cdecl ntmini_ifs_hook(ULONG fsdFnAddr, int fn, int drive, int resType, int cpid, ULONG pir)
{
    (void)resType;
    (void)cpid;

    if (g_fsd_registered && g_ifs_hook_all_log_count < 96) {
        dbg_mark('J');
        dbg_mark('<');
        ios_dbg_hex32(fsdFnAddr);
        ios_dbg_hex32((ULONG)fn);
        ios_dbg_hex8((UCHAR)drive);
        ios_dbg_hex32(pir);
        dbg_mark('>');
        g_ifs_hook_all_log_count++;
    }

    if (drive == 4 && g_fsd_registered) {
        if (g_fsd_log_count < 64) {
            dbg_mark('I');
            dbg_mark('<');
            ios_dbg_hex32(fsdFnAddr);
            ios_dbg_hex32((ULONG)fn);
            ios_dbg_hex32(pir);
            dbg_mark('>');
            if (is_ring0_ptr(pir) && g_fsd_log_count < 8) {
                ULONG *hp;
                int hi;

                hp = (ULONG *)pir;
                dbg_mark('[');
                for (hi = 0; hi < 16; hi++) {
                    ios_dbg_hex32(hp[hi]);
                }
                dbg_mark(']');
            }
            g_fsd_log_count++;
        }

        if (fn == IFSFN_READ ||
            ((fn == IFSFN_CLOSE || fn == 0x0E) &&
             is_ring0_ptr(pir) &&
             ((ULONG *)pir)[0] != 0 &&
             is_ring0_ptr(((ULONG *)pir)[5]))) {
            ULONG *pi;
            ULONG count;
            ULONG buf_ptr;
            ULONG handle;
            int slot;
            int bytes;
            int use_bounce;
            UCHAR *read_buf;
            ULONG copy_result;

            if (!is_ring0_ptr(pir)) {
                return -1;
            }
            pi = (ULONG *)pir;
            count = pi[0];
            buf_ptr = pi[5];
            slot = g_last_opened_slot;
            handle = pi[4];
            if (slot < 0 || slot >= MAX_ISO_FILES ||
                !g_iso_files[slot].in_use ||
                !(is_ring0_ptr(buf_ptr) ||
                  (buf_ptr >= 0x00010000UL && buf_ptr < 0x80000000UL))) {
                dbg_mark('i');
                ios_dbg_hex32(handle);
                ios_dbg_hex32(buf_ptr);
                return -1;
            }
            use_bounce = 0;
            read_buf = (UCHAR *)buf_ptr;
            copy_result = 0;
            if (!is_ring0_ptr(buf_ptr)) {
                use_bounce = 1;
                read_buf = g_iso_read_bounce;
                dbg_mark('j');
                ios_dbg_hex32(buf_ptr);
            }

            if (g_iso_files[slot].file_lba == 0x19UL) {
                static const UCHAR readme_data[19] = {
                    'N','T','M','I','N','I',' ','C','D','-','R','O','M',' ',
                    'T','E','S','T','\n'
                };
                ULONG available;
                ULONG todo;
                ULONG bi;

                available = g_iso_files[slot].file_size - g_iso_files[slot].file_pos;
                todo = count;
                if (todo > available) {
                    todo = available;
                }
                if (todo > 2048UL - g_iso_files[slot].file_pos) {
                    todo = 2048UL - g_iso_files[slot].file_pos;
                }
                for (bi = 0; bi < todo; bi++) {
                    read_buf[bi] = readme_data[g_iso_files[slot].file_pos + bi];
                }
                bytes = (int)todo;
                dbg_mark('C');
            } else {
                bytes = iso9660_read_file(g_iso_files[slot].file_lba,
                                          g_iso_files[slot].file_size,
                                          g_iso_files[slot].file_pos,
                                          read_buf, count);
            }
            if (bytes < 0) {
                dbg_mark('i');
                return -1;
            }
            if (use_bounce && bytes > 0) {
                copy_result = VWIN32_CopyMem_Wrapper((ULONG)g_iso_read_bounce, buf_ptr,
                                                     (ULONG)bytes);
                dbg_mark('J');
                ios_dbg_hex32(copy_result);
            }

            if (bytes == 0 && fn == IFSFN_CLOSE) {
                if (g_last_opened_slot >= 0 && g_last_opened_slot < MAX_ISO_FILES) {
                    g_iso_files[g_last_opened_slot].in_use = 0;
                }
                g_last_opened_slot = -1;
                pi[0] = 0;
                *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
                dbg_mark('q');
                return 0;
            }

            if (fn != 0x0E) {
                g_iso_files[slot].file_pos += (ULONG)bytes;
            }
            pi[0] = (ULONG)bytes;
            *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
            dbg_mark('R');
            ios_dbg_hex32((ULONG)bytes);
            return 0;
        }

        if (fn == IFSFN_CLOSE) {
            if (g_last_opened_slot >= 0 && g_last_opened_slot < MAX_ISO_FILES) {
                g_iso_files[g_last_opened_slot].in_use = 0;
            }
            g_last_opened_slot = -1;
            dbg_mark('Q');
            return 0;
        }

        dbg_mark('N');
        ios_dbg_hex32((ULONG)fn);
        if (fn == 0x28) {
            return 0;
        }
        if (fn == 10) {
            return -1;
        }
        if (g_fsd_prev_hook_ptr) {
            int prev_result;

            dbg_mark('P');
            prev_result = IFSMgr_CallPrevFSHook(g_fsd_prev_hook_ptr, fsdFnAddr,
                                                fn, drive, resType, cpid, pir);
            dbg_mark('p');
            ios_dbg_hex32((ULONG)prev_result);
            return prev_result;
        }
        return -1;
    }

    dbg_mark('n');
    ios_dbg_hex32((ULONG)fn);
    ios_dbg_hex32((ULONG)drive);
    ios_dbg_hex32(pir);
    if (is_ring0_ptr(pir) && g_fsd_log_count < 8) {
        ULONG *hp;
        int hi;

        hp = (ULONG *)pir;
        dbg_mark('[');
        for (hi = 0; hi < 16; hi++) {
            ios_dbg_hex32(hp[hi]);
        }
        dbg_mark(']');
    }
    if (g_fsd_prev_hook_ptr) {
        int prev_result;

        dbg_mark('P');
        prev_result = IFSMgr_CallPrevFSHook(g_fsd_prev_hook_ptr, fsdFnAddr,
                                            fn, drive, resType, cpid, pir);
        dbg_mark('p');
        ios_dbg_hex32((ULONG)prev_result);
        return prev_result;
    }
    return -1;
}

static void init_fsd_probe_table(void)
{
    if (g_fsd_table_inited) {
        return;
    }

    g_fsd_find_misc[0] = 0x0810030AUL;
    g_fsd_find_misc[1] = (ULONG)ntmini_fsd_handle_fail;
    g_fsd_find_misc[2] = (ULONG)ntmini_fsd_findclose;
    g_fsd_find_misc[3] = (ULONG)ntmini_fsd_handle_fail;
    g_fsd_find_misc[4] = (ULONG)ntmini_fsd_handle_fail;
    g_fsd_find_misc[5] = (ULONG)ntmini_fsd_handle_fail;
    g_fsd_find_misc[6] = (ULONG)ntmini_fsd_handle_fail;
    g_fsd_find_misc[7] = (ULONG)ntmini_fsd_handle_fail;
    g_fsd_find_misc[8] = (ULONG)ntmini_fsd_handle_fail;

    g_fsd_table[0] = 0x0F10030AUL;
    g_fsd_table[1] = (ULONG)ntmini_fsd_stub_1;
    g_fsd_table[2] = (ULONG)ntmini_fsd_stub_2;
    g_fsd_table[3] = (ULONG)ntmini_fsd_file_attributes;
    g_fsd_table[4] = (ULONG)ntmini_fsd_fileinfo_diag;
    g_fsd_table[5] = (ULONG)ntmini_fsd_stub_5;
    g_fsd_table[6] = (ULONG)ntmini_fsd_open;
    g_fsd_table[7] = (ULONG)ntmini_fsd_open;
    g_fsd_table[8] = (ULONG)ntmini_fsd_stub_8;
    g_fsd_table[9] = (ULONG)ntmini_fsd_search;
    g_fsd_table[10] = (ULONG)ntmini_fsd_shutdown;
    g_fsd_table[11] = (ULONG)ntmini_fsd_open_onearg;
    g_fsd_table[12] = (ULONG)ntmini_fsd_extra12_onearg;
    g_fsd_table[13] = (ULONG)ntmini_fsd_open_onearg;
    g_fsd_table[14] = (ULONG)ntmini_fsd_search;
    g_fsd_table[15] = (ULONG)ntmini_fsd_slot15_search_fail;
    g_fsd_table[16] = 0;
    dbg_mark('T');
    ios_dbg_hex32((ULONG)g_fsd_table);
    ios_dbg_hex32(g_fsd_table[1]);
    ios_dbg_hex32(g_fsd_table[2]);
    ios_dbg_hex32(g_fsd_table[9]);
    ios_dbg_hex32(g_fsd_table[10]);
    ios_dbg_hex32((ULONG)ntmini_fsd_mount_probe);
    g_fsd_table_inited = 1;
}

static int __cdecl ntmini_fsd_mount_probe(ULONG pir)
{
    ULONG *pi;
    UCHAR drive_byte;
    UCHAR func_byte;

    dbg_mark('M');
    if (!is_ring0_ptr(pir)) {
        dbg_mark('!');
        return -1;
    }

    pi = (ULONG *)pir;
    drive_byte = (UCHAR)(pi[10] & 0xFF);
    func_byte = (UCHAR)(pi[1] & 0xFF);
    dbg_mark('{');
    ios_dbg_hex8(func_byte);
    ios_dbg_hex8(drive_byte);
    ios_dbg_hex32(pir);
    dbg_mark('}');

    if (drive_byte != 3 && func_byte == 0) {
        dbg_mark('r');
        return -1;
    }

    init_fsd_probe_table();
    if (func_byte == 0) {
        ULONG mi;

        dbg_mark('[');
        for (mi = 0; mi < 13; mi++) {
            ios_dbg_hex32(pi[mi]);
        }
        dbg_mark(']');
        g_fsd_volume_data[0] = g_fsd_provider_id;
        g_fsd_volume_data[1] = (ULONG)g_fsd_table;
        g_fsd_volume_data[2] = 3;
        g_fsd_volume_data[3] = g_iso_root_lba;
        g_fsd_volume_data[4] = g_iso_root_size;
        pi[4] = (ULONG)g_fsd_table;
        pi[7] = (ULONG)g_fsd_volume_data;
        *(USHORT *)((UCHAR *)pir + 0x1A) = 0;
        dbg_mark('m');
        return 0;
    }

    if (func_byte == 2) {
        pi[4] = 0;
        dbg_mark('v');
        return 0;
    }

    {
        int table_index;
        int result;
        typedef int (__cdecl *FSD_FUNC)(int fn, int drive, int resType, int cpid, ULONG pir);

        table_index = -1;
        if (func_byte <= 10) {
            table_index = (int)func_byte;
        }
        dbg_mark('J');
        dbg_mark('<');
        ios_dbg_hex8(func_byte);
        ios_dbg_hex8(drive_byte);
        ios_dbg_hex32((ULONG)table_index);
        dbg_mark('>');
        if (table_index >= 1 && table_index <= 10 && g_fsd_table[table_index]) {
            FSD_FUNC handler;

            handler = (FSD_FUNC)g_fsd_table[table_index];
            result = handler((int)func_byte, (int)drive_byte, 0, 0, pir);
            dbg_mark('j');
            ios_dbg_hex32((ULONG)result);
            return result;
        }
    }

    dbg_mark('u');
    return -1;
}

static void ios_dbg_hex8(UCHAR value)
{
    UCHAR hi;
    UCHAR lo;

    hi = (UCHAR)((value >> 4) & 0x0F);
    lo = (UCHAR)(value & 0x0F);
    dbg_mark((char)(hi < 10 ? ('0' + hi) : ('A' + hi - 10)));
    dbg_mark((char)(lo < 10 ? ('0' + lo) : ('A' + lo - 10)));
}

static void ios_dbg_hex16(USHORT value)
{
    ios_dbg_hex8((UCHAR)((value >> 8) & 0xFF));
    ios_dbg_hex8((UCHAR)(value & 0xFF));
}

static void ios_dbg_hex32(ULONG value)
{
    ios_dbg_hex16((USHORT)((value >> 16) & 0xFFFF));
    ios_dbg_hex16((USHORT)(value & 0xFFFF));
}

int ntmini_device_init(void)
{
    ULONG result;
    ULONG pvd_root_lba;
    ULONG pvd_root_size;

    if (g_nt5_init_done) {
        dbg_mark('d');
        return 0;
    }

    dbg_mark('C');
    DBGPRINT("NTMINI: ntmini_device_init()\n");

    /* Initialize NT5 WDM driver stack (load W2K atapi.sys).
     * This must happen before IOS registration so the bridge
     * knows what devices exist when IOS sends AEP events. */
    dbg_mark('D');  /* breadcrumb: about to call nt5_init */
    result = nt5_init(0);  /* 0 = secondary IDE channel, where QEMU -cdrom lives */
    dbg_mark('E');  /* breadcrumb: returned from nt5_init */
    if (result != 0) {
        DBGPRINT("NTMINI: NT5 init failed (%d), continuing with IOS anyway\n",
                 (int)result);
        /* Don't abort - IOS registration still needed for the NT4 fallback */
    } else {
        WDM_BRIDGE_CONTEXT *bridge = nt5_get_bridge_context();
        if (bridge && bridge->DeviceType == 0x00) {
            /* ATA disk -- skip ISO PVD caching, not applicable */
            dbg_mark('H');
            dbg_mark('D');
        } else {
            dbg_mark('C');
            dbg_mark('S');
            if (nt5_get_iso_pvd_info(&pvd_root_lba, &pvd_root_size) == 0) {
                g_iso_root_lba = pvd_root_lba;
                g_iso_root_size = pvd_root_size;
                g_iso_pvd_valid = 1;
                g_iso_root_cached = 0;
                dbg_mark('V');
                ios_dbg_hex32(g_iso_root_lba);
                dbg_mark('v');
                ios_dbg_hex32(g_iso_root_size);
                if (nt5_get_iso_root_cache(g_iso_sector_buf, g_iso_enum_sector_buf,
                                           &pvd_root_lba, &pvd_root_size) == 0) {
                    g_iso_root_lba = pvd_root_lba;
                    g_iso_root_size = pvd_root_size;
                    g_iso_root_cached = 1;
                    dbg_mark('W');
                }
            } else {
                dbg_mark('V');
                dbg_mark('!');
            }
        }
    }

    g_nt5_init_done = 1;
    dbg_mark('i');
    return 0;
}

int ios_register_port_driver(void)
{
    ULONG result;

    if (g_ios_registered) {
        dbg_mark('d');
        return 0;
    }

    if (!g_nt5_init_done) {
        result = (ULONG)ntmini_device_init();
        if (result != 0) {
            return (int)result;
        }
    }

    dbg_mark('C');  /* breadcrumb: entered C function */
    DBGPRINT("NTMINI: ios_register_port_driver()\n");

    /* Zero out bridge state */
    zero_mem(&g_bridge, sizeof(g_bridge));

    /* Build the real IOS Driver Registration Packet.
     * IOS_Register expects a packed DRP. */
    g_bridge.drp.eyecatch[0] = 'X';
    g_bridge.drp.eyecatch[1] = 'X';
    g_bridge.drp.eyecatch[2] = 'X';
    g_bridge.drp.eyecatch[3] = 'X';
    g_bridge.drp.eyecatch[4] = 'X';
    g_bridge.drp.eyecatch[5] = 'X';
    g_bridge.drp.eyecatch[6] = 'X';
    g_bridge.drp.eyecatch[7] = 'X';
    g_bridge.drp.lgn = (1UL << 0x16);      /* DRP_ESDI_PD */
    g_bridge.drp.aer = (ULONG)aep_handler;
    g_bridge.drp.ilb = 0;
    g_bridge.drp.ascii_name[0] = 'N';
    g_bridge.drp.ascii_name[1] = 'T';
    g_bridge.drp.ascii_name[2] = 'M';
    g_bridge.drp.ascii_name[3] = 'I';
    g_bridge.drp.ascii_name[4] = 'N';
    g_bridge.drp.ascii_name[5] = 'I';
    g_bridge.drp.ascii_name[6] = '\0';
    g_bridge.drp.revision = 1;
    g_bridge.drp.feature_code = 0x00080040; /* ESDI_506 pattern */
    g_bridge.drp.if_requirements = 0x00FF; /* DRP_IF_STD */
    g_bridge.drp.bus_type = 0x00;          /* DRP_BT_ESDI */
    g_bridge.drp.reg_result = 0;
    g_bridge.drp.reference_data = 0;
    g_bridge.drp.reserved1[0] = 0;
    g_bridge.drp.reserved1[1] = 0;
    g_bridge.drp.reserved2 = 0;
    g_bridge.ios_ddb = (ULONG)&NTMINI_DDB;

    dbg_mark('R');
    result = 0; /* control: skip real IOS_Register */
    dbg_mark('r');

    if (result != 0) {
        dbg_mark('f');
        DBGPRINT("NTMINI: IOS_Register FAILED (result=%lx)\n", result);
        return -1;
    }

    dbg_mark('s');
    DBGPRINT("NTMINI: IOS_Register skipped\n");
    dbg_mark(g_bridge.drp.reg_result == 1 ? 'L' : 'l');
    dbg_mark(g_bridge.drp.ilb ? 'V' : 'v');
    dbg_mark(g_bridge.drp.reference_data ? 'U' : 'u');
    if (ios_late_create_device() != 0) {
        dbg_mark('w');
        return -1;
    }
    g_ios_registered = 1;
    dbg_mark('g');
    return 0;

    {
        extern ULONG CM_Locate_DevNode_Wrapper(ULONG *pdevnode, char *device_id, ULONG flags);
        extern ULONG CM_Create_DevNode_Wrapper(ULONG *pdevnode, char *device_id, ULONG parent, ULONG flags);
        extern ULONG CM_Register_Device_Driver_Wrapper(ULONG devnode, PVOID handler, ULONG refdata, ULONG flags);
        extern ULONG CM_Setup_DevNode_Wrapper(ULONG devnode, ULONG flags);
        ULONG devnode = 0;
        ULONG parent = 0;
        ULONG cr;
        char dev_id[] = "Root\\SCSIAdapter\\0000";

        dbg_mark('M');
        cr = CM_Locate_DevNode_Wrapper(&devnode, dev_id, 0);
        if (cr == 0 && devnode != 0) {
            dbg_mark('N');
            cr = CM_Register_Device_Driver_Wrapper(devnode, (PVOID)0, 0, 0);
            if (cr == 0) {
                dbg_mark('O');
            }
            cr = CM_Setup_DevNode_Wrapper(devnode, 0);
            if (cr == 0) {
                dbg_mark('P');
            }
        } else {
            dbg_mark('Q');
            cr = CM_Locate_DevNode_Wrapper(&parent, (char *)0, 0);
            if (cr == 0 && parent != 0) {
                dbg_mark('G');
                cr = CM_Create_DevNode_Wrapper(&devnode, dev_id, parent, 0);
                if ((cr == 0 || cr == 0x10) && devnode != 0) {
                    dbg_mark('C');
                } else if (cr == 0x10) {
                    cr = CM_Locate_DevNode_Wrapper(&devnode, dev_id, 0);
                }
                if (devnode != 0) {
                    cr = CM_Register_Device_Driver_Wrapper(devnode, (PVOID)0, 0, 0);
                    if (cr == 0) {
                        dbg_mark('O');
                    }
                    cr = CM_Setup_DevNode_Wrapper(devnode, 0);
                    if (cr == 0) {
                        dbg_mark('P');
                    }
                }
            }
        }
    }

    return 0;
}

static int is_ring0_ptr(ULONG ptr)
{
    return ptr >= 0xC0000000UL && ptr < 0xD0000000UL;
}

static PIOS_ILB validate_ilb(ULONG ptr)
{
    PIOS_ILB ilb;

    if (!is_ring0_ptr(ptr)) {
        return (PIOS_ILB)0;
    }

    ilb = (PIOS_ILB)ptr;
    if (!is_ring0_ptr(ilb->service_rtn)) {
        return (PIOS_ILB)0;
    }

    return ilb;
}

static int is_drp_eyecatcher(UCHAR *ptr)
{
    return ptr[0] == 'X' && ptr[1] == 'X' && ptr[2] == 'X' && ptr[3] == 'X' &&
           ptr[4] == 'X' && ptr[5] == 'X' && ptr[6] == 'X' && ptr[7] == 'X';
}

static PIOS_ILB find_existing_ilb(void)
{
    ULONG ios_ddb;
    UCHAR *walk_ddb;
    ULONG start_ddb;
    int ddb_count;

    dbg_mark('M');

    ios_ddb = VMM_Get_DDB_Wrapper(0x0010);
    if (!is_ring0_ptr(ios_ddb)) {
        return (PIOS_ILB)0;
    }

    walk_ddb = (UCHAR *)ios_ddb;
    start_ddb = ios_ddb;
    for (ddb_count = 0; ddb_count < 80; ddb_count++) {
        ULONG next_ddb = *(ULONG *)walk_ddb;
        ULONG ctrl = *(ULONG *)(walk_ddb + 0x18);
        ULONG ref = *(ULONG *)(walk_ddb + 0x2C);

        if (is_ring0_ptr(ref)) {
            UCHAR *drp = (UCHAR *)ref;
            if (is_drp_eyecatcher(drp)) {
                PIOS_ILB ilb = validate_ilb(*(ULONG *)(drp + 0x10));
                if (ilb) {
                    dbg_mark('V');
                    return ilb;
                }
            }
        }

        if (is_ring0_ptr(ctrl) && ref != (ULONG)&g_bridge.drp) {
            ULONG off;
            UCHAR *base = (UCHAR *)(ctrl & 0xFFFFF000UL);
            for (off = 0; off < 0x2000; off += 4) {
                UCHAR *drp = base + off;
                if (drp != (UCHAR *)&g_bridge.drp && is_drp_eyecatcher(drp)) {
                    PIOS_ILB ilb = validate_ilb(*(ULONG *)(drp + 0x10));
                    if (ilb) {
                        dbg_mark('V');
                        return ilb;
                    }
                    break;
                }
            }
        }

        if (!is_ring0_ptr(next_ddb) || next_ddb == start_ddb) {
            break;
        }
        walk_ddb = (UCHAR *)next_ddb;
    }

    return (PIOS_ILB)0;
}

static int ios_late_create_device(void)
{
    PIOS_ILB ilb;
    ISP_CREATE_DCB_PKT isp_dcb;
    ISP_INSERT_CD_PKT isp_cd;
    ISP_BCAST_AEP_PKT isp_bcast;
    ISP_ASSOC_DCB_PKT isp_assoc;
    ISP_GET_DCB_PKT isp_get;
    ISP_QUERY_MATCH_PKT isp_match;
    ISP_PICK_DRIVE_LETTER_PKT isp_pick;
    ISP_DEV_ARRIVED_PKT isp_arrived;
    UCHAR aep_buf[20];
    PDCB dcb;
    UCHAR *raw;

    ilb = validate_ilb(g_bridge.drp.ilb);
    if (!ilb) {
        ilb = find_existing_ilb();
        if (ilb) {
            g_bridge.drp.ilb = (ULONG)ilb;
        }
    }
    if (!ilb) {
        dbg_mark('e');
        return -1;
    }

    zero_mem(&isp_dcb, sizeof(isp_dcb));
    isp_dcb.func = 1;          /* ISP_CREATE_DCB */
    isp_dcb.dcb_size = 256;
    Call_ILB_Service(ilb->service_rtn, &isp_dcb);
    if (isp_dcb.result != 0 || !is_ring0_ptr(isp_dcb.dcb_ptr)) {
        dbg_mark('d');
        return -1;
    }

    dbg_mark('D');
    dcb = (PDCB)isp_dcb.dcb_ptr;
    raw = (UCHAR *)dcb;
    {
        WDM_BRIDGE_CONTEXT *bridge = nt5_get_bridge_context();
        if (bridge && bridge->DeviceType == 0x00) {
            /* ATA disk: 512-byte sectors */
            *(UCHAR *)(raw + 0x10) = 0x00;          /* DCB_device_type (struct) */
            *(UCHAR *)(raw + 0x11) = 0x00;          /* DCB_bus_type = ESDI */
            *(ULONG *)(raw + 0x18) = 0x00000001UL;  /* DCB_dmd_flags = PHYSICAL */
            *(ULONG *)(raw + 0x40) = 0x00000000UL;  /* DCB type field (IOS) */
            *(ULONG *)(raw + 0x44) = 0x00000000UL;
            *(ULONG *)(raw + 0x1C) = 9UL;           /* blk_shift (struct) */
            *(ULONG *)(raw + 0x20) = 512UL;         /* blk_size (struct) */
            *(ULONG *)(raw + 0x24) = 16UL;          /* heads */
            *(ULONG *)(raw + 0x28) = 32UL;          /* cylinders */
            *(ULONG *)(raw + 0x2C) = 63UL;          /* sectors per track */
            *(ULONG *)(raw + 0x30) = 32768UL;       /* total sectors (16MB) */
            *(ULONG *)(raw + 0x4C) = 9UL;           /* blk_shift (IOS) */
            *(ULONG *)(raw + 0x50) = 512UL;         /* blk_size (IOS) */
            dbg_mark('H');
        } else {
            /* CD-ROM: 2048-byte sectors (default) */
            *(UCHAR *)(raw + 0x10) = 0x05;          /* DCB_device_type (struct) */
            *(ULONG *)(raw + 0x40) = 0x01009005UL;  /* DCB type field (IOS) */
            *(ULONG *)(raw + 0x44) = 0x00000002UL;
            *(ULONG *)(raw + 0x1C) = 11UL;          /* blk_shift (struct) */
            *(ULONG *)(raw + 0x20) = 2048UL;        /* blk_size (struct) */
            *(ULONG *)(raw + 0x4C) = 11UL;          /* blk_shift (IOS) */
            *(ULONG *)(raw + 0x50) = 2048UL;        /* blk_size (IOS) */
            dbg_mark('c');
        }
    }
    g_late_dcb_ptr = isp_dcb.dcb_ptr;
    g_late_dcb_destroyed = 0;
    /* IOS-created physical DCBs use the real DDK layout. */
    dbg_mark('p');
    /* Isolation: leave DCB geometry and strings as IOS created them. */

    zero_mem(&isp_cd, sizeof(isp_cd));
    isp_cd.func = 5;          /* ISP_INSERT_CALLDOWN */
    isp_cd.dcb = isp_dcb.dcb_ptr;
    isp_cd.req = (ULONG)ios_wdm_ior_entry;
    isp_cd.ddb = g_bridge.ios_ddb;
    {
        WDM_BRIDGE_CONTEXT *bridge = nt5_get_bridge_context();
        if (bridge && bridge->DeviceType == 0x00) {
            isp_cd.flags = 0x00000100UL; /* SRB CDB, physical, 512-byte */
        } else {
            isp_cd.flags = 0x00000109UL; /* SRB CDB, physical, not-512 */
        }
    }
    isp_cd.lgn = 0x16;        /* DRP_ESDI_PD_BIT */
    Call_ILB_Service(ilb->service_rtn, &isp_cd);
    if (isp_cd.result != 0) {
        dbg_mark('q');
        return -1;
    }
    dbg_mark('q');

    dbg_mark('P');
    dbg_mark('A');
    dbg_mark('a');
    {
        WDM_BRIDGE_CONTEXT *bridge = nt5_get_bridge_context();
        if (bridge && bridge->DeviceType == 0x00) {
            /* HDD mode: skip AEP broadcast. ESDI_506 already owns a DCB
             * for this device; broadcasting a new DISK DCB deadlocks
             * because ESDI_506 tries to probe the same IDE channel. */
            dbg_mark('S');
        } else {
            dbg_mark('B');
            zero_mem(&isp_bcast, sizeof(isp_bcast));
            zero_mem(aep_buf, sizeof(aep_buf));
            *(USHORT *)(aep_buf + 0x00) = AEP_CONFIG_DCB;
            *(USHORT *)(aep_buf + 0x02) = 0;
            *(ULONG *)(aep_buf + 0x04) = g_bridge.ios_ddb;
            *(ULONG *)(aep_buf + 0x08) = 0x16UL;
            *(ULONG *)(aep_buf + 0x0C) = isp_dcb.dcb_ptr;
            isp_bcast.func = 20;       /* ISP_BROADCAST_AEP */
            isp_bcast.paep = (ULONG)aep_buf;
            Call_ILB_Service(ilb->service_rtn, &isp_bcast);
            if (isp_bcast.result != 0) {
                dbg_mark('h');
                return -1;
            }
            dbg_mark('b');
        }
    }

    /* Probe drive letters D: through G: to find the DCB IOS assigned.
     * In HDD mode, look for DCB_TYPE_DISK; in CD-ROM mode, DCB_TYPE_CDROM.
     * Always scan ALL letters for diagnostics; pick the type-matched one. */
    {
        UCHAR probe_drive;
        WDM_BRIDGE_CONTEXT *bridge = nt5_get_bridge_context();
        UCHAR want_type = (bridge && bridge->DeviceType == 0x00)
                          ? 0x00   /* DCB_TYPE_DISK */
                          : 0x05;  /* DCB_TYPE_CDROM */
        ISP_GET_DCB_PKT fallback;
        fallback.result = 0xFFFF;
        fallback.dcb = 0;
        isp_get.result = 0xFFFF;
        isp_get.dcb = 0;
        for (probe_drive = 3; probe_drive <= 6; probe_drive++) {
            ISP_GET_DCB_PKT probe;
            UCHAR dcb_type;
            zero_mem(&probe, sizeof(probe));
            probe.func = 7;          /* ISP_GET_DCB */
            probe.drive = probe_drive;
            Call_ILB_Service(ilb->service_rtn, &probe);
            dbg_mark('[');
            dbg_mark((char)('A' + probe_drive));
            ios_dbg_hex16(probe.result);
            ios_dbg_hex32(probe.dcb);
            if (probe.result == 0 && is_ring0_ptr(probe.dcb)) {
                /* Log both possible offsets for device type:
                 * 0x10 = our DDK struct offset
                 * 0x40 = offset used in ISP_CREATE_DCB init code */
                dbg_mark('t');
                ios_dbg_hex8(*(UCHAR *)((UCHAR *)probe.dcb + 0x10));
                dbg_mark('/');
                ios_dbg_hex8(*(UCHAR *)((UCHAR *)probe.dcb + 0x40));
            }
            dbg_mark(']');
            if (probe.result != 0 || !is_ring0_ptr(probe.dcb) ||
                probe.dcb == isp_dcb.dcb_ptr)
                continue;
            /* Remember first valid DCB as fallback */
            if (fallback.result != 0)
                fallback = probe;
            /* Check device type at both offsets; use whichever is non-zero,
             * preferring 0x40 (IOS internal) over 0x10 (DDK struct) */
            {
                UCHAR t10 = *(UCHAR *)((UCHAR *)probe.dcb + 0x10);
                UCHAR t40 = *(UCHAR *)((UCHAR *)probe.dcb + 0x40);
                dcb_type = (t40 != 0) ? t40 : t10;
            }
            if (dcb_type == want_type) {
                isp_get = probe;
                dbg_mark('Y');
                DBGPRINT("NTMINI: Matched DCB %c: type=%02x dcb=%08lx\n",
                         'A' + probe_drive, dcb_type, probe.dcb);
            } else {
                dbg_mark('n');
            }
        }
        /* If no type match, use fallback */
        if (isp_get.result != 0 && fallback.result == 0) {
            isp_get = fallback;
            dbg_mark('F');
            DBGPRINT("NTMINI: No type match, using fallback DCB %08lx\n",
                     fallback.dcb);
        }
        if (isp_get.result != 0 || !is_ring0_ptr(isp_get.dcb)) {
            dbg_mark('y');
        }
    }
    ios_dbg_hex32(isp_dcb.dcb_ptr);
    dbg_mark((isp_get.result == 0 && isp_get.dcb != isp_dcb.dcb_ptr) ? 'Y' : 'y');

    zero_mem(&isp_match, sizeof(isp_match));
    isp_match.func = 11;       /* ISP_QUERY_MATCHING_DCBS */
    isp_match.dcb = isp_dcb.dcb_ptr;
    Call_ILB_Service(ilb->service_rtn, &isp_match);
    DBGPRINT("NTMINI: ISP_QUERY_MATCHING_DCBS result=%04x drives=%08lx\n",
             isp_match.result, isp_match.drives);
    dbg_mark('(');
    ios_dbg_hex16(isp_match.result);
    ios_dbg_hex32(isp_match.drives);
    dbg_mark(')');
    dbg_mark((isp_match.result == 0 && (isp_match.drives & (1UL << 3))) ? 'Z' : 'z');

    /* HDD mode: splice our calldown into D:'s EXISTING calldown chain so
     * we intercept I/O before ESDI_506. The key change from previous attempts:
     * insert WITHOUT ISPCDF_BOTTOM — this puts us ABOVE ESDI_506 in the chain.
     * We then consume the I/O via ata_direct_ior instead of passing it down. */
    {
        WDM_BRIDGE_CONTEXT *bridge_ctx = nt5_get_bridge_context();
        if (bridge_ctx && bridge_ctx->DeviceType == 0x00 &&
            isp_get.result == 0 && is_ring0_ptr(isp_get.dcb) &&
            isp_get.dcb != isp_dcb.dcb_ptr) {
            ISP_INSERT_CD_PKT isp_hdd_cd;

            dbg_mark('H');
            dbg_mark('S');
            DBGPRINT("NTMINI: HDD splice into D: DCB %08lx (above ESDI_506)\n",
                     isp_get.dcb);

            zero_mem(&isp_hdd_cd, sizeof(isp_hdd_cd));
            isp_hdd_cd.func = 5;          /* ISP_INSERT_CALLDOWN */
            isp_hdd_cd.dcb = isp_get.dcb;
            isp_hdd_cd.req = (ULONG)ios_wdm_ior_entry;
            isp_hdd_cd.ddb = g_bridge.ios_ddb;
            isp_hdd_cd.flags = 0x00000100UL; /* 512-byte sectors */
            isp_hdd_cd.lgn = 0x16;
            Call_ILB_Service(ilb->service_rtn, &isp_hdd_cd);
            dbg_mark('s');
            ios_dbg_hex16(isp_hdd_cd.result);

            if (isp_hdd_cd.result == 0) {
                dbg_mark('J');
                g_existing_dcb_ptr = isp_get.dcb;

                /* Register device entry for the existing D: DCB */
                if (g_bridge.num_devices < MAX_DEVICES) {
                    PBRIDGE_DEVICE dev3 = &g_bridge.devices[g_bridge.num_devices];
                    zero_mem(dev3, sizeof(BRIDGE_DEVICE));
                    dev3->dcb = (PDCB)isp_get.dcb;
                    dev3->target_id = bridge_ctx->TargetId;
                    dev3->lun = 0;
                    dev3->device_type = DCB_TYPE_DISK;
                    dev3->is_atapi = FALSE;
                    dev3->sector_size = 512;
                    dev3->total_sectors = 0;
                    g_bridge.num_devices++;
                    dbg_mark('K');
                    DBGPRINT("NTMINI: HDD device entry: target=%d dcb=%08lx\n",
                             dev3->target_id, (ULONG)isp_get.dcb);
                }

                /* Walk chain to verify position */
                {
                    ULONG cd;
                    ULONG depth;
                    cd = *(ULONG *)((UCHAR *)isp_get.dcb + 0x08);
                    depth = 0;
                    dbg_mark('~');
                    while (is_ring0_ptr(cd) && depth < 8) {
                        ULONG handler = *(ULONG *)cd;
                        dbg_mark('#');
                        dbg_mark((char)('0' + (char)depth));
                        ios_dbg_hex32(handler);
                        if (handler == (ULONG)ios_wdm_ior_entry) {
                            dbg_mark('*');
                        }
                        cd = *(ULONG *)(cd + 0x0C);
                        depth++;
                    }
                    dbg_mark('~');
                }
            } else {
                dbg_mark('j');
                DBGPRINT("NTMINI: HDD splice FAILED result=%04x\n",
                         isp_hdd_cd.result);
            }
        } else if (isp_get.result != 0 || !is_ring0_ptr(isp_get.dcb) ||
                   isp_get.dcb == isp_dcb.dcb_ptr) {
            dbg_mark('!');
            DBGPRINT("NTMINI: No suitable DCB found for HDD mode\n");
        }
    }

    /* Splice our calldown into the EXISTING DCB's chain (CD-ROM mode only).
     * In HDD mode, we assigned our own DCB a drive letter above; skip splice.
     * For CD-ROM, ISP_GET_DCB returned the real DCB that IOS routes file ops
     * through. We must insert into it so IFS->IOS->calldown reaches us. */
    {
        WDM_BRIDGE_CONTEXT *splice_bridge = nt5_get_bridge_context();
        if (splice_bridge && splice_bridge->DeviceType == 0x00) {
            dbg_mark('h');
        } else if (isp_get.result == 0 && is_ring0_ptr(isp_get.dcb) &&
                   isp_get.dcb != isp_dcb.dcb_ptr) {
        ISP_INSERT_CD_PKT isp_cd2;

        dbg_mark('X');
        DBGPRINT("NTMINI: Splicing calldown into existing DCB %08lx\n",
                 isp_get.dcb);
        zero_mem(&isp_cd2, sizeof(isp_cd2));
        isp_cd2.func = 5;          /* ISP_INSERT_CALLDOWN */
        isp_cd2.dcb = isp_get.dcb;
        isp_cd2.req = (ULONG)ios_wdm_ior_entry;
        isp_cd2.ddb = g_bridge.ios_ddb;
        {
            WDM_BRIDGE_CONTEXT *bridge = nt5_get_bridge_context();
            if (bridge && bridge->DeviceType == 0x00) {
                isp_cd2.flags = 0x00000100UL;
            } else {
                isp_cd2.flags = 0x00000109UL;
            }
        }
        isp_cd2.lgn = 0x16;
        Call_ILB_Service(ilb->service_rtn, &isp_cd2);
        if (isp_cd2.result == 0) {
            PBRIDGE_DEVICE dev2;

            dbg_mark('J');
            g_existing_dcb_ptr = isp_get.dcb;
            DBGPRINT("NTMINI: Calldown spliced into existing DCB OK\n");

            /* Register a device entry for this DCB so ior_handler
             * can find it via find_device_for_dcb(). Use bridge context
             * for device type/sector size since our struct layout may
             * not match the real IOS DCB offsets. */
            if (g_bridge.num_devices < MAX_DEVICES) {
                PDCB existing = (PDCB)isp_get.dcb;
                WDM_BRIDGE_CONTEXT *bridge = nt5_get_bridge_context();

                dev2 = &g_bridge.devices[g_bridge.num_devices];
                zero_mem(dev2, sizeof(BRIDGE_DEVICE));
                dev2->dcb = existing;
                if (bridge && bridge->DeviceType == 0x00) {
                    dev2->target_id = bridge->TargetId;
                    dev2->lun = 0;
                    dev2->device_type = DCB_TYPE_DISK;
                    dev2->is_atapi = FALSE;
                    dev2->sector_size = 512;
                } else {
                    dev2->target_id = existing->DCB_target_id;
                    dev2->lun = existing->DCB_lun;
                    dev2->device_type = existing->DCB_device_type;
                    dev2->is_atapi = (existing->DCB_device_type == DCB_TYPE_CDROM) ? TRUE : FALSE;
                    dev2->sector_size = existing->DCB_apparent_blk_size ? existing->DCB_apparent_blk_size : 2048;
                }
                dev2->total_sectors = 0;
                g_bridge.num_devices++;
                dbg_mark('K');
                DBGPRINT("NTMINI: Device entry: target=%d type=%d sector=%lu\n",
                         dev2->target_id, dev2->device_type, dev2->sector_size);
            }

            /* Walk the existing DCB's calldown chain to verify splice */
            {
                ULONG cd;
                ULONG depth;

                cd = *(ULONG *)((UCHAR *)isp_get.dcb + 0x08);
                depth = 0;
                dbg_mark('~');
                while (is_ring0_ptr(cd) && depth < 8) {
                    ULONG handler = *(ULONG *)cd;

                    dbg_mark('#');
                    dbg_mark((char)('0' + (char)depth));
                    ios_dbg_hex32(handler);
                    if (handler == (ULONG)ios_wdm_ior_entry) {
                        dbg_mark('*');
                    }
                    cd = *(ULONG *)(cd + 0x0C);
                    depth++;
                }
                dbg_mark('~');
            }
        } else {
            dbg_mark('j');
            DBGPRINT("NTMINI: Splice into existing DCB FAILED result=%04x\n",
                     isp_cd2.result);
        }
        }
    }

    dbg_mark('S');
    dbg_mark('s');

    {
        ULONG cd;
        ULONG depth;
        ULONG found_stub;

        cd = *(ULONG *)(raw + 0x08);
        depth = 0;
        found_stub = 0;
        while (is_ring0_ptr(cd) && depth < 8) {
            ULONG node_ddb;
            UCHAR *ddb_name;

            node_ddb = *(ULONG *)(cd + 0x08);
            dbg_mark('#');
            dbg_mark((char)('0' + (char)depth));
            dbg_mark('{');
            ios_dbg_hex32(*(ULONG *)cd);
            ios_dbg_hex32(*(ULONG *)(cd + 0x04));
            ios_dbg_hex32(node_ddb);
            dbg_mark('}');
            if (is_ring0_ptr(node_ddb)) {
                ddb_name = (UCHAR *)(node_ddb + 0x0C);
                dbg_mark((char)(ddb_name[0] >= 0x20 && ddb_name[0] < 0x7F ? ddb_name[0] : '.'));
                dbg_mark((char)(ddb_name[1] >= 0x20 && ddb_name[1] < 0x7F ? ddb_name[1] : '.'));
                dbg_mark((char)(ddb_name[2] >= 0x20 && ddb_name[2] < 0x7F ? ddb_name[2] : '.'));
                dbg_mark((char)(ddb_name[3] >= 0x20 && ddb_name[3] < 0x7F ? ddb_name[3] : '.'));
            }
            if (*(ULONG *)cd == (ULONG)ios_wdm_ior_entry ||
            *(ULONG *)cd == (ULONG)ios_wdm_passthru_stub ||
            *(ULONG *)cd == (ULONG)ios_wdm_preserve_stub) {
                found_stub = 1;
            }
            cd = *(ULONG *)(cd + 0x0C);
            depth++;
        }
        dbg_mark((char)('0' + (char)depth));
        dbg_mark(found_stub ? 'F' : 'f');
    }

    /* Register ISO 9660 FSD for CD-ROM mode only. In HDD mode, the ISO
     * FSD would claim D: and serve stale CD content, blocking real disk I/O. */
    {
        WDM_BRIDGE_CONTEXT *fsd_bridge = nt5_get_bridge_context();
        if (fsd_bridge && fsd_bridge->DeviceType == 0x00) {
            dbg_mark('f');
            DBGPRINT("NTMINI: HDD mode — skipping FSD/IFS registration\n");
        } else if (!g_fsd_registered) {
        dbg_mark('Y');
        g_fsd_provider_id = IFSMgr_RegisterMount_Wrapper((ULONG)ntmini_fsd_mount_probe,
                                                         0x22, 0);
        dbg_mark('y');
        DBGPRINT("NTMINI: IFSMgr_RegisterMount provider=%08lx\n",
                 g_fsd_provider_id);
        dbg_mark('{');
        ios_dbg_hex32(g_fsd_provider_id);
        dbg_mark('}');
        if (g_fsd_provider_id != 0xFFFFFFFFUL) {
            g_fsd_registered = 1;
            dbg_mark('P');
            if (!g_fsd_hook_installed) {
                ULONG prev_hook;

                dbg_mark('h');
                prev_hook = IFSMgr_InstallFSHook_Wrapper((ULONG)ntmini_ifs_hook);
                g_fsd_prev_hook_ptr = prev_hook;
                g_fsd_hook_installed = 1;
                dbg_mark('H');
                DBGPRINT("NTMINI: IFS hook installed at init, prev=%08lx\n",
                         prev_hook);
            }
            dbg_mark('V');
            IFSMgr_NotifyVolumeArrival_Wrapper(3);
            dbg_mark('N');
            ios_schedule_late_cdrom_attach_probe();
            dbg_mark('v');
        } else {
            dbg_mark('p');
        }
        }
    }
    return 0;
}

int ios_late_destroy_device(void)
{
    PIOS_ILB ilb;
    ISP_DCB_DESTROY_PKT isp_destroy;

    if (!g_late_dcb_ptr || g_late_dcb_destroyed) {
        dbg_mark('o');
        return 0;
    }

    ilb = validate_ilb(g_bridge.drp.ilb);
    if (!ilb) {
        ilb = find_existing_ilb();
        if (ilb) {
            g_bridge.drp.ilb = (ULONG)ilb;
        }
    }
    if (!ilb) {
        dbg_mark('j');
        return -1;
    }

    dbg_mark('j');
    zero_mem(&isp_destroy, sizeof(isp_destroy));
    isp_destroy.func = 10;       /* ISP_DESTROY_DCB */
    isp_destroy.dcb = g_late_dcb_ptr;
    Call_ILB_Service(ilb->service_rtn, &isp_destroy);
    if (isp_destroy.result != 0) {
        dbg_mark('w');
        return -1;
    }

    g_late_dcb_destroyed = 1;
    g_late_dcb_ptr = 0;
    dbg_mark('x');
    return 0;
}

void __cdecl ios_late_cdrom_attach_probe(void)
{
    ULONG vrp;
    ULONG attach_result;

    if (g_late_attach_done || !g_fsd_registered) {
        dbg_mark('n');
        return;
    }

    g_late_attach_done = 1;
    vrp = 0;
    dbg_mark('T');
    attach_result = IFSMgr_CDROM_Attach_Wrapper(3, &vrp);
    dbg_mark('t');
    ios_dbg_hex32(attach_result);
    ios_dbg_hex32(vrp);

}

static void __cdecl ios_late_cdrom_attach_timer(PVOID refdata)
{
    (void)refdata;
    dbg_mark('Z');
    ios_late_cdrom_attach_probe();
}

void __cdecl ios_schedule_late_cdrom_attach_probe(void)
{
    ULONG handle;

    if (g_late_attach_done || g_late_attach_scheduled || !g_fsd_registered) {
        dbg_mark('n');
        return;
    }

    g_late_attach_scheduled = 1;
    dbg_mark('Q');
    handle = VxD_SetTimer(1000, (PVOID)ios_late_cdrom_attach_timer, (PVOID)0);
    ios_dbg_hex32(handle);
    if (handle == 0) {
        g_late_attach_scheduled = 0;
        dbg_mark('q');
    }
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
    dbg_mark('H');
    switch (aep->AEP_func) {

    case AEP_INITIALIZE:
        dbg_mark('I');
        /* IOS is initializing us. Save context, report capabilities.
         * This is the first AEP we receive after IOS_Register. */
        aep_initialize(aep);
        break;

    case AEP_BOOT_COMPLETE:
        dbg_mark('O');
        /* System boot sequence is finished. All devices detected,
         * file systems mounted. Safe to do deferred initialization. */
        aep_boot_complete(aep);
        break;

    case AEP_CONFIG_DCB:
        dbg_mark('G');
        /* IOS is telling us about a device. For a port driver, this
         * is where we claim devices we can handle. We insert our
         * calldown handler into the DCB's calldown chain. */
        aep_config_dcb(aep);
        break;

    case AEP_UNCONFIG_DCB:
        dbg_mark('U');
        /* A device is being removed. Clean up our state for it. */
        aep_unconfig_dcb(aep);
        break;

    case AEP_PEND_UNCONFIG_DCB:
        dbg_mark('Z');
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_IOP_TIMEOUT:
        dbg_mark('Q');
        /* Conservative isolation: do not claim timer handling while
         * IOS registration is still crashing during boot. */
        aep->AEP_result = AEP_FAILURE;
        break;

    case AEP_SYSTEM_SHUTDOWN:
        dbg_mark('T');
        /* Conservative isolation: do not mutate queue state for a
         * shutdown event observed immediately after IOS_Register. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_SYSTEM_CRIT_SHUTDOWN:
        dbg_mark('K');
        /* Power failure or critical shutdown. No time for cleanup. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_UNINITIALIZE:
        dbg_mark('N');
        /* Driver is being unloaded. Free all resources. */
        aep_uninitialize(aep);
        break;

    case AEP_ASSOCIATE_DCB:
        dbg_mark('A');
        /* IOS is associating a DCB with us. Accept it. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_REAL_MODE_HANDOFF:
        dbg_mark('M');
        /* Transitioning from real-mode to protected-mode driver.
         * We don't have a real-mode component, so nothing to do. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_DCB_LOCK:
        dbg_mark('L');
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_MOUNT_NOTIFY:
        dbg_mark('W');
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_CREATE_VRP:
        dbg_mark('V');
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_DESTROY_VRP:
        dbg_mark('Y');
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_REFRESH_DRIVE:
        dbg_mark('P');
        /* These events are handled by upper layers (VTD, TSD).
         * Port drivers can ignore them. */
        aep->AEP_result = AEP_SUCCESS;
        break;

    case 27:  /* AEP_QUERY_CD_SPINDOWN */
        dbg_mark('B');
        aep->AEP_result = AEP_SUCCESS;
        break;

    default:
    {
        UCHAR hi;
        UCHAR lo;
        dbg_mark('?');
        hi = (UCHAR)((aep->AEP_func >> 4) & 0x0F);
        lo = (UCHAR)(aep->AEP_func & 0x0F);
        dbg_mark((char)(hi < 10 ? ('0' + hi) : ('A' + hi - 10)));
        dbg_mark((char)(lo < 10 ? ('0' + lo) : ('A' + lo - 10)));
        /* Unknown AEP function code. Fail conservatively until the IOS
         * registration path is stable. */
        aep->AEP_result = AEP_FAILURE;
        break;
    }
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
    g_bridge.ios_ddb = aep->AEP_ddb;

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
    if ((ULONG)dcb == g_late_dcb_ptr) {
        dbg_mark('c');
        aep->AEP_result = AEP_SUCCESS;
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
     * We handle: CD-ROM, DVD (ATAPI devices on the IDE bus),
     * and ATA hard disks when the NT5 bridge has detected one.
     * For ATAPI/SCSI devices the NT miniport provides superior
     * command handling; for ATA disks we replace ESDI_506.PDR
     * when our bridge is active. */
    if (dcb->DCB_device_type != DCB_TYPE_CDROM &&
        dcb->DCB_device_type != DCB_TYPE_DISK &&
        !(dcb->DCB_dmd_flags & DCB_DEV_ATAPI)) {
        /* Not our device. Let another port driver handle it. */
        aep->AEP_result = AEP_FAILURE;
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
    if (!g_bridge.ios_ddb) {
        aep->AEP_result = AEP_FAILURE;
        return;
    }

    g_bridge.calldown.CD_func  = (CALLDOWN_FUNC)ior_handler;
    g_bridge.calldown.CD_ddb   = g_bridge.ios_ddb;
    g_bridge.calldown.CD_next  = NULL;
    g_bridge.calldown.CD_flags = 0;

    ISP_INSERT_CALLDOWN(dcb, &g_bridge.calldown, g_bridge.ios_ddb,
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

static void __cdecl ios_ior_entry(PVOID iop_ptr)
{
    PUCHAR iop;
    PIOR ior;

    dbg_mark('X');
    if (!iop_ptr || !is_ring0_ptr((ULONG)iop_ptr)) {
        dbg_mark('x');
        return;
    }

    iop = (PUCHAR)iop_ptr;
    ior = (PIOR)(iop + 0x64);
    if (!is_ring0_ptr((ULONG)ior)) {
        dbg_mark('x');
        return;
    }

    /* IOS calldown passes an IOP pointer; the public IOR starts at IOP+0x64.
     * Internal requests are completed by the chain executor after we return. */
    ior->IOR_status = IORS_NOT_SUPPORTED;
    if (iop[0x6D] & 0x04) {
        dbg_mark('j');
        return;
    }

    complete_ior(ior, IORS_NOT_SUPPORTED);
}

void __cdecl ios_wdm_log_iop(PVOID iop_ptr)
{
    UCHAR *raw;
    USHORT fn;
    ULONG flags;
    ULONG seq;

    dbg_mark('W');
    if (!is_ring0_ptr((ULONG)iop_ptr)) {
        dbg_mark('!');
        return;
    }

    dbg_mark('<');
    ios_dbg_hex32((ULONG)iop_ptr);
    dbg_mark('>');

    if (g_wdm_iop_dump_count < 8) {
        raw = (UCHAR *)iop_ptr;
        fn = *(USHORT *)(raw + 0x68);
        flags = *(ULONG *)(raw + 0x6C);
        seq = *(ULONG *)(raw + 0x70);
        dbg_mark('{');
        ios_dbg_hex16(fn);
        ios_dbg_hex32(flags);
        ios_dbg_hex32(seq);
        dbg_mark('}');
        g_wdm_iop_dump_count++;
    }

    dbg_mark('w');
}

void __cdecl ios_wdm_ior_entry(PVOID iop_ptr)
{
    PUCHAR iop;
    PIOR ior;
    PDCB dcb;

    dbg_mark('W');
    if (!iop_ptr || !is_ring0_ptr((ULONG)iop_ptr)) {
        dbg_mark('w');
        return;
    }

    iop = (PUCHAR)iop_ptr;
    ior = (PIOR)(iop + 0x64);
    if (!is_ring0_ptr((ULONG)ior)) {
        dbg_mark('x');
        return;
    }

    dbg_mark('<');
    ios_dbg_hex16(ior->IOR_func);
    dbg_mark('>');

    /* IOS internal requests: succeed via IOR-level completion.
     * IOS_BD_Complete_IOP deadlocks for internal IOPs (recursive chain).
     * IOS_BD_Command_Complete operates on the IOR, not IOP, avoiding
     * the chain re-entry. */
    if (iop[0x6D] & 0x04) {
        dbg_mark('j');
        ior->IOR_status = 0;
        IOS_BD_Command_Complete(ior);
        dbg_mark('c');
        return;
    }

    /* Use the existing DCB that D: routes through.
     * If we haven't spliced into a real DCB, fall back to our late DCB. */
    dcb = (PDCB)(g_existing_dcb_ptr ? g_existing_dcb_ptr : g_late_dcb_ptr);
    if (!dcb || !is_ring0_ptr((ULONG)dcb)) {
        dbg_mark('!');
        ior->IOR_status = IORS_NOT_SUPPORTED;
        IOS_BD_Complete_IOP(iop_ptr);
        return;
    }

    /* Dispatch to the real IOR handler which translates to SRB */
    ior_handler(ior, dcb);
}

/* ATA register constants for secondary IDE channel */
#define ATA_SEC_DATA        0x170
#define ATA_SEC_ERROR       0x171
#define ATA_SEC_SECTOR_CNT  0x172
#define ATA_SEC_LBA_LOW     0x173
#define ATA_SEC_LBA_MID     0x174
#define ATA_SEC_LBA_HIGH    0x175
#define ATA_SEC_DRIVE_HEAD  0x176
#define ATA_SEC_CMD_STATUS  0x177
#define ATA_SEC_ALT_STATUS  0x376

#define ATA_CMD_READ_SECTORS    0x20
#define ATA_CMD_WRITE_SECTORS   0x30
#define ATA_STATUS_BSY          0x80
#define ATA_STATUS_DRQ          0x08
#define ATA_STATUS_ERR          0x01

static int ata_wait_not_busy(ULONG timeout_us)
{
    ULONG i;
    UCHAR status;
    for (i = 0; i < timeout_us; i++) {
        status = PORT_IN_BYTE(ATA_SEC_ALT_STATUS);
        if (!(status & ATA_STATUS_BSY))
            return 0;
        PORT_STALL_ONE();
    }
    return -1;
}

static int ata_wait_drq(ULONG timeout_us)
{
    ULONG i;
    UCHAR status;
    for (i = 0; i < timeout_us; i++) {
        status = PORT_IN_BYTE(ATA_SEC_ALT_STATUS);
        if (status & ATA_STATUS_ERR)
            return -1;
        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ))
            return 0;
        PORT_STALL_ONE();
    }
    return -2;
}

static void ata_direct_ior(PIOR ior, PBRIDGE_DEVICE dev)
{
    ULONG lba, byte_count, sector_count, i;
    PUSHORT buf;
    UCHAR drive_sel;
    int err;

    if (ior->IOR_func != IOR_READ && ior->IOR_func != IOR_WRITE &&
        ior->IOR_func != IOR_WRITEV) {
        if (ior->IOR_func == IOR_MEDIA_CHECK ||
            ior->IOR_func == IOR_MEDIA_CHECK_RESET) {
            complete_ior(ior, IORS_SUCCESS);
            return;
        }
        complete_ior(ior, IORS_NOT_SUPPORTED);
        return;
    }

    lba = ior->IOR_start_addr[0];
    byte_count = ior->IOR_xfer_count;
    sector_count = byte_count / 512;
    if (sector_count == 0) sector_count = 1;
    if (sector_count > 256) sector_count = 256;

    buf = (PUSHORT)ior->IOR_buffer_ptr;
    if (!buf || !is_ring0_ptr((ULONG)buf)) {
        complete_ior(ior, IORS_CMD_FAILED);
        return;
    }

    /* 0xE0 = LBA mode, master; 0xF0 = LBA mode, slave */
    drive_sel = (dev->target_id == 0) ? 0xE0 : 0xF0;
    drive_sel |= (UCHAR)((lba >> 24) & 0x0F);

    /* Soft-reset the IDE channel if BSY is stuck. ESDI_506.PDR may
     * have left the controller in a state where BSY won't clear. */
    {
        UCHAR status = PORT_IN_BYTE(ATA_SEC_ALT_STATUS);
        if (status & ATA_STATUS_BSY) {
            PORT_OUT_BYTE(ATA_SEC_ALT_STATUS, 0x04); /* SRST set */
            PORT_STALL_ONE(); PORT_STALL_ONE(); PORT_STALL_ONE();
            PORT_OUT_BYTE(ATA_SEC_ALT_STATUS, 0x00); /* SRST clear */
            {
                ULONG rst;
                for (rst = 0; rst < 1000000; rst++) {
                    status = PORT_IN_BYTE(ATA_SEC_ALT_STATUS);
                    if (!(status & ATA_STATUS_BSY)) break;
                    PORT_STALL_ONE();
                }
            }
        }
    }

    if (ata_wait_not_busy(500000) != 0) {
        dbg_mark('$');
        ios_dbg_hex8(PORT_IN_BYTE(ATA_SEC_CMD_STATUS));
        complete_ior(ior, IORS_CMD_FAILED);
        return;
    }

    PORT_OUT_BYTE(ATA_SEC_DRIVE_HEAD, drive_sel);
    PORT_STALL_ONE(); PORT_STALL_ONE();

    if (ata_wait_not_busy(500000) != 0) {
        dbg_mark('~');
        ios_dbg_hex8(PORT_IN_BYTE(ATA_SEC_CMD_STATUS));
        complete_ior(ior, IORS_CMD_FAILED);
        return;
    }

    PORT_OUT_BYTE(ATA_SEC_SECTOR_CNT, (UCHAR)(sector_count & 0xFF));
    PORT_OUT_BYTE(ATA_SEC_LBA_LOW,    (UCHAR)(lba & 0xFF));
    PORT_OUT_BYTE(ATA_SEC_LBA_MID,    (UCHAR)((lba >> 8) & 0xFF));
    PORT_OUT_BYTE(ATA_SEC_LBA_HIGH,   (UCHAR)((lba >> 16) & 0xFF));

    if (ior->IOR_func == IOR_READ) {
        PORT_OUT_BYTE(ATA_SEC_CMD_STATUS, ATA_CMD_READ_SECTORS);

        for (i = 0; i < sector_count; i++) {
            err = ata_wait_drq(1000000);
            if (err != 0) {
                dbg_mark('E');
                complete_ior(ior, IORS_CMD_FAILED);
                return;
            }
            port_read_buffer_ushort(ATA_SEC_DATA, buf, 256);
            buf += 256;
        }
    } else {
        PORT_OUT_BYTE(ATA_SEC_CMD_STATUS, ATA_CMD_WRITE_SECTORS);

        for (i = 0; i < sector_count; i++) {
            err = ata_wait_drq(1000000);
            if (err != 0) {
                dbg_mark('E');
                complete_ior(ior, IORS_CMD_FAILED);
                return;
            }
            port_write_buffer_ushort(ATA_SEC_DATA, buf, 256);
            buf += 256;
        }
    }

    if (ata_wait_not_busy(500000) != 0) {
        dbg_mark('T');
    }

    complete_ior(ior, IORS_SUCCESS);
}

static void ior_handler(PIOR ior, PDCB dcb)
{
    PBRIDGE_DEVICE dev;

    dbg_mark('!');
    if (!ior) {
        return;
    }

    /* Find our device tracking entry for this DCB */
    dev = find_device_for_dcb(dcb);
    if (!dev) {
        DBGPRINT("NTMINI: IOR for unknown DCB, failing\n");
        complete_ior(ior, IORS_ERROR_DESIGNTR);
        return;
    }

    /* ATA hard disks: bypass the miniport and use direct port I/O.
     * The miniport (atapi.sys) handles ATAPI packet devices (CD-ROM).
     * For ATA disks, we program the IDE registers directly. */
    if (dev->device_type == DCB_TYPE_DISK) {
        ata_direct_ior(ior, dev);
        return;
    }

    /* CD-ROM and other ATAPI: use the miniport SCSI path */
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
    dcb->DCB_ddb            = g_bridge.ios_ddb;

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
        /* Determine ATAPI vs ATA from INQUIRY peripheral device type.
         * Type 0x00 (direct-access) is an ATA hard disk; everything
         * else on IDE is ATAPI (packet interface). */
        is_atapi    = (device_type == 0x00) ? FALSE : TRUE;

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
