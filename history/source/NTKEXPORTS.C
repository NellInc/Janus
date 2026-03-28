/*
 * NTKEXPORTS.C - Export Tables for Multi-DLL PE Loader
 *
 * Defines the import resolution tables that map NT kernel function
 * names (as they appear in PE import tables) to our shim
 * implementations across NTKSHIM, IRPMGR, PNPMGR, and PCIBUS.
 *
 * The PE loader (PELOAD.C) calls resolve_import() against these
 * tables when processing each DLL's import directory. Three tables
 * cover the three DLLs that NT5 drivers typically import from:
 *
 *   ntoskrnl_exports[] - ntoskrnl.exe: Ke*, Ex*, Io*, Zw*, Rtl*, Po*
 *   hal_exports[]      - HAL.dll:      READ/WRITE_PORT_*, Hal*
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
    { "KeInitializeSpinLock",           (void *)KeInitializeSpinLock },
    { "KeAcquireSpinLock",              (void *)KeAcquireSpinLock },
    { "KeReleaseSpinLock",              (void *)KeReleaseSpinLock },
    { "KeAcquireSpinLockAtDpcLevel",    (void *)KeAcquireSpinLockAtDpcLevel },
    { "KeReleaseSpinLockFromDpcLevel",  (void *)KeReleaseSpinLockFromDpcLevel },

    /* --- DPC (NTKSHIM) --- */
    { "KeInitializeDpc",                (void *)KeInitializeDpc },
    { "KeInsertQueueDpc",               (void *)KeInsertQueueDpc },

    /* --- Timers (NTKSHIM) --- */
    { "KeInitializeTimer",              (void *)KeInitializeTimer },
    { "KeInitializeTimerEx",            (void *)KeInitializeTimer },
    { "KeSetTimer",                     (void *)KeSetTimer },
    { "KeSetTimerEx",                   (void *)KeSetTimer },
    { "KeCancelTimer",                  (void *)KeCancelTimer },
    { "KeQuerySystemTime",              (void *)KeQuerySystemTime },

    /* --- Events (NTKSHIM) --- */
    { "KeInitializeEvent",              (void *)KeInitializeEvent },
    { "KeSetEvent",                     (void *)KeSetEvent },
    { "KeResetEvent",                   (void *)KeResetEvent },
    { "KeWaitForSingleObject",          (void *)KeWaitForSingleObject },

    /* --- IRQL (NTKSHIM) --- */
    { "KeGetCurrentIrql",               (void *)KeGetCurrentIrql },

    /* --- Pool allocation (NTKSHIM) --- */
    { "ExAllocatePool",                 (void *)ExAllocatePool },
    { "ExAllocatePoolWithTag",          (void *)ExAllocatePoolWithTag },
    { "ExFreePool",                     (void *)ExFreePoolWithTag },
    { "ExFreePoolWithTag",              (void *)ExFreePoolWithTag },

    /* --- IRP allocation and dispatch (IRPMGR) --- */
    { "IoAllocateIrp",                  (void *)IrpMgr_IoAllocateIrp },
    { "IoFreeIrp",                      (void *)IrpMgr_IoFreeIrp },
    { "IoCallDriver",                   (void *)IrpMgr_IoCallDriver },
    { "IofCallDriver",                  (void *)IrpMgr_IoCallDriver },
    { "IoCompleteRequest",              (void *)IrpMgr_IoCompleteRequest },
    { "IofCompleteRequest",             (void *)IrpMgr_IoCompleteRequest },

    /* --- Device object management (IRPMGR) --- */
    { "IoCreateDevice",                 (void *)IrpMgr_IoCreateDevice },
    { "IoDeleteDevice",                 (void *)IrpMgr_IoDeleteDevice },
    { "IoAttachDeviceToDeviceStack",    (void *)IrpMgr_IoAttachDeviceToDeviceStack },
    { "IoDetachDevice",                 (void *)IrpMgr_IoDetachDevice },
    { "IoGetAttachedDeviceReference",   (void *)IrpMgr_IoGetAttachedDeviceReference },

    /* --- Error logging stubs (IRPMGR) --- */
    { "IoAllocateErrorLogEntry",        (void *)IrpMgr_IoAllocateErrorLogEntry },
    { "IoWriteErrorLogEntry",           (void *)IrpMgr_IoWriteErrorLogEntry },

    /* --- Power manager (PNPMGR) --- */
    { "PoStartNextPowerIrp",            (void *)PnpMgr_PoStartNextPowerIrp },
    { "PoCallDriver",                   (void *)PnpMgr_PoCallDriver },
    { "PoRequestPowerIrp",              (void *)PnpMgr_PoRequestPowerIrp },
    { "PoSetPowerState",                (void *)PnpMgr_PoSetPowerState },

    /* --- Registry stubs (NTKSHIM) --- */
    { "ZwOpenKey",                      (void *)ZwOpenKey },
    { "ZwQueryValueKey",                (void *)ZwQueryValueKey },
    { "ZwSetValueKey",                  (void *)ZwSetValueKey },
    { "ZwClose",                        (void *)ZwClose },

    /* --- RTL memory utilities (NTKSHIM) --- */
    { "RtlZeroMemory",                  (void *)RtlZeroMemory },
    { "RtlCopyMemory",                  (void *)RtlCopyMemory },
    { "RtlMoveMemory",                  (void *)RtlMoveMemory },
    { "RtlCompareMemory",              (void *)RtlCompareMemory },

    /* --- Miscellaneous (NTKSHIM) --- */
    { "DbgPrint",                       (void *)DbgPrint },
    { "KeBugCheckEx",                   (void *)KeBugCheckEx },
    { "IoGetCurrentProcess",            (void *)IoGetCurrentProcess },

    /* NULL terminator */
    { NULL, NULL }
};

/* ================================================================
 * HAL.dll EXPORT TABLE
 *
 * Maps HAL import names to NTKSHIM I/O port and bus access
 * functions. On x86 Win9x, HAL functions are thin wrappers
 * around IN/OUT instructions and PCI config space access.
 * ================================================================ */

static const IMPORT_FUNC_ENTRY hal_exports[] = {

    /* --- I/O port access (NTKSHIM) --- */
    { "READ_PORT_UCHAR",               (void *)READ_PORT_UCHAR },
    { "READ_PORT_USHORT",              (void *)READ_PORT_USHORT },
    { "READ_PORT_ULONG",               (void *)READ_PORT_ULONG },
    { "WRITE_PORT_UCHAR",              (void *)WRITE_PORT_UCHAR },
    { "WRITE_PORT_USHORT",             (void *)WRITE_PORT_USHORT },
    { "WRITE_PORT_ULONG",              (void *)WRITE_PORT_ULONG },

    /* --- Bus access (NTKSHIM) --- */
    { "HalTranslateBusAddress",         (void *)HalTranslateBusAddress },
    { "HalGetBusData",                  (void *)HalGetBusData },
    { "HalGetBusDataByOffset",          (void *)HalGetBusData },
    { "HalSetBusData",                  (void *)HalSetBusData },
    { "HalSetBusDataByOffset",          (void *)HalSetBusData },

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
    { "SCSIPORT.SYS",  scsiport_funcs,     SCSIPORT_EXPORT_COUNT },
    { NULL,             NULL,               0 }
};
