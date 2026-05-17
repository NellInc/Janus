/*
 * VIDEO_SHIM.C - VideoPort Driver Shim for Win98 VxD PE Loader
 *
 * Provides a VIDEOPRT.SYS import resolution layer enabling XP display
 * miniport .sys files to run on Win98 via the VxD PE loader (PELOAD.C).
 *
 * Architecture:
 *   - Registers port driver shim: "VIDEOPRT.SYS"
 *   - Provides VideoPort API functions (VideoPortInitialize, etc.)
 *   - Stores miniport HW_INITIALIZATION_DATA callbacks
 *   - Exercises: DriverEntry -> VideoPortInitialize -> HwFindAdapter
 *     -> HwInitialize -> mode enumeration
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VxD wrapper)
 *
 * Target drivers: ATI Rage 128 (ati128.sys), NVIDIA GeForce (nv4_mini.sys),
 *                 Intel i810 (i810_mini.sys), S3 Savage (s3sav4.sys)
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
typedef LONG               *PLONG;
typedef unsigned short      WCHAR;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

typedef LONG NTSTATUS;
typedef ULONG VP_STATUS;

#define NO_ERROR            0x00000000UL
#define ERROR_INVALID_PARAMETER 0x00000057UL
#define ERROR_DEV_NOT_EXIST 0x00000037UL
#define ERROR_NOT_ENOUGH_MEMORY 0x00000008UL

/* ================================================================
 * Inline port I/O (ring 0, no libc)
 * ================================================================ */

#ifdef __WATCOMC__
unsigned char  _port_inb(unsigned short port);
#pragma aux _port_inb = "in al, dx" parm [dx] value [al];
unsigned short _port_inw(unsigned short port);
#pragma aux _port_inw = "in ax, dx" parm [dx] value [ax];
unsigned long  _port_ind(unsigned short port);
#pragma aux _port_ind = "in eax, dx" parm [dx] value [eax];
void _port_outb(unsigned short port, unsigned char val);
#pragma aux _port_outb = "out dx, al" parm [dx] [al];
void _port_outw(unsigned short port, unsigned short val);
#pragma aux _port_outw = "out dx, ax" parm [dx] [ax];
void _port_outd(unsigned short port, unsigned long val);
#pragma aux _port_outd = "out dx, eax" parm [dx] [eax];
#endif

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
extern PVOID VxD_MapPhysToLinear(ULONG physAddr, ULONG nBytes);
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
 * Local helpers
 * ================================================================ */

static void vp_memset(void *dst, int val, ULONG n) {
    UCHAR *d = (UCHAR *)dst;
    while (n--) *d++ = (UCHAR)val;
}

static void vp_memcpy(void *dst, const void *src, ULONG n) {
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    while (n--) *d++ = *s++;
}

static void vp_log_hex(const char *prefix, ULONG v, const char *suffix) {
    static const char hx[] = "0123456789ABCDEF";
    char h[12];
    int i;
    h[0] = '0'; h[1] = 'x';
    for (i = 7; i >= 0; i--) h[2 + (7 - i)] = hx[(v >> (i * 4)) & 0xF];
    h[10] = 0;
    VxD_Debug_Printf(prefix);
    VxD_Debug_Printf(h);
    VxD_Debug_Printf(suffix);
}

/* ================================================================
 * VIDEO_HW_INITIALIZATION_DATA
 *
 * The miniport fills this structure and passes it to VideoPortInitialize.
 * We extract the callback function pointers from it.
 *
 * Layout (XP DDK, VIDEO_HW_INITIALIZATION_DATA):
 *   +0x00: ULONG HwInitDataSize
 *   +0x04: ULONG AdapterInterfaceType (PCIBus=5)
 *   +0x08: ULONG HwDeviceExtensionSize
 *   +0x0C: ULONG StartingDeviceNumber
 *   +0x10: ULONG HwFindAdapter offset/function ptr
 *   +0x14: ULONG HwInitialize
 *   +0x18: ULONG HwInterrupt
 *   +0x1C: ULONG HwStartIO (IOCTL handler)
 *   +0x20: ULONG HwResetHw
 *   +0x24: ULONG HwTimer
 *   +0x28: ULONG HwStartDma (obsolete)
 *   +0x2C: ULONG HwSetPowerState
 *   +0x30: ULONG HwGetPowerState
 *   +0x34: ULONG HwGetVideoChildDescriptor
 *   +0x38: ULONG HwQueryDeviceInterface (NDIS 5.0+)
 *   +0x3C: ULONG HwChildDeviceExtensionSize (XP+)
 *   +0x40: ULONG HwLegacyResourceList
 *   +0x44: ULONG HwLegacyResourceCount
 *   +0x48: ULONG HwGetLegacyResources
 *   +0x4C: ULONG AllowEarlyEnumeration
 * ================================================================ */

/* Corrected from ReactOS/XP DDK (84 bytes total): */
#define VP_HWINIT_SIZE         0x54
#define VP_HWINIT_FIND_ADAPTER 0x08
#define VP_HWINIT_INITIALIZE   0x0C
#define VP_HWINIT_INTERRUPT    0x10
#define VP_HWINIT_START_IO     0x14
#define VP_HWINIT_DEV_EXT_SIZE 0x18
#define VP_HWINIT_RESET        0x20
#define VP_HWINIT_TIMER        0x24

/* Stored miniport callbacks */
static struct {
    PVOID HwFindAdapter;
    PVOID HwInitialize;
    PVOID HwInterrupt;
    PVOID HwStartIO;
    PVOID HwResetHw;
    PVOID HwTimer;
    ULONG DeviceExtensionSize;
    ULONG AdapterInterfaceType;
    PVOID DeviceExtension;
} g_video_miniport;

/* ================================================================
 * VideoPort API Implementations
 * ================================================================ */

/*
 * VideoPortInitialize
 *
 * Called from DriverEntry. Stores HW_INITIALIZATION_DATA callbacks,
 * allocates DeviceExtension, calls HwFindAdapter + HwInitialize.
 *
 * ULONG VideoPortInitialize(
 *   IN PVOID Argument1,           // DriverObject
 *   IN PVOID Argument2,           // RegistryPath
 *   IN PVOID HwInitializationData,
 *   IN PVOID HwContext)
 */
static ULONG __stdcall vp_Initialize(
    PVOID Argument1,
    PVOID Argument2,
    PVOID HwInitializationData,
    PVOID HwContext)
{
    PUCHAR hwid = (PUCHAR)HwInitializationData;
    ULONG devExtSize;

    VxD_Debug_Printf("VP: VideoPortInitialize\r\n");

    if (!hwid) return ERROR_INVALID_PARAMETER;

    /* Extract callbacks from HW_INITIALIZATION_DATA */
    g_video_miniport.HwFindAdapter = *(PVOID *)(hwid + VP_HWINIT_FIND_ADAPTER);
    g_video_miniport.HwInitialize  = *(PVOID *)(hwid + VP_HWINIT_INITIALIZE);
    g_video_miniport.HwInterrupt   = *(PVOID *)(hwid + VP_HWINIT_INTERRUPT);
    g_video_miniport.HwStartIO     = *(PVOID *)(hwid + VP_HWINIT_START_IO);
    g_video_miniport.HwResetHw     = *(PVOID *)(hwid + VP_HWINIT_RESET);
    g_video_miniport.HwTimer       = *(PVOID *)(hwid + VP_HWINIT_TIMER);
    g_video_miniport.DeviceExtensionSize = *(ULONG *)(hwid + VP_HWINIT_DEV_EXT_SIZE);
    g_video_miniport.AdapterInterfaceType = *(ULONG *)(hwid + 0x04);

    vp_log_hex("  DevExtSize=", g_video_miniport.DeviceExtensionSize, "\r\n");
    vp_log_hex("  HwFindAdapter=", (ULONG)g_video_miniport.HwFindAdapter, "\r\n");
    vp_log_hex("  HwInitialize=", (ULONG)g_video_miniport.HwInitialize, "\r\n");
    vp_log_hex("  HwStartIO=", (ULONG)g_video_miniport.HwStartIO, "\r\n");
    vp_log_hex("  BusType=", g_video_miniport.AdapterInterfaceType, "\r\n");

    /* Allocate device extension */
    devExtSize = g_video_miniport.DeviceExtensionSize;
    if (devExtSize == 0) devExtSize = 4096;
    g_video_miniport.DeviceExtension = VxD_PageAllocate(
        (devExtSize + PAGESIZE - 1) / PAGESIZE, PAGEFIXED);
    if (!g_video_miniport.DeviceExtension) {
        VxD_Debug_Printf("VP: DeviceExtension alloc FAILED\r\n");
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    vp_memset(g_video_miniport.DeviceExtension, 0, devExtSize);
    vp_log_hex("  DevExt @", (ULONG)g_video_miniport.DeviceExtension, "\r\n");

    return NO_ERROR;
}

/*
 * VideoPortGetDeviceBase — maps a physical address range for port/memory I/O.
 * For I/O ports: returns the port base as-is (identity mapped in ring 0).
 * For memory: maps physical to linear via VMM.
 *
 * PVOID VideoPortGetDeviceBase(HwDevExt, IoAddress, NumberOfUchars, InIoSpace)
 * IoAddress is PHYSICAL_ADDRESS (8 bytes by value = 2 DWORDs on stack).
 * Total: 5 DWORDs = ret 20.
 */
static PVOID __stdcall vp_GetDeviceBase(
    PVOID HwDeviceExtension,
    ULONG IoAddress_Low,  /* PHYSICAL_ADDRESS low DWORD */
    ULONG IoAddress_High, /* PHYSICAL_ADDRESS high DWORD */
    ULONG NumberOfUchars,
    UCHAR InIoSpace)      /* TRUE=I/O ports, FALSE=memory */
{
    vp_log_hex("VP: GetDeviceBase addr=", IoAddress_Low, "");
    vp_log_hex(" len=", NumberOfUchars, "");
    vp_log_hex(" io=", (ULONG)InIoSpace, "\r\n");

    if (InIoSpace) {
        return (PVOID)IoAddress_Low;
    } else {
        PVOID mapped = VxD_MapPhysToLinear(IoAddress_Low, NumberOfUchars);
        vp_log_hex("  -> mapped=", (ULONG)mapped, "\r\n");
        return mapped;
    }
}

/*
 * VideoPortFreeDeviceBase
 */
static void __stdcall vp_FreeDeviceBase(
    PVOID HwDeviceExtension,
    PVOID MappedAddress)
{
    vp_log_hex("VP: FreeDeviceBase ", (ULONG)MappedAddress, "\r\n");
}

/*
 * VideoPortGetBusData / VideoPortSetBusData — PCI config space access
 */
static ULONG __stdcall vp_GetBusData(
    PVOID HwDeviceExtension,
    ULONG BusDataType,
    ULONG SlotNumber,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    /* PCI config space read via I/O ports 0xCF8/0xCFC */
    ULONG bus = 0, dev = (SlotNumber >> 0) & 0x1F, func = (SlotNumber >> 5) & 0x07;
    ULONG addr, i;
    PUCHAR buf = (PUCHAR)Buffer;

    vp_log_hex("VP: GetBusData slot=", SlotNumber, "");
    vp_log_hex(" off=", Offset, "");
    vp_log_hex(" len=", Length, "\r\n");

    for (i = 0; i < Length; i += 4) {
        addr = 0x80000000UL | (bus << 16) | (dev << 11) | (func << 8) |
               ((Offset + i) & 0xFC);
        _port_outd(0xCF8, addr);
        *(ULONG *)(buf + i) = _port_ind(0xCFC);
    }

    return Length;
}

static ULONG __stdcall vp_SetBusData(
    PVOID HwDeviceExtension,
    ULONG BusDataType,
    ULONG SlotNumber,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    ULONG bus = 0, dev = (SlotNumber >> 0) & 0x1F, func = (SlotNumber >> 5) & 0x07;
    ULONG addr, i;
    PUCHAR buf = (PUCHAR)Buffer;

    vp_log_hex("VP: SetBusData slot=", SlotNumber, "");
    vp_log_hex(" off=", Offset, "");
    vp_log_hex(" len=", Length, "\r\n");

    for (i = 0; i < Length; i += 4) {
        addr = 0x80000000UL | (bus << 16) | (dev << 11) | (func << 8) |
               ((Offset + i) & 0xFC);
        _port_outd(0xCF8, addr);
        _port_outd(0xCFC, *(ULONG *)(buf + i));
    }

    return Length;
}

/* Port I/O functions — same pattern as HAL exports */
static UCHAR __stdcall vp_ReadPortUchar(PUCHAR Port)
{
    return _port_inb((unsigned short)(ULONG)Port);
}
static USHORT __stdcall vp_ReadPortUshort(PUSHORT Port)
{
    return _port_inw((unsigned short)(ULONG)Port);
}
static ULONG __stdcall vp_ReadPortUlong(PULONG Port)
{
    return _port_ind((unsigned short)(ULONG)Port);
}
static void __stdcall vp_WritePortUchar(PUCHAR Port, UCHAR Value)
{
    _port_outb((unsigned short)(ULONG)Port, Value);
}
static void __stdcall vp_WritePortUshort(PUSHORT Port, USHORT Value)
{
    _port_outw((unsigned short)(ULONG)Port, Value);
}
static void __stdcall vp_WritePortUlong(PULONG Port, ULONG Value)
{
    _port_outd((unsigned short)(ULONG)Port, Value);
}

/* Register (memory-mapped) I/O — direct pointer access */
static UCHAR __stdcall vp_ReadRegisterUchar(PUCHAR Register)
{
    return *Register;
}
static USHORT __stdcall vp_ReadRegisterUshort(PUSHORT Register)
{
    return *Register;
}
static ULONG __stdcall vp_ReadRegisterUlong(PULONG Register)
{
    return *Register;
}
static void __stdcall vp_WriteRegisterUchar(PUCHAR Register, UCHAR Value)
{
    *Register = Value;
}
static void __stdcall vp_WriteRegisterUshort(PUSHORT Register, USHORT Value)
{
    *Register = Value;
}
static void __stdcall vp_WriteRegisterUlong(PULONG Register, ULONG Value)
{
    *Register = Value;
}

/* Memory operations */
static void __stdcall vp_ZeroMemory(PVOID Destination, ULONG Length)
{
    vp_memset(Destination, 0, Length);
}
static void __stdcall vp_MoveMemory(PVOID Destination, PVOID Source, ULONG Length)
{
    vp_memcpy(Destination, Source, Length);
}
static void __stdcall vp_ZeroDeviceMemory(PVOID Destination, ULONG Length)
{
    vp_memset(Destination, 0, Length);
}

/* Memory allocation */
static PVOID __stdcall vp_AllocatePool(PVOID HwDeviceExtension, ULONG PoolType, ULONG Size, ULONG Tag)
{
    PVOID mem;
    ULONG nPages = (Size + PAGESIZE - 1) / PAGESIZE;
    vp_log_hex("VP: AllocatePool size=", Size, "\r\n");
    mem = VxD_PageAllocate(nPages, PAGEFIXED);
    if (mem) vp_memset(mem, 0, nPages * PAGESIZE);
    return mem;
}

static void __stdcall vp_FreePool(PVOID HwDeviceExtension, PVOID Ptr)
{
    if (Ptr) VxD_PageFree(Ptr);
}

/* Registry access (stubs — return not-found for most queries) */
static VP_STATUS __stdcall vp_GetRegistryParameters(
    PVOID HwDeviceExtension,
    PVOID ParameterName,    /* PWSTR */
    UCHAR IsParameterFileName,
    PVOID CallbackRoutine,
    PVOID Context)
{
    VxD_Debug_Printf("VP: GetRegistryParameters (stub)\r\n");
    return ERROR_INVALID_PARAMETER;
}

static VP_STATUS __stdcall vp_SetRegistryParameters(
    PVOID HwDeviceExtension,
    PVOID ValueName,
    PVOID ValueData,
    ULONG ValueLength)
{
    VxD_Debug_Printf("VP: SetRegistryParameters (stub)\r\n");
    return NO_ERROR;
}

/* Access ranges */
static VP_STATUS __stdcall vp_GetAccessRanges(
    PVOID HwDeviceExtension,
    ULONG NumRequestedResources,
    PVOID RequestedResources,      /* PIO_RESOURCE_DESCRIPTOR */
    ULONG NumAccessRanges,
    PVOID AccessRanges,            /* PVIDEO_ACCESS_RANGE */
    PVOID VendorId,
    PVOID DeviceId,
    PULONG Slot)
{
    vp_log_hex("VP: GetAccessRanges numReq=", NumRequestedResources, "");
    vp_log_hex(" numAR=", NumAccessRanges, "\r\n");
    /* TODO: PCI BAR enumeration for video device */
    return NO_ERROR;
}

static VP_STATUS __stdcall vp_VerifyAccessRanges(
    PVOID HwDeviceExtension,
    ULONG NumAccessRanges,
    PVOID AccessRanges)
{
    VxD_Debug_Printf("VP: VerifyAccessRanges\r\n");
    return NO_ERROR;
}

/* Stall execution */
static void __stdcall vp_StallExecution(ULONG Microseconds)
{
    ULONG i;
    for (i = 0; i < Microseconds; i++) {
        _port_inb(0x80); /* ~1us per ISA bus cycle */
    }
}

/* Logging */
static void __stdcall vp_LogError(
    PVOID HwDeviceExtension,
    PVOID Vrp,
    VP_STATUS ErrorCode,
    ULONG UniqueId)
{
    vp_log_hex("VP: LogError code=", ErrorCode, "");
    vp_log_hex(" id=", UniqueId, "\r\n");
}

/* Debug print */
static void __stdcall vp_DebugPrint(ULONG Level, PVOID Message, ULONG p1, ULONG p2, ULONG p3)
{
    VxD_Debug_Printf("VP: DebugPrint\r\n");
}

/* Misc stubs */
static BOOLEAN __stdcall vp_ScanRom(PVOID HwDev, PUCHAR Base, ULONG Len, PVOID Str)
{
    VxD_Debug_Printf("VP: ScanRom (stub)\r\n");
    return FALSE;
}

static PVOID __stdcall vp_GetDeviceData(PVOID HwDev, ULONG Type, PVOID Callback, PVOID Ctx)
{
    VxD_Debug_Printf("VP: GetDeviceData (stub)\r\n");
    return NULL;
}

/* VideoPortMapMemory: 6 params (HwDev, PhysAddr[2], Length*, InIoSpace*, VirtualAddress*) = ret 24 */
static VP_STATUS __stdcall vp_MapMemory(
    PVOID HwDev, ULONG PhysLow, ULONG PhysHigh, PULONG Length,
    PULONG InIoSpace, PVOID *VirtualAddress)
{
    vp_log_hex("VP: MapMemory phys=", PhysLow, "");
    vp_log_hex(" len=", *Length, "\r\n");
    *VirtualAddress = VxD_MapPhysToLinear(PhysLow, *Length);
    return NO_ERROR;
}

/* VideoPortUnmapMemory: 3 params = ret 12 */
static VP_STATUS __stdcall vp_UnmapMemory(PVOID HwDev, PVOID VAddr, PVOID ProcessHandle)
{
    VxD_Debug_Printf("VP: UnmapMemory\r\n");
    return NO_ERROR;
}

static VP_STATUS __stdcall vp_DisableInterrupt(PVOID HwDev) { return NO_ERROR; }
static VP_STATUS __stdcall vp_EnableInterrupt(PVOID HwDev) { return NO_ERROR; }

/* VideoPortInt10: 2 params (HwDevExt, BiosArguments) = ret 8
   Executes a real-mode INT 10h BIOS call for VESA mode switching.
   On Win98, we can use the VMM Exec_VxD_Int service. */
static VP_STATUS __stdcall vp_Int10(PVOID HwDeviceExtension, PVOID BiosArguments)
{
    /* VIDEO_X86_BIOS_ARGUMENTS: EAX, EBX, ECX, EDX, ESI, EDI, EBP at offsets 0-24 */
    PULONG args = (PULONG)BiosArguments;
    vp_log_hex("VP: Int10 EAX=", args[0], "");
    vp_log_hex(" EBX=", args[1], "\r\n");
    /* TODO: V86 mode INT 10h via VMM Exec_VxD_Int or Begin_Nest_V86_Exec */
    VxD_Debug_Printf("VP: Int10 NOT YET IMPLEMENTED\r\n");
    return ERROR_INVALID_PARAMETER;
}

/* VideoPortGetRomImage: 4 params = ret 16 */
static PVOID __stdcall vp_GetRomImage(PVOID HwDev, PVOID Unused1, ULONG Unused2, ULONG Length)
{
    vp_log_hex("VP: GetRomImage len=", Length, "\r\n");
    /* Video BIOS ROM at physical C0000h (standard VGA BIOS location) */
    return VxD_MapPhysToLinear(0xC0000, Length ? Length : 0x10000);
}

/* VideoPortGetCurrentIrql: 0 params = ret 0 (cdecl-like? Actually stdcall 0 args) */
static ULONG __stdcall vp_GetCurrentIrql(void)
{
    return 0; /* PASSIVE_LEVEL — we're always in VxD context */
}

/* VideoPortSynchronizeExecution: 4 params = ret 16 */
static BOOLEAN __stdcall vp_SynchronizeExecution(
    PVOID HwDev, ULONG Priority, PVOID SyncRoutine, PVOID Context)
{
    typedef BOOLEAN (__stdcall *SYNC_FUNC)(PVOID);
    SYNC_FUNC fn = (SYNC_FUNC)SyncRoutine;
    if (fn) return fn(Context);
    return FALSE;
}

/* VideoPortStartTimer / StopTimer: 1 param each = ret 4 */
static void __stdcall vp_StartTimer(PVOID HwDev)
{
    VxD_Debug_Printf("VP: StartTimer (stub)\r\n");
}
static void __stdcall vp_StopTimer(PVOID HwDev)
{
    VxD_Debug_Printf("VP: StopTimer (stub)\r\n");
}

/* VideoPortQuerySystemTime: 1 param = ret 4 */
static void __stdcall vp_QuerySystemTime(PVOID CurrentTime)
{
    if (CurrentTime) *(ULONG *)CurrentTime = 0;
}

/* VideoPortGetVersion: 2 params = ret 8 */
static VP_STATUS __stdcall vp_GetVersion(PVOID HwDev, PVOID VersionInfo)
{
    /* Return "Windows XP" version to keep miniport happy */
    if (VersionInfo) {
        /* OSVERSIONINFOW: dwMajorVersion=5, dwMinorVersion=1 at offsets 4,8 */
        PULONG vi = (PULONG)VersionInfo;
        vi[0] = 276; /* dwOSVersionInfoSize */
        vi[1] = 5;   /* dwMajorVersion */
        vi[2] = 1;   /* dwMinorVersion */
        vi[3] = 2600;/* dwBuildNumber */
        vi[4] = 2;   /* dwPlatformId = VER_PLATFORM_WIN32_NT */
    }
    return NO_ERROR;
}

/* ================================================================
 * VideoPort Import Function Table
 * ================================================================ */

static const IMPORT_FUNC_ENTRY videoport_funcs[] = {
    { "VideoPortInitialize",            (PVOID)vp_Initialize },
    { "VideoPortGetDeviceBase",         (PVOID)vp_GetDeviceBase },
    { "VideoPortFreeDeviceBase",        (PVOID)vp_FreeDeviceBase },
    { "VideoPortGetBusData",            (PVOID)vp_GetBusData },
    { "VideoPortSetBusData",            (PVOID)vp_SetBusData },
    { "VideoPortReadPortUchar",         (PVOID)vp_ReadPortUchar },
    { "VideoPortReadPortUshort",        (PVOID)vp_ReadPortUshort },
    { "VideoPortReadPortUlong",         (PVOID)vp_ReadPortUlong },
    { "VideoPortWritePortUchar",        (PVOID)vp_WritePortUchar },
    { "VideoPortWritePortUshort",       (PVOID)vp_WritePortUshort },
    { "VideoPortWritePortUlong",        (PVOID)vp_WritePortUlong },
    { "VideoPortReadRegisterUchar",     (PVOID)vp_ReadRegisterUchar },
    { "VideoPortReadRegisterUshort",    (PVOID)vp_ReadRegisterUshort },
    { "VideoPortReadRegisterUlong",     (PVOID)vp_ReadRegisterUlong },
    { "VideoPortWriteRegisterUchar",    (PVOID)vp_WriteRegisterUchar },
    { "VideoPortWriteRegisterUshort",   (PVOID)vp_WriteRegisterUshort },
    { "VideoPortWriteRegisterUlong",    (PVOID)vp_WriteRegisterUlong },
    { "VideoPortZeroMemory",            (PVOID)vp_ZeroMemory },
    { "VideoPortMoveMemory",            (PVOID)vp_MoveMemory },
    { "VideoPortZeroDeviceMemory",      (PVOID)vp_ZeroDeviceMemory },
    { "VideoPortAllocatePool",          (PVOID)vp_AllocatePool },
    { "VideoPortFreePool",              (PVOID)vp_FreePool },
    { "VideoPortGetRegistryParameters", (PVOID)vp_GetRegistryParameters },
    { "VideoPortSetRegistryParameters", (PVOID)vp_SetRegistryParameters },
    { "VideoPortGetAccessRanges",       (PVOID)vp_GetAccessRanges },
    { "VideoPortVerifyAccessRanges",    (PVOID)vp_VerifyAccessRanges },
    { "VideoPortStallExecution",        (PVOID)vp_StallExecution },
    { "VideoPortLogError",              (PVOID)vp_LogError },
    { "VideoPortDebugPrint",            (PVOID)vp_DebugPrint },
    { "VideoPortScanRom",               (PVOID)vp_ScanRom },
    { "VideoPortGetDeviceData",         (PVOID)vp_GetDeviceData },
    { "VideoPortMapMemory",             (PVOID)vp_MapMemory },
    { "VideoPortUnmapMemory",           (PVOID)vp_UnmapMemory },
    { "VideoPortDisableInterrupt",      (PVOID)vp_DisableInterrupt },
    { "VideoPortEnableInterrupt",       (PVOID)vp_EnableInterrupt },
    { "VideoPortInt10",                 (PVOID)vp_Int10 },
    { "VideoPortGetRomImage",           (PVOID)vp_GetRomImage },
    { "VideoPortGetCurrentIrql",        (PVOID)vp_GetCurrentIrql },
    { "VideoPortSynchronizeExecution",  (PVOID)vp_SynchronizeExecution },
    { "VideoPortStartTimer",            (PVOID)vp_StartTimer },
    { "VideoPortStopTimer",             (PVOID)vp_StopTimer },
    { "VideoPortQuerySystemTime",       (PVOID)vp_QuerySystemTime },
    { "VideoPortGetVersion",            (PVOID)vp_GetVersion },
    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Registration
 * ================================================================ */

static PORT_DRIVER_SHIM videoport_shim = {
    "VIDEOPRT.SYS",
    videoport_funcs,
    0,
    NULL, NULL, NULL
};

void video_shim_register(void)
{
    VxD_Debug_Printf("VP: Registering VIDEOPRT.SYS shim\r\n");
    register_port_driver(&videoport_shim);
}

/* ================================================================
 * Test Entrypoint
 *
 * Built-in test exercises VideoPort shim without needing a real driver.
 * Tests: VideoPortInitialize, port I/O, PCI config, memory alloc,
 *        ROM access, version query.
 * ================================================================ */

#ifdef VIDEO_TEST_BUILTIN
#include "TESTVIDEO_EMBEDDED.H"
#endif

int video_test_miniport(void)
{
#ifdef VIDEO_TEST_BUILTIN
    video_shim_register();
    return test_video_miniport();
#else
    VxD_Debug_Printf("VP: No test driver embedded\r\n");
    return -99;
#endif
}
