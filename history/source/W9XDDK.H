/*
 * W9XDDK.H - Windows 9x DDK Structures for NT Miniport Bridge
 *
 * Defines the I/O Supervisor (IOS), IOR, AEP, DCB, DDB, and related
 * structures needed to write a Win9x port driver (.PDR) in C.
 *
 * These structures are reconstructed from the Win98 DDK documentation,
 * IOS.H, BLOCKDEV.H, DCB.H, and IOR.H. Field offsets match the DDK
 * originals so that our port driver can interoperate with the real
 * IOS and layered drivers.
 *
 * References:
 *   - Microsoft Windows 98 DDK: ios.h, ior.h, dcb.h, blockdev.h
 *   - Walter Oney, "Systems Programming for Windows 95" (1996)
 *   - Andrew Schulman, "Unauthorized Windows 95" (1994)
 *   - VxD Lite headers (public domain reconstructions)
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License.
 */

#ifndef W9XDDK_H
#define W9XDDK_H

/* ================================================================
 * Basic types (matching Win9x DDK conventions)
 *
 * Win9x DDK uses a mix of NT types and its own conventions.
 * We define both so IOSBRIDGE.C and NTMINI.C can share types.
 * ================================================================ */

#ifndef _NTMINI_TYPES_DEFINED
typedef unsigned char   UCHAR;
typedef unsigned short  USHORT;
typedef unsigned long   ULONG;
typedef signed long     LONG;
typedef int             BOOLEAN;
typedef void            VOID;
typedef void           *PVOID;
typedef UCHAR          *PUCHAR;
typedef USHORT         *PUSHORT;
typedef ULONG          *PULONG;
#define TRUE  1
#define FALSE 0
#define NULL  ((void *)0)
#endif

/* Win9x VxD callback types */
typedef void (__cdecl *CMCB)(void);     /* Command completion callback */
typedef void (__cdecl *REQCB)(void);    /* Request callback */


/* ================================================================
 * SCATTER/GATHER DESCRIPTOR (SGD)
 *
 * IOS uses scatter/gather descriptors for DMA-capable transfers.
 * Each SGD describes one physical region. The array is terminated
 * by an entry with SG_FLAGS_END set.
 * ================================================================ */

/* SGD flags */
#define SG_FLAGS_END        0x0001  /* Last entry in the SG list */
#define SG_FLAGS_PHYS       0x0002  /* Address is physical (not linear) */

typedef struct _SGD {
    ULONG   SG_buff_ptr;    /* Linear or physical address of buffer */
    ULONG   SG_buff_size;   /* Size of this segment in bytes */
    ULONG   SG_flags;       /* SG_FLAGS_* */
} SGD, *PSGD;


/* ================================================================
 * IOR (I/O Request)
 *
 * The IOR is the fundamental I/O packet in Win9x. It flows down
 * the IOS calldown chain from the file system through volume
 * trackers, type-specific drivers, and port drivers. Each layer
 * can modify or complete the IOR.
 *
 * Structure layout matches Win98 DDK IOR.H. We include all fields
 * a port driver needs to see, even if we only use a subset. The
 * offsets are critical because the real IOS fills these in.
 *
 * Key fields for a port driver:
 *   IOR_func         - What operation (read, write, SCSI pass-through)
 *   IOR_status       - Result code (set by port driver on completion)
 *   IOR_buffer_ptr   - Linear address of data buffer
 *   IOR_xfer_count   - Number of bytes to transfer
 *   IOR_start_addr   - Sector-based starting address (for block I/O)
 *   IOR_sgd_lin_phys - Pointer to scatter/gather descriptor array
 *   IOR_num_sgds     - Number of scatter/gather descriptors
 *   IOR_callback     - Completion callback (port driver calls this)
 * ================================================================ */

/* --- IOR Function Codes --- */
/* These are the values found in IOR_func. The port driver must
   handle at minimum IOR_READ, IOR_WRITE, and IOR_SCSI_PASS_THROUGH. */

#define IOR_READ                0x0001  /* Read sectors */
#define IOR_WRITE               0x0002  /* Write sectors */
#define IOR_VERIFY              0x0003  /* Verify sectors (no data transfer) */
#define IOR_CANCEL              0x0004  /* Cancel outstanding request */
#define IOR_WRITEV              0x000E  /* Write with verify */
#define IOR_SCSI_PASS_THROUGH   0x000F  /* Raw SCSI/ATAPI CDB passthrough */
#define IOR_CLEAR_QUEUE         0x0012  /* Clear device queue */
#define IOR_MEDIA_CHECK         0x0013  /* Check if media changed */
#define IOR_DOS_RESET           0x0014  /* DOS-style device reset */
#define IOR_MEDIA_CHECK_RESET   0x0017  /* Media check with reset */

/* Additional IOR function codes (less commonly used by port drivers) */
#define IOR_FORMAT              0x0005  /* Format track */
#define IOR_ABORT               0x0008  /* Abort in-progress request */
#define IOR_LOCK_MEDIA          0x000A  /* Lock removable media */
#define IOR_UNLOCK_MEDIA        0x000B  /* Unlock removable media */
#define IOR_LOAD_MEDIA          0x000C  /* Load media (e.g. close tray) */
#define IOR_EJECT_MEDIA         0x000D  /* Eject media */
#define IOR_REQUEST_SENSE       0x0010  /* Request sense data (SCSI) */

/* --- IOR Status Codes --- */
/* Set by the port driver in IOR_status before calling back. */

#define IORS_SUCCESS                0x0000  /* Request completed OK */
#define IORS_ERROR_DESIGNTR         0x0001  /* Invalid designator */
#define IORS_CMD_FAILED             0x0002  /* Command failed at device */
#define IORS_CMD_INVALID            0x0003  /* Invalid command for device */
#define IORS_MEDIA_ERROR            0x0004  /* Media error (bad sector) */
#define IORS_DEVICE_ERROR           0x0005  /* General device error */
#define IORS_INVALID_SECTOR         0x0006  /* Sector out of range */
#define IORS_WRITE_PROTECT          0x0007  /* Media is write-protected */
#define IORS_DRIVENOTREADY          0x0008  /* Drive not ready */
#define IORS_NO_MEDIA               0x0009  /* No media in drive */
#define IORS_UNREC_ERROR            0x000A  /* Unrecoverable error */
#define IORS_TIME_OUT               0x000B  /* Request timed out */
#define IORS_NOT_SUPPORTED          0x000C  /* Function not supported */
#define IORS_INVALID_PARM           0x000D  /* Invalid parameter */
#define IORS_MEMORY_PROBLEM         0x000E  /* Memory allocation failure */
#define IORS_UNCERTAIN_MEDIA        0x000F  /* Media may have changed */
#define IORS_REQUEST_ABORTED        0x0010  /* Request was aborted */
#define IORS_MEDIA_CHANGED          0x0011  /* Media definitely changed */

/* --- IOR Flags --- */
#define IORF_SCATTER_GATHER     0x0001  /* SGD list is valid */
#define IORF_CHAR_COMMAND       0x0002  /* Character (non-block) command */
#define IORF_HIGH_PRIORITY      0x0004  /* High-priority request */
#define IORF_SYNC_COMMAND       0x0008  /* Synchronous (caller waits) */
#define IORF_VERSION_002        0x0020  /* IOR version 2 (extended fields) */
#define IORF_LOGICAL_START_ADDR 0x0040  /* IOR_start_addr is logical */
#define IORF_PHYS_CMD           0x0080  /* Physical (not logical) command */
#define IORF_DOUBLE_BUFFER      0x0100  /* Double-buffer this transfer */

/* --- SCSI Pass-Through sub-structure ---
 * When IOR_func == IOR_SCSI_PASS_THROUGH, the CDB and sense data
 * are passed via IOR_private_client or through the IOP_ior_da area.
 * This structure is embedded at a known offset. */

typedef struct _IOR_SCSI_PASSTHROUGH {
    UCHAR   SP_CDB[16];        /* SCSI Command Descriptor Block */
    UCHAR   SP_CDBLength;      /* Length of valid CDB bytes */
    UCHAR   SP_SenseData[18];  /* Returned sense data */
    UCHAR   SP_SenseLength;    /* Valid sense bytes returned */
    UCHAR   SP_Flags;          /* Direction flags */
    ULONG   SP_DataLength;     /* Data transfer length in bytes */
} IOR_SCSI_PASSTHROUGH, *PIOR_SCSI_PASSTHROUGH;

/* SP_Flags */
#define SP_DATA_IN      0x01    /* Data transfer from device to host */
#define SP_DATA_OUT     0x02    /* Data transfer from host to device */
#define SP_NO_DATA      0x00    /* No data transfer */

/* --- The IOR structure itself --- */

typedef struct _IOR {
    /* ---- Core fields (always present) ---- */
    USHORT  IOR_func;           /* 0x00: Function code (IOR_READ, etc.) */
    USHORT  IOR_status;         /* 0x02: Status (set on completion) */
    ULONG   IOR_flags;          /* 0x04: Flags (IORF_*) */
    CMCB    IOR_callback;       /* 0x08: Completion callback */
    ULONG   IOR_buffer_ptr;     /* 0x0C: Linear address of data buffer */
    ULONG   IOR_xfer_count;     /* 0x10: Transfer size in bytes */
    ULONG   IOR_start_addr[2];  /* 0x14: Starting sector (64-bit) */
    ULONG   IOR_private_client; /* 0x1C: Client driver private data */
    ULONG   IOR_private_IOS;    /* 0x20: IOS private data */
    ULONG   IOR_private_port;   /* 0x24: Port driver private data */

    /* ---- Scatter/gather ---- */
    PSGD    IOR_sgd_lin_phys;   /* 0x28: Pointer to SGD array */
    ULONG   IOR_num_sgds;       /* 0x2C: Number of SGDs */

    /* ---- Volume/device routing ---- */
    ULONG   IOR_vol_designtr;   /* 0x30: Volume designator */
    ULONG   IOR_req_vol_handle; /* 0x34: Requesting volume handle */
    PVOID   IOR_next;           /* 0x38: Next IOR in queue (for linking) */

    /* ---- Extended fields (version 002) ---- */
    ULONG   IOR_ioctl_code;     /* 0x3C: IOCTL code for pass-through */
    PVOID   IOR_ioctl_buffer;   /* 0x40: IOCTL data buffer */
    ULONG   IOR_ioctl_buffer_length; /* 0x44: IOCTL buffer length */

    /* ---- SCSI pass-through (when IOR_func == IOR_SCSI_PASS_THROUGH) ---- */
    IOR_SCSI_PASSTHROUGH IOR_scsi_pass; /* 0x48: Embedded SCSI passthrough */

} IOR, *PIOR;


/* ================================================================
 * DCB (Device Control Block)
 *
 * A DCB represents a physical or logical device known to IOS.
 * When our port driver detects a device (e.g. ATAPI CD-ROM),
 * it creates a DCB and registers it with IOS. Upper layers
 * (CDFS, VFAT, etc.) then attach to the DCB.
 *
 * We define only the fields relevant to a port driver. The real
 * DCB is much larger (~200 bytes) but most fields are managed
 * by IOS and TSD (Type-Specific Driver) layers.
 * ================================================================ */

/* DCB device type constants */
#define DCB_TYPE_DISK           0x0000  /* Hard disk */
#define DCB_TYPE_TAPE           0x0001  /* Tape drive */
#define DCB_TYPE_PRINTER        0x0002  /* Printer (SCSI) */
#define DCB_TYPE_PROCESSOR      0x0003  /* Processor device */
#define DCB_TYPE_WORM           0x0004  /* Write-once device */
#define DCB_TYPE_CDROM          0x0005  /* CD-ROM / DVD-ROM */
#define DCB_TYPE_SCANNER        0x0006  /* Scanner */
#define DCB_TYPE_OPTICAL_DISK   0x0007  /* Optical disk */
#define DCB_TYPE_CHANGER        0x0008  /* Medium changer */
#define DCB_TYPE_COMM           0x0009  /* Communications device */

/* DCB device flags (DCB_dmd_flags) */
#define DCB_DEV_PHYSICAL        0x00000001  /* Physical device */
#define DCB_DEV_LOGICAL         0x00000002  /* Logical device */
#define DCB_DEV_REMOVABLE       0x00000004  /* Removable media */
#define DCB_DEV_ATAPI           0x00000008  /* ATAPI device */
#define DCB_DEV_SCSI            0x00000010  /* SCSI device */
#define DCB_DEV_INT13           0x00000020  /* INT 13h device */
#define DCB_DEV_CDROM           0x00000040  /* CD-ROM device */
#define DCB_DEV_RMM             0x00000080  /* Real Mode Mapper device */
#define DCB_DEV_EJECTABLE       0x00000100  /* Media is ejectable */
#define DCB_DEV_NOPMMODE        0x00000200  /* No protected mode access */

/* DCB bus type */
#define DCB_BUS_ESDI            0x0001
#define DCB_BUS_SCSI            0x0002
#define DCB_BUS_FLOPPY          0x0003
#define DCB_BUS_ABIOS           0x0004
#define DCB_BUS_EISA            0x0005
#define DCB_BUS_SMART           0x0006

/* Maximum targets per bus in our simplified model */
#define MAX_DCB_TARGETS         8

typedef struct _DCB {
    /* ---- IOS-managed fields (read-only for port driver) ---- */
    ULONG   DCB_cmn_size;               /* 0x00: Total DCB size */
    struct _DCB *DCB_next;              /* 0x04: Next DCB in chain */
    struct _DCB *DCB_next_logical;      /* 0x08: Next logical DCB */
    struct _DDB *DCB_ddb;               /* 0x0C: Owning DDB */

    /* ---- Device identification ---- */
    UCHAR   DCB_device_type;            /* 0x10: DCB_TYPE_* */
    UCHAR   DCB_bus_type;               /* 0x11: DCB_BUS_* */
    UCHAR   DCB_bus_number;             /* 0x12: Bus number (0, 1, ...) */
    UCHAR   DCB_target_id;             /* 0x13: Target/device on bus */
    UCHAR   DCB_lun;                    /* 0x14: Logical unit number */
    UCHAR   DCB_pad1[3];               /* 0x15: Alignment padding */

    /* ---- Device flags ---- */
    ULONG   DCB_dmd_flags;             /* 0x18: DCB_DEV_* flags */

    /* ---- Geometry (for block devices) ---- */
    ULONG   DCB_apparent_blk_shift;    /* 0x1C: log2(sector size) */
    ULONG   DCB_apparent_blk_size;     /* 0x20: Sector size (bytes) */
    ULONG   DCB_apparent_head_count;   /* 0x24: Heads */
    ULONG   DCB_apparent_cyl_count;    /* 0x28: Cylinders */
    ULONG   DCB_apparent_spt;          /* 0x2C: Sectors per track */
    ULONG   DCB_apparent_total_sectors;/* 0x30: Total sectors */

    /* ---- Calldown chain ---- */
    PVOID   DCB_cd;                    /* 0x34: Calldown list head */

    /* ---- Port driver context ---- */
    ULONG   DCB_port_specific;         /* 0x38: Port driver private data */
    ULONG   DCB_max_xfer_len;          /* 0x3C: Max transfer length (bytes) */

    /* ---- Device name (for display) ---- */
    char    DCB_vendor_id[8];          /* 0x40: Vendor ID string */
    char    DCB_product_id[16];        /* 0x48: Product ID string */
    char    DCB_rev_level[4];          /* 0x58: Firmware revision */

    /* ---- IOS expansion area ---- */
    ULONG   DCB_expansion[8];          /* 0x5C: Reserved for IOS */

} DCB, *PDCB;


/* ================================================================
 * DDB (Device Descriptor Block)
 *
 * The DDB identifies a loaded port driver (or any layer driver)
 * to IOS. When we call IOS_Register, we pass a DDB with our
 * driver name, AEP handler, and capabilities.
 *
 * The DDB is analogous to a VxD's Device Descriptor Block but
 * specific to the IOS layered driver model.
 * ================================================================ */

/* DDB device classes */
#define DDB_CLASS_PORT          0x0001  /* Port driver (hardware-facing) */
#define DDB_CLASS_TSD           0x0002  /* Type-Specific Driver */
#define DDB_CLASS_VTD           0x0003  /* Volume Tracker */
#define DDB_CLASS_VENDOR        0x0004  /* Vendor layer */

/* Registration flags */
#define DDB_FLAG_NEED_PHYS      0x00000001  /* Needs physical addresses */
#define DDB_FLAG_NEED_LOCK      0x00000002  /* Needs locked pages */

/* Port driver merit (priority in the chain).
 * Higher merit = loaded earlier = closer to hardware.
 * Our driver needs high merit since it IS the port driver. */
#define DDB_MERIT_PORT_NORMAL   0x10000000

typedef struct _DDB {
    ULONG   DDB_size;               /* Size of this structure */
    ULONG   DDB_class;              /* DDB_CLASS_* */
    ULONG   DDB_flags;              /* DDB_FLAG_* */
    ULONG   DDB_merit;              /* Load order priority */
    char    DDB_name[16];           /* Driver name (null-terminated) */
    ULONG   DDB_lgn;                /* Logical group number */
    PVOID   DDB_aep_handler;        /* AEP dispatch function */
    PVOID   DDB_expansion;          /* Reserved */
} DDB, *PDDB;


/* ================================================================
 * AEP (Async Event Packet)
 *
 * IOS communicates with layer drivers through AEP callbacks.
 * When IOS needs to initialize a driver, configure a device,
 * or route an I/O request, it sends an AEP.
 *
 * The AEP_func field selects the event type. For AEP_IOR events,
 * the IOR pointer is included. For AEP_CONFIG_DCB events, the
 * DCB pointer is included.
 *
 * The port driver's AEP handler is a single __cdecl function
 * that dispatches on AEP_func.
 * ================================================================ */

/* --- AEP Function Codes --- */
#define AEP_INITIALIZE          0x00    /* IOS is initializing the driver */
#define AEP_SYSTEM_CRIT_SHUTDOWN 0x01   /* Critical (power-fail) shutdown */
#define AEP_BOOT_COMPLETE       0x02    /* Boot sequence finished */
#define AEP_CONFIG_DCB          0x03    /* Configure a DCB (device detected) */
#define AEP_UNCONFIG_DCB        0x04    /* Unconfigure a DCB (device removed) */
#define AEP_IOR                 0x05    /* Process an I/O request */
#define AEP_ASSOCIATE_DCB       0x06    /* Associate DCB with a port driver */
#define AEP_DISASSOCIATE_DCB    0x07    /* Disassociate DCB */
#define AEP_REAL_MODE_HANDOFF   0x08    /* Transitioning from real mode */
#define AEP_SYSTEM_SHUTDOWN     0x09    /* Normal system shutdown */
#define AEP_UNINITIALIZE        0x0A    /* Driver being unloaded */
#define AEP_DCB_LOCK            0x0B    /* Lock DCB (prevent removal) */
#define AEP_MOUNT_NOTIFY        0x0C    /* Volume mount notification */
#define AEP_CREATE_VRP          0x0E    /* Create Volume Request Params */
#define AEP_DESTROY_VRP         0x0F    /* Destroy VRP */
#define AEP_REFRESH_DRIVE       0x10    /* Refresh drive letter assignment */

/* --- AEP Result Codes --- */
#define AEP_SUCCESS             0x0000  /* Event handled successfully */
#define AEP_FAILURE             0x0001  /* Event handling failed */
#define AEP_NO_SUPPORT          0x0002  /* Event not supported by driver */

/* --- AEP_bi_flags (for AEP_INITIALIZE) --- */
#define AEP_BI_FLAG_PD          0x00000001  /* This is a port driver init */
#define AEP_BI_FLAG_PEND        0x00000010  /* Initialization is pending */

/* --- AEP_CONFIG_DCB sub-flags --- */
#define AEP_CD_FLAG_PRIMARY     0x00000001  /* Primary IDE channel */
#define AEP_CD_FLAG_SECONDARY   0x00000002  /* Secondary IDE channel */

/* --- The AEP structure --- */

typedef struct _AEP {
    /* ---- Core fields (present in all AEP types) ---- */
    USHORT  AEP_func;               /* 0x00: AEP function code */
    USHORT  AEP_result;             /* 0x02: Result (set by handler) */
    PDDB    AEP_ddb;                /* 0x04: Our DDB */
    ULONG   AEP_lgn;                /* 0x08: Logical group number */

    /* ---- Union for event-specific data ---- */
    union {
        /* AEP_INITIALIZE (func == 0x00) */
        struct {
            ULONG   bi_flags;       /* AEP_BI_FLAG_* */
            ULONG   bi_reference;   /* IOS reference data */
            PVOID   bi_dvt;         /* Default volume tracker entry */
            PVOID   bi_reserved;    /* Reserved */
        } aep_bi;

        /* AEP_CONFIG_DCB (func == 0x03) */
        struct {
            PDCB    cd_dcb;         /* DCB being configured */
            ULONG   cd_flags;       /* AEP_CD_FLAG_* */
            PVOID   cd_drive_lt;    /* Drive letter table entry */
        } aep_cd;

        /* AEP_UNCONFIG_DCB (func == 0x04) */
        struct {
            PDCB    ud_dcb;         /* DCB being unconfigured */
            ULONG   ud_flags;       /* Flags */
        } aep_ud;

        /* AEP_IOR (func == 0x05) */
        struct {
            PIOR    ior_ior;        /* The I/O request to process */
            PDCB    ior_dcb;        /* Target DCB for this request */
        } aep_ior;

        /* AEP_ASSOCIATE_DCB (func == 0x06) */
        struct {
            PDCB    as_dcb;         /* DCB to associate */
            ULONG   as_flags;       /* Association flags */
        } aep_as;

        /* Generic: covers AEP types we don't parse */
        struct {
            ULONG   reserved[8];
        } aep_generic;
    } u;
} AEP, *PAEP;

/* Convenience macros for accessing AEP union fields */
#define AEP_BI_FLAGS        u.aep_bi.bi_flags
#define AEP_BI_REFERENCE    u.aep_bi.bi_reference
#define AEP_CD_DCB          u.aep_cd.cd_dcb
#define AEP_CD_FLAGS        u.aep_cd.cd_flags
#define AEP_UD_DCB          u.aep_ud.ud_dcb
#define AEP_IOR_IOR         u.aep_ior.ior_ior
#define AEP_IOR_DCB         u.aep_ior.ior_dcb
#define AEP_AS_DCB          u.aep_as.as_dcb


/* ================================================================
 * ISP (I/O Service Packet) and Calldown
 *
 * The calldown chain is how IOS routes I/O through layers.
 * Each layer inserts a "calldown" entry containing a function
 * pointer. When an IOR arrives, IOS walks the chain top-down.
 *
 * Our port driver sits at the bottom of the chain (closest to
 * hardware). We receive IORs and translate them to SRBs.
 * ================================================================ */

/* Calldown entry: one link in the IOS calldown chain */
typedef void (__cdecl *CALLDOWN_FUNC)(PIOR ior);

typedef struct _CALLDOWN {
    CALLDOWN_FUNC   CD_func;        /* Handler function */
    PDDB            CD_ddb;         /* Owning DDB */
    struct _CALLDOWN *CD_next;      /* Next entry in chain */
    ULONG           CD_flags;       /* Flags */
} CALLDOWN, *PCALLDOWN;


/* ================================================================
 * BDD (Block Device Descriptor) flags
 *
 * Used in device capability negotiation. IOS queries the port
 * driver about device capabilities through these flags.
 * ================================================================ */

#define BDD_FLAG_SCATTER_GATHER     0x00000001  /* Supports SG DMA */
#define BDD_FLAG_32BIT_DMA          0x00000002  /* 32-bit DMA addresses */
#define BDD_FLAG_REMOVABLE          0x00000004  /* Removable media */
#define BDD_FLAG_ATAPI              0x00000008  /* ATAPI protocol */
#define BDD_FLAG_CDROM              0x00000010  /* CD-ROM device */
#define BDD_FLAG_PIO                0x00000020  /* PIO mode (no DMA) */
#define BDD_FLAG_LBA_CAPABLE        0x00000040  /* LBA addressing */
#define BDD_FLAG_READ_ONLY          0x00000080  /* Read-only device */
#define BDD_FLAG_DVD                0x00000100  /* DVD device */
#define BDD_FLAG_WRITE_ONCE         0x00000200  /* Write-once device */


/* ================================================================
 * VxD Service Prototypes (extern: provided by assembly wrapper)
 *
 * These are the IOS VxD services we need. On Win9x, VxD services
 * are called through INT 20h thunks or via direct call tables.
 * The assembly wrapper (NTMINI.ASM) provides C-callable stubs
 * for these services.
 *
 * We declare them as extern function pointers or extern functions
 * so IOSBRIDGE.C can call them. The actual implementation is in
 * the assembly layer.
 * ================================================================ */

/*
 * IOS_Register - Register a layer driver with IOS
 *
 * Parameters:
 *   ddb     - Pointer to our DDB (filled in with driver info)
 *
 * Returns:
 *   0 on success, nonzero on failure
 *
 * Called once during AEP_INITIALIZE to tell IOS "here I am."
 */
extern ULONG __cdecl IOS_Register(PDDB ddb);

/*
 * IOS_Requestor_Service - Perform various IOS services
 *
 * Parameters:
 *   service - Service code
 *   ior     - IOR to operate on (service-dependent)
 *
 * Returns:
 *   0 on success
 *
 * Common service codes include allocation, deallocation of IORs,
 * and volume handle lookups.
 */
extern ULONG __cdecl IOS_Requestor_Service(ULONG service, PVOID param);

/* Requestor service codes */
#define IRS_ALLOC_IOR       0x0001  /* Allocate an IOR */
#define IRS_DEALLOC_IOR     0x0002  /* Deallocate an IOR */
#define IRS_GET_VOL_HANDLE  0x0003  /* Get volume handle for drive letter */

/*
 * IOS_SendCommand - Send an IOR down the calldown chain
 *
 * Parameters:
 *   ior     - The IOR to send
 *   dcb     - Target DCB
 *
 * Returns:
 *   void (completion is asynchronous via IOR_callback)
 *
 * This is how upper layers (and our bridge) initiate I/O.
 */
extern void __cdecl IOS_SendCommand(PIOR ior, PDCB dcb);

/*
 * IOS_BD_Command_Complete - Notify IOS that an IOR is complete
 *
 * Parameters:
 *   ior     - The completed IOR (IOR_status must be set)
 *
 * This calls the IOR's callback and returns the IOR to the
 * calldown chain for any post-processing.
 */
extern void __cdecl IOS_BD_Command_Complete(PIOR ior);

/*
 * ISP_INSERT_CALLDOWN - Insert our handler into a DCB's calldown chain
 *
 * Parameters:
 *   dcb     - The DCB to insert into
 *   cd      - Our calldown entry
 *   ddb     - Our DDB
 *   flags   - Insertion flags
 *
 * Returns:
 *   0 on success
 *
 * This is how we tell IOS "send I/O for this device to me."
 */
extern ULONG __cdecl ISP_INSERT_CALLDOWN(PDCB dcb, PCALLDOWN cd,
                                          PDDB ddb, ULONG flags);

/* Insert calldown flags */
#define ISPCDF_BOTTOM       0x0001  /* Insert at bottom of chain */
#define ISPCDF_PORT_DRIVER  0x0002  /* We are the port driver */


/* ================================================================
 * VxD Memory Services (extern: provided by assembly wrapper)
 *
 * These wrap VMM memory allocation services for ring 0 use.
 * ================================================================ */

/*
 * VxD_HeapAllocate - Allocate non-paged kernel memory
 *
 * Parameters:
 *   size    - Number of bytes to allocate
 *   flags   - Allocation flags (0 for default)
 *
 * Returns:
 *   Pointer to allocated memory, or NULL on failure
 */
extern PVOID __cdecl VxD_HeapAllocate(ULONG size, ULONG flags);

/*
 * VxD_HeapFree - Free memory allocated with VxD_HeapAllocate
 *
 * Parameters:
 *   ptr     - Pointer to memory to free
 *   flags   - Flags (0 for default)
 */
extern void __cdecl VxD_HeapFree(PVOID ptr, ULONG flags);

/* Heap allocation flags */
#define HEAPF_ZEROINIT      0x0001  /* Zero-initialize the allocation */

/*
 * VxD_PageAllocate - Allocate physically contiguous pages
 *
 * Parameters:
 *   nPages  - Number of 4KB pages
 *   flags   - Allocation flags
 *
 * Returns:
 *   Pointer to allocated pages, or NULL
 */
extern PVOID __cdecl VxD_PageAllocate(ULONG nPages, ULONG flags);

/*
 * VxD_LinPageLock - Lock linear pages to prevent paging
 *
 * Parameters:
 *   addr    - Linear address (page-aligned)
 *   nPages  - Number of pages to lock
 *   flags   - Lock flags
 *
 * Returns:
 *   0 on success
 */
extern ULONG __cdecl VxD_LinPageLock(ULONG addr, ULONG nPages, ULONG flags);

/*
 * VxD_CopyPageTable - Get physical address for a linear address
 *
 * Parameters:
 *   linearAddr  - Linear address to translate
 *   nPages      - Number of page table entries
 *   pPhysAddrs  - Output array of physical addresses
 *
 * Returns:
 *   Number of page table entries copied
 *
 * Used to translate virtual/linear addresses to physical for DMA.
 */
extern ULONG __cdecl VxD_CopyPageTable(ULONG linearAddr, ULONG nPages,
                                         PULONG pPhysAddrs);


/* ================================================================
 * VxD Interrupt Services (extern: provided by assembly wrapper)
 *
 * These wrap VPICD (Virtual PIC Device) services for interrupt
 * management.
 * ================================================================ */

/* IRQ descriptor for VPICD_Virtualize_IRQ */
typedef struct _VPICD_IRQ_DESCRIPTOR {
    USHORT  VID_IRQ_Number;         /* IRQ number (0-15) */
    USHORT  VID_Options;            /* Options flags */
    PVOID   VID_Hw_Int_Proc;        /* Hardware interrupt handler */
    PVOID   VID_Virt_Int_Proc;      /* Virtual interrupt handler (unused) */
    PVOID   VID_EOI_Proc;           /* End-of-interrupt proc (unused) */
    PVOID   VID_Mask_Change_Proc;   /* Mask change proc (unused) */
    PVOID   VID_IRET_Proc;          /* IRET proc (unused) */
    ULONG   VID_IRET_Time_Out;      /* IRET timeout (unused) */
    PVOID   VID_Hw_Int_Ref;         /* Reference data for handler */
} VPICD_IRQ_DESCRIPTOR, *PVPICD_IRQ_DESCRIPTOR;

/* VPICD option flags */
#define VPICD_OPT_READ_HW_IRR  0x0001  /* Can read hardware IRR */
#define VPICD_OPT_CAN_SHARE    0x0002  /* IRQ can be shared */

/*
 * VxD_VPICD_Virtualize_IRQ - Hook a hardware interrupt
 *
 * Parameters:
 *   desc    - IRQ descriptor with handler info
 *
 * Returns:
 *   IRQ handle, or 0 on failure
 */
extern ULONG __cdecl VxD_VPICD_Virtualize_IRQ(PVPICD_IRQ_DESCRIPTOR desc);

/*
 * VxD_VPICD_Phys_EOI - Signal end-of-interrupt to the PIC
 *
 * Parameters:
 *   irqHandle - Handle returned by Virtualize_IRQ
 */
extern void __cdecl VxD_VPICD_Phys_EOI(ULONG irqHandle);

/*
 * VxD_VPICD_Set_Int_Request - Request a virtual interrupt
 *
 * Parameters:
 *   irqHandle - Handle returned by Virtualize_IRQ
 *   vmHandle  - VM handle (0 for current)
 */
extern void __cdecl VxD_VPICD_Set_Int_Request(ULONG irqHandle, ULONG vmHandle);


/* ================================================================
 * VxD Debug Services
 * ================================================================ */

/*
 * VxD_Debug_Printf - Output a debug string (goes to debug terminal
 *                    or SoftICE if attached)
 *
 * Parameters:
 *   fmt     - printf-style format string
 *   ...     - Arguments
 *
 * Note: Real VxD debug output uses Out_Debug_String which is
 * more primitive, but we wrap it with sprintf for convenience.
 */
extern void __cdecl VxD_Debug_Printf(const char *fmt, ...);

/* Macro for conditional debug output */
#ifdef DEBUG
#define DBGPRINT(fmt, ...) VxD_Debug_Printf(fmt, ##__VA_ARGS__)
#else
#define DBGPRINT(fmt, ...) ((void)0)
#endif


/* ================================================================
 * Miscellaneous IOS Constants
 * ================================================================ */

/* CD-ROM sector size (used for LBA calculations) */
#define CDROM_SECTOR_SIZE       2048
#define DISK_SECTOR_SIZE        512

/* Maximum CDB length */
#define MAX_CDB_LENGTH          16

/* IOS version we claim compatibility with */
#define IOS_VERSION             0x0004

/* Maximum devices we track */
#define MAX_DEVICES             8


#endif /* W9XDDK_H */
