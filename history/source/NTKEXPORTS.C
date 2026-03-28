/*
 * NTKEXPORTS.C - Export Tables for Multi-DLL PE Loader
 *
 * Defines the import resolution tables that map NT kernel function
 * names (as they appear in PE import tables) to our shim
 * implementations across NTKSHIM, IRPMGR, PNPMGR, and PCIBUS.
 *
 * The PE loader (PELOAD.C) calls resolve_import() against these
 * tables when processing each DLL's import directory. Four tables
 * cover the DLLs that the W2K atapi.sys imports from:
 *
 *   ntoskrnl_exports[] - ntoskrnl.exe: Ke*, Ex*, Io*, Zw*, Rtl*, Po*
 *   hal_exports[]      - HAL.dll:      READ/WRITE_PORT_*, Hal*, Kf*
 *   wmilib_exports[]   - WMILIB.SYS:   Wmi*
 *   scsiport_exports[] - SCSIPORT.SYS: ScsiPort* (extern from V5)
 *
 * A master table (g_dll_tables[]) maps DLL names to their function
 * tables for automatic lookup during PE import resolution.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#include "NTKSHIM.H"
#include "IRPMGR.H"
#include "PNPMGR.H"
#include "PCIBUS.H"
#include "NTKRNL.H"    /* IMPORT_FUNC_ENTRY typedef */

/* ================================================================
 * EXTERN DECLARATIONS
 *
 * Functions implemented in other modules (NTKSHIM, IRPMGR, PNPMGR)
 * that are not prototyped in included headers, or that need stub
 * implementations to be added later. Cast to (PVOID) in the tables
 * to avoid type mismatch warnings.
 * ================================================================ */

/*
 * Stub functions not yet implemented in any module.
 * These are declared here so the export table compiles.
 * Each should be implemented as a proper stub or full
 * function before the driver is actually loaded.
 */
extern NTSTATUS NTAPI ntk_IoBuildDeviceIoControlRequest(
    ULONG, PDEVICE_OBJECT, PVOID, ULONG, PVOID, ULONG,
    BOOLEAN, PVOID, PVOID);
extern NTSTATUS NTAPI ntk_IoConnectInterrupt(
    PVOID, PVOID, PVOID, PVOID, ULONG, UCHAR, UCHAR,
    ULONG, BOOLEAN, ULONG, BOOLEAN);
extern PCONFIGURATION_INFORMATION NTAPI ntk_IoGetConfigurationInformation(void);
extern NTSTATUS NTAPI ntk_IoCreateSymbolicLink(PVOID, PVOID);
extern NTSTATUS NTAPI ntk_IoDeleteSymbolicLink(PVOID);
extern VOID    NTAPI ntk_MmUnmapIoSpace(PVOID, ULONG);
extern VOID    NTAPI ntk_IoQueueWorkItem(PVOID, PVOID, ULONG, PVOID);
extern PVOID   NTAPI ntk_IoAllocateWorkItem(PVOID);
extern VOID    NTAPI ntk_IoFreeWorkItem(PVOID);
extern NTSTATUS NTAPI ntk_ObReferenceObjectByPointer(PVOID, ULONG, PVOID, ULONG);
extern VOID    NTAPI ntk_RtlAnsiStringToUnicodeString(PVOID, PVOID, BOOLEAN);
extern VOID    NTAPI ntk_RtlInitAnsiString(PVOID, PVOID);
extern LONG    NTAPI ntk_InterlockedDecrement(LONG *);
extern VOID    NTAPI ntk_IoDisconnectInterrupt(PVOID);
extern LONG    NTAPI ntk_RtlCompareUnicodeString(PVOID, PVOID, BOOLEAN);
extern VOID    NTAPI ntk_IoInvalidateDeviceRelations(PVOID, ULONG);
extern NTSTATUS NTAPI ntk_IoReportDetectedDevice(
    PVOID, ULONG, ULONG, ULONG, PVOID, PVOID, BOOLEAN, PVOID);
extern LONG    NTAPI ntk_InterlockedIncrement(LONG *);
extern PVOID   NTAPI ntk_MmMapIoSpace(PHYSICAL_ADDRESS, ULONG, ULONG);
extern VOID    __cdecl ntk_ObfDereferenceObject(PVOID);
extern VOID    NTAPI ntk_IoInvalidateDeviceState(PVOID);
extern int     __cdecl ntk_sprintf(char *, const char *, ...);
extern ULONG   NTAPI ntk_RtlxAnsiStringToUnicodeSize(PVOID);
extern BOOLEAN NTAPI ntk_KeInsertByKeyDeviceQueue(PVOID, PVOID, ULONG);
extern PVOID   NTAPI ntk_PoRegisterDeviceForIdleDetection(PVOID, ULONG, ULONG, ULONG);
extern LONG    NTAPI ntk_InterlockedExchange(LONG *, LONG);
extern PVOID   NTAPI ntk_MmMapLockedPagesSpecifyCache(PVOID, ULONG, ULONG, PVOID, ULONG, ULONG);
extern NTSTATUS NTAPI ntk_ObReferenceObjectByHandle(PVOID, ULONG, PVOID, ULONG, PVOID, PVOID);
extern NTSTATUS NTAPI ntk_ZwCreateDirectoryObject(PVOID, ULONG, PVOID);
extern PVOID   NTAPI ntk_IoBuildSynchronousFsdRequest(ULONG, PVOID, PVOID, ULONG, PVOID, PVOID, PVOID);
extern VOID    NTAPI ntk_RtlCopyUnicodeString(PVOID, PVOID);
extern NTSTATUS NTAPI ntk_IoAllocateDriverObjectExtension(PVOID, PVOID, ULONG, PVOID);
extern NTSTATUS NTAPI ntk_IoReportResourceForDetection(PVOID, PVOID, ULONG, PVOID, PVOID, ULONG, PVOID);
extern VOID   *__cdecl ntk_memmove(void *, const void *, ULONG);
extern VOID    NTAPI ntk_RtlFreeUnicodeString(PVOID);
extern VOID    NTAPI ntk_IoStartTimer(PVOID);
extern VOID    NTAPI ntk_ZwCreateKey(PVOID, ULONG, PVOID, ULONG, PVOID, ULONG, PVOID);
extern VOID    NTAPI ntk_RtlAppendUnicodeStringToString(PVOID, PVOID);
extern NTSTATUS NTAPI ntk_RtlIntegerToUnicodeString(ULONG, ULONG, PVOID);
extern VOID    NTAPI ntk_IoStartPacket(PVOID, PVOID, PVOID, PVOID);
extern BOOLEAN NTAPI ntk_KeSynchronizeExecution(PVOID, PVOID, PVOID);
extern VOID    NTAPI ntk_IoStartNextPacket(PVOID, BOOLEAN);
extern PVOID   NTAPI ntk_IoBuildAsynchronousFsdRequest(ULONG, PVOID, PVOID, ULONG, PVOID, PVOID);
extern VOID    NTAPI ntk_IoFreeMdl(PVOID);
extern VOID    NTAPI ntk_MmUnlockPages(PVOID);
extern PVOID   NTAPI ntk_KeRemoveByKeyDeviceQueue(PVOID, ULONG);
extern PVOID   NTAPI ntk_MmBuildMdlForNonPagedPool(PVOID);
extern PVOID   NTAPI ntk_IoAllocateMdl(PVOID, ULONG, BOOLEAN, BOOLEAN, PVOID);
extern PVOID   NTAPI ntk_KeRemoveDeviceQueue(PVOID);
extern NTSTATUS NTAPI ntk_IoOpenDeviceRegistryKey(PVOID, ULONG, ULONG, PVOID);
extern NTSTATUS NTAPI ntk_RtlWriteRegistryValue(ULONG, PVOID, PVOID, ULONG, PVOID, ULONG);
extern long long __cdecl ntk_aulldiv(long long, long long);
extern long long __cdecl ntk_allmul(long long, long long);
extern long long __cdecl ntk_alldiv(long long, long long);
extern int     __cdecl ntk_except_handler3(PVOID, PVOID, PVOID, PVOID);
extern PVOID   NTAPI ntk_MmLockPagableDataSection(PVOID);
extern VOID    NTAPI ntk_MmUnlockPagableImageSection(PVOID);
extern BOOLEAN NTAPI ntk_RtlPrefixUnicodeString(PVOID, PVOID, BOOLEAN);
extern NTSTATUS NTAPI ntk_IoWMIRegistrationControl(PVOID, ULONG);
extern int     __cdecl ntk_swprintf(PVOID, PVOID, ...);
extern VOID    NTAPI ntk_RtlInitUnicodeString(PVOID, PVOID);
extern PVOID   NTAPI ntk_IoGetDriverObjectExtension(PVOID, PVOID);
extern VOID    NTAPI ntk_IoInitializeTimer(PVOID, PVOID, PVOID);
extern NTSTATUS NTAPI ntk_RtlQueryRegistryValues(ULONG, PVOID, PVOID, PVOID, PVOID);
extern char   *__cdecl ntk_strstr(const char *, const char *);
extern char   *__cdecl ntk_strupr(char *);

/* Globals that W2K atapi.sys imports */
extern ULONG  ntk_KeTickCount;
extern ULONG  ntk_KeQueryTimeIncrement;
extern ULONG  ntk_NlsMbCodePageTag;
extern ULONG  ntk_InitSafeBootMode;
extern PVOID  ntk_MmHighestUserAddress;

/* WMI functions (WMILIB.SYS) */
extern NTSTATUS NTAPI ntk_WmiCompleteRequest(PVOID, PVOID, NTSTATUS, ULONG, UCHAR);
extern NTSTATUS NTAPI ntk_WmiSystemControl(PVOID, PVOID, PVOID, PVOID);

/* ================================================================
 * ntoskrnl.exe EXPORT TABLE
 *
 * Maps NT kernel import names to NTKSHIM, IRPMGR, and PNPMGR
 * function implementations. The string names must match exactly
 * what appears in the PE import directory of NT5 .sys files.
 *
 * Note: IofCallDriver and IofCompleteRequest are the "fast-call"
 * aliases that many drivers import instead of IoCallDriver /
 * IoCompleteRequest. Both resolve to the same implementation.
 * ================================================================ */

static const IMPORT_FUNC_ENTRY ntoskrnl_exports[] = {

    /* --- Spinlocks (NTKSHIM) --- */
    { "KeInitializeSpinLock",           (PVOID)KeInitializeSpinLock },
    { "KeAcquireSpinLock",              (PVOID)KeAcquireSpinLock },
    { "KeReleaseSpinLock",              (PVOID)KeReleaseSpinLock },
    { "KeAcquireSpinLockAtDpcLevel",    (PVOID)KeAcquireSpinLockAtDpcLevel },
    { "KeReleaseSpinLockFromDpcLevel",  (PVOID)KeReleaseSpinLockFromDpcLevel },
    { "KefAcquireSpinLockAtDpcLevel",   (PVOID)KeAcquireSpinLockAtDpcLevel },
    { "KefReleaseSpinLockFromDpcLevel", (PVOID)KeReleaseSpinLockFromDpcLevel },

    /* --- DPC (NTKSHIM) --- */
    { "KeInitializeDpc",                (PVOID)KeInitializeDpc },
    { "KeInsertQueueDpc",               (PVOID)KeInsertQueueDpc },

    /* --- Timers (NTKSHIM) --- */
    { "KeInitializeTimer",              (PVOID)KeInitializeTimer },
    { "KeInitializeTimerEx",            (PVOID)KeInitializeTimer },
    { "KeSetTimer",                     (PVOID)KeSetTimer },
    { "KeSetTimerEx",                   (PVOID)KeSetTimer },
    { "KeCancelTimer",                  (PVOID)KeCancelTimer },
    { "KeQuerySystemTime",              (PVOID)KeQuerySystemTime },

    /* --- Events (NTKSHIM) --- */
    { "KeInitializeEvent",              (PVOID)KeInitializeEvent },
    { "KeSetEvent",                     (PVOID)KeSetEvent },
    { "KeResetEvent",                   (PVOID)KeResetEvent },
    { "KeWaitForSingleObject",          (PVOID)KeWaitForSingleObject },

    /* --- IRQL (NTKSHIM) --- */
    { "KeGetCurrentIrql",               (PVOID)KeGetCurrentIrql },

    /* --- Pool allocation (NTKSHIM) --- */
    { "ExAllocatePool",                 (PVOID)ExAllocatePool },
    { "ExAllocatePoolWithTag",          (PVOID)ExAllocatePoolWithTag },
    { "ExFreePool",                     (PVOID)ExFreePoolWithTag },
    { "ExFreePoolWithTag",              (PVOID)ExFreePoolWithTag },

    /* --- IRP allocation and dispatch (IRPMGR) --- */
    { "IoAllocateIrp",                  (PVOID)IrpMgr_IoAllocateIrp },
    { "IoFreeIrp",                      (PVOID)IrpMgr_IoFreeIrp },
    { "IoCallDriver",                   (PVOID)IrpMgr_IoCallDriver },
    { "IofCallDriver",                  (PVOID)IrpMgr_IoCallDriver },
    { "IoCompleteRequest",              (PVOID)IrpMgr_IoCompleteRequest },
    { "IofCompleteRequest",             (PVOID)IrpMgr_IoCompleteRequest },

    /* --- Device object management (IRPMGR) --- */
    { "IoCreateDevice",                 (PVOID)IrpMgr_IoCreateDevice },
    { "IoDeleteDevice",                 (PVOID)IrpMgr_IoDeleteDevice },
    { "IoAttachDeviceToDeviceStack",    (PVOID)IrpMgr_IoAttachDeviceToDeviceStack },
    { "IoDetachDevice",                 (PVOID)IrpMgr_IoDetachDevice },
    { "IoGetAttachedDeviceReference",   (PVOID)IrpMgr_IoGetAttachedDeviceReference },

    /* --- Error logging (IRPMGR) --- */
    { "IoAllocateErrorLogEntry",        (PVOID)IrpMgr_IoAllocateErrorLogEntry },
    { "IoWriteErrorLogEntry",           (PVOID)IrpMgr_IoWriteErrorLogEntry },

    /* --- Power manager (PNPMGR) --- */
    { "PoStartNextPowerIrp",            (PVOID)PnpMgr_PoStartNextPowerIrp },
    { "PoCallDriver",                   (PVOID)PnpMgr_PoCallDriver },
    { "PoRequestPowerIrp",              (PVOID)PnpMgr_PoRequestPowerIrp },
    { "PoSetPowerState",                (PVOID)PnpMgr_PoSetPowerState },
    { "PoRegisterDeviceForIdleDetection", (PVOID)ntk_PoRegisterDeviceForIdleDetection },

    /* --- Registry stubs (NTKSHIM) --- */
    { "ZwOpenKey",                      (PVOID)ZwOpenKey },
    { "ZwSetValueKey",                  (PVOID)ZwSetValueKey },
    { "ZwClose",                        (PVOID)ZwClose },
    { "ZwCreateKey",                    (PVOID)ntk_ZwCreateKey },
    { "ZwCreateDirectoryObject",        (PVOID)ntk_ZwCreateDirectoryObject },
    { "RtlQueryRegistryValues",         (PVOID)ntk_RtlQueryRegistryValues },
    { "RtlWriteRegistryValue",          (PVOID)ntk_RtlWriteRegistryValue },
    { "IoOpenDeviceRegistryKey",        (PVOID)ntk_IoOpenDeviceRegistryKey },

    /* --- RTL memory utilities (NTKSHIM) --- */
    { "RtlZeroMemory",                  (PVOID)RtlZeroMemory },
    { "RtlCopyMemory",                  (PVOID)RtlCopyMemory },
    { "RtlMoveMemory",                  (PVOID)RtlMoveMemory },
    { "RtlCompareMemory",              (PVOID)RtlCompareMemory },
    { "memmove",                        (PVOID)ntk_memmove },

    /* --- RTL string utilities --- */
    { "RtlInitUnicodeString",           (PVOID)ntk_RtlInitUnicodeString },
    { "RtlCopyUnicodeString",           (PVOID)ntk_RtlCopyUnicodeString },
    { "RtlCompareUnicodeString",        (PVOID)ntk_RtlCompareUnicodeString },
    { "RtlFreeUnicodeString",           (PVOID)ntk_RtlFreeUnicodeString },
    { "RtlAppendUnicodeStringToString", (PVOID)ntk_RtlAppendUnicodeStringToString },
    { "RtlIntegerToUnicodeString",      (PVOID)ntk_RtlIntegerToUnicodeString },
    { "RtlPrefixUnicodeString",         (PVOID)ntk_RtlPrefixUnicodeString },
    { "RtlInitAnsiString",              (PVOID)ntk_RtlInitAnsiString },
    { "RtlAnsiStringToUnicodeString",   (PVOID)ntk_RtlAnsiStringToUnicodeString },
    { "RtlxAnsiStringToUnicodeSize",    (PVOID)ntk_RtlxAnsiStringToUnicodeSize },

    /* --- I/O Manager: IRP build functions --- */
    { "IoBuildDeviceIoControlRequest",  (PVOID)ntk_IoBuildDeviceIoControlRequest },
    { "IoBuildSynchronousFsdRequest",   (PVOID)ntk_IoBuildSynchronousFsdRequest },
    { "IoBuildAsynchronousFsdRequest",  (PVOID)ntk_IoBuildAsynchronousFsdRequest },

    /* --- I/O Manager: interrupt --- */
    { "IoConnectInterrupt",             (PVOID)ntk_IoConnectInterrupt },
    { "IoDisconnectInterrupt",          (PVOID)ntk_IoDisconnectInterrupt },

    /* --- I/O Manager: PnP and resource reporting --- */
    { "IoInvalidateDeviceRelations",    (PVOID)ntk_IoInvalidateDeviceRelations },
    { "IoReportDetectedDevice",         (PVOID)ntk_IoReportDetectedDevice },
    { "IoReportResourceForDetection",   (PVOID)ntk_IoReportResourceForDetection },
    { "IoInvalidateDeviceState",        (PVOID)ntk_IoInvalidateDeviceState },
    { "IoGetConfigurationInformation",  (PVOID)ntk_IoGetConfigurationInformation },

    /* --- I/O Manager: symbolic links --- */
    { "IoCreateSymbolicLink",           (PVOID)ntk_IoCreateSymbolicLink },
    { "IoDeleteSymbolicLink",           (PVOID)ntk_IoDeleteSymbolicLink },

    /* --- I/O Manager: work items --- */
    { "IoAllocateWorkItem",             (PVOID)ntk_IoAllocateWorkItem },
    { "IoFreeWorkItem",                 (PVOID)ntk_IoFreeWorkItem },
    { "IoQueueWorkItem",                (PVOID)ntk_IoQueueWorkItem },

    /* --- I/O Manager: driver object extension --- */
    { "IoAllocateDriverObjectExtension", (PVOID)ntk_IoAllocateDriverObjectExtension },
    { "IoGetDriverObjectExtension",     (PVOID)ntk_IoGetDriverObjectExtension },

    /* --- I/O Manager: start I/O --- */
    { "IoStartPacket",                  (PVOID)ntk_IoStartPacket },
    { "IoStartNextPacket",              (PVOID)ntk_IoStartNextPacket },
    { "IoStartTimer",                   (PVOID)ntk_IoStartTimer },
    { "IoInitializeTimer",              (PVOID)ntk_IoInitializeTimer },

    /* --- I/O Manager: MDL --- */
    { "IoAllocateMdl",                  (PVOID)ntk_IoAllocateMdl },
    { "IoFreeMdl",                      (PVOID)ntk_IoFreeMdl },

    /* --- I/O Manager: WMI --- */
    { "IoWMIRegistrationControl",       (PVOID)ntk_IoWMIRegistrationControl },

    /* --- Memory Manager --- */
    { "MmMapIoSpace",                   (PVOID)ntk_MmMapIoSpace },
    { "MmUnmapIoSpace",                 (PVOID)ntk_MmUnmapIoSpace },
    { "MmMapLockedPagesSpecifyCache",   (PVOID)ntk_MmMapLockedPagesSpecifyCache },
    { "MmBuildMdlForNonPagedPool",      (PVOID)ntk_MmBuildMdlForNonPagedPool },
    { "MmLockPagableDataSection",       (PVOID)ntk_MmLockPagableDataSection },
    { "MmUnlockPagableImageSection",    (PVOID)ntk_MmUnlockPagableImageSection },
    { "MmUnlockPages",                  (PVOID)ntk_MmUnlockPages },
    { "MmHighestUserAddress",           (PVOID)&ntk_MmHighestUserAddress },

    /* --- Object Manager --- */
    { "ObReferenceObjectByPointer",     (PVOID)ntk_ObReferenceObjectByPointer },
    { "ObReferenceObjectByHandle",      (PVOID)ntk_ObReferenceObjectByHandle },
    { "ObfDereferenceObject",           (PVOID)ntk_ObfDereferenceObject },

    /* --- Interlocked operations --- */
    { "InterlockedDecrement",           (PVOID)ntk_InterlockedDecrement },
    { "InterlockedIncrement",           (PVOID)ntk_InterlockedIncrement },
    { "InterlockedExchange",            (PVOID)ntk_InterlockedExchange },

    /* --- Synchronization extras --- */
    { "KeSynchronizeExecution",         (PVOID)ntk_KeSynchronizeExecution },

    /* --- Device queue --- */
    { "KeInsertByKeyDeviceQueue",       (PVOID)ntk_KeInsertByKeyDeviceQueue },
    { "KeRemoveByKeyDeviceQueue",       (PVOID)ntk_KeRemoveByKeyDeviceQueue },
    { "KeRemoveDeviceQueue",            (PVOID)ntk_KeRemoveDeviceQueue },

    /* --- C runtime helpers --- */
    { "sprintf",                        (PVOID)ntk_sprintf },
    { "swprintf",                       (PVOID)ntk_swprintf },
    { "strstr",                         (PVOID)ntk_strstr },
    { "_strupr",                        (PVOID)ntk_strupr },
    { "_allmul",                         (PVOID)ntk_allmul },
    { "_alldiv",                         (PVOID)ntk_alldiv },
    { "_aulldiv",                        (PVOID)ntk_aulldiv },
    { "_except_handler3",               (PVOID)ntk_except_handler3 },

    /* --- Globals exported as data --- */
    { "KeTickCount",                    (PVOID)&ntk_KeTickCount },
    { "KeQueryTimeIncrement",           (PVOID)&ntk_KeQueryTimeIncrement },
    { "NlsMbCodePageTag",               (PVOID)&ntk_NlsMbCodePageTag },
    { "InitSafeBootMode",               (PVOID)&ntk_InitSafeBootMode },

    /* --- Miscellaneous (NTKSHIM) --- */
    { "DbgPrint",                       (PVOID)DbgPrint },
    { "KeBugCheckEx",                   (PVOID)KeBugCheckEx },
    { "IoGetCurrentProcess",            (PVOID)IoGetCurrentProcess },

    /* NULL terminator */
    { NULL, NULL }
};

/* ================================================================
 * HAL.dll EXPORT TABLE
 *
 * Maps HAL import names to NTKSHIM I/O port, bus access, and
 * IRQL/spinlock functions. On x86 Win9x, HAL functions are thin
 * wrappers around IN/OUT instructions and PCI config space access.
 *
 * Note: KfAcquireSpinLock, KfReleaseSpinLock, KfRaiseIrql, and
 * KfLowerIrql are __fastcall on NT. Our ntk_Kf* implementations
 * are __cdecl; the PE loader or ASM wrapper must handle the
 * calling convention translation.
 * ================================================================ */

static const IMPORT_FUNC_ENTRY hal_exports[] = {

    /* --- I/O port access (NTKSHIM) --- */
    { "READ_PORT_UCHAR",               (PVOID)READ_PORT_UCHAR },
    { "READ_PORT_USHORT",              (PVOID)READ_PORT_USHORT },
    { "READ_PORT_ULONG",               (PVOID)READ_PORT_ULONG },
    { "WRITE_PORT_UCHAR",              (PVOID)WRITE_PORT_UCHAR },
    { "WRITE_PORT_USHORT",             (PVOID)WRITE_PORT_USHORT },
    { "WRITE_PORT_ULONG",              (PVOID)WRITE_PORT_ULONG },

    /* --- Buffer I/O (NTKSHIM) --- */
    { "READ_PORT_BUFFER_USHORT",       (PVOID)ntk_READ_PORT_BUFFER_USHORT },
    { "WRITE_PORT_BUFFER_USHORT",      (PVOID)ntk_WRITE_PORT_BUFFER_USHORT },

    /* --- Bus access (NTKSHIM) --- */
    { "HalTranslateBusAddress",         (PVOID)HalTranslateBusAddress },
    { "HalGetBusData",                  (PVOID)HalGetBusData },
    { "HalGetBusDataByOffset",          (PVOID)HalGetBusData },
    { "HalSetBusData",                  (PVOID)HalSetBusData },
    { "HalSetBusDataByOffset",          (PVOID)HalSetBusData },
    { "HalGetInterruptVector",          (PVOID)ntk_HalGetInterruptVector },

    /* --- IRQL management (NTKSHIM, cdecl wrappers for fastcall) --- */
    { "KeGetCurrentIrql",               (PVOID)KeGetCurrentIrql },
    { "KfAcquireSpinLock",              (PVOID)ntk_KfAcquireSpinLock },
    { "KfReleaseSpinLock",              (PVOID)ntk_KfReleaseSpinLock },
    { "KfRaiseIrql",                    (PVOID)ntk_KfRaiseIrql },
    { "KfLowerIrql",                    (PVOID)ntk_KfLowerIrql },

    /* --- Stall execution (NTKSHIM) --- */
    { "KeStallExecutionProcessor",      (PVOID)ntk_KeStallExecutionProcessor },

    /* NULL terminator */
    { NULL, NULL }
};

/* ================================================================
 * WMILIB.SYS EXPORT TABLE
 *
 * W2K atapi.sys imports WmiCompleteRequest and WmiSystemControl
 * from WMILIB.SYS.
 * ================================================================ */

static const IMPORT_FUNC_ENTRY wmilib_exports[] = {

    { "WmiCompleteRequest",             (PVOID)ntk_WmiCompleteRequest },
    { "WmiSystemControl",               (PVOID)ntk_WmiSystemControl },

    /* NULL terminator */
    { NULL, NULL }
};

/* ================================================================
 * SCSIPORT.SYS EXPORT TABLE
 *
 * The ScsiPort function table is defined in NTMINI_V5.C as a
 * static local (scsiport_funcs[]). For the multi-DLL loader,
 * we declare it as extern here. If NTMINI_V5.C is not linked,
 * the loader should skip SCSIPORT.SYS resolution or provide
 * a stub table.
 *
 * TODO: Make scsiport_funcs[] non-static in NTMINI_V5.C and
 * rename to scsiport_exports[] for consistency, or duplicate
 * the table here once the sp_* functions are factored out.
 * ================================================================ */

/* Forward declaration: defined in NTMINI_V5.C */
extern const IMPORT_FUNC_ENTRY scsiport_funcs[];

/* ================================================================
 * EXPORT COUNT MACROS
 *
 * Subtract 1 for the NULL terminator entry.
 * ================================================================ */

#define NTOSKRNL_EXPORT_COUNT \
    (sizeof(ntoskrnl_exports) / sizeof(ntoskrnl_exports[0]) - 1)

#define HAL_EXPORT_COUNT \
    (sizeof(hal_exports) / sizeof(hal_exports[0]) - 1)

#define WMILIB_EXPORT_COUNT \
    (sizeof(wmilib_exports) / sizeof(wmilib_exports[0]) - 1)

/*
 * ScsiPort count must be computed at runtime or hardcoded because
 * the table is extern (sizeof not available). NTMINI_V5.C has
 * 29 entries + NULL terminator.
 */
#define SCSIPORT_EXPORT_COUNT   29

/* ================================================================
 * DLL EXPORT TABLE
 *
 * Master lookup table for the multi-DLL PE loader. Given a DLL
 * name from a PE import directory entry, the loader searches
 * this table to find the corresponding function table.
 *
 * DLL name matching should be case-insensitive.
 * ================================================================ */

typedef struct _DLL_EXPORT_TABLE {
    const char              *dll_name;
    const IMPORT_FUNC_ENTRY *func_table;
    ULONG                    func_count;
} DLL_EXPORT_TABLE;

static const DLL_EXPORT_TABLE g_dll_tables[] = {
    { "ntoskrnl.exe",   ntoskrnl_exports,   NTOSKRNL_EXPORT_COUNT },
    { "HAL.dll",        hal_exports,        HAL_EXPORT_COUNT },
    { "WMILIB.SYS",    wmilib_exports,     WMILIB_EXPORT_COUNT },
    { "SCSIPORT.SYS",  scsiport_funcs,     SCSIPORT_EXPORT_COUNT },
    { NULL,             NULL,               0 }
};
