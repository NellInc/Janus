/*
 * HID_SHIM.C - HIDCLASS.SYS Shim for Win98 VxD PE Loader
 *
 * Provides a HIDCLASS.SYS import resolution layer enabling
 * XP HID miniport .sys files (e.g. hidusb.sys, mouhid.sys) to run
 * on Win98 via the VxD PE loader (PELOAD.C).
 *
 * Architecture:
 *   - Registers port driver shim: "HIDCLASS.SYS"
 *   - Resolves HidRegisterMinidriver, HidNotifyPresence,
 *     HidCompleteRequest exports
 *   - Stores minidriver registration info
 *   - Win98 SE already has HID stack (hid.dll + hidclass.sys in WDM
 *     mode); this shim resolves imports and forwards where possible
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VxD wrapper)
 *
 * Calling convention: all functions called by the miniport use __stdcall
 * (NT kernel convention).
 */

/* ================================================================
 * Basic type definitions (must match PELOAD.C / NTMINI_V5.C)
 * ================================================================ */

typedef unsigned char       UCHAR;
typedef unsigned short      USHORT;
typedef unsigned long       ULONG;
typedef signed long         LONG;
typedef unsigned char       BOOLEAN;
typedef void                VOID;
typedef void               *PVOID;
typedef UCHAR              *PUCHAR;
typedef USHORT             *PUSHORT;
typedef ULONG              *PULONG;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

#define STATUS_SUCCESS      0x00000000UL
#define STATUS_UNSUCCESSFUL 0xC0000001UL

typedef ULONG NTSTATUS;

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
extern void  VxD_Debug_Printf(const char *fmt, ...);

#define PAGESIZE    4096
#define PAGEFIXED   0x00000001

/* PE loader externals */
typedef struct { const char *name; PVOID func; } IMPORT_FUNC_ENTRY;

typedef struct _PORT_DRIVER_SHIM {
    const char              *dll_name;
    const IMPORT_FUNC_ENTRY *func_table;
    int                      func_count;
    int  (*bridge_init)(void *context);
    int  (*bridge_io)(void *request);
    void (*bridge_cleanup)(void);
} PORT_DRIVER_SHIM;

extern int register_port_driver(PORT_DRIVER_SHIM *shim);

/* ================================================================
 * UNICODE_STRING (minimal, matches NT DDK layout)
 * ================================================================ */

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PUSHORT Buffer;
} UNICODE_STRING;

/* ================================================================
 * HID_MINIDRIVER_REGISTRATION structure
 *
 * +0x00: ULONG Revision         (must be 1)
 * +0x04: PVOID DriverObject
 * +0x08: UNICODE_STRING RegistryPath (8 bytes: Length, MaxLen, Buffer)
 * +0x10: ULONG DeviceExtensionSize
 * +0x14: BOOLEAN DevicesArePolled
 * ================================================================ */

typedef struct _HID_MINIDRIVER_REGISTRATION {
    ULONG           Revision;
    PVOID           DriverObject;
    UNICODE_STRING  RegistryPath;
    ULONG           DeviceExtensionSize;
    BOOLEAN         DevicesArePolled;
} HID_MINIDRIVER_REGISTRATION;

/* ================================================================
 * Minidriver State
 * ================================================================ */

static struct {
    PVOID   DriverObject;
    ULONG   DeviceExtensionSize;
    BOOLEAN DevicesArePolled;
    BOOLEAN Registered;
} g_hid_minidriver;

/* ================================================================
 * HID Shim Functions (__stdcall)
 * ================================================================ */

/*
 * HidRegisterMinidriver(DriverObject, RegistryPath, MinidriverRegistration)
 *
 * Called by HID miniport DriverEntry to register with HIDCLASS.
 * We store the registration and return success.
 */
static NTSTATUS __stdcall hid_RegisterMinidriver(
    PVOID DriverObject,
    PVOID RegistryPath,
    HID_MINIDRIVER_REGISTRATION *Registration)
{
    VxD_Debug_Printf("HID: HidRegisterMinidriver called\r\n");

    if (Registration == NULL) {
        VxD_Debug_Printf("HID:   ERROR: NULL registration\r\n");
        return STATUS_UNSUCCESSFUL;
    }

    if (Registration->Revision != 1) {
        VxD_Debug_Printf("HID:   WARNING: Revision != 1\r\n");
    }

    g_hid_minidriver.DriverObject        = DriverObject;
    g_hid_minidriver.DeviceExtensionSize  = Registration->DeviceExtensionSize;
    g_hid_minidriver.DevicesArePolled     = Registration->DevicesArePolled;
    g_hid_minidriver.Registered           = TRUE;

    VxD_Debug_Printf("HID:   DeviceExtensionSize = ");
    VxD_Debug_Printf(Registration->DevicesArePolled ? "polled\r\n" : "interrupt\r\n");

    return STATUS_SUCCESS;
}

/*
 * HidNotifyPresence(DeviceObject, IsPresent)
 *
 * Notifies HIDCLASS that a device has arrived or departed.
 * Stub: logs and returns success.
 */
static NTSTATUS __stdcall hid_NotifyPresence(PVOID DeviceObject, BOOLEAN IsPresent)
{
    VxD_Debug_Printf("HID: HidNotifyPresence ");
    VxD_Debug_Printf(IsPresent ? "ARRIVED\r\n" : "DEPARTED\r\n");
    return STATUS_SUCCESS;
}

/*
 * HidCompleteRequest(DeviceObject, Irp, Status, Info)
 *
 * Completes an IRP that was pended by the minidriver.
 * Stub: logs the completion status.
 */
static VOID __stdcall hid_CompleteRequest(
    PVOID DeviceObject,
    PVOID Irp,
    NTSTATUS Status,
    ULONG Info)
{
    VxD_Debug_Printf("HID: HidCompleteRequest status=");
    VxD_Debug_Printf(Status == STATUS_SUCCESS ? "OK" : "FAIL");
    VxD_Debug_Printf("\r\n");
    (void)DeviceObject;
    (void)Irp;
    (void)Info;
}

/* ================================================================
 * Import Function Table (for HIDCLASS.SYS resolution)
 * ================================================================ */

static const IMPORT_FUNC_ENTRY hid_funcs[] = {
    { "HidRegisterMinidriver",  (PVOID)hid_RegisterMinidriver },
    { "HidNotifyPresence",      (PVOID)hid_NotifyPresence },
    { "HidCompleteRequest",     (PVOID)hid_CompleteRequest },
    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Structure
 * ================================================================ */

static PORT_DRIVER_SHIM hidclass_shim = {
    "HIDCLASS.SYS",     /* dll_name */
    hid_funcs,          /* func_table */
    0,                  /* func_count (uses null terminator) */
    NULL,               /* bridge_init */
    NULL,               /* bridge_io */
    NULL                /* bridge_cleanup */
};

/* ================================================================
 * Public Registration
 * ================================================================ */

void hid_shim_register(void)
{
    VxD_Debug_Printf("HID: Registering HIDCLASS.SYS shim\r\n");
    register_port_driver(&hidclass_shim);
}
