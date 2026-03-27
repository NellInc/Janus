/*
 * NTKRNL.H - NT Kernel Types and Structures for Win9x Shim Layer
 *
 * Defines every NT kernel type needed to host a Windows 2000/XP
 * WDM driver (specifically atapi.sys with 119 imports) inside a
 * Windows 9x VxD environment.
 *
 * This is Approach 3: the moonshot. A complete ntoskrnl.exe +
 * HAL.dll + WMILIB.SYS shim layer for running NT WDM drivers
 * on Win9x.
 *
 * AUTHOR: Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#ifndef _NTKRNL_H_
#define _NTKRNL_H_

/* ================================================================
 * COMPILER CONFIGURATION
 * ================================================================ */

/* Calling conventions for kernel-mode functions */
#ifdef __WATCOMC__
  #define NTAPI       __stdcall
  #define FASTCALL    __fastcall
  #define CDECL       __cdecl
#else /* GCC */
  #define NTAPI       __attribute__((stdcall))
  #define FASTCALL    __attribute__((fastcall))
  #define CDECL       __attribute__((cdecl))
#endif

/* Packing for structures that must match NT binary layout */
#pragma pack(push, 1)

/* ================================================================
 * PRIMITIVE TYPES
 * ================================================================ */

typedef unsigned char       UCHAR;
typedef unsigned short      USHORT;
typedef unsigned long       ULONG;
typedef unsigned long       DWORD;
typedef long                LONG;
typedef long long           LONGLONG;
typedef unsigned long long  ULONGLONG;
typedef short               SHORT;
typedef int                 BOOLEAN;
typedef int                 BOOL;
typedef char                CHAR;
typedef unsigned char       BYTE;
typedef void                VOID;

typedef void               *PVOID;
typedef char               *PCHAR;
typedef unsigned char      *PUCHAR;
typedef unsigned short     *PUSHORT;
typedef unsigned long      *PULONG;
typedef long               *PLONG;
typedef BOOLEAN            *PBOOLEAN;
typedef const char         *PCSTR;
typedef const void         *PCVOID;

typedef ULONG               SIZE_T;
typedef ULONG              *PSIZE_T;

typedef ULONG               ULONG_PTR;
typedef LONG                LONG_PTR;

typedef unsigned short      WCHAR;
typedef WCHAR              *PWCHAR;
typedef WCHAR              *PWSTR;
typedef const WCHAR        *PCWSTR;

/* Handle types */
typedef PVOID               HANDLE;
typedef HANDLE             *PHANDLE;

/* NTSTATUS */
typedef LONG                NTSTATUS;

/* KIRQL - Interrupt Request Level */
typedef UCHAR               KIRQL;
typedef KIRQL              *PKIRQL;

/* KSPIN_LOCK */
typedef ULONG               KSPIN_LOCK;
typedef KSPIN_LOCK         *PKSPIN_LOCK;

/* KPROCESSOR_MODE */
typedef CHAR                KPROCESSOR_MODE;
#define KernelMode          0
#define UserMode            1

/* ACCESS_MASK */
typedef ULONG               ACCESS_MASK;

/* ================================================================
 * LARGE_INTEGER / PHYSICAL_ADDRESS
 * ================================================================ */

typedef union _LARGE_INTEGER {
    struct {
        ULONG LowPart;
        LONG  HighPart;
    } u;
    struct {
        ULONG LowPart;
        LONG  HighPart;
    };
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        ULONG LowPart;
        ULONG HighPart;
    } u;
    struct {
        ULONG LowPart;
        ULONG HighPart;
    };
    ULONGLONG QuadPart;
} ULARGE_INTEGER, *PULARGE_INTEGER;

typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

/* ================================================================
 * STATUS CODES
 * ================================================================ */

#define STATUS_SUCCESS                  ((NTSTATUS)0x00000000L)
#define STATUS_PENDING                  ((NTSTATUS)0x00000103L)
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#define STATUS_UNSUCCESSFUL             ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED          ((NTSTATUS)0xC0000002L)
#define STATUS_INVALID_PARAMETER        ((NTSTATUS)0xC000000DL)
#define STATUS_NO_SUCH_DEVICE           ((NTSTATUS)0xC000000EL)
#define STATUS_NO_MEMORY                ((NTSTATUS)0xC0000017L)
#define STATUS_INSUFFICIENT_RESOURCES   ((NTSTATUS)0xC000009AL)
#define STATUS_DEVICE_DOES_NOT_EXIST    ((NTSTATUS)0xC00000C0L)
#define STATUS_OBJECT_NAME_NOT_FOUND    ((NTSTATUS)0xC0000034L)
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010L)
#define STATUS_NOT_SUPPORTED            ((NTSTATUS)0xC00000BBL)
#define STATUS_CANCELLED                ((NTSTATUS)0xC0000120L)
#define STATUS_TIMEOUT                  ((NTSTATUS)0x00000102L)
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xC0000016L)
#define STATUS_DEVICE_NOT_CONNECTED     ((NTSTATUS)0xC000009DL)
#define STATUS_CONFLICTING_ADDRESSES    ((NTSTATUS)0xC0000018L)

#define NT_SUCCESS(Status)      ((NTSTATUS)(Status) >= 0)
#define NT_INFORMATION(Status)  ((ULONG)(Status) >> 30 == 1)
#define NT_WARNING(Status)      ((ULONG)(Status) >> 30 == 2)
#define NT_ERROR(Status)        ((ULONG)(Status) >> 30 == 3)

/* ================================================================
 * TRUE / FALSE / NULL
 * ================================================================ */

#ifndef TRUE
#define TRUE    1
#endif
#ifndef FALSE
#define FALSE   0
#endif
#ifndef NULL
#define NULL    ((void*)0)
#endif

/* ================================================================
 * UNICODE_STRING / ANSI_STRING
 * ================================================================ */

typedef struct _UNICODE_STRING {
    USHORT  Length;          /* Length in bytes (not chars), not including terminator */
    USHORT  MaximumLength;  /* Total buffer size in bytes */
    PWSTR   Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef const UNICODE_STRING *PCUNICODE_STRING;

typedef struct _STRING {
    USHORT  Length;
    USHORT  MaximumLength;
    PCHAR   Buffer;
} STRING, *PSTRING, ANSI_STRING, *PANSI_STRING;

typedef const STRING *PCSTRING;

/* ================================================================
 * OBJECT_ATTRIBUTES
 * ================================================================ */

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

#define OBJ_CASE_INSENSITIVE    0x00000040L
#define OBJ_KERNEL_HANDLE       0x00000200L

#define InitializeObjectAttributes(p, n, a, r, s) {  \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES);          \
    (p)->RootDirectory = (r);                         \
    (p)->Attributes = (a);                            \
    (p)->ObjectName = (n);                            \
    (p)->SecurityDescriptor = (s);                    \
    (p)->SecurityQualityOfService = NULL;             \
}

/* ================================================================
 * IO_STATUS_BLOCK
 * ================================================================ */

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID    Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

/* ================================================================
 * LIST_ENTRY (doubly-linked list)
 * ================================================================ */

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

#define InitializeListHead(ListHead) {  \
    (ListHead)->Flink = (ListHead);     \
    (ListHead)->Blink = (ListHead);     \
}

#define IsListEmpty(ListHead) \
    ((ListHead)->Flink == (ListHead))

#define InsertTailList(ListHead, Entry) {    \
    PLIST_ENTRY _EX_Blink;                   \
    _EX_Blink = (ListHead)->Blink;           \
    (Entry)->Flink = (ListHead);             \
    (Entry)->Blink = _EX_Blink;             \
    _EX_Blink->Flink = (Entry);             \
    (ListHead)->Blink = (Entry);             \
}

#define InsertHeadList(ListHead, Entry) {    \
    PLIST_ENTRY _EX_Flink;                   \
    _EX_Flink = (ListHead)->Flink;           \
    (Entry)->Flink = _EX_Flink;             \
    (Entry)->Blink = (ListHead);             \
    _EX_Flink->Blink = (Entry);             \
    (ListHead)->Flink = (Entry);             \
}

#define RemoveEntryList(Entry) {            \
    PLIST_ENTRY _EX_Blink;                   \
    PLIST_ENTRY _EX_Flink;                   \
    _EX_Flink = (Entry)->Flink;             \
    _EX_Blink = (Entry)->Blink;             \
    _EX_Blink->Flink = _EX_Flink;          \
    _EX_Flink->Blink = _EX_Blink;          \
}

#define RemoveHeadList(ListHead)            \
    (ListHead)->Flink;                       \
    { RemoveEntryList((ListHead)->Flink); }

#define CONTAINING_RECORD(address, type, field) \
    ((type *)((PCHAR)(address) - (ULONG_PTR)(&((type *)0)->field)))

/* ================================================================
 * KEVENT
 * ================================================================ */

typedef enum _EVENT_TYPE {
    NotificationEvent = 0,
    SynchronizationEvent = 1
} EVENT_TYPE;

typedef struct _DISPATCHER_HEADER {
    UCHAR  Type;
    UCHAR  Absolute;
    UCHAR  Size;
    UCHAR  Inserted;
    LONG   SignalState;
    LIST_ENTRY WaitListHead;
} DISPATCHER_HEADER;

typedef struct _KEVENT {
    DISPATCHER_HEADER Header;
} KEVENT, *PKEVENT, *PRKEVENT;

/* ================================================================
 * KDPC (Deferred Procedure Call)
 * ================================================================ */

typedef VOID (NTAPI *PKDEFERRED_ROUTINE)(
    struct _KDPC *Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
);

typedef struct _KDPC {
    SHORT                Type;
    UCHAR                Number;
    UCHAR                Importance;
    LIST_ENTRY           DpcListEntry;
    PKDEFERRED_ROUTINE   DeferredRoutine;
    PVOID                DeferredContext;
    PVOID                SystemArgument1;
    PVOID                SystemArgument2;
    PVOID                DpcData;
} KDPC, *PKDPC, *PRKDPC;

/* ================================================================
 * KTIMER
 * ================================================================ */

typedef struct _KTIMER {
    DISPATCHER_HEADER Header;
    ULARGE_INTEGER    DueTime;
    LIST_ENTRY        TimerListEntry;
    PKDPC             Dpc;
    LONG              Period;
    /* Our shim additions: */
    ULONG             ShimTimerHandle;  /* VMM timer handle */
    BOOLEAN           ShimActive;
} KTIMER, *PKTIMER;

/* ================================================================
 * KDEVICE_QUEUE / KDEVICE_QUEUE_ENTRY
 * ================================================================ */

typedef struct _KDEVICE_QUEUE {
    SHORT       Type;
    SHORT       Size;
    LIST_ENTRY  DeviceListHead;
    KSPIN_LOCK  Lock;
    BOOLEAN     Busy;
} KDEVICE_QUEUE, *PKDEVICE_QUEUE;

typedef struct _KDEVICE_QUEUE_ENTRY {
    LIST_ENTRY  DeviceListEntry;
    ULONG       SortKey;
    BOOLEAN     Inserted;
} KDEVICE_QUEUE_ENTRY, *PKDEVICE_QUEUE_ENTRY;

/* ================================================================
 * MDL (Memory Descriptor List)
 * ================================================================ */

typedef struct _MDL {
    struct _MDL    *Next;
    SHORT           Size;
    SHORT           MdlFlags;
    PVOID           Process;        /* PEPROCESS, unused in our shim */
    PVOID           MappedSystemVa;
    PVOID           StartVa;
    ULONG           ByteCount;
    ULONG           ByteOffset;
} MDL, *PMDL;

#define MDL_MAPPED_TO_SYSTEM_VA     0x0001
#define MDL_PAGES_LOCKED            0x0002
#define MDL_SOURCE_IS_NONPAGED_POOL 0x0004
#define MDL_ALLOCATED_FIXED_SIZE    0x0008
#define MDL_PARTIAL                 0x0010
#define MDL_PARTIAL_HAS_BEEN_MAPPED 0x0020
#define MDL_IO_PAGE_READ            0x0040
#define MDL_WRITE_OPERATION         0x0080
#define MDL_PARENT_MAPPED_SYSTEM_VA 0x0100
#define MDL_IO_SPACE                0x8000

#define MmGetMdlByteCount(Mdl)      ((Mdl)->ByteCount)
#define MmGetMdlByteOffset(Mdl)     ((Mdl)->ByteOffset)
#define MmGetMdlVirtualAddress(Mdl) \
    ((PVOID)((PCHAR)((Mdl)->StartVa) + (Mdl)->ByteOffset))
#define MmGetSystemAddressForMdlSafe(Mdl, Priority) \
    (((Mdl)->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) ? \
     ((Mdl)->MappedSystemVa) : MmMapLockedPagesSpecifyCache((Mdl), KernelMode, 0, NULL, FALSE, (Priority)))

/* ================================================================
 * POOL_TYPE
 * ================================================================ */

typedef enum _POOL_TYPE {
    NonPagedPool            = 0,
    PagedPool               = 1,
    NonPagedPoolMustSucceed = 2,
    DontUseThisType         = 3,
    NonPagedPoolCacheAligned = 4,
    PagedPoolCacheAligned   = 5,
    NonPagedPoolCacheAlignedMustS = 6,
    MaxPoolType             = 7
} POOL_TYPE;

/* ================================================================
 * MEMORY_CACHING_TYPE
 * ================================================================ */

typedef enum _MEMORY_CACHING_TYPE {
    MmNonCached             = 0,
    MmCached                = 1,
    MmWriteCombined         = 2,
    MmHardwareCoherentCached = 3,
    MmMaximumCacheType      = 4
} MEMORY_CACHING_TYPE;

/* MM_PAGE_PRIORITY */
typedef enum _MM_PAGE_PRIORITY {
    LowPagePriority         = 0,
    NormalPagePriority       = 16,
    HighPagePriority        = 32
} MM_PAGE_PRIORITY;

/* ================================================================
 * Forward declarations
 * ================================================================ */

typedef struct _DRIVER_OBJECT    DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _DEVICE_OBJECT   DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _FILE_OBJECT     FILE_OBJECT, *PFILE_OBJECT;
typedef struct _IRP             IRP, *PIRP;
typedef struct _IO_STACK_LOCATION IO_STACK_LOCATION, *PIO_STACK_LOCATION;

/* ================================================================
 * DEVICE_TYPE values
 * ================================================================ */

#define FILE_DEVICE_DISK            0x00000007
#define FILE_DEVICE_DISK_FILE_SYSTEM 0x00000008
#define FILE_DEVICE_CD_ROM          0x00000002
#define FILE_DEVICE_CONTROLLER      0x00000004
#define FILE_DEVICE_MASS_STORAGE    0x0000002D
#define FILE_DEVICE_UNKNOWN         0x00000022
#define FILE_DEVICE_BUS_EXTENDER    0x0000002A

/* Device characteristics */
#define FILE_REMOVABLE_MEDIA        0x00000001
#define FILE_READ_ONLY_DEVICE       0x00000002
#define FILE_DEVICE_SECURE_OPEN     0x00000100

/* Create disposition for IoCreateDevice */
#define DO_BUFFERED_IO              0x00000004
#define DO_DIRECT_IO                0x00000010
#define DO_EXCLUSIVE                0x00000008
#define DO_DEVICE_INITIALIZING      0x00000080
#define DO_POWER_PAGABLE            0x00002000
#define DO_POWER_INRUSH             0x00004000

/* ================================================================
 * IRP MAJOR/MINOR FUNCTION CODES
 * ================================================================ */

#define IRP_MJ_CREATE                   0x00
#define IRP_MJ_CREATE_NAMED_PIPE        0x01
#define IRP_MJ_CLOSE                    0x02
#define IRP_MJ_READ                     0x03
#define IRP_MJ_WRITE                    0x04
#define IRP_MJ_QUERY_INFORMATION        0x05
#define IRP_MJ_SET_INFORMATION          0x06
#define IRP_MJ_QUERY_EA                 0x07
#define IRP_MJ_SET_EA                   0x08
#define IRP_MJ_FLUSH_BUFFERS            0x09
#define IRP_MJ_QUERY_VOLUME_INFORMATION 0x0A
#define IRP_MJ_SET_VOLUME_INFORMATION   0x0B
#define IRP_MJ_DIRECTORY_CONTROL        0x0C
#define IRP_MJ_FILE_SYSTEM_CONTROL      0x0D
#define IRP_MJ_DEVICE_CONTROL           0x0E
#define IRP_MJ_INTERNAL_DEVICE_CONTROL  0x0F
#define IRP_MJ_SHUTDOWN                 0x10
#define IRP_MJ_LOCK_CONTROL             0x11
#define IRP_MJ_CLEANUP                  0x12
#define IRP_MJ_CREATE_MAILSLOT          0x13
#define IRP_MJ_QUERY_SECURITY           0x14
#define IRP_MJ_SET_SECURITY             0x15
#define IRP_MJ_POWER                    0x16
#define IRP_MJ_SYSTEM_CONTROL           0x17
#define IRP_MJ_DEVICE_CHANGE            0x18
#define IRP_MJ_QUERY_QUOTA              0x19
#define IRP_MJ_SET_QUOTA                0x1A
#define IRP_MJ_PNP                      0x1B
#define IRP_MJ_PNP_POWER                IRP_MJ_PNP
#define IRP_MJ_MAXIMUM_FUNCTION         0x1B

/* PnP minor function codes */
#define IRP_MN_START_DEVICE                 0x00
#define IRP_MN_QUERY_REMOVE_DEVICE          0x01
#define IRP_MN_REMOVE_DEVICE                0x02
#define IRP_MN_CANCEL_REMOVE_DEVICE         0x03
#define IRP_MN_STOP_DEVICE                  0x04
#define IRP_MN_QUERY_STOP_DEVICE            0x05
#define IRP_MN_CANCEL_STOP_DEVICE           0x06
#define IRP_MN_QUERY_DEVICE_RELATIONS       0x07
#define IRP_MN_QUERY_INTERFACE              0x08
#define IRP_MN_QUERY_CAPABILITIES           0x09
#define IRP_MN_QUERY_RESOURCES              0x0A
#define IRP_MN_QUERY_RESOURCE_REQUIREMENTS  0x0B
#define IRP_MN_QUERY_DEVICE_TEXT            0x0C
#define IRP_MN_FILTER_RESOURCE_REQUIREMENTS 0x0D
#define IRP_MN_READ_CONFIG                  0x0F
#define IRP_MN_WRITE_CONFIG                 0x10
#define IRP_MN_EJECT                        0x11
#define IRP_MN_SET_LOCK                     0x12
#define IRP_MN_QUERY_ID                     0x13
#define IRP_MN_QUERY_PNP_DEVICE_STATE       0x14
#define IRP_MN_QUERY_BUS_INFORMATION        0x15
#define IRP_MN_DEVICE_USAGE_NOTIFICATION    0x16
#define IRP_MN_SURPRISE_REMOVAL             0x17

/* Power minor function codes */
#define IRP_MN_WAIT_WAKE                    0x00
#define IRP_MN_POWER_SEQUENCE               0x01
#define IRP_MN_SET_POWER                    0x02
#define IRP_MN_QUERY_POWER                  0x03

/* WMI minor function codes */
#define IRP_MN_QUERY_ALL_DATA               0x00
#define IRP_MN_QUERY_SINGLE_INSTANCE        0x01
#define IRP_MN_CHANGE_SINGLE_INSTANCE       0x02
#define IRP_MN_CHANGE_SINGLE_ITEM           0x03
#define IRP_MN_ENABLE_EVENTS                0x04
#define IRP_MN_DISABLE_EVENTS               0x05
#define IRP_MN_ENABLE_COLLECTION            0x06
#define IRP_MN_DISABLE_COLLECTION           0x07
#define IRP_MN_REGINFO                      0x08
#define IRP_MN_EXECUTE_METHOD               0x09

/* IOCTL transfer types */
#define METHOD_BUFFERED         0
#define METHOD_IN_DIRECT        1
#define METHOD_OUT_DIRECT       2
#define METHOD_NEITHER          3

/* IRP flags */
#define IRP_NOCACHE                     0x00000001
#define IRP_PAGING_IO                   0x00000002
#define IRP_MOUNT_COMPLETION            0x00000002
#define IRP_SYNCHRONOUS_API             0x00000004
#define IRP_ASSOCIATED_IRP              0x00000008
#define IRP_BUFFERED_IO                 0x00000010
#define IRP_DEALLOCATE_BUFFER           0x00000020
#define IRP_INPUT_OPERATION             0x00000040
#define IRP_SYNCHRONOUS_PAGING_IO       0x00000040
#define IRP_CREATE_OPERATION            0x00000080
#define IRP_READ_OPERATION              0x00000100
#define IRP_WRITE_OPERATION             0x00000200
#define IRP_CLOSE_OPERATION             0x00000400
#define IRP_DEFER_IO_COMPLETION         0x00000800

/* Slot flag for IO_STACK_LOCATION */
#define SL_PENDING_RETURNED             0x01

/* ================================================================
 * DEVICE_RELATIONS
 * ================================================================ */

typedef enum _DEVICE_RELATION_TYPE {
    BusRelations = 0,
    EjectionRelations = 1,
    PowerRelations = 2,
    RemovalRelations = 3,
    TargetDeviceRelation = 4
} DEVICE_RELATION_TYPE;

typedef struct _DEVICE_RELATIONS {
    ULONG           Count;
    PDEVICE_OBJECT  Objects[1]; /* Variable length */
} DEVICE_RELATIONS, *PDEVICE_RELATIONS;

/* ================================================================
 * POWER TYPES
 * ================================================================ */

typedef enum _SYSTEM_POWER_STATE {
    PowerSystemUnspecified = 0,
    PowerSystemWorking     = 1,
    PowerSystemSleeping1   = 2,
    PowerSystemSleeping2   = 3,
    PowerSystemSleeping3   = 4,
    PowerSystemHibernate   = 5,
    PowerSystemShutdown    = 6,
    PowerSystemMaximum     = 7
} SYSTEM_POWER_STATE, *PSYSTEM_POWER_STATE;

typedef enum _DEVICE_POWER_STATE {
    PowerDeviceUnspecified  = 0,
    PowerDeviceD0           = 1,
    PowerDeviceD1           = 2,
    PowerDeviceD2           = 3,
    PowerDeviceD3           = 4,
    PowerDeviceMaximum      = 5
} DEVICE_POWER_STATE, *PDEVICE_POWER_STATE;

typedef union _POWER_STATE {
    SYSTEM_POWER_STATE  SystemState;
    DEVICE_POWER_STATE  DeviceState;
} POWER_STATE, *PPOWER_STATE;

typedef enum _POWER_STATE_TYPE {
    SystemPowerState = 0,
    DevicePowerState = 1
} POWER_STATE_TYPE, *PPOWER_STATE_TYPE;

typedef enum _POWER_ACTION {
    PowerActionNone         = 0,
    PowerActionReserved     = 1,
    PowerActionSleep        = 2,
    PowerActionHibernate    = 3,
    PowerActionShutdown     = 4,
    PowerActionShutdownReset = 5,
    PowerActionShutdownOff  = 6,
    PowerActionWarmEject    = 7
} POWER_ACTION, *PPOWER_ACTION;

/* Power completion callback */
typedef VOID (NTAPI *PREQUEST_POWER_COMPLETE)(
    PDEVICE_OBJECT DeviceObject,
    UCHAR MinorFunction,
    POWER_STATE PowerState,
    PVOID Context,
    PIO_STATUS_BLOCK IoStatus
);

/* ================================================================
 * KINTERRUPT
 * ================================================================ */

typedef BOOLEAN (NTAPI *PKSERVICE_ROUTINE)(
    struct _KINTERRUPT *Interrupt,
    PVOID ServiceContext
);

typedef BOOLEAN (NTAPI *PKSYNCHRONIZE_ROUTINE)(
    PVOID SynchronizeContext
);

typedef struct _KINTERRUPT {
    SHORT               Type;
    SHORT               Size;
    LIST_ENTRY          InterruptListEntry;
    PKSERVICE_ROUTINE   ServiceRoutine;
    PVOID               ServiceContext;
    KSPIN_LOCK          SpinLock;
    ULONG               Vector;
    KIRQL               Irql;
    KIRQL               SynchronizeIrql;
    BOOLEAN             ShareVector;
    ULONG               Mode; /* KINTERRUPT_MODE */
    /* Shim-specific: */
    ULONG               ShimIrqHandle;   /* VPICD handle */
    BOOLEAN             ShimConnected;
} KINTERRUPT, *PKINTERRUPT;

typedef enum _KINTERRUPT_MODE {
    LevelSensitive = 0,
    Latched        = 1
} KINTERRUPT_MODE;

/* ================================================================
 * FILE_OBJECT (simplified)
 * ================================================================ */

struct _FILE_OBJECT {
    SHORT                   Type;
    SHORT                   Size;
    PDEVICE_OBJECT          DeviceObject;
    UNICODE_STRING          FileName;
    LARGE_INTEGER           CurrentByteOffset;
    ULONG                   Flags;
};

/* ================================================================
 * DRIVER_OBJECT
 * ================================================================ */

/* Maximum number of IRP major functions */
#define IRP_MJ_MAXIMUM     (IRP_MJ_MAXIMUM_FUNCTION + 1)

/* Driver dispatch routine type */
typedef NTSTATUS (NTAPI *PDRIVER_DISPATCH)(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
);

/* DriverStartIo routine type */
typedef VOID (NTAPI *PDRIVER_STARTIO)(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
);

/* AddDevice routine type */
typedef NTSTATUS (NTAPI *PDRIVER_ADD_DEVICE)(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT PhysicalDeviceObject
);

/* Driver unload type */
typedef VOID (NTAPI *PDRIVER_UNLOAD)(
    PDRIVER_OBJECT DriverObject
);

/* IO_TIMER_ROUTINE */
typedef VOID (NTAPI *PIO_TIMER_ROUTINE)(
    PDEVICE_OBJECT DeviceObject,
    PVOID Context
);

/* DRIVER_EXTENSION */
typedef struct _DRIVER_EXTENSION {
    PDRIVER_OBJECT      DriverObject;
    PDRIVER_ADD_DEVICE  AddDevice;
    ULONG               Count;
    UNICODE_STRING      ServiceKeyName;
    /* For IoAllocateDriverObjectExtension: */
    PVOID               ClientDriverExtension;
    PVOID               ClientIdentifier;
} DRIVER_EXTENSION, *PDRIVER_EXTENSION;

struct _DRIVER_OBJECT {
    SHORT               Type;
    SHORT               Size;
    PDEVICE_OBJECT      DeviceObject;       /* Head of device list */
    ULONG               Flags;
    PVOID               DriverStart;
    ULONG               DriverSize;
    PVOID               DriverSection;
    PDRIVER_EXTENSION    DriverExtension;
    UNICODE_STRING      DriverName;
    PUNICODE_STRING     HardwareDatabase;
    PVOID               FastIoDispatch;
    PVOID               DriverInit;         /* DriverEntry */
    PDRIVER_STARTIO     DriverStartIo;
    PDRIVER_UNLOAD      DriverUnload;
    PDRIVER_DISPATCH    MajorFunction[IRP_MJ_MAXIMUM];
};

/* ================================================================
 * DEVICE_OBJECT
 * ================================================================ */

struct _DEVICE_OBJECT {
    SHORT               Type;
    USHORT              Size;
    LONG                ReferenceCount;
    PDRIVER_OBJECT      DriverObject;
    PDEVICE_OBJECT      NextDevice;         /* Next in driver's list */
    PDEVICE_OBJECT      AttachedDevice;     /* Higher device */
    PIRP                CurrentIrp;
    PVOID               Timer;              /* PIO_TIMER */
    ULONG               Flags;
    ULONG               Characteristics;
    PVOID               Vpb;
    PVOID               DeviceExtension;
    ULONG               DeviceType;
    CHAR                StackSize;
    union {
        LIST_ENTRY          ListEntry;
        PVOID               Wcb;            /* WAIT_CONTEXT_BLOCK */
    } Queue;
    ULONG               AlignmentRequirement;
    KDEVICE_QUEUE       DeviceQueue;
    KDPC                Dpc;
    ULONG               ActiveThreadCount;
    PVOID               SecurityDescriptor;
    KEVENT              DeviceLock;
    USHORT              SectorSize;
    USHORT              Spare1;
    PVOID               DeviceObjectExtension;
    PVOID               Reserved;
    /* Shim additions (after NT structure for binary compat): */
    PDEVICE_OBJECT      ShimLowerDevice;     /* AttachDevice target */
    PIO_TIMER_ROUTINE   ShimTimerRoutine;
    PVOID               ShimTimerContext;
    ULONG               ShimTimerHandle;
    BOOLEAN             ShimTimerActive;
};

/* ================================================================
 * IO_TIMER (simplified)
 * ================================================================ */

typedef struct _IO_TIMER {
    SHORT               Type;
    SHORT               TimerFlag;
    LIST_ENTRY          TimerList;
    PIO_TIMER_ROUTINE   TimerRoutine;
    PVOID               Context;
    PDEVICE_OBJECT      DeviceObject;
} IO_TIMER, *PIO_TIMER;

/* ================================================================
 * IRP and IO_STACK_LOCATION
 * ================================================================ */

/* IO completion routine */
typedef NTSTATUS (NTAPI *PIO_COMPLETION_ROUTINE)(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
);

/* IO work item callback */
typedef VOID (NTAPI *PIO_WORKITEM_ROUTINE)(
    PDEVICE_OBJECT DeviceObject,
    PVOID Context
);

/* IO work item */
typedef struct _IO_WORKITEM {
    PDEVICE_OBJECT          DeviceObject;
    PIO_WORKITEM_ROUTINE    Routine;
    PVOID                   Context;
    ULONG                   QueueType;
    LIST_ENTRY              ListEntry;
} IO_WORKITEM, *PIO_WORKITEM;

/* Work queue type */
typedef enum _WORK_QUEUE_TYPE {
    CriticalWorkQueue = 0,
    DelayedWorkQueue  = 1,
    HyperCriticalWorkQueue = 2,
    MaximumWorkQueue  = 3
} WORK_QUEUE_TYPE;

/* IO_STACK_LOCATION */
struct _IO_STACK_LOCATION {
    UCHAR                   MajorFunction;
    UCHAR                   MinorFunction;
    UCHAR                   Flags;
    UCHAR                   Control;

    union {
        /* IRP_MJ_CREATE */
        struct {
            PVOID SecurityContext;
            ULONG Options;
            USHORT FileAttributes;
            USHORT ShareAccess;
            ULONG EaLength;
        } Create;

        /* IRP_MJ_READ / IRP_MJ_WRITE */
        struct {
            ULONG           Length;
            ULONG           Key;
            LARGE_INTEGER   ByteOffset;
        } Read;

        struct {
            ULONG           Length;
            ULONG           Key;
            LARGE_INTEGER   ByteOffset;
        } Write;

        /* IRP_MJ_DEVICE_CONTROL / IRP_MJ_INTERNAL_DEVICE_CONTROL */
        struct {
            ULONG           OutputBufferLength;
            ULONG           InputBufferLength;
            ULONG           IoControlCode;
            PVOID           Type3InputBuffer;
        } DeviceIoControl;

        /* IRP_MJ_SCSI (internal IOCTL) */
        struct {
            PVOID           Srb; /* PSCSI_REQUEST_BLOCK */
        } Scsi;

        /* IRP_MJ_PNP */
        struct {
            union {
                DEVICE_RELATION_TYPE Type; /* QueryDeviceRelations */
                struct {                   /* StartDevice */
                    PVOID AllocatedResources;
                    PVOID AllocatedResourcesTranslated;
                } StartDevice;
                struct {                   /* QueryId */
                    ULONG IdType;          /* BUS_QUERY_ID_TYPE */
                } QueryId;
                struct {                   /* QueryDeviceText */
                    ULONG DeviceTextType;
                    ULONG LocaleId;
                } QueryDeviceText;
            };
        } QueryDeviceRelations;

        /* IRP_MJ_POWER */
        struct {
            ULONG               SystemContext;
            POWER_STATE_TYPE    Type;
            POWER_STATE         State;
            POWER_ACTION        ShutdownType;
        } Power;

        /* IRP_MJ_SYSTEM_CONTROL (WMI) */
        struct {
            ULONG_PTR   ProviderId;
            PVOID       DataPath;
            ULONG       BufferSize;
            PVOID       Buffer;
        } WMI;

        /* Generic for others */
        struct {
            PVOID Argument1;
            PVOID Argument2;
            PVOID Argument3;
            PVOID Argument4;
        } Others;

    } Parameters;

    PDEVICE_OBJECT          DeviceObject;
    PFILE_OBJECT            FileObject;

    PIO_COMPLETION_ROUTINE  CompletionRoutine;
    PVOID                   Context;
};

/* IO_STACK_LOCATION Control flags */
#define SL_INVOKE_ON_SUCCESS    0x01
#define SL_INVOKE_ON_ERROR      0x02
#define SL_INVOKE_ON_CANCEL     0x04

/* IRP */
struct _IRP {
    SHORT               Type;
    USHORT              Size;
    PMDL                MdlAddress;
    ULONG               Flags;
    union {
        struct _IRP    *MasterIrp;
        LONG            IrpCount;
        PVOID           SystemBuffer;
    } AssociatedIrp;
    LIST_ENTRY          ThreadListEntry;
    IO_STATUS_BLOCK     IoStatus;
    CHAR                RequestorMode;
    BOOLEAN             PendingReturned;
    CHAR                StackCount;
    CHAR                CurrentLocation;
    BOOLEAN             Cancel;
    KIRQL               CancelIrql;
    CHAR                ApcEnvironment;
    UCHAR               AllocationFlags;
    PIO_STATUS_BLOCK    UserIosb;
    PKEVENT             UserEvent;
    union {
        struct {
            PVOID       UserApcRoutine;
            PVOID       UserApcContext;
        } AsynchronousParameters;
        LARGE_INTEGER   AllocationSize;
    } Overlay;
    PVOID               CancelRoutine;
    PVOID               UserBuffer;
    union {
        struct {
            union {
                KDEVICE_QUEUE_ENTRY DeviceQueueEntry;
                struct {
                    PVOID DriverContext[4];
                };
            };
            PVOID       Thread;     /* PETHREAD */
            PCHAR       AuxiliaryBuffer;
            struct {
                LIST_ENTRY  ListEntry;
                union {
                    PIO_STACK_LOCATION  CurrentStackLocation;
                    ULONG               PacketType;
                };
            };
            PFILE_OBJECT    OriginalFileObject;
        } Overlay;
    } Tail;
};

/* IRP accessor macros */
#define IoGetCurrentIrpStackLocation(Irp) \
    ((Irp)->Tail.Overlay.CurrentStackLocation)

#define IoGetNextIrpStackLocation(Irp) \
    ((Irp)->Tail.Overlay.CurrentStackLocation - 1)

#define IoSetNextIrpStackLocation(Irp) {                 \
    (Irp)->CurrentLocation++;                             \
    (Irp)->Tail.Overlay.CurrentStackLocation++;           \
}

#define IoSkipCurrentIrpStackLocation(Irp) {             \
    (Irp)->CurrentLocation++;                             \
    (Irp)->Tail.Overlay.CurrentStackLocation++;           \
}

#define IoCopyCurrentIrpStackLocationToNext(Irp) {       \
    PIO_STACK_LOCATION _irpSp;                            \
    PIO_STACK_LOCATION _nextIrpSp;                        \
    _irpSp = IoGetCurrentIrpStackLocation(Irp);           \
    _nextIrpSp = IoGetNextIrpStackLocation(Irp);          \
    RtlCopyMemory(_nextIrpSp, _irpSp,                    \
        (ULONG)(&_irpSp->CompletionRoutine) -             \
        (ULONG)(_irpSp));                                 \
    _nextIrpSp->Control = 0;                              \
}

#define IoSetCompletionRoutine(Irp, Routine, Ctx, s, e, c) { \
    PIO_STACK_LOCATION _nextIrpSp;                            \
    _nextIrpSp = IoGetNextIrpStackLocation(Irp);              \
    _nextIrpSp->CompletionRoutine = (Routine);                \
    _nextIrpSp->Context = (Ctx);                              \
    _nextIrpSp->Control = 0;                                  \
    if (s) _nextIrpSp->Control |= SL_INVOKE_ON_SUCCESS;      \
    if (e) _nextIrpSp->Control |= SL_INVOKE_ON_ERROR;        \
    if (c) _nextIrpSp->Control |= SL_INVOKE_ON_CANCEL;       \
}

#define IoMarkIrpPending(Irp) \
    (IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED)

#define IoSizeOfIrp(StackSize) \
    ((USHORT)(sizeof(IRP) + ((StackSize) * sizeof(IO_STACK_LOCATION))))

/* ================================================================
 * CM_RESOURCE_LIST and related structures
 * ================================================================ */

typedef enum _CM_RESOURCE_TYPE {
    CmResourceTypeNull          = 0,
    CmResourceTypePort          = 1,
    CmResourceTypeInterrupt     = 2,
    CmResourceTypeMemory        = 3,
    CmResourceTypeDma           = 4,
    CmResourceTypeDeviceSpecific = 5,
    CmResourceTypeBusNumber     = 6
} CM_RESOURCE_TYPE;

typedef enum _CM_SHARE_DISPOSITION {
    CmResourceShareUndetermined = 0,
    CmResourceShareDeviceExclusive = 1,
    CmResourceShareDriverExclusive = 2,
    CmResourceShareShared       = 3
} CM_SHARE_DISPOSITION;

typedef struct _CM_PARTIAL_RESOURCE_DESCRIPTOR {
    UCHAR   Type;
    UCHAR   ShareDisposition;
    USHORT  Flags;
    union {
        struct {
            PHYSICAL_ADDRESS Start;
            ULONG            Length;
        } Port;
        struct {
            ULONG   Level;
            ULONG   Vector;
            ULONG   Affinity;
        } Interrupt;
        struct {
            PHYSICAL_ADDRESS Start;
            ULONG            Length;
        } Memory;
        struct {
            ULONG   Channel;
            ULONG   Port;
            ULONG   Reserved1;
        } Dma;
        struct {
            ULONG   DataSize;
            ULONG   Reserved1;
            ULONG   Reserved2;
        } DeviceSpecificData;
        struct {
            ULONG   Start;
            ULONG   Length;
            ULONG   Reserved;
        } BusNumber;
    } u;
} CM_PARTIAL_RESOURCE_DESCRIPTOR, *PCM_PARTIAL_RESOURCE_DESCRIPTOR;

typedef struct _CM_PARTIAL_RESOURCE_LIST {
    USHORT  Version;
    USHORT  Revision;
    ULONG   Count;
    CM_PARTIAL_RESOURCE_DESCRIPTOR Descriptors[1];
} CM_PARTIAL_RESOURCE_LIST, *PCM_PARTIAL_RESOURCE_LIST;

typedef struct _CM_FULL_RESOURCE_DESCRIPTOR {
    ULONG   InterfaceType;  /* INTERFACE_TYPE */
    ULONG   BusNumber;
    CM_PARTIAL_RESOURCE_LIST PartialResourceList;
} CM_FULL_RESOURCE_DESCRIPTOR, *PCM_FULL_RESOURCE_DESCRIPTOR;

typedef struct _CM_RESOURCE_LIST {
    ULONG                       Count;
    CM_FULL_RESOURCE_DESCRIPTOR List[1];
} CM_RESOURCE_LIST, *PCM_RESOURCE_LIST;

/* ================================================================
 * IO_RESOURCE_REQUIREMENTS_LIST
 * ================================================================ */

typedef struct _IO_RESOURCE_DESCRIPTOR {
    UCHAR   Option;
    UCHAR   Type;
    UCHAR   ShareDisposition;
    UCHAR   Spare1;
    USHORT  Flags;
    USHORT  Spare2;
    union {
        struct {
            ULONG           Length;
            ULONG           Alignment;
            PHYSICAL_ADDRESS MinimumAddress;
            PHYSICAL_ADDRESS MaximumAddress;
        } Port;
        struct {
            ULONG           MinimumVector;
            ULONG           MaximumVector;
        } Interrupt;
        struct {
            ULONG           Length;
            ULONG           Alignment;
            PHYSICAL_ADDRESS MinimumAddress;
            PHYSICAL_ADDRESS MaximumAddress;
        } Memory;
        struct {
            ULONG           MinimumChannel;
            ULONG           MaximumChannel;
        } Dma;
        struct {
            ULONG           Length;
            ULONG           MinBusNumber;
            ULONG           MaxBusNumber;
            ULONG           Reserved;
        } BusNumber;
    } u;
} IO_RESOURCE_DESCRIPTOR, *PIO_RESOURCE_DESCRIPTOR;

typedef struct _IO_RESOURCE_LIST {
    USHORT  Version;
    USHORT  Revision;
    ULONG   Count;
    IO_RESOURCE_DESCRIPTOR Descriptors[1];
} IO_RESOURCE_LIST, *PIO_RESOURCE_LIST;

typedef struct _IO_RESOURCE_REQUIREMENTS_LIST {
    ULONG           ListSize;
    ULONG           InterfaceType;  /* INTERFACE_TYPE */
    ULONG           BusNumber;
    ULONG           SlotNumber;
    ULONG           Reserved[3];
    ULONG           AlternativeLists;
    IO_RESOURCE_LIST List[1];
} IO_RESOURCE_REQUIREMENTS_LIST, *PIO_RESOURCE_REQUIREMENTS_LIST;

/* ================================================================
 * INTERFACE_TYPE
 * ================================================================ */

typedef enum _INTERFACE_TYPE {
    InterfaceTypeUndefined = -1,
    Internal        = 0,
    Isa             = 1,
    Eisa            = 2,
    MicroChannel    = 3,
    TurboChannel    = 4,
    PCIBus          = 5,
    MaximumInterfaceType
} INTERFACE_TYPE, *PINTERFACE_TYPE;

/* ================================================================
 * CONFIGURATION_INFORMATION
 * ================================================================ */

typedef struct _CONFIGURATION_INFORMATION {
    ULONG   DiskCount;
    ULONG   FloppyCount;
    ULONG   CdRomCount;
    ULONG   TapeCount;
    ULONG   ScsiPortCount;
    ULONG   SerialCount;
    ULONG   ParallelCount;
    BOOLEAN AtDiskPrimaryAddressClaimed;
    BOOLEAN AtDiskSecondaryAddressClaimed;
    ULONG   Version;
    ULONG   MediumChangerCount;
} CONFIGURATION_INFORMATION, *PCONFIGURATION_INFORMATION;

/* ================================================================
 * PCI_COMMON_CONFIG
 * ================================================================ */

typedef struct _PCI_COMMON_CONFIG {
    USHORT  VendorID;
    USHORT  DeviceID;
    USHORT  Command;
    USHORT  Status;
    UCHAR   RevisionID;
    UCHAR   ProgIf;
    UCHAR   SubClass;
    UCHAR   BaseClass;
    UCHAR   CacheLineSize;
    UCHAR   LatencyTimer;
    UCHAR   HeaderType;
    UCHAR   BIST;
    union {
        struct {
            ULONG   BaseAddresses[6];
            ULONG   CIS;
            USHORT  SubVendorID;
            USHORT  SubSystemID;
            ULONG   ROMBaseAddress;
            UCHAR   CapabilitiesPtr;
            UCHAR   Reserved1[3];
            ULONG   Reserved2;
            UCHAR   InterruptLine;
            UCHAR   InterruptPin;
            UCHAR   MinimumGrant;
            UCHAR   MaximumLatency;
        } type0;
    } u;
    UCHAR   DeviceSpecific[192];
} PCI_COMMON_CONFIG, *PPCI_COMMON_CONFIG;

#define PCI_COMMON_HDR_LENGTH \
    (sizeof(PCI_COMMON_CONFIG) - sizeof(((PCI_COMMON_CONFIG*)0)->DeviceSpecific))

/* ================================================================
 * RTL_QUERY_REGISTRY_TABLE (for RtlQueryRegistryValues)
 * ================================================================ */

typedef NTSTATUS (NTAPI *PRTL_QUERY_REGISTRY_ROUTINE)(
    PWSTR   ValueName,
    ULONG   ValueType,
    PVOID   ValueData,
    ULONG   ValueLength,
    PVOID   Context,
    PVOID   EntryContext
);

typedef struct _RTL_QUERY_REGISTRY_TABLE {
    PRTL_QUERY_REGISTRY_ROUTINE QueryRoutine;
    ULONG       Flags;
    PWSTR       Name;
    PVOID       EntryContext;
    ULONG       DefaultType;
    PVOID       DefaultData;
    ULONG       DefaultLength;
} RTL_QUERY_REGISTRY_TABLE, *PRTL_QUERY_REGISTRY_TABLE;

/* RtlQueryRegistryValues relative-to flags */
#define RTL_REGISTRY_ABSOLUTE       0
#define RTL_REGISTRY_SERVICES       1
#define RTL_REGISTRY_CONTROL        2
#define RTL_REGISTRY_WINDOWS_NT     3
#define RTL_REGISTRY_DEVICEMAP      4
#define RTL_REGISTRY_USER           5

#define RTL_QUERY_REGISTRY_SUBKEY       0x00000001
#define RTL_QUERY_REGISTRY_TOPKEY       0x00000002
#define RTL_QUERY_REGISTRY_REQUIRED     0x00000004
#define RTL_QUERY_REGISTRY_NOVALUE      0x00000008
#define RTL_QUERY_REGISTRY_NOEXPAND     0x00000010
#define RTL_QUERY_REGISTRY_DIRECT       0x00000020
#define RTL_QUERY_REGISTRY_DELETE       0x00000040

/* Registry value types */
#define REG_NONE                0
#define REG_SZ                  1
#define REG_EXPAND_SZ           2
#define REG_BINARY              3
#define REG_DWORD               4
#define REG_DWORD_LITTLE_ENDIAN 4
#define REG_DWORD_BIG_ENDIAN    5
#define REG_LINK                6
#define REG_MULTI_SZ            7
#define REG_RESOURCE_LIST       8
#define REG_FULL_RESOURCE_DESCRIPTOR 9
#define REG_RESOURCE_REQUIREMENTS_LIST 10

/* ================================================================
 * WMILIB types
 * ================================================================ */

typedef enum _SYSCTL_IRP_DISPOSITION {
    IrpProcessed = 0,
    IrpNotCompleted = 1,
    IrpNotWmi = 2,
    IrpForward = 3
} SYSCTL_IRP_DISPOSITION, *PSYSCTL_IRP_DISPOSITION;

typedef struct _WMILIB_CONTEXT {
    ULONG   GuidCount;
    PVOID   GuidList;           /* PWMIGUIDREGINFO */
    PVOID   QueryWmiRegInfo;
    PVOID   QueryWmiDataBlock;
    PVOID   SetWmiDataBlock;
    PVOID   SetWmiDataItem;
    PVOID   ExecuteWmiMethod;
    PVOID   WmiFunctionControl;
} WMILIB_CONTEXT, *PWMILIB_CONTEXT;

/* ================================================================
 * ERROR LOG
 * ================================================================ */

typedef struct _IO_ERROR_LOG_PACKET {
    UCHAR   MajorFunctionCode;
    UCHAR   RetryCount;
    USHORT  DumpDataSize;
    USHORT  NumberOfStrings;
    USHORT  StringOffset;
    USHORT  EventCategory;
    NTSTATUS ErrorCode;
    ULONG   UniqueErrorValue;
    NTSTATUS FinalStatus;
    ULONG   SequenceNumber;
    ULONG   IoControlCode;
    LARGE_INTEGER DeviceOffset;
    ULONG   DumpData[1];
} IO_ERROR_LOG_PACKET, *PIO_ERROR_LOG_PACKET;

#define ERROR_LOG_MAXIMUM_SIZE  240

/* ================================================================
 * IRQL LEVELS
 * ================================================================ */

#define PASSIVE_LEVEL   0
#define LOW_LEVEL       0
#define APC_LEVEL       1
#define DISPATCH_LEVEL  2
#define DEVICE_LEVEL    27  /* Arbitrary, above DISPATCH */
#define PROFILE_LEVEL   27
#define CLOCK2_LEVEL    28
#define IPI_LEVEL       29
#define POWER_LEVEL     30
#define HIGH_LEVEL      31

/* ================================================================
 * IMPORT TABLE ENTRY (for PE loader resolution)
 * ================================================================ */

typedef struct _IMPORT_FUNC_ENTRY {
    const char *name;
    void       *func;
} IMPORT_FUNC_ENTRY;

/* ================================================================
 * BUS_QUERY_ID_TYPE
 * ================================================================ */

typedef enum _BUS_QUERY_ID_TYPE {
    BusQueryDeviceID = 0,
    BusQueryHardwareIDs = 1,
    BusQueryCompatibleIDs = 2,
    BusQueryInstanceID = 3,
    BusQueryDeviceSerialNumber = 4
} BUS_QUERY_ID_TYPE;

/* ================================================================
 * DEVICE_TEXT_TYPE
 * ================================================================ */

typedef enum _DEVICE_TEXT_TYPE {
    DeviceTextDescription = 0,
    DeviceTextLocationInformation = 1
} DEVICE_TEXT_TYPE;

/* ================================================================
 * FUNCTION PROTOTYPES
 *
 * Every function implemented in NTKRNL.C
 * ================================================================ */

/* --- String/Memory utilities --- */
VOID    NTAPI RtlInitUnicodeString(PUNICODE_STRING Dst, PCWSTR Src);
VOID    NTAPI RtlCopyUnicodeString(PUNICODE_STRING Dst, PCUNICODE_STRING Src);
LONG    NTAPI RtlCompareUnicodeString(PCUNICODE_STRING Str1, PCUNICODE_STRING Str2, BOOLEAN CaseInsensitive);
VOID    NTAPI RtlFreeUnicodeString(PUNICODE_STRING Str);
NTSTATUS NTAPI RtlAppendUnicodeStringToString(PUNICODE_STRING Dst, PCUNICODE_STRING Src);
NTSTATUS NTAPI RtlIntegerToUnicodeString(ULONG Value, ULONG Base, PUNICODE_STRING Str);
VOID    NTAPI RtlInitAnsiString(PANSI_STRING Dst, PCSTR Src);
NTSTATUS NTAPI RtlAnsiStringToUnicodeString(PUNICODE_STRING Dst, PANSI_STRING Src, BOOLEAN AllocDst);
ULONG   NTAPI RtlxAnsiStringToUnicodeSize(PANSI_STRING Src);
SIZE_T  NTAPI RtlCompareMemory(const VOID *Source1, const VOID *Source2, SIZE_T Length);
int     CDECL sprintf(char *buf, const char *fmt, ...);
int     CDECL swprintf(WCHAR *buf, const WCHAR *fmt, ...);
char   *CDECL strstr(const char *haystack, const char *needle);
char   *CDECL _strupr(char *str);
void   *CDECL memmove(void *dest, const void *src, SIZE_T count);

/* Helper: memcpy for macros */
void    RtlCopyMemory_impl(void *dst, const void *src, SIZE_T len);
#define RtlCopyMemory(d,s,n)  RtlCopyMemory_impl((d),(s),(n))
#define RtlZeroMemory(d,n)    RtlFillMemory_impl((d),(n),0)
void    RtlFillMemory_impl(void *dst, SIZE_T len, UCHAR fill);

/* --- Memory allocation --- */
PVOID   NTAPI ExAllocatePoolWithTag(POOL_TYPE PoolType, SIZE_T NumberOfBytes, ULONG Tag);
VOID    NTAPI ExFreePoolWithTag(PVOID P, ULONG Tag);
PVOID   NTAPI MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, SIZE_T NumberOfBytes, MEMORY_CACHING_TYPE CacheType);
VOID    NTAPI MmUnmapIoSpace(PVOID BaseAddress, SIZE_T NumberOfBytes);
PVOID   NTAPI MmAllocateMappingAddress(SIZE_T NumberOfBytes, ULONG PoolTag);
VOID    NTAPI MmFreeMappingAddress(PVOID BaseAddress, ULONG PoolTag);
PVOID   NTAPI MmMapLockedPagesSpecifyCache(PMDL Mdl, KPROCESSOR_MODE AccessMode, MEMORY_CACHING_TYPE CacheType, PVOID BaseAddress, ULONG BugCheckOnFailure, ULONG Priority);
PVOID   NTAPI MmMapLockedPagesWithReservedMapping(PVOID MappingAddress, ULONG PoolTag, PMDL Mdl, MEMORY_CACHING_TYPE CacheType);
VOID    NTAPI MmUnmapReservedMapping(PVOID BaseAddress, ULONG PoolTag, PMDL Mdl);
VOID    NTAPI MmBuildMdlForNonPagedPool(PMDL Mdl);
PVOID   NTAPI MmLockPagableDataSection(PVOID AddressWithinSection);
VOID    NTAPI MmUnlockPagableImageSection(PVOID ImageSectionHandle);
VOID    NTAPI MmUnlockPages(PMDL Mdl);

/* MmHighestUserAddress is a global pointer */
extern PVOID *MmHighestUserAddress;

/* --- Synchronization --- */
VOID    NTAPI KeInitializeSpinLock(PKSPIN_LOCK SpinLock);
VOID    FASTCALL KefAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock);
VOID    FASTCALL KefReleaseSpinLockFromDpcLevel(PKSPIN_LOCK SpinLock);
VOID    NTAPI KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State);
LONG    NTAPI KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, ULONG WaitReason, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
BOOLEAN NTAPI KeSynchronizeExecution(PKINTERRUPT Interrupt, PKSYNCHRONIZE_ROUTINE SynchronizeRoutine, PVOID SynchronizeContext);

/* --- HAL spinlocks --- */
KIRQL   FASTCALL KfAcquireSpinLock(PKSPIN_LOCK SpinLock);
VOID    FASTCALL KfReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);

/* --- IRQL --- */
KIRQL   NTAPI KeGetCurrentIrql(void);
KIRQL   FASTCALL KfRaiseIrql(KIRQL NewIrql);
VOID    FASTCALL KfLowerIrql(KIRQL NewIrql);

/* --- Timer/DPC --- */
VOID    NTAPI KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext);
BOOLEAN NTAPI KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2);
VOID    NTAPI KeInitializeTimer(PKTIMER Timer);
BOOLEAN NTAPI KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);
BOOLEAN NTAPI KeCancelTimer(PKTIMER Timer);
VOID    NTAPI IoStartTimer(PDEVICE_OBJECT DeviceObject);
VOID    NTAPI IoInitializeTimer(PDEVICE_OBJECT DeviceObject, PIO_TIMER_ROUTINE TimerRoutine, PVOID Context);

/* --- I/O Manager --- */
NTSTATUS NTAPI IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize, PUNICODE_STRING DeviceName, ULONG DeviceType, ULONG DeviceCharacteristics, BOOLEAN Exclusive, PDEVICE_OBJECT *DeviceObject);
VOID    NTAPI IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
PDEVICE_OBJECT NTAPI IoAttachDeviceToDeviceStack(PDEVICE_OBJECT SourceDevice, PDEVICE_OBJECT TargetDevice);
VOID    NTAPI IoDetachDevice(PDEVICE_OBJECT TargetDevice);
NTSTATUS FASTCALL IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID    FASTCALL IofCompleteRequest(PIRP Irp, CHAR PriorityBoost);
VOID    NTAPI IoStartPacket(PDEVICE_OBJECT DeviceObject, PIRP Irp, PULONG Key, PVOID CancelFunction);
VOID    NTAPI IoStartNextPacket(PDEVICE_OBJECT DeviceObject, BOOLEAN Cancelable);
PIRP    NTAPI IoAllocateIrp(CHAR StackSize, BOOLEAN ChargeQuota);
VOID    NTAPI IoFreeIrp(PIRP Irp);
PIRP    NTAPI IoBuildDeviceIoControlRequest(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength, BOOLEAN InternalDeviceIoControl, PKEVENT Event, PIO_STATUS_BLOCK IoStatusBlock);
PIRP    NTAPI IoBuildSynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset, PKEVENT Event, PIO_STATUS_BLOCK IoStatusBlock);
PIRP    NTAPI IoBuildAsynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset, PIO_STATUS_BLOCK IoStatusBlock);
VOID    NTAPI IoInitializeIrp(PIRP Irp, USHORT PacketSize, CHAR StackSize);
PMDL    NTAPI IoAllocateMdl(PVOID VirtualAddress, ULONG Length, BOOLEAN SecondaryBuffer, BOOLEAN ChargeQuota, PIRP Irp);
VOID    NTAPI IoFreeMdl(PMDL Mdl);
PVOID   NTAPI IoAllocateErrorLogEntry(PVOID IoObject, UCHAR EntrySize);
VOID    NTAPI IoFreeErrorLogEntry(PVOID ElEntry);
VOID    NTAPI IoWriteErrorLogEntry(PVOID ElEntry);
PCONFIGURATION_INFORMATION NTAPI IoGetConfigurationInformation(void);
NTSTATUS NTAPI IoAllocateDriverObjectExtension(PDRIVER_OBJECT DriverObject, PVOID ClientIdentificationAddress, ULONG DriverObjectExtensionSize, PVOID *DriverObjectExtension);
PVOID   NTAPI IoGetDriverObjectExtension(PDRIVER_OBJECT DriverObject, PVOID ClientIdentificationAddress);
PIO_WORKITEM NTAPI IoAllocateWorkItem(PDEVICE_OBJECT DeviceObject);
VOID    NTAPI IoFreeWorkItem(PIO_WORKITEM IoWorkItem);
VOID    NTAPI IoQueueWorkItem(PIO_WORKITEM IoWorkItem, PIO_WORKITEM_ROUTINE WorkerRoutine, WORK_QUEUE_TYPE QueueType, PVOID Context);
PDEVICE_OBJECT NTAPI IoGetAttachedDeviceReference(PDEVICE_OBJECT DeviceObject);

/* --- Interrupt --- */
NTSTATUS NTAPI IoConnectInterrupt(PKINTERRUPT *InterruptObject, PKSERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext, PKSPIN_LOCK SpinLock, ULONG Vector, KIRQL Irql, KIRQL SynchronizeIrql, KINTERRUPT_MODE InterruptMode, BOOLEAN ShareVector, ULONG ProcessorEnableMask, BOOLEAN FloatingSave);
VOID    NTAPI IoDisconnectInterrupt(PKINTERRUPT InterruptObject);

/* --- PnP --- */
VOID    NTAPI IoInvalidateDeviceRelations(PDEVICE_OBJECT DeviceObject, DEVICE_RELATION_TYPE Type);
NTSTATUS NTAPI IoReportDetectedDevice(PDRIVER_OBJECT DriverObject, INTERFACE_TYPE LegacyBusType, ULONG BusNumber, ULONG SlotNumber, PCM_RESOURCE_LIST ResourceList, PIO_RESOURCE_REQUIREMENTS_LIST ResourceRequirements, BOOLEAN ResourceAssigned, PDEVICE_OBJECT *DeviceObject);
NTSTATUS NTAPI IoReportResourceForDetection(PDRIVER_OBJECT DriverObject, PCM_RESOURCE_LIST DriverList, ULONG DriverListSize, PDEVICE_OBJECT DeviceObject, PCM_RESOURCE_LIST DeviceList, ULONG DeviceListSize, PBOOLEAN ConflictDetected);
VOID    NTAPI IoInvalidateDeviceState(PDEVICE_OBJECT PhysicalDeviceObject);
NTSTATUS NTAPI IoCreateSymbolicLink(PUNICODE_STRING SymbolicLinkName, PUNICODE_STRING DeviceName);
NTSTATUS NTAPI IoDeleteSymbolicLink(PUNICODE_STRING SymbolicLinkName);

/* --- Registry --- */
NTSTATUS NTAPI RtlQueryRegistryValues(ULONG RelativeTo, PCWSTR Path, PRTL_QUERY_REGISTRY_TABLE QueryTable, PVOID Context, PVOID Environment);
NTSTATUS NTAPI RtlWriteRegistryValue(ULONG RelativeTo, PCWSTR Path, PCWSTR ValueName, ULONG ValueType, PVOID ValueData, ULONG ValueLength);
NTSTATUS NTAPI ZwOpenKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS NTAPI ZwCreateKey(PHANDLE KeyHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG TitleIndex, PUNICODE_STRING Class, ULONG CreateOptions, PULONG Disposition);
NTSTATUS NTAPI ZwSetValueKey(HANDLE KeyHandle, PUNICODE_STRING ValueName, ULONG TitleIndex, ULONG Type, PVOID Data, ULONG DataSize);
NTSTATUS NTAPI ZwClose(HANDLE Handle);
NTSTATUS NTAPI ZwCreateDirectoryObject(PHANDLE DirectoryHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSTATUS NTAPI IoOpenDeviceRegistryKey(PDEVICE_OBJECT DeviceObject, ULONG DevInstKeyType, ACCESS_MASK DesiredAccess, PHANDLE DevInstRegKey);

/* --- Object Manager --- */
NTSTATUS NTAPI ObReferenceObjectByPointer(PVOID Object, ACCESS_MASK DesiredAccess, PVOID ObjectType, KPROCESSOR_MODE AccessMode);
NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE Handle, ACCESS_MASK DesiredAccess, PVOID ObjectType, KPROCESSOR_MODE AccessMode, PVOID *Object, PVOID HandleInformation);
VOID    FASTCALL ObfDereferenceObject(PVOID Object);

/* --- Power Management --- */
NTSTATUS NTAPI PoRequestPowerIrp(PDEVICE_OBJECT DeviceObject, UCHAR MinorFunction, POWER_STATE PowerState, PREQUEST_POWER_COMPLETE CompletionFunction, PVOID Context, PIRP *Irp);
VOID    NTAPI PoStartNextPowerIrp(PIRP Irp);
POWER_STATE NTAPI PoSetPowerState(PDEVICE_OBJECT DeviceObject, POWER_STATE_TYPE Type, POWER_STATE State);
NTSTATUS FASTCALL PoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
PULONG  NTAPI PoRegisterDeviceForIdleDetection(PDEVICE_OBJECT DeviceObject, ULONG ConservationIdleTime, ULONG PerformanceIdleTime, DEVICE_POWER_STATE State);

/* --- WMI --- */
NTSTATUS NTAPI IoWMIRegistrationControl(PDEVICE_OBJECT DeviceObject, ULONG Action);
NTSTATUS NTAPI WmiSystemControl(PWMILIB_CONTEXT WmiLibInfo, PDEVICE_OBJECT DeviceObject, PIRP Irp, PSYSCTL_IRP_DISPOSITION IrpDisposition);
NTSTATUS NTAPI WmiCompleteRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp, NTSTATUS Status, ULONG BufferUsed, CHAR PriorityBoost);

/* --- HAL Port I/O --- */
UCHAR   NTAPI READ_PORT_UCHAR(PUCHAR Port);
USHORT  NTAPI READ_PORT_USHORT(PUSHORT Port);
VOID    NTAPI READ_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);
VOID    NTAPI WRITE_PORT_UCHAR(PUCHAR Port, UCHAR Value);
VOID    NTAPI WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer, ULONG Count);

/* --- HAL Misc --- */
ULONG   NTAPI HalGetInterruptVector(INTERFACE_TYPE InterfaceType, ULONG BusNumber, ULONG BusInterruptLevel, ULONG BusInterruptVector, PKIRQL Irql, PULONG Affinity);
BOOLEAN NTAPI HalTranslateBusAddress(INTERFACE_TYPE InterfaceType, ULONG BusNumber, PHYSICAL_ADDRESS BusAddress, PULONG AddressSpace, PPHYSICAL_ADDRESS TranslatedAddress);
VOID    NTAPI KeStallExecutionProcessor(ULONG MicroSeconds);

/* --- Device Queue --- */
BOOLEAN NTAPI KeInsertByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue, PKDEVICE_QUEUE_ENTRY DeviceQueueEntry, ULONG SortKey);
PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue, ULONG SortKey);
PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveDeviceQueue(PKDEVICE_QUEUE DeviceQueue);

/* --- Misc --- */
LONGLONG CDECL _allmul(LONGLONG a, LONGLONG b);
ULONGLONG CDECL _aulldiv(ULONGLONG a, ULONGLONG b);
int     CDECL _except_handler3(void *ExceptionRecord, void *EstablisherFrame, void *ContextRecord, void *DispatcherContext);
VOID    NTAPI KeBugCheckEx(ULONG BugCheckCode, ULONG_PTR BugCheckParameter1, ULONG_PTR BugCheckParameter2, ULONG_PTR BugCheckParameter3, ULONG_PTR BugCheckParameter4);
VOID    NTAPI KeQuerySystemTime(PLARGE_INTEGER CurrentTime);

/* --- Globals --- */
extern ULONG InitSafeBootMode;
extern ULONG NlsMbCodePageTag;

/* ================================================================
 * IMPORT TABLES (for PE loader)
 * ================================================================ */

extern IMPORT_FUNC_ENTRY ntkrnl_functions[];
extern IMPORT_FUNC_ENTRY hal_functions[];
extern IMPORT_FUNC_ENTRY wmilib_functions[];

#pragma pack(pop)

#endif /* _NTKRNL_H_ */
