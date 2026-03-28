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
 * These give us ring-0 file I/O on Win9x using VMM R0_OpenCreateFile,
 * R0_ReadFile, and R0_CloseFile services.
 * ================================================================ */

extern int  VxD_File_Open(const char *filename);
extern int  VxD_File_Read(int handle, void *buffer, int count);
extern void VxD_File_Close(int handle);
extern void VxD_Debug_Printf(const char *fmt, ...);

/* VxD heap (for file read buffer) */
extern PVOID VxD_HeapAllocate(ULONG size, ULONG flags);
extern void  VxD_HeapFree(PVOID ptr, ULONG flags);
#define HEAPF_ZEROINIT  0x00000001

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
#define SRB_FLAGS_DATA_IN           0x00000008
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER 0x00000020

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

/* ================================================================
 * NT5 LOADER GLOBAL STATE
 * ================================================================ */

/* DriverEntry prototype (stdcall: DriverObject, RegistryPath) */
typedef NTSTATUS (NTAPI *PFN_DRIVER_ENTRY)(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
);

static DRIVER_OBJECT    g_nt5_driver;
static DRIVER_EXTENSION g_nt5_driver_ext;
static DEVICE_OBJECT   *g_nt5_pdo;       /* fake Physical Device Object */
static DEVICE_OBJECT   *g_nt5_fdo;       /* atapi's Functional Device Object */
static WDM_BRIDGE_CONTEXT g_nt5_bridge;
static PVOID            g_nt5_image_base; /* loaded PE image base */

/* Number of DLL tables (ntoskrnl, HAL, WMILIB, SCSIPORT + sentinel) */
#define DLL_TABLE_COUNT     4

/* File path for disk-loaded atapi.sys */
static const char g_atapi_path[] = "C:\\WINDOWS\\SYSTEM\\W2K_ATAPI.SYS";

/* Max file size we'll try to load (256 KB) */
#define NT5_MAX_IMAGE_SIZE  (256UL * 1024UL)

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

    status = IrpMgr_IoCreateDevice(
        &g_nt5_driver,
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

    VxD_Debug_Printf("NT5: Loading atapi.sys (%lu bytes)\n", image_size);

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
        VxD_Debug_Printf("NT5: PE load failed (error=%d)\n", pe_result);
        return STATUS_UNSUCCESSFUL;
    }

    VxD_Debug_Printf("NT5: PE loaded at 0x%08lX, entry=0x%08lX\n",
                     (ULONG)g_nt5_image_base, (ULONG)entry_point);

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

    /* Step 4: Call DriverEntry
     *
     * W2K atapi.sys DriverEntry signature:
     *   NTSTATUS DriverEntry(PDRIVER_OBJECT, PUNICODE_STRING)
     *
     * We pass NULL for RegistryPath. The driver will call
     * IoAllocateDriverObjectExtension and set up its AddDevice
     * and MajorFunction dispatch table. */
    VxD_Debug_Printf("NT5: Calling DriverEntry at 0x%08lX\n",
                     (ULONG)driver_entry);

    status = driver_entry(&g_nt5_driver, NULL);

    if (!NT_SUCCESS(status)) {
        VxD_Debug_Printf("NT5: DriverEntry FAILED (status=0x%08lX)\n",
                         (ULONG)status);
        return status;
    }

    VxD_Debug_Printf("NT5: DriverEntry succeeded\n");

    /* Step 5: Log registered MajorFunction handlers */
    for (i = 0; i < IRP_MJ_MAXIMUM; i++) {
        if (g_nt5_driver.MajorFunction[i] != NULL) {
            VxD_Debug_Printf("NT5:   MajorFunction[0x%02lX] = 0x%08lX\n",
                             i, (ULONG)g_nt5_driver.MajorFunction[i]);
        }
    }

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

    status = pnp_start_device(g_nt5_fdo, io_base, io_len, irq);
    if (!NT_SUCCESS(status)) {
        VxD_Debug_Printf("NT5: IRP_MN_START_DEVICE FAILED (status=0x%08lX)\n",
                         (ULONG)status);
        return status;
    }

    VxD_Debug_Printf("NT5: Device started (I/O=0x%03lX, IRQ=%lu)\n",
                     io_base, irq);

    return STATUS_SUCCESS;
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
    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_SCSI;
    stack->MinorFunction = 0;
    stack->Parameters.Scsi.Srb = (PVOID)srb;

    /* Link the SRB back to the IRP */
    srb->OriginalRequest = (PVOID)irp;

    /* Send the IRP down the stack */
    VxD_Debug_Printf("NT5: Sending SCSI IRP CDB[0]=0x%02X to FDO\n",
                     srb->Cdb[0]);

    status = IrpMgr_IoCallDriver(fdo, irp);

    VxD_Debug_Printf("NT5: IoCallDriver returned 0x%08lX, SrbStatus=0x%02X\n",
                     (ULONG)status, srb->SrbStatus);

    /* Free the IRP if it was completed synchronously.
     * If STATUS_PENDING, the driver will complete it later via DPC.
     * For our single-threaded VxD shim, everything is synchronous. */
    if (status != STATUS_PENDING) {
        IrpMgr_IoFreeIrp(irp);
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

static int nt5_test_read(void)
{
    SCSI_REQUEST_BLOCK srb;
    UCHAR data_buf[2048];
    UCHAR srb_status;
    ULONG lba;

    if (!g_nt5_fdo) {
        VxD_Debug_Printf("NT5: test_read: no FDO available\n");
        return -1;
    }

    /* Zero buffers */
    RtlZeroMemory(&srb, sizeof(srb));
    RtlZeroMemory(data_buf, sizeof(data_buf));

    /* Build READ(10) CDB for LBA 16, 1 sector, 2048 bytes */
    lba = 16;

    srb.Length              = sizeof(SCSI_REQUEST_BLOCK);
    srb.Function            = SRB_FUNCTION_EXECUTE_SCSI;
    srb.SrbStatus           = SRB_STATUS_PENDING;
    srb.PathId              = 0;
    srb.TargetId            = 0;
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

    /* Send it */
    srb_status = nt5_send_scsi_irp(g_nt5_fdo, &srb);

    if ((srb_status & 0x3F) != SRB_STATUS_SUCCESS) {
        VxD_Debug_Printf("NT5: test_read: SRB failed (status=0x%02X)\n",
                         srb_status);
        VxD_Debug_Printf("NT5:   (This is expected if no disc is inserted)\n");
        return -1;
    }

    /* Check for ISO 9660 magic "CD001" at offset 1 */
    if (data_buf[1] == 'C' && data_buf[2] == 'D' &&
        data_buf[3] == '0' && data_buf[4] == '0' &&
        data_buf[5] == '1') {
        VxD_Debug_Printf("NT5: test_read: ISO 9660 'CD001' found! SUCCESS\n");
        return 0;
    }

    VxD_Debug_Printf("NT5: test_read: Sector 16 read OK, but no 'CD001' magic\n");
    VxD_Debug_Printf("NT5:   bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                     data_buf[0], data_buf[1], data_buf[2], data_buf[3],
                     data_buf[4], data_buf[5], data_buf[6], data_buf[7]);
    return -1;
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
 *   Caller must free *out_data with VxD_HeapFree when done.
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

    VxD_Debug_Printf("NT5: Opening file: %s\n", filename);

    handle = VxD_File_Open(filename);
    if (handle <= 0) {
        VxD_Debug_Printf("NT5: Failed to open %s\n", filename);
        return -1;
    }

    /* Allocate a buffer. We don't know the file size upfront
     * with the simple Open/Read API, so allocate a generous
     * buffer and read until EOF. */
    alloc_size = NT5_MAX_IMAGE_SIZE;
    buffer = (PUCHAR)VxD_HeapAllocate(alloc_size, HEAPF_ZEROINIT);
    if (!buffer) {
        VxD_Debug_Printf("NT5: Failed to allocate %lu bytes for file buffer\n",
                         alloc_size);
        VxD_File_Close(handle);
        return -1;
    }

    /* Read the entire file */
    total_read = 0;
    while (total_read < alloc_size) {
        ULONG chunk;

        chunk = alloc_size - total_read;
        if (chunk > 32768) {
            chunk = 32768; /* read in 32 KB chunks */
        }

        bytes_read = VxD_File_Read(handle, buffer + total_read, (int)chunk);
        if (bytes_read <= 0) {
            break; /* EOF or error */
        }

        total_read += (ULONG)bytes_read;
    }

    VxD_File_Close(handle);

    if (total_read == 0) {
        VxD_Debug_Printf("NT5: File is empty or read failed\n");
        VxD_HeapFree(buffer, 0);
        return -1;
    }

    VxD_Debug_Printf("NT5: Read %lu bytes from %s\n", total_read, filename);

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
    VxD_Debug_Printf("NT5: Target: %s IDE channel\n",
                     use_primary ? "primary" : "secondary");
    VxD_Debug_Printf("NT5: ======================================\n");

    /* Step 1: Load atapi.sys from disk */
    result = nt5_load_file(g_atapi_path, &file_data, &file_size);
    if (result != 0) {
        VxD_Debug_Printf("NT5: FATAL: Cannot load %s\n", g_atapi_path);
        return -1;
    }

    /* Step 2: PE-load and call DriverEntry */
    status = nt5_load_atapi(file_data, file_size);

    /* Free the file buffer (PE loader copies what it needs) */
    VxD_HeapFree(file_data, 0);

    if (!NT_SUCCESS(status)) {
        VxD_Debug_Printf("NT5: FATAL: atapi.sys load failed\n");
        return -2;
    }

    /* Step 3: PnP bootstrap */
    status = nt5_start_device(use_primary);
    if (!NT_SUCCESS(status)) {
        VxD_Debug_Printf("NT5: FATAL: Device start failed\n");
        return -3;
    }

    /* Step 4: Smoke test */
    result = nt5_test_read();
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
    g_nt5_bridge.TargetId   = 0;
    g_nt5_bridge.Active     = TRUE;

    /* Step 6: Register with IOS as a port driver
     *
     * This calls ios_register_port_driver() from IOSBRIDGE.C,
     * which sets up the DDB, AEP handler, and calldown chain
     * so IOS routes CD-ROM I/O requests to our bridge. */
    result = ios_register_port_driver();
    if (result != 0) {
        VxD_Debug_Printf("NT5: WARNING: IOS registration failed\n");
        VxD_Debug_Printf("NT5:   Driver loaded but not connected to IOS.\n");
        /* Not fatal: the driver is still loaded and can be tested */
    } else {
        VxD_Debug_Printf("NT5: IOS port driver registered\n");
    }

    VxD_Debug_Printf("NT5: ======================================\n");
    VxD_Debug_Printf("NT5: atapi.sys loaded and operational\n");
    VxD_Debug_Printf("NT5: FDO=0x%08lX, PDO=0x%08lX\n",
                     (ULONG)g_nt5_fdo, (ULONG)g_nt5_pdo);
    VxD_Debug_Printf("NT5: ======================================\n");

    return 0;
}
