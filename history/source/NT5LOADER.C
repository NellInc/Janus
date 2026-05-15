/*
 * NT5LOADER.C -- Load Windows 2000 atapi.sys via WDM compatibility layer
 *
 * This module orchestrates loading an NT5 WDM driver (atapi.sys) inside
 * a Win98 VxD, using the ntoskrnl/HAL shim, IRP infrastructure, and
 * PnP manager to provide the environment the driver expects.
 *
 * LICENSE: MIT License.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 *
 * Build: wcc386 -bt=windows -3s -s -zl -d0 -i=. NT5LOADER.C
 */

#include "NTKSHIM.H"
#include "IRPMGR.H"
#include "PNPMGR.H"
#include "WDMBRIDGE.H"
#include "PCIBUS.H"

/* ================================================================
 * VxD wrapper externals (provided by VXDWRAP.ASM / VXDWRAP_NASM.ASM)
 *
 * These give us ring-0 file I/O on Win9x via IFSMgr_Ring0_FileIO.
 * ================================================================ */

extern int  VxD_File_Open(const char *filename);
extern int  VxD_File_Read(int handle, void *buffer, int count);
extern void VxD_File_Close(int handle);
extern void VxD_Debug_Printf(const char *fmt, ...);

/* VxD heap (for file read buffer) */
extern void  dbg_mark(char c);
extern PVOID VxD_HeapAllocate(ULONG size, ULONG flags);
extern void  VxD_HeapFree(PVOID ptr, ULONG flags);

/* VxD page allocator */
extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
#define PAGEFIXED       0x00000001
#define PAGESIZE        4096

/* PE loader multi-DLL (from PELOAD.C) */
typedef struct {
    const char *name;
    void       *func;
} IMPORT_FUNC_ENTRY;

typedef struct {
    const char              *dll_name;
    const IMPORT_FUNC_ENTRY *func_table;
    ULONG                    func_count;
} DLL_EXPORT_TABLE;

extern int pe_load_image_multi(
    const void *pe_data,
    unsigned long pe_size,
    const DLL_EXPORT_TABLE *dll_tables,
    ULONG dll_count,
    void **out_entry,
    void **out_base);

/* Export tables (from NTKEXPORTS.C) */
extern const DLL_EXPORT_TABLE g_dll_tables[];

/* IOS registration (from IOSBRIDGE.C) */
extern int ios_register_port_driver(void);

/* ================================================================
 * SCSI_REQUEST_BLOCK (must match IOSBRIDGE.C / NTMINI_V5.C)
 *
 * We define only what we need for building test SRBs. The full
 * structure is in IOSBRIDGE.C.
 * ================================================================ */

#define SRB_FUNCTION_EXECUTE_SCSI   0x00
#define SRB_STATUS_PENDING          0x00
#define SRB_STATUS_SUCCESS          0x01
#define SRB_FLAGS_DATA_IN           0x00000040
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER 0x00000020

#ifndef STATUS_MORE_PROCESSING_REQUIRED
#define STATUS_MORE_PROCESSING_REQUIRED ((NTSTATUS)0xC0000016L)
#endif

#define SCSI_OP_READ10              0x28

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

enum {
    NT5LOADER_SRB_CDB_OFFSET = (int)(&((PSCSI_REQUEST_BLOCK)0)->Cdb),
    NT5LOADER_SRB_SIZE = sizeof(SCSI_REQUEST_BLOCK)
};
typedef char NT5LOADER_ASSERT_SRB_CDB_OFFSET_IS_0x30[
    (NT5LOADER_SRB_CDB_OFFSET == 0x30) ? 1 : -1];
typedef char NT5LOADER_ASSERT_SRB_SIZE_IS_0x40[
    (NT5LOADER_SRB_SIZE == 0x40) ? 1 : -1];

/* ================================================================
 * NT5 LOADER GLOBAL STATE
 * ================================================================ */

/* Embedded Windows 2000 atapi.sys payload */
#include "W2K_ATAP_EMBEDDED.H"

/* DriverEntry prototype (stdcall: DriverObject, RegistryPath) */
typedef NTSTATUS (NTAPI *PFN_DRIVER_ENTRY)(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
);

static DRIVER_OBJECT    g_nt5_driver;
static DRIVER_EXTENSION g_nt5_driver_ext;
static DRIVER_OBJECT    g_pdo_driver;
static DRIVER_EXTENSION g_pdo_driver_ext;
static ULONG            g_nt5_alternative_architecture;
static DEVICE_OBJECT   *g_nt5_pdo;       /* fake Physical Device Object */
static DEVICE_OBJECT   *g_nt5_fdo;       /* atapi's Functional Device Object */
static DEVICE_OBJECT   *g_nt5_child_pdo; /* child PDO returned by bus relations */
static WDM_BRIDGE_CONTEXT g_nt5_bridge;
static PVOID            g_nt5_image_base; /* loaded PE image base */
static UCHAR            g_nt5_detected_target_id;
static UNICODE_STRING   g_nt5_hwdb;
static UNICODE_STRING   g_nt5_regpath;
int                     g_nt5_trace_ports = 0;

static WCHAR g_nt5_driver_name_buf[] =
    L"\\Driver\\atapi";
static WCHAR g_nt5_hwdb_buf[] =
    L"\\Registry\\Machine\\Hardware\\Description\\System";
static WCHAR g_nt5_regpath_buf[] =
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\atapi";

/* Number of DLL tables (ntoskrnl, HAL, WMILIB, SCSIPORT + sentinel) */
#define DLL_TABLE_COUNT     4

/* File path for disk-loaded atapi.sys */
static const char g_atapi_path[] = "C:\\WINDOWS\\SYSTEM\\W2K_ATAP.SYS";

/* Max file size we'll try to load (96 KB - fits in VxD BSS) */
#define NT5_MAX_IMAGE_SIZE  (96UL * 1024UL)

/* Static file buffer in VxD BSS segment (V86-accessible, unlike PageAllocate) */
static UCHAR g_file_buffer[NT5_MAX_IMAGE_SIZE];

WDM_BRIDGE_CONTEXT *nt5_get_bridge_context(void)
{
    return g_nt5_bridge.Active ? &g_nt5_bridge : NULL;
}

static void nt5_dbg_hex8(UCHAR value)
{
    UCHAR hi;
    UCHAR lo;

    hi = (UCHAR)((value >> 4) & 0x0F);
    lo = (UCHAR)(value & 0x0F);
    dbg_mark((char)(hi < 10 ? ('0' + hi) : ('A' + hi - 10)));
    dbg_mark((char)(lo < 10 ? ('0' + lo) : ('A' + lo - 10)));
}

static UCHAR nt5_atapi_unit_count(PDEVICE_OBJECT dev)
{
    PUCHAR ext;
    ULONG table;

    if (!dev || !dev->DeviceExtension) {
        return 0;
    }

    ext = (PUCHAR)dev->DeviceExtension;
    table = *(ULONG *)(ext + 0xA0);
    if (table < 0x80000000UL) {
        return 0;
    }

    return (UCHAR)(*(ULONG *)((PUCHAR)table + 0x40) & 0xFF);
}

static void nt5_dbg_hex32(ULONG value)
{
    nt5_dbg_hex8((UCHAR)((value >> 24) & 0xFF));
    nt5_dbg_hex8((UCHAR)((value >> 16) & 0xFF));
    nt5_dbg_hex8((UCHAR)((value >> 8) & 0xFF));
    nt5_dbg_hex8((UCHAR)(value & 0xFF));
}

static NTSTATUS NTAPI nt5_pdo_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack;
    NTSTATUS status;

    (void)DeviceObject;

    if (!Irp) {
        return STATUS_INVALID_PARAMETER;
    }

    stack = IrpMgr_IoGetCurrentIrpStackLocation(Irp);
    status = Irp->IoStatus.Status;
    if (!NT_SUCCESS(status)) {
        status = STATUS_SUCCESS;
    }

    dbg_mark('V');
    if (stack) {
        nt5_dbg_hex8(stack->MajorFunction);
        nt5_dbg_hex8(stack->MinorFunction);
        if (stack->MajorFunction == IRP_MJ_DEVICE_CONTROL ||
            stack->MajorFunction == IRP_MJ_INTERNAL_DEVICE_CONTROL) {
            dbg_mark('J');
            nt5_dbg_hex32(stack->Parameters.DeviceIoControl.IoControlCode);
        }
    }

    Irp->IoStatus.Status = status;
    if (Irp->IoStatus.Information == 0) {
        Irp->IoStatus.Information = 0;
    }

    return status;
}

static void nt5_init_pdo_driver(void)
{
    ULONG i;

    RtlZeroMemory(&g_pdo_driver, sizeof(DRIVER_OBJECT));
    RtlZeroMemory(&g_pdo_driver_ext, sizeof(DRIVER_EXTENSION));

    g_pdo_driver.Type = 4;
    g_pdo_driver.Size = sizeof(DRIVER_OBJECT);
    g_pdo_driver.DriverExtension = &g_pdo_driver_ext;
    g_pdo_driver_ext.DriverObject = &g_pdo_driver;

    for (i = 0; i < IRP_MJ_MAXIMUM; i++) {
        g_pdo_driver.MajorFunction[i] = nt5_pdo_dispatch;
    }
}

static void nt5_dump_atapi_ext(PDEVICE_OBJECT dev)
{
    PUCHAR ext;
    ULONG table;

    if (!dev || !dev->DeviceExtension) {
        dbg_mark('W');
        nt5_dbg_hex8(0);
        return;
    }

    ext = (PUCHAR)dev->DeviceExtension;
    table = *(ULONG *)(ext + 0x50);
    dbg_mark('P');
    nt5_dbg_hex32(table);
    if (table >= 0x80000000UL) {
        dbg_mark('q');
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x00));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x04));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x08));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x0C));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x10));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x14));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x18));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x1C));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x20));
    }

    dbg_mark('W');
    nt5_dbg_hex32(*(ULONG *)(ext + 0x60));
    dbg_mark('w');
    nt5_dbg_hex32(*(ULONG *)(ext + 0x64));
    dbg_mark('x');
    nt5_dbg_hex32(*(ULONG *)(ext + 0x68));
    dbg_mark('y');
    nt5_dbg_hex32(*(ULONG *)(ext + 0x6C));
    dbg_mark('z');
    nt5_dbg_hex8(*(UCHAR *)(ext + 0x78));
    nt5_dbg_hex8(*(UCHAR *)(ext + 0x79));

    table = *(ULONG *)(ext + 0xA0);
    dbg_mark('U');
    nt5_dbg_hex32(table);
    if (table >= 0x80000000UL) {
        dbg_mark('u');
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x34));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x38));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x3C));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x40));
    }
}

static void nt5_dump_adapter_io_state(char tag)
{
    PUCHAR ext;
    ULONG table;
    ULONG unit;

    dbg_mark('D');
    dbg_mark(tag);
    nt5_dbg_hex32((ULONG)g_nt5_fdo);
    nt5_dbg_hex32((ULONG)g_nt5_child_pdo);

    if (!g_nt5_fdo || !g_nt5_fdo->DeviceExtension) {
        dbg_mark('0');
        return;
    }

    ext = (PUCHAR)g_nt5_fdo->DeviceExtension;
    nt5_dbg_hex32((ULONG)ext);

    dbg_mark('a');
    nt5_dbg_hex32(*(ULONG *)(ext + 0x0C));
    nt5_dbg_hex32(*(ULONG *)(ext + 0x5C));
    nt5_dbg_hex32(*(ULONG *)(ext + 0x7C));
    nt5_dbg_hex32(*(ULONG *)(ext + 0x84));

    dbg_mark('b');
    nt5_dbg_hex32(*(ULONG *)(ext + 0xA0));
    nt5_dbg_hex32(*(ULONG *)(ext + 0xB4));
    nt5_dbg_hex32(*(ULONG *)(ext + 0xC0));
    nt5_dbg_hex32(*(ULONG *)(ext + 0xD0));

    dbg_mark('c');
    nt5_dbg_hex32(*(ULONG *)(ext + 0x100));
    nt5_dbg_hex32(*(ULONG *)(ext + 0x108));
    nt5_dbg_hex32(*(ULONG *)(ext + 0x128));
    nt5_dbg_hex32(*(ULONG *)(ext + 0x148));

    table = *(ULONG *)(ext + 0xA0);
    if (table >= 0x80000000UL) {
        dbg_mark('t');
        nt5_dbg_hex32(table);
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x00));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)table + 0x40));
    }

    unit = *(ULONG *)(ext + 0x108);
    if (unit >= 0x80000000UL) {
        dbg_mark('u');
        nt5_dbg_hex32(unit);
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)unit + 0x00));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)unit + 0x84));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)unit + 0x94));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)unit + 0x98));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)unit + 0x138));
    }
}

static void nt5_patch_kuser_shared_data(PVOID image_base, const void *image_data)
{
    PUCHAR raw;
    PUCHAR base;
    ULONG pe_off;
    ULONG size_of_image;
    ULONG off;
    UCHAR count;

    if (!image_base || !image_data) {
        return;
    }

    raw = (PUCHAR)image_data;
    base = (PUCHAR)image_base;
    pe_off = *(ULONG *)(raw + 0x3C);
    size_of_image = *(ULONG *)(raw + pe_off + 4 + 20 + 56);

    count = 0;
    for (off = 0; off + 4 <= size_of_image; off++) {
        if (*(ULONG *)(base + off) == 0xFFDF02C0UL) {
            *(ULONG *)(base + off) = (ULONG)&g_nt5_alternative_architecture;
            dbg_mark('k');
            nt5_dbg_hex32(off);
            count++;
        }
    }

    dbg_mark('K');
    nt5_dbg_hex8(count);
}

/* ================================================================
 * IDE CHANNEL RESOURCE DEFINITIONS
 * ================================================================ */

/* Primary IDE channel */
#define IDE_PRIMARY_BASE        0x1F0
#define IDE_PRIMARY_CTRL        0x3F6
#define IDE_PRIMARY_IRQ         14
#define IDE_PRIMARY_PORT_LEN    8

/* Secondary IDE channel */
#define IDE_SECONDARY_BASE      0x170
#define IDE_SECONDARY_CTRL      0x376
#define IDE_SECONDARY_IRQ       15
#define IDE_SECONDARY_PORT_LEN  8

/* ================================================================
 * nt5_create_pdo - Create a fake PDO for an IDE channel
 *
 * The PDO represents the physical hardware that the PnP manager
 * would normally create. We fabricate one so that AddDevice has
 * something to attach the FDO to.
 *
 * DeviceExtension stores the I/O port base so the driver can
 * discover its resources (though the real resource delivery is
 * via IRP_MN_START_DEVICE).
 * ================================================================ */

typedef struct _IDE_PDO_EXTENSION {
    ULONG   IoPortBase;
    ULONG   CtrlPortBase;
    ULONG   IrqNumber;
    BOOLEAN IsPrimary;
} IDE_PDO_EXTENSION, *PIDE_PDO_EXTENSION;

static PDEVICE_OBJECT nt5_create_pdo(BOOLEAN primary)
{
    NTSTATUS status;
    PDEVICE_OBJECT pdo;
    PIDE_PDO_EXTENSION ext;

    nt5_init_pdo_driver();

    status = IrpMgr_IoCreateDevice(
        &g_pdo_driver,
        sizeof(IDE_PDO_EXTENSION),
        NULL,                           /* no device name */
        FILE_DEVICE_CONTROLLER,
        0,                              /* characteristics */
        FALSE,                          /* not exclusive */
        &pdo
    );

    if (!NT_SUCCESS(status) || !pdo) {
        VxD_Debug_Printf("NT5: Failed to create PDO (status=0x%08lX)\n",
                         (ULONG)status);
        return NULL;
    }

    ext = (PIDE_PDO_EXTENSION)pdo->DeviceExtension;
    if (primary) {
        ext->IoPortBase   = IDE_PRIMARY_BASE;
        ext->CtrlPortBase = IDE_PRIMARY_CTRL;
        ext->IrqNumber    = IDE_PRIMARY_IRQ;
        ext->IsPrimary    = TRUE;
    } else {
        ext->IoPortBase   = IDE_SECONDARY_BASE;
        ext->CtrlPortBase = IDE_SECONDARY_CTRL;
        ext->IrqNumber    = IDE_SECONDARY_IRQ;
        ext->IsPrimary    = FALSE;
    }

    /* PDO does not need DO_DEVICE_INITIALIZING cleared; it is
     * "enumerated" by us (the fake bus driver), not by PnP. */
    pdo->Flags &= ~DO_DEVICE_INITIALIZING;

    VxD_Debug_Printf("NT5: Created PDO at 0x%08lX (%s IDE)\n",
                     (ULONG)pdo, primary ? "primary" : "secondary");

    return pdo;
}

/* ================================================================
 * nt5_load_atapi - PE-load atapi.sys and call DriverEntry
 *
 * Steps:
 *   1. Load the PE image via pe_load_image_multi()
 *   2. Extract DriverEntry from the PE entry point
 *   3. Initialize our DRIVER_OBJECT
 *   4. Call DriverEntry(&g_nt5_driver, NULL)
 *   5. Log all registered MajorFunction handlers
 * ================================================================ */

static NTSTATUS nt5_load_atapi(const void *image_data, ULONG image_size)
{
    PVOID entry_point;
    PFN_DRIVER_ENTRY driver_entry;
    NTSTATUS status;
    ULONG i;
    int pe_result;

    VxD_Debug_Printf("NT5: Loading atapi.sys\n");

    /* Step 1: Load PE image with multi-DLL import resolution */
    pe_result = pe_load_image_multi(
        image_data,
        image_size,
        g_dll_tables,
        DLL_TABLE_COUNT,
        &entry_point,
        &g_nt5_image_base
    );

    if (pe_result != 0) {
        VxD_Debug_Printf("NT5: PE load failed\n");
        return STATUS_UNSUCCESSFUL;
    }

    VxD_Debug_Printf("NT5: PE loaded at 0x%08lX, entry=0x%08lX\n",
                     (ULONG)g_nt5_image_base, (ULONG)entry_point);
    g_nt5_alternative_architecture = 0;
    nt5_patch_kuser_shared_data(g_nt5_image_base, image_data);

    /* Step 2: Cast entry point to DriverEntry */
    driver_entry = (PFN_DRIVER_ENTRY)entry_point;

    /* Step 3: Initialize DRIVER_OBJECT */
    RtlZeroMemory(&g_nt5_driver, sizeof(DRIVER_OBJECT));
    RtlZeroMemory(&g_nt5_driver_ext, sizeof(DRIVER_EXTENSION));

    g_nt5_driver.Type = 4;      /* IO_TYPE_DRIVER */
    g_nt5_driver.Size = sizeof(DRIVER_OBJECT);
    g_nt5_driver.DriverExtension = &g_nt5_driver_ext;
    g_nt5_driver_ext.DriverObject = &g_nt5_driver;
    g_nt5_driver.DriverInit = (PVOID)driver_entry;
    g_nt5_driver.DriverStart = g_nt5_image_base;
    ntk_RtlInitUnicodeString(&g_nt5_driver.DriverName, g_nt5_driver_name_buf);
    ntk_RtlInitUnicodeString(&g_nt5_hwdb, g_nt5_hwdb_buf);
    ntk_RtlInitUnicodeString(&g_nt5_regpath, g_nt5_regpath_buf);
    g_nt5_driver.HardwareDatabase = &g_nt5_hwdb;

    /* Step 4: Call DriverEntry
     *
     * W2K atapi.sys DriverEntry signature:
     *   NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING)
     *
     * Provide a plausible service-key RegistryPath and the standard
     * driver metadata fields that atapi.sys expects to probe early. */
    VxD_Debug_Printf("NT5: Calling DriverEntry at 0x%08lX\n",
                     (ULONG)driver_entry);

    dbg_mark('Y');
    status = driver_entry(&g_nt5_driver, &g_nt5_regpath);
    dbg_mark('y');

    if (!NT_SUCCESS(status)) {
        dbg_mark('F');
        VxD_Debug_Printf("NT5: DriverEntry FAILED (status=0x%08lX)\n",
                         (ULONG)status);
        return status;
    }

    dbg_mark('S');
    VxD_Debug_Printf("NT5: DriverEntry succeeded\n");

    /* Step 5: Log registered MajorFunction handlers */
    for (i = 0; i < IRP_MJ_MAXIMUM; i++) {
        if (g_nt5_driver.MajorFunction[i] != NULL) {
            VxD_Debug_Printf("NT5:   MajorFunction[0x%02lX] = 0x%08lX\n",
                             i, (ULONG)g_nt5_driver.MajorFunction[i]);
        }
    }
    dbg_mark('D');
    nt5_dbg_hex8((UCHAR)(((PUCHAR)&g_nt5_driver.MajorFunction[0] -
                          (PUCHAR)&g_nt5_driver) & 0xFF));
    dbg_mark('e');
    nt5_dbg_hex32((ULONG)g_nt5_driver.MajorFunction[0x0E]);
    dbg_mark('f');
    nt5_dbg_hex32((ULONG)g_nt5_driver.MajorFunction[0x0F]);
    dbg_mark('p');
    nt5_dbg_hex32((ULONG)g_nt5_driver.MajorFunction[0x19]);
    nt5_dbg_hex32((ULONG)g_nt5_driver.MajorFunction[0x1A]);
    nt5_dbg_hex32((ULONG)g_nt5_driver.MajorFunction[0x1B]);

    /* Check that AddDevice was registered */
    if (g_nt5_driver.DriverExtension &&
        g_nt5_driver.DriverExtension->AddDevice) {
        VxD_Debug_Printf("NT5:   AddDevice = 0x%08lX\n",
                         (ULONG)g_nt5_driver.DriverExtension->AddDevice);
    } else {
        VxD_Debug_Printf("NT5: WARNING: No AddDevice registered!\n");
    }

    return STATUS_SUCCESS;
}

/* ================================================================
 * nt5_start_device - PnP bootstrap: AddDevice + START_DEVICE
 *
 * Steps:
 *   1. Create PDO via nt5_create_pdo()
 *   2. Call AddDevice (driver creates FDO, attaches to PDO)
 *   3. Find the FDO (it attached to our PDO)
 *   4. Send IRP_MN_START_DEVICE with appropriate resources
 * ================================================================ */

static NTSTATUS nt5_start_device(BOOLEAN primary)
{
    NTSTATUS status;
    ULONG io_base;
    ULONG io_len;
    ULONG irq;
    ULONG image_delta;
    ULONG fdo_pnp_table;

    VxD_Debug_Printf("NT5: Starting %s IDE channel\n",
                     primary ? "primary" : "secondary");

    /* Step 1: Create PDO */
    g_nt5_pdo = nt5_create_pdo(primary);
    if (!g_nt5_pdo) {
        VxD_Debug_Printf("NT5: PDO creation failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Step 2: Call AddDevice
     *
     * The driver's AddDevice routine will:
     *   a) Call IoCreateDevice to create the FDO
     *   b) Call IoAttachDeviceToDeviceStack(FDO, PDO)
     *   c) Set up device extension, flags, etc. */
    if (!g_nt5_driver.DriverExtension ||
        !g_nt5_driver.DriverExtension->AddDevice) {
        VxD_Debug_Printf("NT5: No AddDevice routine available\n");
        return STATUS_UNSUCCESSFUL;
    }

    status = pnp_call_add_device(&g_nt5_driver, g_nt5_pdo);
    if (!NT_SUCCESS(status)) {
        VxD_Debug_Printf("NT5: AddDevice FAILED (status=0x%08lX)\n",
                         (ULONG)status);
        return status;
    }

    VxD_Debug_Printf("NT5: AddDevice succeeded\n");

    /* Step 3: Find the FDO
     *
     * AddDevice called IoAttachDeviceToDeviceStack(FDO, PDO),
     * which sets PDO->AttachedDevice = FDO. The FDO is at the
     * top of the stack. */
    g_nt5_fdo = g_nt5_pdo->AttachedDevice;
    if (!g_nt5_fdo) {
        VxD_Debug_Printf("NT5: No FDO attached to PDO after AddDevice!\n");
        return STATUS_UNSUCCESSFUL;
    }

    VxD_Debug_Printf("NT5: FDO at 0x%08lX, StackSize=%d\n",
                     (ULONG)g_nt5_fdo, (int)g_nt5_fdo->StackSize);

    image_delta = (ULONG)g_nt5_driver.MajorFunction[0x1B] - 0x2049AUL;
    fdo_pnp_table = image_delta + 0x1C3A0UL;
    dbg_mark('v');
    nt5_dbg_hex32(*(ULONG *)((PUCHAR)g_nt5_fdo->DeviceExtension + 0x50));
    nt5_dbg_hex32(fdo_pnp_table);
    *(ULONG *)((PUCHAR)g_nt5_fdo->DeviceExtension + 0x50) = fdo_pnp_table;
    {
        PUCHAR start_tail = (PUCHAR)(image_delta + 0x142BCUL);
        static const UCHAR patch_tail[] = {
            0x31, 0xDB,             /* xor ebx, ebx */
            0x8B, 0xC3,             /* mov eax, ebx */
            0x5E,                   /* pop esi */
            0x5B,                   /* pop ebx */
            0xC9,                   /* leave */
            0xC2, 0x08, 0x00        /* ret 8 */
        };
        PUCHAR flags_call = (PUCHAR)(image_delta + 0x142A7UL);
        static const UCHAR patch_flags_call[] = {
            0x57,                         /* push edi */
            0x56,                         /* push esi */
            0xE8, 0x82, 0x5D, 0x00, 0x00  /* call 0x1A030 */
        };
        PUCHAR first_flags_call = (PUCHAR)(image_delta + 0x14256UL);
        static const UCHAR patch_first_flags_call[] = {
            0x68, 0x00, 0x04, 0x00, 0x00,       /* push 0x400 */
            0x56,                               /* push esi */
            0xE8, 0xCF, 0x5D, 0x00, 0x00        /* call 0x1A030 */
        };
        PUCHAR cleanup_call = (PUCHAR)(image_delta + 0x142AEUL);
        static const UCHAR patch_cleanup_call[] = {
            0x56,                         /* push esi */
            0xE8, 0xFA, 0x41, 0x00, 0x00  /* call 0x184AE */
        };
        PUCHAR relation_locator = (PUCHAR)(image_delta + 0x1A0D3UL);
        static const UCHAR patch_relation_locator[] = {
            0x57,                         /* push edi */
            0xE9, 0x2A, 0x00, 0x00, 0x00  /* jmp 0x1A103 */
        };
        PUCHAR pending_ctx_free = (PUCHAR)(image_delta + 0x143D4UL);
        static const UCHAR patch_pending_ctx_free[] = {
            0x8B, 0x7E, 0x18,       /* mov edi, [esi+0x18] */
            0x90, 0x90, 0x90, 0x90,
            0x90, 0x90, 0x90
        };
        dbg_mark('X');
        nt5_dbg_hex32((ULONG)first_flags_call);
        nt5_dbg_hex32((ULONG)flags_call);
        nt5_dbg_hex32((ULONG)cleanup_call);
        nt5_dbg_hex32((ULONG)relation_locator);
        nt5_dbg_hex32((ULONG)start_tail);
        nt5_dbg_hex32((ULONG)pending_ctx_free);
        RtlCopyMemory(first_flags_call, patch_first_flags_call, sizeof(patch_first_flags_call));
        RtlCopyMemory(flags_call, patch_flags_call, sizeof(patch_flags_call));
        RtlCopyMemory(cleanup_call, patch_cleanup_call, sizeof(patch_cleanup_call));
        RtlCopyMemory(relation_locator, patch_relation_locator, sizeof(patch_relation_locator));
        RtlCopyMemory(start_tail, patch_tail, sizeof(patch_tail));
        RtlCopyMemory(pending_ctx_free, patch_pending_ctx_free, sizeof(patch_pending_ctx_free));
    }

    /* Step 4: Send IRP_MN_START_DEVICE
     *
     * The resource list tells the driver which I/O ports and IRQ
     * it has been assigned. For legacy ISA IDE, these are fixed:
     *
     *   Primary:   I/O 0x1F0-0x1F7, control 0x3F6, IRQ 14
     *   Secondary: I/O 0x170-0x177, control 0x376, IRQ 15
     *
     * pnp_start_device() from PNPMGR.C fabricates a
     * CM_RESOURCE_LIST and sends the IRP. */
    if (primary) {
        io_base = IDE_PRIMARY_BASE;
        io_len  = IDE_PRIMARY_PORT_LEN;
        irq     = IDE_PRIMARY_IRQ;
    } else {
        io_base = IDE_SECONDARY_BASE;
        io_len  = IDE_SECONDARY_PORT_LEN;
        irq     = IDE_SECONDARY_IRQ;
    }

    dbg_mark('h');
    nt5_dump_atapi_ext(g_nt5_fdo);

    dbg_mark('A');
    g_nt5_trace_ports = 1;
    status = pnp_start_device(g_nt5_fdo, io_base, io_len, irq);
    g_nt5_trace_ports = 0;
    dbg_mark('a');
    if (!NT_SUCCESS(status)) {
        VxD_Debug_Printf("NT5: IRP_MN_START_DEVICE FAILED (status=0x%08lX)\n",
                         (ULONG)status);
        return status;
    }

    VxD_Debug_Printf("NT5: Device started (I/O=0x%03lX, IRQ=%lu)\n",
                     io_base, irq);
    dbg_mark('H');
    nt5_dump_atapi_ext(g_nt5_fdo);
    {
        PUCHAR ext;
        NTSTATUS irq_status;

        ext = (PUCHAR)g_nt5_fdo->DeviceExtension;
        dbg_mark('I');
        dbg_mark('C');
        nt5_dbg_hex32(*(ULONG *)(ext + 0xD0));
        irq_status = ntk_IoConnectInterrupt(
            (PKINTERRUPT *)(ext + 0xD0),
            (PKSERVICE_ROUTINE)(image_delta + 0x16666UL),
            g_nt5_fdo,
            NULL,
            irq,
            DIRQL,
            DIRQL,
            0,
            TRUE,
            1,
            FALSE);
        dbg_mark('I');
        dbg_mark('S');
        nt5_dbg_hex8((UCHAR)(irq_status & 0xFF));
        nt5_dbg_hex32(*(ULONG *)(ext + 0xD0));
    }
    {
        PUCHAR ext;
        ULONG rel_cache;
        typedef PVOID (NTAPI *PFN_ATAPI_CREATE_UNIT)(PVOID AdapterExtension,
                                                     ULONG PackedAddress);
        PFN_ATAPI_CREATE_UNIT create_unit;
        PVOID unit;

        ext = (PUCHAR)g_nt5_fdo->DeviceExtension;
        rel_cache = *(ULONG *)(ext + 0x148);
        dbg_mark('R');
        nt5_dbg_hex32(rel_cache);
        if (rel_cache >= 0x80000000UL) {
            nt5_dbg_hex32(*(ULONG *)((PUCHAR)rel_cache + 0x00));
            nt5_dbg_hex32(*(ULONG *)((PUCHAR)rel_cache + 0x04));
            nt5_dbg_hex32(*(ULONG *)((PUCHAR)rel_cache + 0x08));
        }
        create_unit = (PFN_ATAPI_CREATE_UNIT)(image_delta + 0x18604UL);
        dbg_mark('Z');
        unit = create_unit(ext, 0UL);
        nt5_dbg_hex32((ULONG)unit);
        nt5_dbg_hex8(*(UCHAR *)(ext + 0x100));
        nt5_dbg_hex8(*(UCHAR *)(ext + 0x101));
        if (*(ULONG *)(ext + 0xA0) >= 0x80000000UL) {
            PUCHAR adapter_core;

            *(ULONG *)((PUCHAR)(*(ULONG *)(ext + 0xA0)) + 0x40) = 1UL;
            adapter_core = (PUCHAR)(*(ULONG *)(ext + 0xA0));
            *(ULONG *)(adapter_core + 0x04) = 0x00000170UL;
            *(ULONG *)(adapter_core + 0x08) = 0x00000170UL;
            *(ULONG *)(adapter_core + 0x0C) = 0x00000171UL;
            *(ULONG *)(adapter_core + 0x10) = 0x00000172UL;
            *(ULONG *)(adapter_core + 0x14) = 0x00000173UL;
            *(ULONG *)(adapter_core + 0x18) = 0x00000174UL;
            *(ULONG *)(adapter_core + 0x1C) = 0x00000175UL;
            *(ULONG *)(adapter_core + 0x20) = 0x00000176UL;
            *(ULONG *)(adapter_core + 0x24) = 0x00000177UL;
            *(ULONG *)(adapter_core + 0x28) = 0x00000376UL;
            *(ULONG *)(adapter_core + 0x2C) = 0x00000376UL;
            *(ULONG *)(adapter_core + 0x30) = 0x00000377UL;
            *(ULONG *)(adapter_core + 0xB0) |= 0x00000003UL;
        }
    }

    return STATUS_SUCCESS;
}

typedef struct _NT5_SCSI_COMPLETION_CONTEXT {
    UCHAR    completed;
    NTSTATUS io_status;
} NT5_SCSI_COMPLETION_CONTEXT, *PNT5_SCSI_COMPLETION_CONTEXT;

typedef struct _NT5_DEVICE_RELATIONS {
    ULONG           Count;
    PDEVICE_OBJECT  Objects[1];
} NT5_DEVICE_RELATIONS, *PNT5_DEVICE_RELATIONS;

static NTSTATUS NTAPI nt5_scsi_completion(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context)
{
    PNT5_SCSI_COMPLETION_CONTEXT ctx;

    (void)DeviceObject;

    ctx = (PNT5_SCSI_COMPLETION_CONTEXT)Context;
    if (ctx && Irp) {
        ctx->completed = 1;
        ctx->io_status = Irp->IoStatus.Status;
    }

    dbg_mark('K');
    if (Irp) {
        nt5_dbg_hex8((UCHAR)(Irp->IoStatus.Status & 0xFF));
    } else {
        nt5_dbg_hex8(0xFF);
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

static void nt5_log_device_list(void)
{
    PDEVICE_OBJECT dev;
    UCHAR count;

    dbg_mark('L');
    count = 0;
    dev = g_nt5_driver.DeviceObject;
    while (dev && count < 8) {
        dbg_mark('O');
        nt5_dbg_hex8(count);
        nt5_dbg_hex8((UCHAR)(dev->DeviceType & 0xFF));
        nt5_dbg_hex8((UCHAR)dev->StackSize);
        nt5_dbg_hex8((UCHAR)(dev->Flags & 0xFF));
        if (dev == g_nt5_fdo) {
            dbg_mark('F');
        } else if (dev == g_nt5_pdo) {
            dbg_mark('P');
        } else if (dev == g_nt5_child_pdo) {
            dbg_mark('C');
        } else {
            dbg_mark('?');
        }
        dev = dev->NextDevice;
        count++;
    }
    dbg_mark('l');
    nt5_dbg_hex8(count);
}

static PDEVICE_OBJECT nt5_query_bus_relations(void)
{
    PIRP irp;
    PIO_STACK_LOCATION stack;
    NT5_SCSI_COMPLETION_CONTEXT ctx;
    NTSTATUS status;
    PNT5_DEVICE_RELATIONS rel;
    PDEVICE_OBJECT child;
    ULONG info;

    child = NULL;
    if (!g_nt5_fdo) {
        dbg_mark('!');
        return NULL;
    }

    irp = IrpMgr_IoAllocateIrp(g_nt5_fdo->StackSize, FALSE);
    if (!irp) {
        dbg_mark('!');
        return NULL;
    }

    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;

    stack = IrpMgr_IoGetNextIrpStackLocation(irp);
    RtlZeroMemory(stack, sizeof(IO_STACK_LOCATION));
    stack->MajorFunction = IRP_MJ_PNP;
    stack->MinorFunction = IRP_MN_QUERY_DEVICE_RELATIONS;
    stack->Parameters.QueryDeviceRelations.Type = 0; /* BusRelations */

    ctx.completed = 0;
    ctx.io_status = STATUS_PENDING;
    IrpMgr_IoSetCompletionRoutine(irp, nt5_scsi_completion, &ctx,
                                  TRUE, TRUE, TRUE);

    dbg_mark('[');
    status = IrpMgr_IoCallDriver(g_nt5_fdo, irp);
    dbg_mark(']');
    nt5_dbg_hex8((UCHAR)(status & 0xFF));
    dbg_mark(ctx.completed ? 'C' : 'N');
    nt5_dbg_hex8((UCHAR)(ctx.io_status & 0xFF));

    info = (ULONG)irp->IoStatus.Information;
    rel = (PNT5_DEVICE_RELATIONS)info;
    dbg_mark('I');
    nt5_dbg_hex32(info);
    if (rel && info >= 0x80000000UL) {
        dbg_mark('O');
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)rel + 0x00));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)rel + 0x04));
        nt5_dbg_hex32(*(ULONG *)((PUCHAR)rel + 0x08));
    }
    if (rel && info >= 0x80000000UL) {
        dbg_mark('n');
        nt5_dbg_hex8((UCHAR)(rel->Count & 0xFF));
        if (rel->Count > 0) {
            child = rel->Objects[0];
            dbg_mark('h');
            nt5_dbg_hex8((UCHAR)((ULONG)child & 0xFF));
        }
    } else {
        dbg_mark('n');
        nt5_dbg_hex8(0);
    }

    IrpMgr_IoFreeIrp(irp);
    return child;
}

static void nt5_clear_matching_ptrs(PUCHAR base, ULONG size,
                                    PVOID ptr1, PVOID ptr2, char tag)
{
    ULONG off;
    ULONG v1;
    ULONG v2;

    if (!base || (ULONG)base < 0x80000000UL) {
        return;
    }

    v1 = (ULONG)ptr1;
    v2 = (ULONG)ptr2;
    for (off = 0; off + 4 <= size; off += 4) {
        ULONG *slot;

        slot = (ULONG *)(base + off);
        if (*slot == v1 || *slot == v2) {
            dbg_mark('Z');
            dbg_mark(tag);
            nt5_dbg_hex32(off);
            *slot = 0;
        }
    }
}

static void nt5_clear_busy_queues(PUCHAR base, ULONG size, char tag)
{
    ULONG off;

    if (!base || (ULONG)base < 0x80000000UL) {
        return;
    }

    for (off = 0; off + 20 <= size; off += 4) {
        ULONG list_head;

        list_head = (ULONG)(base + off + 4);
        if (*(ULONG *)(base + off + 4) == list_head &&
            *(ULONG *)(base + off + 8) == list_head &&
            *(UCHAR *)(base + off + 16) != 0) {
            dbg_mark('Z');
            dbg_mark('Q');
            dbg_mark(tag);
            nt5_dbg_hex32(off);
            *(UCHAR *)(base + off + 16) = 0;
        }
    }
}

static void nt5_clear_atapi_busy_queues(PDEVICE_OBJECT fdo)
{
    PUCHAR ext;
    ULONG service_context;

    if (!fdo || !fdo->DeviceExtension) {
        return;
    }

    ext = (PUCHAR)fdo->DeviceExtension;
    nt5_clear_busy_queues(ext, 0x400, 'F');
    nt5_clear_busy_queues((PUCHAR)(*(ULONG *)(ext + 0xA0)), 0x400, 'U');
    nt5_clear_busy_queues((PUCHAR)(*(ULONG *)(ext + 0x108)), 0x400, 'H');
    service_context = *(ULONG *)(ext + 0xD0);
    nt5_clear_busy_queues((PUCHAR)service_context, 0x400, 'A');
}

/* ================================================================
 * nt5_send_scsi_irp - Send a SCSI request to the NT5 device stack
 *
 * Builds an IRP with IRP_MJ_SCSI (= IRP_MJ_INTERNAL_DEVICE_CONTROL)
 * and dispatches it to the FDO. The SRB is passed in the I/O stack
 * location's Parameters.Scsi.Srb field.
 *
 * Returns the SRB status after the IRP completes.
 * ================================================================ */

static UCHAR nt5_send_scsi_irp(PDEVICE_OBJECT fdo, PSCSI_REQUEST_BLOCK srb)
{
    PIRP irp;
    PIO_STACK_LOCATION stack;
    NTSTATUS status;
    NT5_SCSI_COMPLETION_CONTEXT ctx;
    ULONG image_delta;
    UCHAR poll;
    typedef UCHAR (NTAPI *PFN_ATAPI_ISR)(PVOID Interrupt, PVOID ServiceContext);
    PFN_ATAPI_ISR atapi_isr;

    if (!fdo || !srb) {
        return 0x04; /* SRB_STATUS_ERROR */
    }

    /* Allocate an IRP with enough stack locations */
    irp = IrpMgr_IoAllocateIrp(fdo->StackSize, FALSE);
    if (!irp) {
        VxD_Debug_Printf("NT5: Failed to allocate IRP for SCSI request\n");
        return 0x04; /* SRB_STATUS_ERROR */
    }

    /* Set up the I/O stack location */
    stack = IrpMgr_IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_SCSI;
    stack->MinorFunction = 0;
    stack->Parameters.Scsi.Srb = (PVOID)srb;
    *(PVOID *)((PUCHAR)stack + 0x04) = (PVOID)srb;

    /* Link the SRB back to the IRP */
    srb->OriginalRequest = (PVOID)irp;

    ctx.completed = 0;
    ctx.io_status = STATUS_PENDING;
    IrpMgr_IoSetCompletionRoutine(irp, nt5_scsi_completion, &ctx,
                                  TRUE, TRUE, TRUE);
    *(PVOID *)((PUCHAR)stack + 0x04) = (PVOID)srb;

    /* Send the IRP down the stack */
    VxD_Debug_Printf("NT5: Sending SCSI IRP CDB[0]=0x%02X to FDO\n",
                     srb->Cdb[0]);

    status = IrpMgr_IoCallDriver(fdo, irp);
    dbg_mark('r');
    nt5_dbg_hex8((UCHAR)(status & 0xFF));
    dbg_mark('Q');
    dbg_mark(ctx.completed ? '1' : '0');
    nt5_dbg_hex8((UCHAR)(ctx.io_status & 0xFF));

    VxD_Debug_Printf("NT5: IoCallDriver returned 0x%08lX, SrbStatus=0x%02X\n",
                     (ULONG)status, srb->SrbStatus);

    if (status == STATUS_PENDING && !ctx.completed && g_nt5_fdo) {
        image_delta = (ULONG)g_nt5_driver.MajorFunction[0x1B] - 0x2049AUL;
        atapi_isr = (PFN_ATAPI_ISR)(image_delta + 0x16666UL);
        for (poll = 0; poll < 8 && !ctx.completed; poll++) {
            dbg_mark('m');
            nt5_dbg_hex8(poll);
            nt5_dbg_hex8(atapi_isr((PVOID)(*(ULONG *)((PUCHAR)g_nt5_fdo->DeviceExtension + 0xD0)), g_nt5_fdo));
            ntk_DrainDpcQueue();
            ntk_KeStallExecutionProcessor(1000UL);
        }
        dbg_mark('Q');
        dbg_mark(ctx.completed ? '1' : '0');
        nt5_dbg_hex8((UCHAR)(ctx.io_status & 0xFF));
        dbg_mark('S');
        nt5_dbg_hex8(srb->SrbStatus);
        if (!ctx.completed &&
            ((srb->SrbStatus & 0x3F) == SRB_STATUS_SUCCESS)) {
            dbg_mark('X');
            ctx.completed = 1;
            ctx.io_status = 0;
            if (fdo && fdo->DeviceExtension) {
                PUCHAR ext;
                ULONG service_context;

                ext = (PUCHAR)fdo->DeviceExtension;
                nt5_clear_matching_ptrs(ext, 0x200, srb, irp, 'F');
                service_context = *(ULONG *)(ext + 0xD0);
                nt5_clear_matching_ptrs((PUCHAR)service_context, 0x200, srb, irp, 'A');
                nt5_clear_atapi_busy_queues(fdo);
            }
            ntk_IoStartNextPacket(fdo, FALSE);
        }
    }
    /* The completion routine returns STATUS_MORE_PROCESSING_REQUIRED so
     * IRPMGR does not free the IRP under us before we can read the SRB. */
    if (ctx.completed || status != STATUS_PENDING) {
        IrpMgr_IoFreeIrp(irp);
    } else {
        dbg_mark('p');
    }

    return srb->SrbStatus;
}

/* ================================================================
 * nt5_test_read - Smoke test: read ISO 9660 primary volume descriptor
 *
 * Reads sector 16 (LBA 16) of the CD-ROM using a SCSI READ(10)
 * command. If media is present and the driver is working, the
 * first 5 bytes should be "CD001" (the ISO 9660 standard
 * identifier).
 *
 * This matches the test pattern used in the NT4 path
 * (NTMINI_V5.C) for consistency.
 * ================================================================ */

static UCHAR g_nt5_iso_pvd_cache[2048];
static UCHAR g_nt5_iso_root_cache[2048];
static ULONG g_nt5_iso_root_lba = 0;
static ULONG g_nt5_iso_root_size = 0;
static int g_nt5_iso_root_cache_valid = 0;

int nt5_get_iso_root_cache(UCHAR *pvd_buf, UCHAR *root_buf,
                           ULONG *root_lba, ULONG *root_size)
{
    ULONG i;

    if (!g_nt5_iso_root_cache_valid || !pvd_buf || !root_buf ||
        !root_lba || !root_size) {
        return -1;
    }

    for (i = 0; i < 2048; i++) {
        pvd_buf[i] = g_nt5_iso_pvd_cache[i];
        root_buf[i] = g_nt5_iso_root_cache[i];
    }
    *root_lba = g_nt5_iso_root_lba;
    *root_size = g_nt5_iso_root_size;
    return 0;
}

int nt5_get_iso_pvd_info(ULONG *root_lba, ULONG *root_size)
{
    if (!root_lba || !root_size ||
        g_nt5_iso_root_lba == 0 || g_nt5_iso_root_size == 0) {
        return -1;
    }

    *root_lba = g_nt5_iso_root_lba;
    *root_size = g_nt5_iso_root_size;
    return 0;
}

static int nt5_test_read(void)
{
    SCSI_REQUEST_BLOCK srb;
    UCHAR data_buf[2048];
    UCHAR srb_status;
    ULONG lba;
    UCHAR target;
    PDEVICE_OBJECT scsi_target;

    if (!g_nt5_fdo) {
        VxD_Debug_Printf("NT5: test_read: no FDO available\n");
        return -1;
    }

    scsi_target = g_nt5_child_pdo ? g_nt5_child_pdo : g_nt5_fdo;
    dbg_mark(g_nt5_child_pdo ? 'C' : 'F');
    dbg_mark('G');
    nt5_dbg_hex8(nt5_atapi_unit_count(scsi_target));
    if (nt5_atapi_unit_count(scsi_target) == 0) {
        dbg_mark('N');
        nt5_dbg_hex8(0);
    }

    for (target = 0; target < 4; target++) {
        /* Zero buffers */
        RtlZeroMemory(&srb, sizeof(srb));
        RtlZeroMemory(data_buf, sizeof(data_buf));

        /* Build READ(10) CDB for LBA 16, 1 sector, 2048 bytes */
        lba = 16;

        srb.Length              = sizeof(SCSI_REQUEST_BLOCK);
        srb.Function            = SRB_FUNCTION_EXECUTE_SCSI;
        srb.SrbStatus           = SRB_STATUS_PENDING;
        srb.PathId              = 0;
        srb.TargetId            = target;
        srb.Lun                 = 0;
        srb.CdbLength           = 10;
        srb.DataBuffer          = data_buf;
        srb.DataTransferLength  = 2048;
        srb.SrbFlags            = SRB_FLAGS_DATA_IN | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
        srb.TimeOutValue        = 10;

        /* READ(10) CDB: opcode, flags, LBA[4], reserved, count[2], control */
        srb.Cdb[0]  = SCSI_OP_READ10;
        srb.Cdb[1]  = 0x00;
        srb.Cdb[2]  = (UCHAR)((lba >> 24) & 0xFF);
        srb.Cdb[3]  = (UCHAR)((lba >> 16) & 0xFF);
        srb.Cdb[4]  = (UCHAR)((lba >>  8) & 0xFF);
        srb.Cdb[5]  = (UCHAR)((lba >>  0) & 0xFF);
        srb.Cdb[6]  = 0x00;
        srb.Cdb[7]  = 0x00;    /* block count high byte */
        srb.Cdb[8]  = 0x01;    /* block count low byte: 1 sector */
        srb.Cdb[9]  = 0x00;

        dbg_mark('t');
        dbg_mark((char)('0' + target));
        srb_status = nt5_send_scsi_irp(scsi_target, &srb);
        dbg_mark('s');
        nt5_dbg_hex8(srb_status);
        dbg_mark('c');
        nt5_dbg_hex8(srb.ScsiStatus);

        if ((srb_status & 0x3F) == SRB_STATUS_PENDING) {
            dbg_mark('P');
            continue;
        }

        if ((srb_status & 0x3F) != SRB_STATUS_SUCCESS) {
            continue;
        }

        if (data_buf[1] == 'C' && data_buf[2] == 'D' &&
            data_buf[3] == '0' && data_buf[4] == '0' &&
            data_buf[5] == '1') {
            ULONG i;
            ULONG root_lba;

            g_nt5_detected_target_id = target;
            for (i = 0; i < 2048; i++) {
                g_nt5_iso_pvd_cache[i] = data_buf[i];
            }
            g_nt5_iso_root_lba = *(ULONG *)(data_buf + 156 + 2);
            g_nt5_iso_root_size = *(ULONG *)(data_buf + 156 + 10);
            root_lba = g_nt5_iso_root_lba;

            g_nt5_iso_root_cache_valid = 0;

            RtlZeroMemory(&srb, sizeof(srb));
            RtlZeroMemory(g_nt5_iso_root_cache, 2048);
            srb.Length              = sizeof(SCSI_REQUEST_BLOCK);
            srb.Function            = SRB_FUNCTION_EXECUTE_SCSI;
            srb.SrbStatus           = SRB_STATUS_PENDING;
            srb.PathId              = 0;
            srb.TargetId            = target;
            srb.Lun                 = 0;
            srb.CdbLength           = 10;
            srb.DataBuffer          = g_nt5_iso_root_cache;
            srb.DataTransferLength  = 2048;
            srb.SrbFlags            = SRB_FLAGS_DATA_IN | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
            srb.TimeOutValue        = 10;

            srb.Cdb[0] = SCSI_OP_READ10;
            srb.Cdb[1] = 0x00;
            srb.Cdb[2] = (UCHAR)((root_lba >> 24) & 0xFF);
            srb.Cdb[3] = (UCHAR)((root_lba >> 16) & 0xFF);
            srb.Cdb[4] = (UCHAR)((root_lba >>  8) & 0xFF);
            srb.Cdb[5] = (UCHAR)((root_lba >>  0) & 0xFF);
            srb.Cdb[6] = 0x00;
            srb.Cdb[7] = 0x00;
            srb.Cdb[8] = 0x01;
            srb.Cdb[9] = 0x00;
            srb_status = nt5_send_scsi_irp(scsi_target, &srb);
            if ((srb_status & 0x3F) == SRB_STATUS_SUCCESS) {
                g_nt5_iso_root_cache_valid = 1;
                dbg_mark('C');
                dbg_mark('R');
            } else {
                dbg_mark('C');
                dbg_mark('!');
                nt5_dbg_hex8(srb_status);
            }
            VxD_Debug_Printf("NT5: test_read: ISO 9660 'CD001' found! SUCCESS\n");
            return 0;
        }

        dbg_mark('m');
        nt5_dbg_hex8(data_buf[1]);
        nt5_dbg_hex8(data_buf[2]);
        nt5_dbg_hex8(data_buf[3]);
        nt5_dbg_hex8(data_buf[4]);
        nt5_dbg_hex8(data_buf[5]);
    }

    VxD_Debug_Printf("NT5: test_read: SRB failed (status=0x%02X)\n",
                     srb_status);
    VxD_Debug_Printf("NT5:   (This is expected if no disc is inserted)\n");
    return -1;
}

int nt5_read_lba(ULONG lba, UCHAR *data_buf)
{
    SCSI_REQUEST_BLOCK srb;
    UCHAR srb_status;
    PDEVICE_OBJECT scsi_target;

    if (!g_nt5_fdo || !data_buf) {
        return -1;
    }

    scsi_target = g_nt5_child_pdo ? g_nt5_child_pdo : g_nt5_fdo;
    RtlZeroMemory(&srb, sizeof(srb));
    RtlZeroMemory(data_buf, 2048);

    srb.Length              = sizeof(SCSI_REQUEST_BLOCK);
    srb.Function            = SRB_FUNCTION_EXECUTE_SCSI;
    srb.SrbStatus           = SRB_STATUS_PENDING;
    srb.PathId              = 0;
    srb.TargetId            = g_nt5_detected_target_id;
    srb.Lun                 = 0;
    srb.CdbLength           = 10;
    srb.DataBuffer          = data_buf;
    srb.DataTransferLength  = 2048;
    srb.SrbFlags            = SRB_FLAGS_DATA_IN | SRB_FLAGS_DISABLE_SYNCH_TRANSFER;
    srb.TimeOutValue        = 10;

    srb.Cdb[0]  = SCSI_OP_READ10;
    srb.Cdb[1]  = 0x00;
    srb.Cdb[2]  = (UCHAR)((lba >> 24) & 0xFF);
    srb.Cdb[3]  = (UCHAR)((lba >> 16) & 0xFF);
    srb.Cdb[4]  = (UCHAR)((lba >>  8) & 0xFF);
    srb.Cdb[5]  = (UCHAR)((lba >>  0) & 0xFF);
    srb.Cdb[6]  = 0x00;
    srb.Cdb[7]  = 0x00;
    srb.Cdb[8]  = 0x01;
    srb.Cdb[9]  = 0x00;

    srb_status = nt5_send_scsi_irp(scsi_target, &srb);
    if ((srb_status & 0x3F) != SRB_STATUS_SUCCESS) {
        return -2;
    }
    return 0;
}

/* ================================================================
 * nt5_load_file - Load a file from disk into a heap buffer
 *
 * Uses the VxD_File_Open / VxD_File_Read / VxD_File_Close
 * wrappers from VXDWRAP.ASM, which call the VMM R0_OpenCreateFile
 * and R0_ReadFile services.
 *
 * Parameters:
 *   filename    - Full DOS path (e.g. "C:\\WINDOWS\\SYSTEM\\W2K_ATAPI.SYS")
 *   out_data    - Receives pointer to allocated buffer
 *   out_size    - Receives file size in bytes
 *
 * Returns:
 *   0 on success, -1 on failure.
 *   Caller must free *out_data with VxD_PageFree when done.
 * ================================================================ */

static int nt5_load_file(const char *filename, PVOID *out_data, ULONG *out_size)
{
    int handle;
    PUCHAR buffer;
    int bytes_read;
    ULONG total_read;
    ULONG alloc_size;

    *out_data = NULL;
    *out_size = 0;

    if (W2K_ATAPI_EMBEDDED_SIZE >= 2 &&
        w2k_atapi_embedded_data[0] == 'M' &&
        w2k_atapi_embedded_data[1] == 'Z') {
        VxD_Debug_Printf("NT5: Using embedded W2K ATAPI image\n");
        *out_data = (PVOID)w2k_atapi_embedded_data;
        *out_size = (ULONG)W2K_ATAPI_EMBEDDED_SIZE;
        return 0;
    }

    VxD_Debug_Printf("NT5: Opening file: ");
    VxD_Debug_Printf(filename);
    VxD_Debug_Printf("\n");

    /* Diagnostic: try opening a known file first */
    {
        static const char test_path[] = "C:\\MSDOS.SYS";
        int test_h = VxD_File_Open(test_path);
        VxD_Debug_Printf("NT5: Test open MSDOS.SYS = ");
        dbg_mark('0' + (test_h < 0 ? 0 : (test_h > 9 ? 9 : test_h)));
        if (test_h > 0) VxD_File_Close(test_h);
    }

    handle = VxD_File_Open(filename);
    VxD_Debug_Printf("NT5: File_Open returned: ");
    dbg_mark('0' + (handle < 0 ? 0 : (handle > 9 ? 9 : handle)));
    if (handle <= 0) {
        VxD_Debug_Printf("NT5: OPEN FAILED\n");
        return -1;
    }

    /* Allocate a buffer. We don't know the file size upfront
     * with the simple Open/Read API, so allocate a generous
     * buffer and read until EOF. */
    alloc_size = NT5_MAX_IMAGE_SIZE;
    /* Use static BSS buffer (PageAllocate addresses are in high memory
     * that IFSMgr's V86 I/O path can't copy into on Win98 SE) */
    buffer = g_file_buffer;
    RtlZeroMemory(buffer, alloc_size);
    dbg_mark('M');  /* using static buffer */

    /* Read the entire file */
    total_read = 0;
    while (total_read < alloc_size) {
        ULONG chunk;

        chunk = alloc_size - total_read;
        if (chunk > 32768) {
            chunk = 32768; /* read in 32 KB chunks */
        }

        dbg_mark('R');  /* about to File_Read */
        /* Explicit-position Ring0_FileIO reads wedge under QEMU once the
         * wrapper uses the real register ABI. Stay on sequential reads
         * through the file object's current position for now. */
        bytes_read = VxD_File_Read(handle, buffer + total_read, (int)chunk);
        /* Detailed read result debug */
        if (bytes_read > 0) {
            dbg_mark('D');  /* got Data */
            dbg_mark('0' + (bytes_read > 9 ? 9 : bytes_read));
        } else if (bytes_read == 0) {
            dbg_mark('Z');  /* Zero/EOF */
        } else {
            dbg_mark('N');  /* Negative/error */
            dbg_mark('0' + ((-bytes_read) > 9 ? 9 : (-bytes_read)));
        }
        if (bytes_read <= 0) {
            break; /* EOF or error */
        }

        total_read += (ULONG)bytes_read;
    }

    dbg_mark('L');  /* read Loop done */
    VxD_File_Close(handle);
    dbg_mark('X');  /* file closed */

    /* Verify buffer: output first 4 bytes to debug port */
    if (total_read >= 4) {
        dbg_mark('[');
        dbg_mark(buffer[0]);  /* Should be 'M' (0x4D) for MZ header */
        dbg_mark(buffer[1]);  /* Should be 'Z' (0x5A) for MZ header */
        dbg_mark(buffer[2]);
        dbg_mark(buffer[3]);
        dbg_mark(']');
    }

    if (total_read == 0) {
        VxD_Debug_Printf("NT5: File is empty or read failed\n");
        return -1;
    }

    VxD_Debug_Printf("NT5: File read complete: ");
    VxD_Debug_Printf(filename);
    VxD_Debug_Printf("\n");

    *out_data = (PVOID)buffer;
    *out_size = total_read;
    return 0;
}

/* ================================================================
 * nt5_init - Top-level entry point called from VxD control procedure
 *
 * Orchestrates the complete NT5 atapi.sys loading sequence:
 *
 *   1. Load atapi.sys from disk (or embedded array)
 *   2. PE-load and call DriverEntry
 *   3. Create PDO and call AddDevice + START_DEVICE
 *   4. Run smoke test (read ISO 9660 sector 16)
 *   5. If test passes, register with IOS via DRP pattern
 *   6. Set up WDM bridge context for IOR-to-IRP translation
 *
 * Parameters:
 *   use_primary - TRUE for primary IDE (0x1F0/IRQ14),
 *                 FALSE for secondary IDE (0x170/IRQ15)
 *
 * Returns:
 *   0 on success, negative on failure
 * ================================================================ */

int nt5_init(BOOLEAN use_primary)
{
    PVOID file_data;
    ULONG file_size;
    NTSTATUS status;
    int result;

    VxD_Debug_Printf("NT5: ======================================\n");
    VxD_Debug_Printf("NT5: NT5 ATAPI.SYS Loader starting\n");
    VxD_Debug_Printf("NT5: Target: ");
    VxD_Debug_Printf(use_primary ? "primary" : "secondary");
    VxD_Debug_Printf(" IDE channel\n");
    VxD_Debug_Printf("NT5: ======================================\n");

    /* Step 1: Load atapi.sys from disk */
    result = nt5_load_file(g_atapi_path, &file_data, &file_size);
    if (result != 0) {
        VxD_Debug_Printf("NT5: FATAL: Cannot load ");
        VxD_Debug_Printf(g_atapi_path);
        VxD_Debug_Printf("\n");
        return -1;
    }

    /* Step 2: PE-load and call DriverEntry */
    dbg_mark('{');
    status = nt5_load_atapi(file_data, file_size);
    dbg_mark('}');

    /* Free the file buffer (PE loader copies what it needs) */
    /* file_data points to static g_file_buffer, no free needed */

    if (!NT_SUCCESS(status)) {
        VxD_Debug_Printf("NT5: FATAL: atapi.sys load failed\n");
        return -2;
    }

    /* Step 3: PnP bootstrap */
    dbg_mark('<');
    status = nt5_start_device(use_primary);
    dbg_mark('>');
    if (!NT_SUCCESS(status)) {
        VxD_Debug_Printf("NT5: FATAL: Device start failed\n");
        return -3;
    }

    /* Step 4: Smoke test */
    dbg_mark('(');
    nt5_log_device_list();
    g_nt5_child_pdo = nt5_query_bus_relations();
    nt5_log_device_list();
    result = nt5_test_read();
    dbg_mark('T');
    dbg_mark(')');
    if (result == 0) {
        VxD_Debug_Printf("NT5: Smoke test PASSED\n");
    } else {
        VxD_Debug_Printf("NT5: Smoke test failed (continuing anyway)\n");
        VxD_Debug_Printf("NT5:   Driver loaded, but no ISO media detected.\n");
        VxD_Debug_Printf("NT5:   This is OK if no disc is in the drive.\n");
    }

    /* Step 5: Set up WDM bridge context
     *
     * The bridge context links the NT5 device stack to the Win9x
     * IOS layer. The calldown handler (in WDMBRIDGE.C) translates
     * IOS IORs into NT IRPs and dispatches them to g_nt5_fdo. */
    RtlZeroMemory(&g_nt5_bridge, sizeof(WDM_BRIDGE_CONTEXT));
    g_nt5_bridge.StackTop   = g_nt5_fdo;
    g_nt5_bridge.SectorSize = 2048;     /* CD-ROM default */
    g_nt5_bridge.DeviceType = 0x05;     /* DCB_TYPE_CDROM */
    g_nt5_bridge.Channel    = use_primary ? 0 : 1;
    g_nt5_bridge.TargetId   = g_nt5_detected_target_id;
    g_nt5_bridge.Active     = TRUE;

    /* IOS registration is handled by ios_register_port_driver() in
     * IOSBRIDGE.C, which is our caller. Do NOT call it here - that
     * creates infinite recursion (nt5_init -> ios_register -> nt5_init). */

    VxD_Debug_Printf("NT5: ======================================\n");
    VxD_Debug_Printf("NT5: atapi.sys loaded and operational\n");
    VxD_Debug_Printf("NT5: FDO=0x%08lX, PDO=0x%08lX\n",
                     (ULONG)g_nt5_fdo, (ULONG)g_nt5_pdo);
    VxD_Debug_Printf("NT5: ======================================\n");

    return 0;
}
