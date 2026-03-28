/*
 * NTKSHIM.H - NT Kernel / HAL Shim Layer for Win9x VxD Environment
 *
 * Provides NT kernel API types and function prototypes so that NT5
 * (Windows 2000/XP) kernel-mode drivers can be hosted inside a
 * Win9x VxD. Each function is implemented in NTKSHIM.C on top of
 * VMM services (heap, timers, interrupts) and direct x86 port I/O.
 *
 * Full structure definitions for IRP, DEVICE_OBJECT, DRIVER_OBJECT
 * live in IRPMGR.H. This header forward-declares them and provides
 * the primitive kernel types, status codes, IRQL levels, pool types,
 * and shim function prototypes.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#ifndef NTKSHIM_H
#define NTKSHIM_H

/* ================================================================
 * COMPILER CONFIGURATION
 * ================================================================ */

#ifdef __WATCOMC__
  #define NTAPI     __stdcall
  #define CDECL     __cdecl
  #define FASTCALL  __fastcall
#else
  #define NTAPI     __attribute__((stdcall))
  #define CDECL     __attribute__((cdecl))
  #define FASTCALL  __attribute__((fastcall))
#endif

/* ================================================================
 * PRIMITIVE TYPES
 *
 * Must match W9XDDK.H definitions. Guard with _NTMINI_TYPES_DEFINED
 * so both headers can coexist without redefinition errors.
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

typedef LONG            NTSTATUS;
typedef ULONG           SIZE_T;
typedef ULONG           ULONG_PTR;
typedef char            CHAR;
typedef CHAR           *PCHAR;
typedef LONG           *PLONG;
typedef long long       LONGLONG;
typedef unsigned long long ULONGLONG;

/* ================================================================
 * WCHAR / UNICODE / ANSI STRING TYPES
 * ================================================================ */

#ifndef _NTK_WCHAR_DEFINED
#define _NTK_WCHAR_DEFINED
typedef USHORT          WCHAR;
typedef WCHAR          *PWSTR;
typedef const WCHAR    *PCWSTR;
#endif

typedef struct _UNICODE_STRING {
    USHORT  Length;          /* bytes, not including terminator */
    USHORT  MaximumLength;  /* total buffer size in bytes */
    PWSTR   Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef const UNICODE_STRING *PCUNICODE_STRING;

typedef struct _ANSI_STRING {
    USHORT  Length;
    USHORT  MaximumLength;
    PCHAR   Buffer;
} ANSI_STRING, *PANSI_STRING;

typedef const ANSI_STRING *PCANSI_STRING;

/* ================================================================
 * LARGE_INTEGER
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
    long long QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef LARGE_INTEGER PHYSICAL_ADDRESS, *PPHYSICAL_ADDRESS;

/* ================================================================
 * NTSTATUS CODES
 * ================================================================ */

#define STATUS_SUCCESS              ((NTSTATUS)0x00000000L)
#define STATUS_UNSUCCESSFUL         ((NTSTATUS)0xC0000001L)
#define STATUS_NOT_IMPLEMENTED      ((NTSTATUS)0xC0000002L)
#define STATUS_INSUFFICIENT_RESOURCES ((NTSTATUS)0xC000009AL)
#define STATUS_PENDING              ((NTSTATUS)0x00000103L)
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)
#define STATUS_TIMEOUT              ((NTSTATUS)0x00000102L)
#define STATUS_BUFFER_OVERFLOW      ((NTSTATUS)0x80000005L)
#define STATUS_INVALID_PARAMETER    ((NTSTATUS)0xC000000DL)

#define NT_SUCCESS(s)   ((NTSTATUS)(s) >= 0)

/* ================================================================
 * IRQL LEVELS
 *
 * On Win9x single-CPU VxD, these are largely notional. We track
 * the current level for compatibility but real synchronisation
 * is done with cli/sti.
 * ================================================================ */

typedef UCHAR           KIRQL;
typedef KIRQL          *PKIRQL;

#define PASSIVE_LEVEL   0
#define APC_LEVEL       1
#define DISPATCH_LEVEL  2
#define DIRQL           26  /* Device IRQL, varies on real NT */

/* ================================================================
 * KSPIN_LOCK
 * ================================================================ */

typedef ULONG           KSPIN_LOCK;
typedef KSPIN_LOCK     *PKSPIN_LOCK;

/* ================================================================
 * POOL_TYPE
 * ================================================================ */

#define NonPagedPool            0
#define PagedPool               1
#define NonPagedPoolMustSucceed 2

/* ================================================================
 * MEMORY_CACHING_TYPE
 * ================================================================ */

#define MmNonCached             0
#define MmCached                1
#define MmWriteCombined         2

/* ================================================================
 * KPROCESSOR_MODE / ACCESS_MASK
 * ================================================================ */

typedef CHAR            KPROCESSOR_MODE;
#define KernelMode      0
#define UserMode        1

typedef ULONG           ACCESS_MASK;

/* ================================================================
 * EVENT_TYPE (for KeInitializeEvent)
 * ================================================================ */

typedef enum _EVENT_TYPE {
    NotificationEvent   = 0,
    SynchronizationEvent = 1
} EVENT_TYPE;

/* ================================================================
 * KINTERRUPT_MODE
 * ================================================================ */

typedef enum _KINTERRUPT_MODE {
    LevelSensitive = 0,
    Latched        = 1
} KINTERRUPT_MODE;

/* ================================================================
 * LIST_ENTRY (doubly-linked list)
 * ================================================================ */

typedef struct _LIST_ENTRY {
    struct _LIST_ENTRY *Flink;
    struct _LIST_ENTRY *Blink;
} LIST_ENTRY, *PLIST_ENTRY;

/* ================================================================
 * KDPC (Deferred Procedure Call)
 * ================================================================ */

struct _KDPC;  /* forward */

typedef VOID (NTAPI *PKDEFERRED_ROUTINE)(
    struct _KDPC *Dpc,
    PVOID DeferredContext,
    PVOID SystemArgument1,
    PVOID SystemArgument2
);

typedef struct _KDPC {
    PKDEFERRED_ROUTINE  DeferredRoutine;
    PVOID               DeferredContext;
    PVOID               SystemArgument1;
    PVOID               SystemArgument2;
    BOOLEAN             Queued;
} KDPC, *PKDPC;

/* ================================================================
 * KTIMER
 * ================================================================ */

typedef struct _KTIMER {
    BOOLEAN             Signaled;
    PKDPC               Dpc;
    ULONG               ShimTimerHandle;  /* VMM timer handle */
    BOOLEAN             Active;
} KTIMER, *PKTIMER;

/* ================================================================
 * KEVENT
 * ================================================================ */

typedef struct _KEVENT {
    LONG                SignalState;
    EVENT_TYPE          Type;
} KEVENT, *PKEVENT;

/* ================================================================
 * KMUTEX (simplified: single-CPU, no contention possible)
 * ================================================================ */

typedef struct _KMUTEX {
    LONG                Count;      /* 1 = available, 0 = held */
    PVOID               OwnerThread;
} KMUTEX, *PKMUTEX;

/* ================================================================
 * MDL (Memory Descriptor List)
 * ================================================================ */

typedef struct _MDL {
    struct _MDL *Next;
    ULONG       Size;
    ULONG       MdlFlags;
    PVOID       MappedSystemVa;
    PVOID       StartVa;
    ULONG       ByteCount;
    ULONG       ByteOffset;
} MDL, *PMDL;

#define MDL_MAPPED_TO_SYSTEM_VA     0x0001
#define MDL_PAGES_LOCKED            0x0002
#define MDL_SOURCE_IS_NONPAGED_POOL 0x0004

/* ================================================================
 * KDEVICE_QUEUE / KDEVICE_QUEUE_ENTRY
 * ================================================================ */

typedef struct _KDEVICE_QUEUE {
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
 * IO_STATUS_BLOCK
 * ================================================================ */

typedef struct _IO_STATUS_BLOCK {
    NTSTATUS    Status;
    ULONG_PTR   Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

/* ================================================================
 * IO_WORKITEM
 * ================================================================ */

typedef VOID (NTAPI *PIO_WORKITEM_ROUTINE)(
    struct _DEVICE_OBJECT *DeviceObject,
    PVOID Context
);

typedef struct _IO_WORKITEM {
    struct _DEVICE_OBJECT  *DeviceObject;
    PIO_WORKITEM_ROUTINE    Routine;
    PVOID                   Context;
    ULONG                   QueueType;
} IO_WORKITEM, *PIO_WORKITEM;

typedef enum _WORK_QUEUE_TYPE {
    CriticalWorkQueue = 0,
    DelayedWorkQueue  = 1
} WORK_QUEUE_TYPE;

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
    PKSERVICE_ROUTINE   ServiceRoutine;
    PVOID               ServiceContext;
    PKSPIN_LOCK         SpinLock;
    ULONG               Vector;
    KIRQL               Irql;
    KIRQL               SynchronizeIrql;
    KINTERRUPT_MODE     InterruptMode;
    BOOLEAN             ShareVector;
    ULONG               ShimIrqHandle;   /* VPICD handle */
    BOOLEAN             ShimConnected;
    KSPIN_LOCK          OwnSpinLock;
} KINTERRUPT, *PKINTERRUPT;

/* ================================================================
 * IO_TIMER_ROUTINE (for IoInitializeTimer/IoStartTimer)
 * ================================================================ */

typedef VOID (NTAPI *PIO_TIMER_ROUTINE)(
    struct _DEVICE_OBJECT *DeviceObject,
    PVOID Context
);

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
 * RTL_QUERY_REGISTRY_TABLE
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
    PVOID   GuidList;
    PVOID   QueryWmiRegInfo;
    PVOID   QueryWmiDataBlock;
    PVOID   SetWmiDataBlock;
    PVOID   SetWmiDataItem;
    PVOID   ExecuteWmiMethod;
    PVOID   WmiFunctionControl;
} WMILIB_CONTEXT, *PWMILIB_CONTEXT;

/* ================================================================
 * FORWARD DECLARATIONS
 *
 * Full definitions live in IRPMGR.H.
 * ================================================================ */

typedef struct _DEVICE_OBJECT  DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT  DRIVER_OBJECT, *PDRIVER_OBJECT;
typedef struct _FILE_OBJECT    FILE_OBJECT, *PFILE_OBJECT;
typedef struct _IRP            IRP, *PIRP;

/* ================================================================
 * DPC QUEUE LIMITS
 * ================================================================ */

#define NTK_MAX_PENDING_DPCS    16

/* ================================================================
 * FUNCTION PROTOTYPES
 * ================================================================ */

/* --- Spinlocks --- */
VOID  NTAPI KeInitializeSpinLock(PKSPIN_LOCK SpinLock);
VOID  NTAPI KeAcquireSpinLock(PKSPIN_LOCK SpinLock, PKIRQL OldIrql);
VOID  NTAPI KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);
VOID  NTAPI KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock);
VOID  NTAPI KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK SpinLock);

/* --- DPC --- */
VOID    NTAPI KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
                              PVOID DeferredContext);
BOOLEAN NTAPI KeInsertQueueDpc(PKDPC Dpc, PVOID Arg1, PVOID Arg2);
VOID    __cdecl ntk_DrainDpcQueue(void);

/* --- Timers --- */
VOID    NTAPI KeInitializeTimer(PKTIMER Timer);
BOOLEAN NTAPI KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc);
BOOLEAN NTAPI KeCancelTimer(PKTIMER Timer);
VOID    NTAPI KeQuerySystemTime(PLARGE_INTEGER CurrentTime);

/* --- I/O Timer --- */
VOID    NTAPI ntk_IoInitializeTimer(PDEVICE_OBJECT DeviceObject,
                                     PIO_TIMER_ROUTINE TimerRoutine,
                                     PVOID Context);
VOID    NTAPI ntk_IoStartTimer(PDEVICE_OBJECT DeviceObject);

/* --- Events --- */
VOID    NTAPI KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type,
                                BOOLEAN State);
LONG    NTAPI KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait);
VOID    NTAPI KeResetEvent(PKEVENT Event);
NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, ULONG WaitReason,
                                     ULONG WaitMode, BOOLEAN Alertable,
                                     PLARGE_INTEGER Timeout);

/* --- Pool allocation --- */
PVOID   NTAPI ExAllocatePoolWithTag(ULONG PoolType, SIZE_T Size, ULONG Tag);
VOID    NTAPI ExFreePoolWithTag(PVOID Ptr, ULONG Tag);
PVOID   NTAPI ExAllocatePool(ULONG PoolType, SIZE_T Size);

/* --- Registry stubs --- */
NTSTATUS NTAPI ZwOpenKey(PVOID KeyHandle, ULONG DesiredAccess,
                         PVOID ObjectAttributes);
NTSTATUS NTAPI ZwQueryValueKey(PVOID KeyHandle, PVOID ValueName,
                               ULONG KeyValueInformationClass,
                               PVOID KeyValueInformation,
                               ULONG Length, PULONG ResultLength);
NTSTATUS NTAPI ZwSetValueKey(PVOID KeyHandle, PVOID ValueName,
                             ULONG TitleIndex, ULONG Type,
                             PVOID Data, ULONG DataSize);
NTSTATUS NTAPI ZwClose(PVOID Handle);
NTSTATUS NTAPI ntk_ZwCreateKey(PVOID KeyHandle, ULONG DesiredAccess,
                                PVOID ObjectAttributes, ULONG TitleIndex,
                                PVOID Class, ULONG CreateOptions,
                                PULONG Disposition);
NTSTATUS NTAPI ntk_ZwCreateDirectoryObject(PVOID DirHandle, ULONG Access,
                                            PVOID ObjectAttributes);

/* --- HAL I/O Port Access --- */
UCHAR   NTAPI READ_PORT_UCHAR(PUCHAR Port);
USHORT  NTAPI READ_PORT_USHORT(PUSHORT Port);
ULONG   NTAPI READ_PORT_ULONG(PULONG Port);
VOID    NTAPI WRITE_PORT_UCHAR(PUCHAR Port, UCHAR Value);
VOID    NTAPI WRITE_PORT_USHORT(PUSHORT Port, USHORT Value);
VOID    NTAPI WRITE_PORT_ULONG(PULONG Port, ULONG Value);

/* --- HAL Bus Access --- */
BOOLEAN NTAPI HalTranslateBusAddress(ULONG InterfaceType, ULONG BusNumber,
                                     PHYSICAL_ADDRESS BusAddress,
                                     PULONG AddressSpace,
                                     PPHYSICAL_ADDRESS TranslatedAddress);
ULONG   NTAPI HalGetBusData(ULONG BusDataType, ULONG BusNumber,
                             ULONG SlotNumber, PVOID Buffer, ULONG Length);
ULONG   NTAPI HalSetBusData(ULONG BusDataType, ULONG BusNumber,
                             ULONG SlotNumber, PVOID Buffer, ULONG Length);

/* --- RTL Memory Utilities --- */
VOID    __cdecl RtlZeroMemory(PVOID Destination, SIZE_T Length);
VOID    __cdecl RtlCopyMemory(PVOID Destination, PVOID Source, SIZE_T Length);
VOID    __cdecl RtlMoveMemory(PVOID Destination, PVOID Source, SIZE_T Length);
SIZE_T  __cdecl RtlCompareMemory(PVOID Source1, PVOID Source2, SIZE_T Length);

/* --- I/O Work Items --- */
PIO_WORKITEM NTAPI ntk_IoAllocateWorkItem(PDEVICE_OBJECT DeviceObject);
VOID    NTAPI ntk_IoQueueWorkItem(PIO_WORKITEM WorkItem,
                                   PIO_WORKITEM_ROUTINE Routine,
                                   ULONG QueueType, PVOID Context);
VOID    NTAPI ntk_IoFreeWorkItem(PIO_WORKITEM WorkItem);

/* --- StartIo Serialization --- */
VOID    NTAPI ntk_IoStartPacket(PDEVICE_OBJECT DeviceObject, PVOID Irp,
                                 PULONG Key, PVOID CancelFunction);
VOID    NTAPI ntk_IoStartNextPacket(PDEVICE_OBJECT DeviceObject,
                                     BOOLEAN Cancelable);

/* --- Device Queue --- */
VOID    NTAPI ntk_KeInitializeDeviceQueue(PKDEVICE_QUEUE DeviceQueue);
BOOLEAN NTAPI ntk_KeInsertByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue,
                                            PKDEVICE_QUEUE_ENTRY Entry,
                                            ULONG SortKey);
PKDEVICE_QUEUE_ENTRY NTAPI ntk_KeRemoveByKeyDeviceQueue(
    PKDEVICE_QUEUE DeviceQueue, ULONG SortKey);
PKDEVICE_QUEUE_ENTRY NTAPI ntk_KeRemoveDeviceQueue(
    PKDEVICE_QUEUE DeviceQueue);

/* --- Memory Manager --- */
PVOID   NTAPI ntk_MmMapIoSpace(PHYSICAL_ADDRESS PhysAddr, SIZE_T Length,
                                ULONG CacheType);
VOID    NTAPI ntk_MmUnmapIoSpace(PVOID BaseAddr, SIZE_T Length);
PVOID   NTAPI ntk_MmMapLockedPagesSpecifyCache(PMDL Mdl, ULONG AccessMode,
                                                 ULONG CacheType,
                                                 PVOID BaseAddr,
                                                 ULONG BugCheckOnFail,
                                                 ULONG Priority);
VOID    NTAPI ntk_MmBuildMdlForNonPagedPool(PMDL Mdl);
PVOID   NTAPI ntk_MmLockPagableDataSection(PVOID Address);
VOID    NTAPI ntk_MmUnlockPagableImageSection(PVOID SectionHandle);
VOID    NTAPI ntk_MmUnlockPages(PMDL Mdl);
PMDL    NTAPI ntk_IoAllocateMdl(PVOID VirtualAddress, ULONG Length,
                                 BOOLEAN SecondaryBuffer,
                                 BOOLEAN ChargeQuota, PVOID Irp);
VOID    NTAPI ntk_IoFreeMdl(PMDL Mdl);

/* MmHighestUserAddress exported variable */
extern PVOID ntk_MmHighestUserAddress;

/* --- Object Manager --- */
NTSTATUS NTAPI ntk_ObReferenceObjectByPointer(PVOID Object, ULONG Access,
                                               PVOID ObjectType,
                                               ULONG AccessMode);
NTSTATUS NTAPI ntk_ObReferenceObjectByHandle(PVOID Handle, ULONG Access,
                                              PVOID ObjectType,
                                              ULONG AccessMode,
                                              PVOID *Object,
                                              PVOID HandleInfo);
VOID    NTAPI ntk_ObfDereferenceObject(PVOID Object);

/* --- Interrupt --- */
NTSTATUS NTAPI ntk_IoConnectInterrupt(
    PKINTERRUPT *InterruptObject,
    PKSERVICE_ROUTINE ServiceRoutine,
    PVOID ServiceContext,
    PKSPIN_LOCK SpinLock,
    ULONG Vector,
    KIRQL Irql,
    KIRQL SyncIrql,
    ULONG InterruptMode,
    BOOLEAN ShareVector,
    ULONG ProcessorMask,
    BOOLEAN FloatingSave);
VOID    NTAPI ntk_IoDisconnectInterrupt(PKINTERRUPT InterruptObject);
BOOLEAN NTAPI ntk_KeSynchronizeExecution(PKINTERRUPT Interrupt,
                                          PKSYNCHRONIZE_ROUTINE SyncRoutine,
                                          PVOID SyncContext);

/* --- Unicode/Ansi String Functions --- */
VOID    NTAPI ntk_RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Source);
VOID    NTAPI ntk_RtlInitAnsiString(PANSI_STRING Dest, const CHAR *Source);
VOID    NTAPI ntk_RtlCopyUnicodeString(PUNICODE_STRING Dest,
                                        PCUNICODE_STRING Source);
VOID    NTAPI ntk_RtlFreeUnicodeString(PUNICODE_STRING Str);
LONG    NTAPI ntk_RtlCompareUnicodeString(PCUNICODE_STRING Str1,
                                           PCUNICODE_STRING Str2,
                                           BOOLEAN CaseInsensitive);
NTSTATUS NTAPI ntk_RtlAnsiStringToUnicodeString(PUNICODE_STRING Dest,
                                                  PANSI_STRING Source,
                                                  BOOLEAN AllocDest);
NTSTATUS NTAPI ntk_RtlAppendUnicodeStringToString(PUNICODE_STRING Dest,
                                                    PCUNICODE_STRING Source);
NTSTATUS NTAPI ntk_RtlIntegerToUnicodeString(ULONG Value, ULONG Base,
                                              PUNICODE_STRING Str);
BOOLEAN NTAPI ntk_RtlPrefixUnicodeString(PCUNICODE_STRING Prefix,
                                          PCUNICODE_STRING String,
                                          BOOLEAN CaseInsensitive);
ULONG   NTAPI ntk_RtlxAnsiStringToUnicodeSize(PANSI_STRING AnsiStr);
NTSTATUS NTAPI ntk_RtlWriteRegistryValue(ULONG RelativeTo, PCWSTR Path,
                                          PCWSTR ValueName, ULONG ValueType,
                                          PVOID ValueData, ULONG ValueLength);
NTSTATUS NTAPI ntk_RtlQueryRegistryValues(ULONG RelativeTo, PCWSTR Path,
                                           PRTL_QUERY_REGISTRY_TABLE Table,
                                           PVOID Context, PVOID Environment);

/* --- IRP Builders --- */
PVOID   NTAPI ntk_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode, PDEVICE_OBJECT DeviceObject,
    PVOID InBuf, ULONG InLen, PVOID OutBuf, ULONG OutLen,
    BOOLEAN InternalDeviceControl, PKEVENT Event,
    PIO_STATUS_BLOCK IoStatusBlock);
PVOID   NTAPI ntk_IoBuildSynchronousFsdRequest(
    ULONG MajorFunction, PDEVICE_OBJECT DeviceObject,
    PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
    PKEVENT Event, PIO_STATUS_BLOCK IoStatusBlock);
PVOID   NTAPI ntk_IoBuildAsynchronousFsdRequest(
    ULONG MajorFunction, PDEVICE_OBJECT DeviceObject,
    PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
    PIO_STATUS_BLOCK IoStatusBlock);

/* --- Driver Object Extensions --- */
NTSTATUS NTAPI ntk_IoAllocateDriverObjectExtension(
    PDRIVER_OBJECT DriverObject, PVOID ClientId,
    ULONG ExtensionSize, PVOID *Extension);
PVOID   NTAPI ntk_IoGetDriverObjectExtension(PDRIVER_OBJECT DriverObject,
                                              PVOID ClientId);

/* --- Misc I/O Stubs --- */
NTSTATUS NTAPI ntk_IoCreateSymbolicLink(PVOID SymLinkName, PVOID DevName);
NTSTATUS NTAPI ntk_IoDeleteSymbolicLink(PVOID SymLinkName);
PCONFIGURATION_INFORMATION NTAPI ntk_IoGetConfigurationInformation(void);
VOID    NTAPI ntk_IoInvalidateDeviceRelations(PDEVICE_OBJECT DeviceObject,
                                               ULONG Type);
VOID    NTAPI ntk_IoInvalidateDeviceState(PDEVICE_OBJECT DeviceObject);
NTSTATUS NTAPI ntk_IoReportDetectedDevice(PDRIVER_OBJECT DriverObject,
                                            ULONG LegacyBusType,
                                            ULONG BusNumber,
                                            ULONG SlotNumber,
                                            PVOID ResourceList,
                                            PVOID ResourceRequirements,
                                            BOOLEAN ResourceAssigned,
                                            PDEVICE_OBJECT *DeviceObject);
NTSTATUS NTAPI ntk_IoReportResourceForDetection(PDRIVER_OBJECT DriverObject,
                                                  PVOID DriverList,
                                                  ULONG DriverListSize,
                                                  PDEVICE_OBJECT DeviceObject,
                                                  PVOID DeviceList,
                                                  ULONG DeviceListSize,
                                                  PVOID ConflictDetected);
NTSTATUS NTAPI ntk_IoOpenDeviceRegistryKey(PDEVICE_OBJECT DeviceObject,
                                            ULONG DevInstKeyType,
                                            ULONG DesiredAccess,
                                            PVOID DevInstRegKey);
PVOID   NTAPI ntk_PoRegisterDeviceForIdleDetection(
    PDEVICE_OBJECT DeviceObject, ULONG ConservationTime,
    ULONG PerformanceTime, ULONG State);
NTSTATUS NTAPI ntk_IoWMIRegistrationControl(PDEVICE_OBJECT DeviceObject,
                                              ULONG Action);

/* --- WMILIB.SYS --- */
NTSTATUS NTAPI ntk_WmiCompleteRequest(PDEVICE_OBJECT DeviceObject,
                                       PVOID Irp, NTSTATUS Status,
                                       ULONG BufferUsed, CHAR PrioBoost);
NTSTATUS NTAPI ntk_WmiSystemControl(PWMILIB_CONTEXT WmiLibInfo,
                                     PDEVICE_OBJECT DeviceObject,
                                     PVOID Irp,
                                     PSYSCTL_IRP_DISPOSITION IrpDisposition);

/* --- C Runtime / Compiler Intrinsics --- */
int     __cdecl ntk_sprintf(PCHAR buf, const PCHAR fmt, ...);
int     __cdecl ntk_swprintf(PWSTR buf, PCWSTR fmt, ...);
PCHAR   __cdecl ntk_strstr(const PCHAR haystack, const PCHAR needle);
PCHAR   __cdecl ntk_strupr(PCHAR str);
PVOID   __cdecl ntk_memmove(PVOID dest, PVOID src, SIZE_T count);
int     __cdecl ntk_except_handler3(PVOID ExceptionRecord,
                                     PVOID EstablisherFrame,
                                     PVOID ContextRecord,
                                     PVOID DispatcherContext);
ULONGLONG __cdecl ntk_aulldiv(ULONGLONG a, ULONGLONG b);
LONGLONG  __cdecl ntk_allmul(LONGLONG a, LONGLONG b);
LONGLONG  __cdecl ntk_alldiv(LONGLONG a, LONGLONG b);

/* --- Interlocked Operations --- */
LONG    NTAPI ntk_InterlockedDecrement(PLONG Addend);
LONG    NTAPI ntk_InterlockedIncrement(PLONG Addend);
LONG    NTAPI ntk_InterlockedExchange(PLONG Target, LONG Value);

/* --- Exported Variables --- */
extern ULONG ntk_NlsMbCodePageTag;
extern ULONG ntk_InitSafeBootMode;
extern ULONG ntk_KeTickCount;

/* --- Misc --- */
ULONG   __cdecl DbgPrint(const char *Format, ...);
VOID    NTAPI KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2,
                           ULONG_PTR P3, ULONG_PTR P4);
PVOID   NTAPI IoGetCurrentProcess(void);
KIRQL   NTAPI KeGetCurrentIrql(void);

#endif /* NTKSHIM_H */
