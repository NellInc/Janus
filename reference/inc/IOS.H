/****************************************************************************
*
*   IOS.H - Windows 98 DDK I/O Supervisor Header (C translation)
*
*   Reconstructed from the Windows 98 DDK (archive.org/details/tool-microsoft-ddk-wdk)
*   Source files: ILB.INC, ISP.INC, DRP.INC, AEP.INC, IOR.INC, IOS.INC,
*                 BLOCKDEV.INC, SGD.INC, DCB.H
*
*   Copyright (c) Microsoft Corporation 1992-1999. All rights reserved.
*   This reconstruction is for reference/research purposes only.
*
****************************************************************************/

#ifndef _IOS_H_
#define _IOS_H_

typedef unsigned char   UCHAR;
typedef unsigned short  USHORT;
typedef unsigned long   ULONG;
typedef unsigned char   BYTE;
typedef void*           PVOID;

/***************************************************************************
 *
 *  IOS Version and Device ID
 *
 ***************************************************************************/

#define IOS_Device_ID           0x0010
#define IOS_Major_Ver           0x03
#define IOS_Minor_Ver           0x0A

/***************************************************************************
 *
 *  ILB - IOS Linkage Block
 *
 *  Returned by IOS_Register. The driver uses ILB_service_rtn to call
 *  ISP services. ILB_internal_request is used to submit internal IOPs.
 *
 ***************************************************************************/

typedef struct _ILB {
    ULONG   ILB_service_rtn;            /* ISP service entry point          */
    ULONG   ILB_dprintf_rtn;            /* debug printf routine             */
    ULONG   ILB_Wait_10th_Sec;          /* wait 1/10 second routine         */
    ULONG   ILB_internal_request;       /* internal I/O request routine     */
    ULONG   ILB_io_criteria_rtn;        /* I/O criteria routine             */
    ULONG   ILB_int_io_criteria_rtn;    /* internal I/O criteria routine    */
    ULONG   ILB_dvt;                    /* pointer to DVT                   */
    ULONG   ILB_ios_mem_virt;           /* IOS memory virtualization        */
    ULONG   ILB_enqueue_iop;            /* enqueue IOP routine              */
    ULONG   ILB_dequeue_iop;            /* dequeue IOP routine              */
    ULONG   ILB_reserved_1;             /* reserved                         */
    ULONG   ILB_reserved_2;             /* reserved                         */
    USHORT  ILB_flags;                  /* flags                            */
    UCHAR   ILB_driver_numb;            /* driver number assigned by IOS    */
    UCHAR   ILB_reserved_3;             /* reserved                         */
} ILB, *PILB;

/***************************************************************************
 *
 *  DRP - Driver Registration Packet
 *
 *  Passed to IOS_Register to register a layered driver.
 *  DRP_eyecatch_str must be 'XXXXXXXX' (the EyeCatcher).
 *  DRP_LGN is the Load Group Number (layer in the stack).
 *  DRP_aer is the Async Event Routine (AEP handler).
 *  DRP_ilb is filled in by IOS with a pointer to the ILB.
 *
 ***************************************************************************/

typedef struct _DRP {
    BYTE    DRP_eyecatch_str[8];        /* must be "XXXXXXXX"               */
    ULONG   DRP_LGN;                    /* load group number (layer)        */
    ULONG   DRP_aer;                    /* async event routine              */
    ULONG   DRP_ilb;                    /* filled by IOS: ptr to ILB        */
    BYTE    DRP_ascii_name[16];         /* driver name string               */
    BYTE    DRP_revision;               /* driver revision                  */
    ULONG   DRP_feature_code;           /* feature code flags               */
    USHORT  DRP_if_requirements;        /* interface requirements           */
    BYTE    DRP_bus_type;               /* bus type                         */
    USHORT  DRP_reg_result;             /* registration result              */
    ULONG   DRP_reference_data;         /* reference data for AER           */
    BYTE    DRP_reserved1[2];           /* reserved                         */
    ULONG   DRP_reserved2;              /* reserved                         */
} DRP, *PDRP;

#define DRP_EyeCatcher  "XXXXXXXX"

/* DRP_feature_code flags */
#define DRP_FC_SCAN_DOWN            0x00000004
#define DRP_FC_IO_FOR_INQ_AEP       0x00000040
#define DRP_FC_HALF_SEC             0x00002000
#define DRP_FC_1_SEC                0x00002000
#define DRP_FC_2_SECS               0x00004000
#define DRP_FC_4_SECS               0x00008000
#define DRP_FC_DYNALOAD             0x00010000
#define DRP_FC_NEED_PRELOAD         0x00020000
#define DRP_FC_NEED_PRE_POST_LOAD   0x00040000

/* DRP_if_requirements flags */
#define DRP_IF_ISA                  0x0001
#define DRP_IF_EISA                 0x0002
#define DRP_IF_MCA                  0x0004
#define DRP_IF_STD                  0x00FF

/* DRP_bus_type values */
#define DRP_BT_ESDI                 0x00
#define DRP_BT_SCSI                 0x01
#define DRP_BT_FLOPPY               0x02
#define DRP_BT_SMART                0x03
#define DRP_BT_ABIOS                0x04

/* DRP_LGN (Load Group Number) bit positions */
#define DRP_IFS_BIT                 0x00
#define DRP_FSD_BIT                 0x02
#define DRP_FSD_EXT_1_BIT           0x03
#define DRP_FSD_EXT_2_BIT           0x04
#define DRP_VOLTRK_BIT              0x05
#define DRP_CLASS_DRV_BIT           0x06
#define DRP_TSD_BIT                 0x07
#define DRP_VSD_1_BIT               0x08
#define DRP_VSD_2_BIT               0x09
#define DRP_VSD_3_BIT               0x0A
#define DRP_SCSI_LAYER_BIT          0x0B
#define DRP_VSD_4_BIT               0x0C
#define DRP_VSD_5_BIT               0x0D
#define DRP_VSD_6_BIT               0x0E
#define DRP_VSD_7_BIT               0x0F
#define DRP_VSD_8_BIT               0x10
#define DRP_VSD_9_BIT               0x11
#define DRP_MISC_PD_BIT             0x13
#define DRP_NT_MPD_BIT              0x14
#define DRP_NT_PD_BIT               0x15
#define DRP_ESDI_PD_BIT             0x16
#define DRP_ESDIEMUL_PD_BIT         0x17
#define DRP_ABIOS_PD_BIT            0x19
#define DRP_NEC_FLOPPY_BIT          0x1B
#define DRP_SOC_SER_DRV_BIT         0x1D
#define DRP_SOC_DRV_BIT             0x1E
#define DRP_IOS_REG_BIT             0x1F
#define DRP_LGN_MAX                 0x1F

/* DRP_LGN layer masks */
#define DRP_IFS                 (1 << DRP_IFS_BIT)
#define DRP_FSD                 (1 << DRP_FSD_BIT)
#define DRP_FSD_EXT_1           (1 << DRP_FSD_EXT_1_BIT)
#define DRP_FSD_EXT_2           (1 << DRP_FSD_EXT_2_BIT)
#define DRP_VOLTRK              (1 << DRP_VOLTRK_BIT)
#define DRP_CLASS_DRV           (1 << DRP_CLASS_DRV_BIT)
#define DRP_TSD                 (1 << DRP_TSD_BIT)
#define DRP_VSD_1               (1 << DRP_VSD_1_BIT)
#define DRP_VSD_2               (1 << DRP_VSD_2_BIT)
#define DRP_VSD_3               (1 << DRP_VSD_3_BIT)
#define DRP_SCSI_LAYER          (1 << DRP_SCSI_LAYER_BIT)
#define DRP_VSD_4               (1 << DRP_VSD_4_BIT)
#define DRP_VSD_5               (1 << DRP_VSD_5_BIT)
#define DRP_VSD_6               (1 << DRP_VSD_6_BIT)
#define DRP_MISC_PD             (1 << DRP_MISC_PD_BIT)
#define DRP_NT_MPD              (1 << DRP_NT_MPD_BIT)
#define DRP_NT_PD               (1 << DRP_NT_PD_BIT)
#define DRP_ESDI_PD             (1 << DRP_ESDI_PD_BIT)
#define DRP_ESDIEMUL_PD         (1 << DRP_ESDIEMUL_PD_BIT)
#define DRP_IOS_REG             (1 << DRP_IOS_REG_BIT)

/* DRP_reg_result values */
#define DRP_REMAIN_RESIDENT         1
#define DRP_MINIMIZE                2
#define DRP_ABORT                   3
#define DRP_INVALID_LAYER           4

/***************************************************************************
 *
 *  ISP - IOS Service Provider function codes and structures
 *
 *  Called via ILB_service_rtn with an ISP packet.
 *
 ***************************************************************************/

/* ISP function codes */
#define ISP_CREATE_DDB              0
#define ISP_CREATE_DCB              1
#define ISP_CREATE_IOP              2
#define ISP_ALLOC_MEM               3
#define ISP_DEALLOC_MEM             4
#define ISP_INSERT_CALLDOWN         5
#define ISP_ASSOCIATE_DCB           6
#define ISP_GET_DCB                 7
#define ISP_GET_FIRST_NEXT_DCB      8
#define ISP_DEALLOC_DDB             9
#define ISP_DESTROY_DCB             10
#define ISP_QUERY_MATCHING_DCBS     11
#define ISP_QUERY_REMOVE_DCB        12
#define ISP_DEVICE_REMOVED          13
#define ISP_DEVICE_ARRIVED          14
#define ISP_DISASSOCIATE_DCB        15
#define ISP_DRIVE_LETTER_PICK       16
#define ISP_REGISTRY_READ           17
#define ISP_FIND_LDM_ENTRY          18
#define ISP_DELETE_LDM_ENTRY        19
#define ISP_BROADCAST_AEP           20

/* ISP base header */
typedef struct _ISP {
    USHORT  ISP_func;                   /* ISP function code                */
    USHORT  ISP_result;                 /* result code                      */
} ISP, *PISP;

/* ISP_CREATE_DDB */
typedef struct _ISP_ddb_create {
    ISP     ISP_ddb_hdr;
    USHORT  ISP_ddb_size;               /* size of DDB to create            */
    ULONG   ISP_ddb_ptr;                /* returned: ptr to created DDB     */
    BYTE    ISP_ddb_flags;              /* flags                            */
    BYTE    ISP_pad0;
} ISP_ddb_create;

#define ISP_DDB_FLAG_NEED_PHYS_CONTIG   0x01

/* ISP_CREATE_DCB */
typedef struct _ISP_dcb_create {
    ISP     ISP_dcb_hdr;
    USHORT  ISP_dcb_size;               /* size of DCB to create            */
    ULONG   ISP_dcb_ptr;                /* returned: ptr to created DCB     */
    BYTE    ISP_pad1[2];
} ISP_dcb_create;

/* ISP_CREATE_IOP */
typedef struct _ISP_IOP_create {
    ISP     ISP_i_c_hdr;
    USHORT  ISP_IOP_size;               /* size of IOP to create            */
    ULONG   ISP_delta_to_ior;           /* offset from IOP to IOR           */
    ULONG   ISP_IOP_ptr;                /* returned: ptr to created IOP     */
    BYTE    ISP_i_c_flags;
    BYTE    ISP_pad2;
} ISP_IOP_create;

/* ISP_ALLOC_MEM */
typedef struct _ISP_mem_alloc {
    ISP     ISP_mem_a_hdr;
    USHORT  ISP_mem_size;               /* size to allocate                 */
    ULONG   ISP_mem_ptr;                /* returned: ptr to memory          */
    USHORT  ISP_mem_type;               /* memory type                      */
    BYTE    ISP_mem_flags;              /* allocation flags                 */
    BYTE    ISP_pad3[3];
} ISP_mem_alloc;

#define ISP_M_FL_INTERRUPT_TIME     0x01
#define ISP_M_FL_MUST_SUCCEED       0x02
#define ISP_M_FL_EXTERNAL_IOP       0x04
#define ISP_M_FL_SMART_ALLOC        0x08
#define ISP_M_FL_PERSISTENT_IOP     0x10
#define ISP_M_FL_USE_SYSTEM_HEAP    0x20
#define ISP_M_FL_LOW_PRIORITY       0x40

/* ISP_DEALLOC_MEM */
typedef struct _ISP_mem_dealloc {
    ISP     ISP_mem_da_hdr;
    ULONG   ISP_mem_ptr_da;             /* ptr to memory to free            */
} ISP_mem_dealloc;

/* ISP_INSERT_CALLDOWN */
typedef struct _ISP_calldown_insert {
    ISP     ISP_i_cd_hdr;
    ULONG   ISP_i_cd_dcb;              /* DCB to insert calldown on        */
    ULONG   ISP_i_cd_req;              /* request handler address           */
    ULONG   ISP_i_cd_ddb;              /* DDB pointer                      */
    USHORT  ISP_i_cd_expan_len;        /* expansion area length            */
    ULONG   ISP_i_cd_flags;            /* demand flags                     */
    BYTE    ISP_i_cd_lgn;              /* load group number                */
    BYTE    ISP_pad4;
} ISP_calldown_insert;

/* ISP_ASSOCIATE_DCB */
typedef struct _ISP_dcb_associate {
    ISP     ISP_d_a_hdr;
    ULONG   ISP_d_a_dcb;               /* DCB to associate                 */
    BYTE    ISP_d_a_drive;              /* drive letter (0=A:, etc.)        */
    BYTE    ISP_d_a_flags;
    BYTE    ISP_pad5[2];
} ISP_dcb_associate;

#define ISP_D_A_FL_NOSHELLMSG       0x01

/* ISP_GET_DCB */
typedef struct _ISP_dcb_get {
    ISP     ISP_g_d_hdr;
    ULONG   ISP_g_d_dcb;               /* returned: DCB pointer            */
    ULONG   ISP_g_d_drive;             /* drive number to look up          */
} ISP_dcb_get;

/* ISP_GET_FIRST_NEXT_DCB */
typedef struct _ISP_GET_FRST_NXT_DCB {
    ISP     ISP_gfnd_hdr;
    ULONG   ISP_gfnd_dcb_offset;       /* offset for iteration             */
    ULONG   ISP_gfnd_found_dcb;        /* returned: found DCB              */
    BYTE    ISP_gfnd_dcb_type;          /* DCB type filter                  */
    BYTE    ISP_pad7[3];
} ISP_GET_FRST_NXT_DCB;

/* ISP_DEALLOC_DDB */
typedef struct _ISP_DDB_DEALLOC {
    ISP     ISP_ddb_d_hdr;
    ULONG   ISP_ddb_d_ddb;             /* DDB to deallocate                */
    ULONG   ISP_ddb_d_dvt;             /* DVT pointer                      */
} ISP_DDB_DEALLOC;

/* ISP_DESTROY_DCB */
typedef struct _ISP_DCB_DESTROY {
    ISP     ISP_dcb_d_hdr;
    ULONG   ISP_dcb_d_dcb;             /* DCB to destroy                   */
} ISP_DCB_DESTROY;

/* ISP_QUERY_MATCHING_DCBS */
typedef struct _ISP_QUERY_MATCH {
    ISP     ISP_q_match_hdr;
    ULONG   ISP_q_match_pdcb;          /* DCB to query                     */
    ULONG   ISP_q_match_drives;        /* returned: drive bitmask          */
} ISP_QUERY_MATCH;

/* ISP_QUERY_REMOVE_DCB */
typedef struct _ISP_QUERY_REMOVE {
    ISP     ISP_q_remove_hdr;
    ULONG   ISP_q_remove_pdcb;         /* DCB to query removal             */
    ULONG   ISP_q_remove_flags;
} ISP_QUERY_REMOVE;

#define ISP_Q_REMOVE_FL_MEDIA_ONLY  1

/* ISP_DEVICE_REMOVED */
typedef struct _ISP_DEV_REMOVED {
    ISP     ISP_d_removed_hdr;
    ULONG   ISP_dev_removed_pdcb;
    ULONG   ISP_dev_removed_flags;
} ISP_DEV_REMOVED;

#define ISP_DEV_REMOVED_FL_MEDIA_ONLY   1

/* ISP_DEVICE_ARRIVED */
typedef struct _ISP_DEV_ARRIVED {
    ISP     ISP_d_arrived_hdr;
    ULONG   ISP_dev_arrived_pdcb;
    ULONG   ISP_dev_arrived_flags;
} ISP_DEV_ARRIVED;

#define ISP_DEV_ARR_FL_MEDIA_ONLY   1

/* ISP_DISASSOCIATE_DCB */
typedef struct _ISP_DISASSOC_DCB {
    ISP     ISP_dis_dcb_hdr;
    ULONG   ISP_dis_dcb_drive;          /* drive letter to disassociate     */
} ISP_DISASSOC_DCB;

/* ISP_DRIVE_LETTER_PICK */
typedef struct _ISP_pick_drive_letter {
    ISP     ISP_p_d_l_hdr;
    ULONG   ISP_p_d_l_pdcb;
    BYTE    ISP_p_d_l_letter[2];
    BYTE    ISP_p_d_l_flags;
    BYTE    ISP_p_d_l_pad;
} ISP_pick_drive_letter;

#define ISP_PDL_FL_USER_ONLY        0x01
#define ISP_PDL_FL_OK_INVALID_RM    0x02
#define ISP_PDL_FL_OK_RM_CD         0x04
#define ISP_PDL_FL_USE_RANGE        0x08
#define ISP_PDL_FL_RELEASE          0x10
#define ISP_PDL_FL_IS_FLOPPY        0x20

/* ISP_REGISTRY_READ */
typedef struct _ISP_read_registry {
    ISP     ISP_r_r_hdr;
    ULONG   ISP_r_r_dcb;
    ULONG   ISP_r_r_string;
    ULONG   ISP_r_r_data_type;
    ULONG   ISP_r_r_data_size;
    ULONG   ISP_r_r_buffer;
} ISP_read_registry;

/* ISP_FIND_LDM_ENTRY */
typedef struct _ISP_FIND_LDM {
    ISP     ISP_fldm_hdr;
    ULONG   ISP_fldm_drive;
    ULONG   ISP_fldm_pldm;
} ISP_FIND_LDM;

/* ISP_BROADCAST_AEP */
typedef struct _ISP_AEP_BROADCAST {
    ISP     ISP_baep_hdr;
    ULONG   ISP_baep_paep;             /* pointer to AEP to broadcast      */
} ISP_AEP_BROADCAST;

/***************************************************************************
 *
 *  AEP - Async Event Packet
 *
 *  Delivered to the driver's AER (Async Event Routine, DRP_aer).
 *
 ***************************************************************************/

#define AEP_PrivateLen              32

/* AEP function codes */
#define AEP_INITIALIZE              0
#define AEP_SYSTEM_CRIT_SHUTDOWN    1
#define AEP_BOOT_COMPLETE           2
#define AEP_CONFIG_DCB              3
#define AEP_UNCONFIG_DCB            4
#define AEP_IOP_TIMEOUT             5
#define AEP_DEVICE_INQUIRY          6
#define AEP_HALF_SEC                7
#define AEP_1_SEC                   8
#define AEP_2_SECS                  9
#define AEP_4_SECS                  10
#define AEP_DBG_DOT_CMD             11
#define AEP_ASSOCIATE_DCB           12
#define AEP_REAL_MODE_HANDOFF       13
#define AEP_SYSTEM_SHUTDOWN         14
#define AEP_UNINITIALIZE            15
#define AEP_DCB_LOCK                16
#define AEP_MOUNT_NOTIFY            17
#define AEP_CREATE_VRP              18
#define AEP_DESTROY_VRP             19
#define AEP_REFRESH_DRIVE           20
#define AEP_PEND_UNCONFIG_DCB       21
#define AEP_1E_VEC_UPDATE           22
#define AEP_CHANGE_RPM              23
#define AEP_QUERY_RPM               24
#define AEP_CONFIG_3MODE            25
#define AEP_SYSTEM_REBOOT           26
#define AEP_QUERY_CD_SPINDOWN       27
#define AEP_FUNC_POWER_MESSAGE      28
#define AEP_FUNC_ACPI_REQUEST       29
#define AEP_MAX_FUNC                29

/* AEP result codes */
#define AEP_SUCCESS                 0
#define AEP_DEREGISTER              1
#define AEP_FAILURE                 (-1)

/* AEP header (common to all AEP types) */
typedef struct _AEPHDR {
    USHORT  AEP_func;                   /* AEP function code                */
    USHORT  AEP_result;                 /* result code                      */
    ULONG   AEP_ddb;                    /* DDB pointer                      */
    BYTE    AEP_lgn;                    /* load group number                */
    BYTE    AEP_align[3];               /* alignment padding                */
} AEPHDR, *PAEPHDR;

/* AEP_INITIALIZE */
typedef struct _AEP_bi_init {
    AEPHDR  AEP_bi_i_hdr;
    ULONG   AEP_bi_reference_data;
    BYTE    AEP_bi_flags;
    BYTE    AEP_bi_i_max_target;
    BYTE    AEP_bi_i_max_lun;
    ULONG   AEP_bi_i_dcb;
    ULONG   AEP_bi_i_hdevnode;
    ULONG   AEP_bi_i_regkey;
    BYTE    AEP_bi_i_align;
} AEP_bi_init;

#define AEP_BI_FL_SCSI_SCAN_DOWN        0x01
#define AEP_BI_FL_SEND_CONFIG_AGAIN     0x02

/* AEP_DEVICE_INQUIRY */
typedef struct _AEP_inquiry_device {
    AEPHDR  AEP_i_d_hdr;
    ULONG   AEP_i_d_dcb;               /* DCB being inquired               */
} AEP_inquiry_device;

#define AEP_NO_INQ_DATA             1
#define AEP_NO_MORE_DEVICES         2

/* AEP_CONFIG_DCB */
typedef struct _AEP_dcb_config {
    AEPHDR  AEP_d_c_hdr;
    ULONG   AEP_d_c_dcb;               /* DCB being configured             */
} AEP_dcb_config;

/* AEP_UNCONFIG_DCB */
typedef struct _AEP_dcb_unconfig {
    AEPHDR  AEP_d_u_hdr;
    ULONG   AEP_d_u_dcb;               /* DCB being unconfigured           */
} AEP_dcb_unconfig;

/* AEP_ASSOCIATE_DCB */
typedef struct _AEP_assoc_dcb {
    AEPHDR  AEP_a_d_hdr;
    ULONG   AEP_a_d_pdcb;
    ULONG   AEP_a_d_drives;            /* bitmask of associated drives     */
} AEP_assoc_dcb;

/* AEP_MOUNT_NOTIFY */
typedef struct _AEP_mnt_notify {
    AEPHDR  AEP_m_n_hdr;
    ULONG   AEP_m_n_pvrp;
    ULONG   AEP_m_n_drivemap;
    ULONG   AEP_m_n_drive;
    ULONG   AEP_m_n_effective_drive;
    ULONG   AEP_m_n_actual_drive;
} AEP_mnt_notify;

/* AEP_CREATE_VRP / AEP_DESTROY_VRP */
typedef struct _AEP_vrp_create_destroy {
    AEPHDR  AEP_v_cd_hdr;
    ULONG   AEP_v_cd_pvrp;
    ULONG   AEP_v_cd_drive;
} AEP_vrp_create_destroy;

/* AEP_DCB_LOCK */
typedef struct _AEP_lock_dcb {
    AEPHDR  AEP_d_l_hdr;
    ULONG   AEP_d_l_pdcb;
    ULONG   AEP_d_l_drives;
    BYTE    AEP_d_l_designtr;
    BYTE    AEP_d_l_align[3];
} AEP_lock_dcb;

/* AEP_IOP_TIMEOUT */
typedef struct _AEP_iop_timeout_occurred {
    AEPHDR  AEP_i_t_o_hdr;
    ULONG   AEP_i_t_o_iop;             /* timed-out IOP                    */
} AEP_iop_timeout_occurred;

/* AEP_FUNC_ACPI_REQUEST */
typedef struct _AEP_ACPI_REQUEST {
    AEPHDR  AEP_hdr;
    ULONG   aep_acpi_ulSubFunction;
    ULONG   aep_acpi_pDdb;
    ULONG   aep_acpi_pDcb;
    ULONG   aep_acpi_ulInputLength;
    ULONG   aep_acpi_pbInputBuffer;
    ULONG   aep_acpi_ulOutputLength;
    ULONG   aep_acpi_pbOutputBuffer;
} AEP_ACPI_REQUEST;

#define AEP_SUBF_ACPI_SET_TASK_FILES    1

/***************************************************************************
 *
 *  SGD - Scatter/Gather Descriptor
 *
 ***************************************************************************/

typedef struct _SGD {
    ULONG   SG_buff_ptr;                /* buffer pointer (lin or phys)     */
    ULONG   SG_buff_size;               /* buffer size in bytes             */
} SGD, *PSGD;

/***************************************************************************
 *
 *  IOR - I/O Request
 *
 *  The core request packet passed through the IOS calldown chain.
 *
 ***************************************************************************/

typedef struct _IOR {
    ULONG   IOR_next;                   /* link to next IOR                 */
    USHORT  IOR_func;                   /* function code                    */
    USHORT  IOR_status;                 /* status/result                    */
    ULONG   IOR_flags;                  /* flags                            */
    ULONG   IOR_callback;               /* completion callback              */
    ULONG   IOR_start_addr[2];          /* starting sector (64-bit)         */
    ULONG   IOR_xfer_count;             /* transfer count (sectors)         */
    ULONG   IOR_buffer_ptr;             /* data buffer pointer              */
    ULONG   IOR_private_client;         /* private: client use              */
    ULONG   IOR_private_IOS;            /* private: IOS use                 */
    ULONG   IOR_private_port;           /* private: port driver use         */
    BYTE    IOR_requestor_usage[20];    /* requestor-specific union area    */
    ULONG   IOR_req_req_handle;         /* requestor request handle         */
    ULONG   IOR_req_vol_handle;         /* requestor volume handle          */
    ULONG   IOR_sgd_lin_phys;           /* ptr to SGD array                 */
    BYTE    IOR_num_sgds;               /* number of SGDs                   */
    BYTE    IOR_vol_designtr;           /* volume designator                */
    USHORT  IOR_ios_private_1;          /* IOS private                      */
    ULONG   IOR_reserved_2[2];          /* reserved                         */
} IOR, *PIOR;

/* IOR function codes */
#define IOR_READ                    0x00
#define IOR_WRITE                   0x01
#define IOR_VERIFY                  0x02
#define IOR_CANCEL                  0x03
#define IOR_WRITEV                  0x04
#define IOR_MEDIA_CHECK             0x05
#define IOR_MEDIA_CHECK_RESET       0x06
#define IOR_LOAD_MEDIA              0x07
#define IOR_EJECT_MEDIA             0x08
#define IOR_LOCK_MEDIA              0x09
#define IOR_UNLOCK_MEDIA            0x0A
#define IOR_REQUEST_SENSE           0x0B
#define IOR_COMPUTE_GEOM            0x0C
#define IOR_GEN_IOCTL               0x0D
#define IOR_FORMAT                  0x0E
#define IOR_SCSI_PASS_THROUGH       0x0F
#define IOR_CLEAR_QUEUE             0x10
#define IOR_DOS_RESET               0x11
#define IOR_SCSI_REQUEST            0x12
#define IOR_SET_WRITE_STATUS        0x22
#define IOR_RESTART_QUEUE           0x23
#define IOR_ABORT_QUEUE             0x24
#define IOR_SPIN_DOWN               0x25
#define IOR_SPIN_UP                 0x26
#define IOR_FLUSH_DRIVE             0x27
#define IOR_FLUSH_DRIVE_AND_DISCARD 0x28
#define IOR_FSD_EXT                 0x29
#define IOR_FLOPTICAL_MODE_SENSE    0x2A
#define IOR_IDE_PASS_THROUGH        0x2B
#define IOR_FLOPTICAL_INQUIRY       0x2C
#define IOR_ENUMERATE_DEVICES       0x2D
#define IOR_ASYNCHRONOUS_DRIVE_SPINUP 0x2E

/* IOR status codes */
#define IORS_SUCCESS                0x00
#define IORS_SUCCESS_WITH_RETRY     0x01
#define IORS_SUCCESS_WITH_ECC       0x02
#define IORS_INVALID_SECTOR         0x10
#define IORS_CANCELED               0x11
#define IORS_CMD_IN_PROGRESS        0x12
#define IORS_INVALID_CMD_PTR        0x13
#define IORS_MEDIA_ERROR            0x14
#define IORS_DEVICE_ERROR           0x15
#define IORS_INVALID_COMMAND        0x16
#define IORS_NO_DEVICE              0x17
#define IORS_NO_MEDIA               0x18
#define IORS_UNCERTAIN_MEDIA        0x19
#define IORS_UNREC_ERROR            0x1A
#define IORS_HW_FAILURE             0x1B
#define IORS_UNFORMATTED_MEDIA      0x1C
#define IORS_MEMORY_ERROR           0x1D
#define IORS_TIME_OUT               0x1E
#define IORS_WRITE_PROTECT          0x1F
#define IORS_NOT_READY              0x20
#define IORS_BUSY                   0x21
#define IORS_VOL_NOT_LOCKED         0x22
#define IORS_VOL_LOCKED             0x23
#define IORS_VOL_NOT_REMOVABLE      0x24
#define IORS_VOL_IN_USE             0x25
#define IORS_LOCK_COUNT_EXCEEDED    0x26
#define IORS_VALID_EJECT_FAILED     0x27
#define IORS_ILLEGAL_ACCESS_MODE    0x28
#define IORS_LOCK_VIOLATION         0x29
#define IORS_WRONG_MEDIA            0x2A
#define IORS_OUT_OF_SPACE           0x2B
#define IORS_BIG_IO_BREAKUP_FAILED  0x2C
#define IORS_ERROR_DESIGNTR         0x10
#define IORS_INVALID_PARM           IORS_ERROR_DESIGNTR

/* IOR flags */
#define IORF_HIGH_PRIORITY          0x00000001
#define IORF_SCATTER_GATHER         0x00000002
#define IORF_DONT_CACHE             0x00000004
#define IORF_BYPASS_VOLTRK          0x00000008
#define IORF_16BIT_IOCTL            0x00000010
#define IORF_SWAPPER_IO             0x00000020
#define IORF_DOUBLE_BUFFER          0x00000040
#define IORF_SYNC_CMD_DONE          0x00000080
#define IORF_SYNC_COMMAND           0x00000100
#define IORF_CHAR_COMMAND           0x00000200
#define IORF_VERSION_002            0x00000400
#define IORF_BYPASS_QUEUE           0x00000800
#define IORF_BLOCKDEV_EMULATE       0x00001000
#define IORF_POSTPONED_VOL_OPS      0x00002000
#define IORF_INHIBIT_GEOM_RECOMPUTE 0x00004000
#define IORF_SRB_VALID              0x00008000
#define IORF_BYPASS_A_B             0x00010000
#define IORF_QUIET_VOLTRK           0x00020000
#define IORF_AUDIO_DATA_READ        0x00040000
#define IORF_LOGICAL_START_SECTOR   0x00080000
#define IORF_PARTITION_BIAS_ADDED   0x00100000
#define IORF_DATA_IN                0x00200000
#define IORF_DATA_OUT               0x00400000
#define IORF_VOL_RETRY              0x00800000
#define IORF_NO_COMPRESS            0x01000000
#define IORF_DIRECT_IO              0x02000000
#define IORF_PHYS_SGDS              0x04000000
#define IORF_IO_TOO_BIG             0x08000000
#define IORF_WIN32                  0x10000000
#define IORF_CHAR_DEVICE            0x20000000
#define IORF_PHYS_CMD               0x40000000
#define IORF_IDE_RESERVED           0x80000000

/***************************************************************************
 *
 *  IOS_Register calling convention
 *
 *  VxD calls IOS_Register via VxDCall:
 *      push  offset DRP_packet     ; pointer to DRP structure
 *      VxDCall IOS_Register
 *
 *  On return:
 *      DRP_ilb     -> pointer to ILB (if DRP_reg_result == DRP_REMAIN_RESIDENT)
 *      DRP_reg_result is set to one of: DRP_REMAIN_RESIDENT, DRP_MINIMIZE,
 *                                       DRP_ABORT, DRP_INVALID_LAYER
 *
 *  The driver then uses ILB_service_rtn to call ISP functions:
 *      lea  esi, ISP_packet
 *      call [ILB.ILB_service_rtn]
 *
 ***************************************************************************/

#define IOSEA_LOCK_THREAD           (1 << 8)

/***************************************************************************
 *
 *  IOS Service Table (VxD service ordinals)
 *
 ***************************************************************************/

/* IOS VxD services (in service table order) */
/*  0 */ /* IOS_Get_Version              */
/*  1 */ /* IOS_BD_Register_Device       */
/*  2 */ /* IOS_Find_Int13_Drive         */
/*  3 */ /* IOS_Get_Device_List          */
/*  4 */ /* IOS_SendCommand              */
/*  5 */ /* IOS_BD_Command_Complete      */
/*  6 */ /* IOS_Synchronous_Command      */
/*  7 */ /* IOS_Register                 */
/*  8 */ /* IOS_Requestor_Service        */
/*  9 */ /* IOS_Exclusive_Access         */
/* 10 */ /* IOS_Send_Next_Command        */
/* 11 */ /* IOS_Set_Async_Time_Out       */
/* 12 */ /* IOS_Signal_Semaphore_No_Switch */
/* 13 */ /* IOSIdleStatus                */
/* 14 */ /* IOSMapIORSToI24              */
/* 15 */ /* IOSMapIORSToI21              */
/* 16 */ /* PrintLog                     */
/* 17 */ /* IOS_deregister               */
/* 18 */ /* IOS_wait                     */
/* 19 */ /* _IOS_SpinDownDrives          */
/* 20 */ /* _IOS_query_udf_mount         */

/***************************************************************************
 *
 *  DCB Calldown Entry (from DCB.H)
 *
 ***************************************************************************/

#define DCB_cd_table_depth          10

typedef struct _DCB_cd_entry {
    PVOID   DCB_cd_io_address;          /* address of request routine       */
    ULONG   DCB_cd_flags;               /* demand bits                      */
    ULONG   DCB_cd_ddb;                 /* driver's DDB pointer             */
    ULONG   DCB_cd_next;                /* pointer to next cd entry         */
    USHORT  DCB_cd_expan_off;           /* offset of expansion area         */
    UCHAR   DCB_cd_layer_flags;         /* flags for layer's use            */
    UCHAR   DCB_cd_lgn;                 /* load group number                */
} DCB_cd_entry, *pDCB_cd_entry;

/***************************************************************************
 *
 *  DCB - Device Control Block (from DCB.H)
 *
 *  Common fields, then physical device extension, then type-specific.
 *  Total size for physical DCB: 299 bytes.
 *
 ***************************************************************************/

/* DCB device type codes */
#define DCB_type_disk               0x00
#define DCB_type_tape               0x01
#define DCB_type_printer            0x02
#define DCB_type_processor          0x03
#define DCB_type_worm               0x04
#define DCB_type_cdrom              0x05
#define DCB_type_scanner            0x06
#define DCB_type_optical_memory     0x07
#define DCB_type_changer            0x08
#define DCB_type_comm               0x09
#define DCB_type_floppy             0x0A

/* DCB bus type codes */
#define DCB_BUS_ESDI                0x00
#define DCB_BUS_SCSI                0x01
#define DCB_BUS_NEC                 0x02
#define DCB_BUS_SMART               0x03
#define DCB_BUS_ABIOS               0x04

/* DCB_device_flags */
#define DCB_DEV_INT13_DRIVE         0x00000001
#define DCB_DEV_WRITEABLE           0x00000002
#define DCB_DEV_REMOVABLE           0x00000004
#define DCB_DEV_REMOTE              0x00000008
#define DCB_DEV_SERIAL_CMD          0x00000010
#define DCB_DEV_CACHE               0x00000020
#define DCB_DEV_I13_IN_PROGRESS     0x00000040
#define DCB_CHAR_DEVICE             0x00000080
#define DCB_DEV_PAGING_DEVICE       0x00000100
#define DCB_DEV_SSTOR_VOL           0x00000200
#define DCB_DEV_MUST_CONFIGURE      0x00000400
#define DCB_DEV_ROM_CFG_REQ         0x00000800
#define DCB_DEV_UNCERTAIN_MEDIA     0x00001000
#define DCB_DEV_RMM                 0x00002000
#define DCB_DEV_LOGICAL             0x00004000
#define DCB_DEV_PHYSICAL            0x00008000
#define DCB_DEV_SYNC_MED_CHG_SUPPORT  0x00010000
#define DCB_DEV_ASYNC_MED_CHG_SUPPORT 0x00020000
#define DCB_DEV_INHIBIT_SW_TIMED_SUPPORT 0x00040000
#define DCB_DEV_INVALID_GEOM        0x00080000
#define DCB_DEV_MEDIA_CHANGED       0x00100000
#define DCB_DEV_QUE_DESCEND         0x00200000
#define DCB_DEV_REAL_MODE           0x00400000
#define DCB_DEV_SINGLE_FLOPPY       0x00800000
#define DCB_DEV_TSD_PROCESSED       0x01000000
#define DCB_DEV_RMM_INT13           0x02000000
#define DCB_DEV_REMOVAL_PENDING     0x04000000
#define DCB_DEV_NO_ASPICAM          0x08000000
#define DCB_DEV_INT13_CONFIGURED    0x10000000
#define DCB_DEV_IO_ACTIVE           0x20000000
#define DCB_DEV_SPUN_DOWN           0x40000000
#define DCB_DEV_SPINDOWN_SUPPORTED  0x80000000

/* DCB_device_flags2 */
#define DCB_DEV2_THREAD_LOCK        0x00000001
#define DCB_DEV2_ATAPI_DEVICE       0x00000002
#define DCB_DEV2_DMF_DISK           0x00000004
#define DCB_DEV2_I13_COPY_PROT      0x00000008
#define DCB_DEV2_FAIL_FORMAT        0x00000010
#define DCB_DEV2_FLOPTICAL          0x00000020
#define DCB_DEV2_IDE_FLOPTICAL      0x00000040

/* CD-ROM specific flags (DCB_cd_device_flags) */
#define DCB_CD_ISDVD                0x0100
#define DCB_CD_ISMMC                0x0200
#define DCB_CD_ISAUDIO              0x0400
#define DCB_CD_ISDVD_MEDIA          0x0800
#define DCB_CD_ISDA                 0x1000
#define DCB_CD_ISMESN               0x2000

/* DCB demand bits (DCB_cd_flags / DCB_dmd_flags) */
#define DCB_dmd_srb_cdb             0x00000001
#define DCB_dmd_logical             0x00000004
#define DCB_dmd_physical            0x00000008
#define DCB_dmd_small_memory        0x00000010
#define DCB_dmd_not_512             0x00000100
#define DCB_dmd_word_align          0x00000200
#define DCB_dmd_dword_align         0x00000400
#define DCB_dmd_phys_sgd            0x00000800
#define DCB_dmd_phys_sgd_ptr        0x00001000
#define DCB_dmd_do_a_b_toggling     0x00002000
#define DCB_dmd_query_remov         0x00004000
#define DCB_dmd_request_sns         0x00008000
#define DCB_dmd_lock_unlock_media   0x00010000
#define DCB_dmd_load_eject_media    0x00020000
#define DCB_dmd_clear_media_chg     0x00040000
#define DCB_dmd_serialize           0x00080000
#define DCB_dmd_prot_mode           0x00100000
#define DCB_dmd_no_xclusive         0x00200000
#define DCB_dmd_pageability         0x00400000
#define DCB_dmd_preload             0x00800000

/***************************************************************************
 *
 *  BlockDev compatibility structures (from BLOCKDEV.INC)
 *
 ***************************************************************************/

#define BD_Major_Ver                3
#define BD_Minor_Ver                0x0A

typedef struct _BlockDev_Device_Descriptor {
    ULONG   BDD_Next;
    BYTE    BDD_Major_Version;
    BYTE    BDD_Minor_Version;
    BYTE    BDD_Device_Type;
    BYTE    BDD_Int_13h_Number;
    ULONG   BDD_Flags;
    ULONG   BDD_Name_Ptr;
    ULONG   BDD_Max_Sector[2];
    ULONG   BDD_Sector_Size;
    ULONG   BDD_Num_Heads;
    ULONG   BDD_Num_Cylinders;
    ULONG   BDD_Num_Sec_Per_Track;
    ULONG   BDD_Sync_Cmd_Proc;
    ULONG   BDD_Command_Proc;
    ULONG   BDD_Hw_Int_Proc;
    BYTE    BDD_Reserved_BlockDev[0x30];
} BlockDev_Device_Descriptor;

/* BlockDev command codes */
#define BDC_Read                    0
#define BDC_Write                   1
#define BDC_Verify                  2
#define BDC_Cancel                  3
#define BDC_Lock_Volume             0x09
#define BDC_Unlock_Volume           0x0A

/* BlockDev device types */
#define BDT_360K_5_Inch_Floppy     0x00
#define BDT_1200K_5_Inch_Floppy    0x01
#define BDT_720K_3_Inch_Floppy     0x02
#define BDT_Fixed_Disk             0x05
#define BDT_Tape_Drive             0x06
#define BDT_Other                  0x07

#endif /* _IOS_H_ */
