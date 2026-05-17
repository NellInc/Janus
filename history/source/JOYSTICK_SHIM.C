/*
 * JOYSTICK_SHIM.C - HID Gamepad to Win98 VJOYD Bridge
 *
 * Translates HID gamepad report descriptors and input reports into
 * Win98's VJOYD.VXD position data format. Allows XP-era USB gamepad
 * HID miniports to feed into Win98's joystick subsystem.
 *
 * Architecture:
 *   - Receives HID report descriptors from HID_SHIM.C
 *   - Parses axis/button usage pages to build VJOYD mappings
 *   - Translates HID input reports into VJOYD position structures
 *   - Registers as port driver shim for diagnostic entrypoint
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VxD wrapper)
 *
 * Known limitations (Phase 1):
 *   - Only one gamepad supported at a time
 *   - Max 6 axes, 16 buttons parsed from HID descriptors
 *   - No hat switch / POV support yet
 *   - No actual VJOYD registration (logs translated data only)
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
 * VJOYD Position Data (matches Win98 VJOYD.VXD JoyPos structure)
 * ================================================================ */

#define MAX_JOY_AXES    6
#define MAX_JOY_BUTTONS 16

typedef struct _JOYSTICK_POSITION {
    ULONG dwXpos;       /* X axis */
    ULONG dwYpos;       /* Y axis */
    ULONG dwZpos;       /* Z axis (throttle) */
    ULONG dwRpos;       /* Rudder */
    ULONG dwUpos;       /* 5th axis */
    ULONG dwVpos;       /* 6th axis */
    ULONG dwButtons;    /* Button bitmask */
    ULONG dwButtonNumber; /* Number of buttons */
} JOYSTICK_POSITION;

/* ================================================================
 * Gamepad State
 * ================================================================ */

static struct {
    BOOLEAN         Registered;
    PVOID           HidDeviceObject;
    ULONG           NumAxes;
    ULONG           NumButtons;
    /* Byte offsets within HID report for each axis (simplified) */
    UCHAR           AxisOffset[MAX_JOY_AXES];
    UCHAR           AxisSize[MAX_JOY_AXES];   /* bits per axis */
    UCHAR           ButtonOffset;             /* byte offset of button field */
    JOYSTICK_POSITION CurrentPos;
} g_joystick;

/* ================================================================
 * Joystick Shim Functions
 * ================================================================ */

/*
 * joystick_register_hid_device
 *
 * Called when HID_SHIM detects a gamepad. Parses the report descriptor
 * to determine axis/button layout. Phase 1: assumes standard gamepad
 * layout (X at byte 1, Y at byte 2, buttons at byte 0).
 */
NTSTATUS joystick_register_hid_device(
    PVOID HidDeviceObject,
    PVOID ReportDescriptor,
    ULONG DescLen)
{
    VxD_Debug_Printf("JOY: Registering HID gamepad device\r\n");

    if (HidDeviceObject == NULL) {
        return STATUS_UNSUCCESSFUL;
    }

    g_joystick.HidDeviceObject = HidDeviceObject;
    g_joystick.Registered = TRUE;

    /* Phase 1: assume standard 2-axis gamepad layout.
     * Real parsing of HID report descriptors (usage pages 0x01/0x09)
     * would walk the descriptor items here. */
    g_joystick.NumAxes = 2;
    g_joystick.NumButtons = 8;
    g_joystick.AxisOffset[0] = 1;  /* X at byte 1 */
    g_joystick.AxisSize[0]   = 8;
    g_joystick.AxisOffset[1] = 2;  /* Y at byte 2 */
    g_joystick.AxisSize[1]   = 8;
    g_joystick.ButtonOffset  = 0;  /* Buttons at byte 0 */

    VxD_Debug_Printf("JOY:   Axes=2, Buttons=8 (default layout)\r\n");

    (void)ReportDescriptor;
    (void)DescLen;

    return STATUS_SUCCESS;
}

/*
 * joystick_process_report
 *
 * Translates a HID input report into VJOYD position data.
 * Phase 1: reads axes and buttons from hardcoded offsets.
 */
NTSTATUS joystick_process_report(PVOID Report, ULONG ReportLen)
{
    PUCHAR data = (PUCHAR)Report;
    ULONG i;

    if (!g_joystick.Registered || Report == NULL || ReportLen < 3) {
        return STATUS_UNSUCCESSFUL;
    }

    /* Read button state */
    g_joystick.CurrentPos.dwButtons = (ULONG)data[g_joystick.ButtonOffset];
    g_joystick.CurrentPos.dwButtonNumber = g_joystick.NumButtons;

    /* Read axes (scale 8-bit 0-255 to 32-bit 0-65535 for VJOYD) */
    for (i = 0; i < g_joystick.NumAxes && i < MAX_JOY_AXES; i++) {
        ULONG raw = (ULONG)data[g_joystick.AxisOffset[i]];
        ULONG scaled = (raw * 65535) / 255;
        switch (i) {
            case 0: g_joystick.CurrentPos.dwXpos = scaled; break;
            case 1: g_joystick.CurrentPos.dwYpos = scaled; break;
            case 2: g_joystick.CurrentPos.dwZpos = scaled; break;
            case 3: g_joystick.CurrentPos.dwRpos = scaled; break;
            case 4: g_joystick.CurrentPos.dwUpos = scaled; break;
            case 5: g_joystick.CurrentPos.dwVpos = scaled; break;
        }
    }

    return STATUS_SUCCESS;
}

/* ================================================================
 * Import Function Table (diagnostic only, no DLL to resolve)
 * ================================================================ */

static const IMPORT_FUNC_ENTRY joystick_funcs[] = {
    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Structure
 * ================================================================ */

static PORT_DRIVER_SHIM joystick_shim_struct = {
    "VJOYD_BRIDGE",     /* dll_name (diagnostic identifier) */
    joystick_funcs,     /* func_table */
    0,                  /* func_count */
    NULL,               /* bridge_init */
    NULL,               /* bridge_io */
    NULL                /* bridge_cleanup */
};

/* ================================================================
 * Public Registration
 * ================================================================ */

void joystick_shim_register(void)
{
    VxD_Debug_Printf("JOY: Registering VJOYD bridge shim\r\n");
    register_port_driver(&joystick_shim_struct);
}
