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
#else
  #define NTAPI     __attribute__((stdcall))
  #define CDECL     __attribute__((cdecl))
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
 * EVENT_TYPE (for KeInitializeEvent)
 * ================================================================ */

typedef enum _EVENT_TYPE {
    NotificationEvent   = 0,
    SynchronizationEvent = 1
} EVENT_TYPE;

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

/* --- Misc --- */
ULONG   __cdecl DbgPrint(const char *Format, ...);
VOID    NTAPI KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2,
                           ULONG_PTR P3, ULONG_PTR P4);
PVOID   NTAPI IoGetCurrentProcess(void);
KIRQL   NTAPI KeGetCurrentIrql(void);

#endif /* NTKSHIM_H */
