/*
 * USB_SHIM.C - USBD.SYS Import Shim for Win98 VxD PE Loader
 *
 * Provides USBD.SYS import resolution for XP USB client drivers
 * (USBSTOR.SYS, USB NIC drivers, etc.) running on Win98 via the
 * VxD PE loader (PELOAD.C).
 *
 * All functions use __stdcall (NT kernel convention).
 * CRITICAL: Parameter counts must be EXACT — wrong count = stack corruption.
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 */

/* === Types (match PELOAD.C / NTMINI_V5.C) === */
typedef unsigned char  UCHAR;
typedef unsigned short USHORT;
typedef unsigned long  ULONG;
typedef signed long    LONG;
typedef unsigned char  BOOLEAN;
typedef void           VOID;
typedef void          *PVOID;
typedef UCHAR         *PUCHAR;
typedef USHORT        *PUSHORT;
typedef ULONG         *PULONG;
typedef ULONG          NTSTATUS;
#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)
#define STATUS_SUCCESS      0x00000000UL
#define STATUS_UNSUCCESSFUL 0xC0000001UL
#define PAGESIZE  4096
#define PAGEFIXED 0x00000001

/* === VxD externals === */
extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
extern void  VxD_Debug_Printf(const char *fmt, ...);

/* === PE loader externals === */
typedef struct { const char *name; PVOID func; } IMPORT_FUNC_ENTRY;
typedef struct _PORT_DRIVER_SHIM {
    const char *dll_name; const IMPORT_FUNC_ENTRY *func_table;
    int func_count;
    int  (*bridge_init)(void *); int (*bridge_io)(void *); void (*bridge_cleanup)(void);
} PORT_DRIVER_SHIM;
extern int register_port_driver(PORT_DRIVER_SHIM *shim);

/* === Helpers === */
static void usb_memset(void *dst, int val, ULONG n) {
    UCHAR *d = (UCHAR *)dst; while (n--) *d++ = (UCHAR)val;
}
static void usb_log_hex(const char *prefix, ULONG v, const char *suffix) {
    static const char hx[] = "0123456789ABCDEF";
    char h[12]; int i;
    h[0] = '0'; h[1] = 'x';
    for (i = 7; i >= 0; i--) h[2 + (7 - i)] = hx[(v >> (i * 4)) & 0xF];
    h[10] = 0;
    VxD_Debug_Printf(prefix); VxD_Debug_Printf(h); VxD_Debug_Printf(suffix);
}

/* === USB Descriptor Byte Offsets === */
/* Config descriptor (9 bytes) */
#define USB_CD_wTotalLength    2  /* USHORT LE */
#define USB_CD_bNumInterfaces  4
/* Interface descriptor (9 bytes) */
#define USB_ID_bLength             0
#define USB_ID_bDescriptorType     1
#define USB_ID_bInterfaceNumber    2
#define USB_ID_bAlternateSetting   3
#define USB_ID_bNumEndpoints       4
#define USB_ID_bInterfaceClass     5
#define USB_ID_bInterfaceSubClass  6
#define USB_ID_bInterfaceProtocol  7
#define USB_ID_SIZE                9
/* Endpoint descriptor (7 bytes) */
#define USB_ED_bEndpointAddress    2
#define USB_ED_SIZE                7
/* Descriptor type constants */
#define USB_INTERFACE_DESCRIPTOR_TYPE  4
#define USB_ENDPOINT_DESCRIPTOR_TYPE   5
/* URB header */
#define URB_HDR_Length    0  /* USHORT */
#define URB_HDR_Function 2  /* USHORT */
#define URB_HDR_Status   4  /* ULONG */
#define URB_HDR_SIZE     8
#define URB_FUNCTION_SELECT_CONFIGURATION 0x0000
/* Version constants */
#define USBDI_VERSION_500  0x00000500UL
#define USB_VERSION_200    0x00000200UL

/* ================================================================
 * USBD Function Implementations
 * ================================================================ */

/* USBD_GetUSBDIVersion — 1 param */
static void __stdcall usbd_GetUSBDIVersion(PVOID VersionInfo)
{
    PULONG vi = (PULONG)VersionInfo;
    VxD_Debug_Printf("USBD: GetUSBDIVersion\r\n");
    if (vi) { vi[0] = USBDI_VERSION_500; vi[1] = USB_VERSION_200; }
}

/* USBD_CreateConfigurationRequestEx — 2 params
 * Returns: allocated URB or NULL */
static PVOID __stdcall usbd_CreateConfigurationRequestEx(
    PVOID ConfigurationDescriptor, PVOID InterfaceList)
{
    PUCHAR cd = (PUCHAR)ConfigurationDescriptor;
    PUCHAR urb;
    VxD_Debug_Printf("USBD: CreateConfigurationRequestEx\r\n");
    if (!cd || !InterfaceList) return NULL;
    urb = (PUCHAR)VxD_PageAllocate(1, PAGEFIXED);
    if (!urb) return NULL;
    usb_memset(urb, 0, PAGESIZE);
    *(PUSHORT)(urb + URB_HDR_Length) = (USHORT)PAGESIZE;
    *(PUSHORT)(urb + URB_HDR_Function) = URB_FUNCTION_SELECT_CONFIGURATION;
    *(PVOID *)(urb + URB_HDR_SIZE) = ConfigurationDescriptor;
    usb_log_hex("  -> URB=", (ULONG)urb, "\r\n");
    return (PVOID)urb;
}

/* USBD_ParseConfigurationDescriptorEx — 7 params
 * Values of -1 mean "don't care". Returns interface desc ptr or NULL. */
static PVOID __stdcall usbd_ParseConfigurationDescriptorEx(
    PVOID ConfigurationDescriptor, PVOID StartPosition,
    LONG InterfaceNumber, LONG AlternateSetting,
    LONG InterfaceClass, LONG InterfaceSubClass, LONG InterfaceProtocol)
{
    PUCHAR cd = (PUCHAR)ConfigurationDescriptor;
    PUCHAR pos = (PUCHAR)StartPosition;
    PUCHAR end;
    USHORT totalLen;
    VxD_Debug_Printf("USBD: ParseConfigDescEx\r\n");
    if (!cd || !pos) return NULL;
    totalLen = (USHORT)(cd[USB_CD_wTotalLength] | (cd[USB_CD_wTotalLength+1] << 8));
    end = cd + totalLen;
    while (pos + USB_ID_SIZE <= end) {
        UCHAR bLen = pos[0], bType = pos[1];
        if (bLen < 2) break;
        if (bType == USB_INTERFACE_DESCRIPTOR_TYPE && bLen >= USB_ID_SIZE) {
            if (InterfaceNumber != -1 && (LONG)pos[USB_ID_bInterfaceNumber] != InterfaceNumber) goto next;
            if (AlternateSetting != -1 && (LONG)pos[USB_ID_bAlternateSetting] != AlternateSetting) goto next;
            if (InterfaceClass != -1 && (LONG)pos[USB_ID_bInterfaceClass] != InterfaceClass) goto next;
            if (InterfaceSubClass != -1 && (LONG)pos[USB_ID_bInterfaceSubClass] != InterfaceSubClass) goto next;
            if (InterfaceProtocol != -1 && (LONG)pos[USB_ID_bInterfaceProtocol] != InterfaceProtocol) goto next;
            return (PVOID)pos;
        }
next:   pos += bLen;
    }
    return NULL;
}

/* USBD_ParseDescriptors — 4 params */
static PVOID __stdcall usbd_ParseDescriptors(
    PVOID DescriptorBuffer, ULONG TotalLength,
    PVOID StartPosition, LONG DescriptorType)
{
    PUCHAR buf = (PUCHAR)DescriptorBuffer;
    PUCHAR pos = (PUCHAR)StartPosition;
    PUCHAR end = buf + TotalLength;
    while (pos + 2 <= end) {
        UCHAR bLen = pos[0];
        if (bLen < 2 || pos + bLen > end) break;
        if ((LONG)pos[1] == DescriptorType) return (PVOID)pos;
        pos += bLen;
    }
    return NULL;
}

/* USBD_GetInterfaceLength — 2 params */
static ULONG __stdcall usbd_GetInterfaceLength(
    PVOID InterfaceDescriptor, PVOID BufferEnd)
{
    PUCHAR pos = (PUCHAR)InterfaceDescriptor;
    PUCHAR end = (PUCHAR)BufferEnd;
    ULONG totalLen = 0;
    BOOLEAN pastFirst = FALSE;
    if (!pos || pos >= end) return 0;
    while (pos + 2 <= end) {
        UCHAR bLen = pos[0];
        if (bLen < 2 || pos + bLen > end) break;
        if (pos[1] == USB_INTERFACE_DESCRIPTOR_TYPE && pastFirst) break;
        pastFirst = TRUE;
        totalLen += bLen;
        pos += bLen;
    }
    return totalLen;
}

/* USBD_CalculateUsbBandwidth — 3 params */
static ULONG __stdcall usbd_CalculateUsbBandwidth(
    ULONG MaxPacketSize, ULONG EndpointType, ULONG LowSpeed)
{
    ULONG bw = MaxPacketSize * 8;
    if (LowSpeed) bw = bw / 8;
    return bw;
}

/* USBD_GetPdoRegistryParameter — 5 params (stub, returns failure) */
static NTSTATUS __stdcall usbd_GetPdoRegistryParameter(
    PVOID PhysicalDeviceObject, PVOID ParameterBuffer,
    ULONG ParameterLength, PVOID ParameterName, ULONG ParameterNameLength)
{
    VxD_Debug_Printf("USBD: GetPdoRegistryParameter (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* USBD_RegisterHcFilter — 2 params (stub) */
static NTSTATUS __stdcall usbd_RegisterHcFilter(
    PVOID DeviceObject, PVOID FilterDeviceObject)
{
    VxD_Debug_Printf("USBD: RegisterHcFilter (stub)\r\n");
    return STATUS_SUCCESS;
}

/* USBD_CreateConfigurationRequest — 2 params (non-Ex, older API) */
static PVOID __stdcall usbd_CreateConfigurationRequest(
    PVOID ConfigurationDescriptor, PUSHORT LengthOut)
{
    PVOID urb;
    VxD_Debug_Printf("USBD: CreateConfigurationRequest\r\n");
    urb = VxD_PageAllocate(1, PAGEFIXED);
    if (urb) {
        usb_memset(urb, 0, PAGESIZE);
        *(PUSHORT)((PUCHAR)urb + URB_HDR_Length) = (USHORT)PAGESIZE;
        *(PUSHORT)((PUCHAR)urb + URB_HDR_Function) = URB_FUNCTION_SELECT_CONFIGURATION;
        if (LengthOut) *LengthOut = (USHORT)PAGESIZE;
    }
    return urb;
}

/* USBD_ParseConfigurationDescriptor — 3 params (non-Ex) */
static PVOID __stdcall usbd_ParseConfigurationDescriptor(
    PVOID ConfigurationDescriptor, ULONG InterfaceNumber, ULONG AlternateSetting)
{
    return usbd_ParseConfigurationDescriptorEx(
        ConfigurationDescriptor, ConfigurationDescriptor,
        (LONG)InterfaceNumber, (LONG)AlternateSetting, -1, -1, -1);
}

/* USBD_QueryBusTime — 2 params */
static NTSTATUS __stdcall usbd_QueryBusTime(PVOID RootHubPdo, PULONG CurrentFrame)
{
    if (CurrentFrame) *CurrentFrame = 0;
    return STATUS_SUCCESS;
}

/* USBD_GetVersion — 0 params, returns ULONG */
static ULONG __stdcall usbd_GetVersion(void)
{
    return USBDI_VERSION_500;
}

/* USBD_CompleteRequest — 2 params (internal) */
static NTSTATUS __stdcall usbd_CompleteRequest(PVOID Irp, ULONG Status)
{
    VxD_Debug_Printf("USBD: CompleteRequest (stub)\r\n");
    return STATUS_SUCCESS;
}

/* USBD_RemoveDevice — 2 params (internal) */
static NTSTATUS __stdcall usbd_RemoveDevice(PVOID DeviceObject, ULONG Flags)
{
    VxD_Debug_Printf("USBD: RemoveDevice (stub)\r\n");
    return STATUS_SUCCESS;
}

/* USBD_RegisterHcDeviceCapabilities — 3 params (internal) */
static NTSTATUS __stdcall usbd_RegisterHcDeviceCapabilities(
    PVOID DeviceObject, PVOID Capabilities, ULONG CapSize)
{
    VxD_Debug_Printf("USBD: RegisterHcDeviceCapabilities (stub)\r\n");
    return STATUS_SUCCESS;
}

/* ================================================================
 * Import Function Table
 * ================================================================ */
static const IMPORT_FUNC_ENTRY usbd_funcs[] = {
    { "USBD_GetUSBDIVersion",                (PVOID)usbd_GetUSBDIVersion },
    { "USBD_CreateConfigurationRequestEx",   (PVOID)usbd_CreateConfigurationRequestEx },
    { "USBD_CreateConfigurationRequest",     (PVOID)usbd_CreateConfigurationRequest },
    { "USBD_ParseConfigurationDescriptorEx", (PVOID)usbd_ParseConfigurationDescriptorEx },
    { "USBD_ParseConfigurationDescriptor",   (PVOID)usbd_ParseConfigurationDescriptor },
    { "USBD_ParseDescriptors",               (PVOID)usbd_ParseDescriptors },
    { "USBD_GetInterfaceLength",             (PVOID)usbd_GetInterfaceLength },
    { "USBD_CalculateUsbBandwidth",          (PVOID)usbd_CalculateUsbBandwidth },
    { "USBD_GetPdoRegistryParameter",        (PVOID)usbd_GetPdoRegistryParameter },
    { "USBD_RegisterHcFilter",               (PVOID)usbd_RegisterHcFilter },
    { "USBD_QueryBusTime",                   (PVOID)usbd_QueryBusTime },
    { "USBD_GetVersion",                     (PVOID)usbd_GetVersion },
    { "USBD_CompleteRequest",                (PVOID)usbd_CompleteRequest },
    { "USBD_RemoveDevice",                   (PVOID)usbd_RemoveDevice },
    { "USBD_RegisterHcDeviceCapabilities",   (PVOID)usbd_RegisterHcDeviceCapabilities },
    { NULL, NULL }
};

/* === Port Driver Shim === */
static PORT_DRIVER_SHIM usbd_shim = {
    "USBD.SYS", usbd_funcs, 0, NULL, NULL, NULL
};

/* === Public API === */
void usb_shim_register(void)
{
    VxD_Debug_Printf("USB: Registering USBD.SYS shim\r\n");
    register_port_driver(&usbd_shim);
}

/* === Test Entrypoint === */
int usb_test(void)
{
    /* Synthetic config: Config(9) + Interface(9) + 2xEndpoint(7) = 32 bytes */
    static UCHAR test_config[] = {
        0x09, 0x02, 0x20, 0x00, 0x01, 0x01, 0x00, 0x80, 0xFA,  /* Config */
        0x09, 0x04, 0x00, 0x00, 0x02, 0x08, 0x06, 0x50, 0x00,  /* Interface: Mass Storage/SCSI/BOT */
        0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,              /* EP1 IN Bulk 512 */
        0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00               /* EP2 OUT Bulk 512 */
    };
    PVOID iface, ep, urb;
    ULONG ifLen;
    ULONG vi[2] = {0, 0};

    VxD_Debug_Printf("USB: === usb_test BEGIN ===\r\n");
    usb_shim_register();

    /* Version */
    usbd_GetUSBDIVersion((PVOID)vi);
    usb_log_hex("USB: USBDI=", vi[0], "");
    usb_log_hex(" USB=", vi[1], "\r\n");

    /* Find mass storage interface (class=0x08) */
    iface = usbd_ParseConfigurationDescriptorEx(
        test_config, test_config, 0, 0, 0x08, -1, -1);
    if (!iface) { VxD_Debug_Printf("USB: FAIL interface\r\n"); return -1; }
    VxD_Debug_Printf("USB: Found mass storage interface\r\n");

    /* Interface length: 9+7+7 = 23 */
    ifLen = usbd_GetInterfaceLength(iface, test_config + sizeof(test_config));
    usb_log_hex("USB: ifLen=", ifLen, "\r\n");

    /* Find first endpoint */
    ep = usbd_ParseDescriptors(iface, ifLen,
        (PUCHAR)iface + USB_ID_SIZE, USB_ENDPOINT_DESCRIPTOR_TYPE);
    if (!ep) { VxD_Debug_Printf("USB: FAIL endpoint\r\n"); return -1; }
    usb_log_hex("USB: EP addr=", (ULONG)((PUCHAR)ep)[USB_ED_bEndpointAddress], "\r\n");

    /* Allocate URB */
    urb = usbd_CreateConfigurationRequestEx(test_config, iface);
    if (!urb) { VxD_Debug_Printf("USB: FAIL URB\r\n"); return -1; }
    VxD_PageFree(urb);

    VxD_Debug_Printf("USB: === usb_test PASS ===\r\n");
    return 0;
}
