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

/* V86 INT 10h execution via VMM nested V86 services.
 * Performs Begin_Nest_V86_Exec -> set client regs -> Exec_Int 0x10 ->
 * read client regs -> End_Nest_Exec in a single ASM function (EBP is
 * repurposed by VMM to point at Client_Reg_Struc, so no C frames
 * can exist between Begin and End).
 * Returns 0 on success, non-zero on failure. */
extern ULONG VxD_Exec_V86_Int10(PVOID BiosArguments);

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

    /* Call HwFindAdapter if present */
    if (g_video_miniport.HwFindAdapter) {
        typedef ULONG (__stdcall *PFN_HW_FIND_ADAPTER)(
            PVOID HwDeviceExtension,
            PVOID HwContext,
            PVOID BusInformation,
            PVOID ConfigInfo,
            PVOID Again);

        PFN_HW_FIND_ADAPTER pfnFind = (PFN_HW_FIND_ADAPTER)g_video_miniport.HwFindAdapter;
        ULONG findResult;
        ULONG again = 0;
        PVOID configInfo;

        /* VIDEO_PORT_CONFIG_INFO (0x70 = 112 bytes on i386):
         *   +0x00 Length (ULONG) — must be >= 0x70
         *   +0x04 SystemIoBusNumber
         *   +0x08 AdapterInterfaceType (Isa=1, PCIBus=5)
         *   +0x0C BusInterruptLevel
         *   +0x10 BusInterruptVector
         *   +0x14 InterruptMode
         *   +0x18 NumEmulatorAccessEntries
         *   +0x1C EmulatorAccessEntries (ptr)
         *   +0x20 EmulatorAccessEntriesContext
         *   +0x28 VdmPhysicalVideoMemoryAddress (8 bytes, aligned)
         *   +0x30 VdmPhysicalVideoMemoryLength
         *   +0x34 HardwareStateSize
         *   +0x60 VideoPortGetProcAddress (ptr)
         *   +0x64 DriverRegistryPath (ptr)
         *   +0x68 SystemMemorySize (8 bytes)
         */
        configInfo = VxD_PageAllocate(1, PAGEFIXED);
        if (configInfo) {
            PULONG ci = (PULONG)configInfo;
            vp_memset(configInfo, 0, PAGESIZE);
            ci[0] = 0x70;  /* Length = sizeof(VIDEO_PORT_CONFIG_INFO) */
            ci[1] = 0;     /* SystemIoBusNumber = 0 */
            ci[2] = g_video_miniport.AdapterInterfaceType;
            /* VGA BIOS video memory at physical A0000h, 128K */
            ci[10] = 0x000A0000;  /* VdmPhysicalVideoMemoryAddress low (+0x28) */
            ci[11] = 0;           /* VdmPhysicalVideoMemoryAddress high */
            ci[12] = 0x00020000;  /* VdmPhysicalVideoMemoryLength = 128K (+0x30) */
            ci[13] = 0x1000;      /* HardwareStateSize (+0x34) */

            VxD_Debug_Printf("VP: Calling HwFindAdapter...\r\n");
            findResult = pfnFind(g_video_miniport.DeviceExtension,
                                 HwContext, NULL, configInfo, &again);
            vp_log_hex("VP: HwFindAdapter returned ", findResult, "\r\n");

            if (findResult == 0 && g_video_miniport.HwInitialize) {
                typedef BOOLEAN (__stdcall *PFN_HW_INIT)(PVOID HwDeviceExtension);
                PFN_HW_INIT pfnInit = (PFN_HW_INIT)g_video_miniport.HwInitialize;
                BOOLEAN initOk;

                VxD_Debug_Printf("VP: Calling HwInitialize...\r\n");
                initOk = pfnInit(g_video_miniport.DeviceExtension);
                vp_log_hex("VP: HwInitialize returned ", (ULONG)initOk, "\r\n");
            }

            VxD_PageFree(configInfo);
        }
    }

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
        PVOID mapped;
        /* Legacy VGA/ISA memory (below 1MB) is identity-mapped in ring 0.
         * _MapPhysToLinear fails for RAM regions on Win98. */
        if (IoAddress_Low < 0x100000) {
            vp_log_hex("  -> identity (legacy) ", IoAddress_Low, "\r\n");
            return (PVOID)IoAddress_Low;
        }
        mapped = VxD_MapPhysToLinear(IoAddress_Low, NumberOfUchars);
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
/*
 * VIDEO_ACCESS_RANGE structure (24 bytes each):
 *   +0x00: PHYSICAL_ADDRESS RangeStart (8 bytes)
 *   +0x08: ULONG RangeLength
 *   +0x0C: UCHAR RangeInMemory (0=I/O, 1=memory)
 *   +0x0D: UCHAR RangeVisible
 *   +0x0E: UCHAR RangeShareable
 *   +0x0F: UCHAR RangePassive
 *   +0x10: (pad to 24 bytes)
 */
#define VIDEO_ACCESS_RANGE_SIZE 24

static VP_STATUS __stdcall vp_GetAccessRanges(
    PVOID HwDeviceExtension,
    ULONG NumRequestedResources,
    PVOID RequestedResources,
    ULONG NumAccessRanges,
    PVOID AccessRanges,
    PVOID VendorId,
    PVOID DeviceId,
    PULONG Slot)
{
    PUCHAR ar = (PUCHAR)AccessRanges;
    ULONG bus, dev, func, bar, barVal, barSize, addr;
    ULONG arIdx = 0;

    vp_log_hex("VP: GetAccessRanges numAR=", NumAccessRanges, "\r\n");

    if (!ar || NumAccessRanges == 0) return NO_ERROR;
    vp_memset(ar, 0, NumAccessRanges * VIDEO_ACCESS_RANGE_SIZE);

    /* Scan PCI bus 0 for VGA-class device (class 0x03) */
    for (dev = 0; dev < 32 && arIdx < NumAccessRanges; dev++) {
        addr = 0x80000000UL | (dev << 11);
        _port_outd(0xCF8, addr);
        barVal = _port_ind(0xCFC);
        if (barVal == 0xFFFFFFFF || barVal == 0) continue;

        /* Read class code at offset 0x08 */
        _port_outd(0xCF8, addr | 0x08);
        barVal = _port_ind(0xCFC);
        if (((barVal >> 24) & 0xFF) != 0x03) continue; /* Not display class */

        vp_log_hex("VP: Found VGA at PCI dev=", dev, "\r\n");
        if (Slot) *Slot = dev;

        /* Read BARs 0-5 (offsets 0x10-0x24) */
        for (bar = 0; bar < 6 && arIdx < NumAccessRanges; bar++) {
            ULONG barOff = 0x10 + bar * 4;
            PUCHAR entry = ar + arIdx * VIDEO_ACCESS_RANGE_SIZE;

            _port_outd(0xCF8, addr | barOff);
            barVal = _port_ind(0xCFC);
            if (barVal == 0) continue;

            /* Determine BAR size: write all 1s, read back, restore */
            _port_outd(0xCF8, addr | barOff);
            _port_outd(0xCFC, 0xFFFFFFFF);
            _port_outd(0xCF8, addr | barOff);
            barSize = _port_ind(0xCFC);
            _port_outd(0xCF8, addr | barOff);
            _port_outd(0xCFC, barVal); /* Restore */

            if (barVal & 1) {
                /* I/O BAR */
                ULONG ioBase = barVal & 0xFFFFFFFC;
                ULONG ioSize = (~(barSize & 0xFFFFFFFC)) + 1;
                *(PULONG)(entry + 0) = ioBase;
                *(PULONG)(entry + 4) = 0;
                *(PULONG)(entry + 8) = ioSize & 0xFFFF;
                entry[0x0C] = 0; /* I/O space */
                vp_log_hex("  BAR", bar, "");
                vp_log_hex(" IO=", ioBase, "");
                vp_log_hex(" sz=", ioSize, "\r\n");
            } else {
                /* Memory BAR */
                ULONG memBase = barVal & 0xFFFFFFF0;
                ULONG memSize = (~(barSize & 0xFFFFFFF0)) + 1;
                *(PULONG)(entry + 0) = memBase;
                *(PULONG)(entry + 4) = 0;
                *(PULONG)(entry + 8) = memSize;
                entry[0x0C] = 1; /* Memory space */
                vp_log_hex("  BAR", bar, "");
                vp_log_hex(" MEM=", memBase, "");
                vp_log_hex(" sz=", memSize, "\r\n");
                if ((barVal & 0x06) == 0x04) bar++; /* 64-bit BAR, skip next */
            }
            arIdx++;
        }
        break; /* Found the VGA device */
    }

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

/* VideoPortCompareMemory — memcmp equivalent, returns count of matching bytes */
static ULONG __stdcall vp_CompareMemory(PVOID Source1, PVOID Source2, ULONG Length)
{
    PUCHAR s1 = (PUCHAR)Source1;
    PUCHAR s2 = (PUCHAR)Source2;
    ULONG i;
    for (i = 0; i < Length; i++) {
        if (s1[i] != s2[i]) return i;
    }
    return Length;
}

/* VideoPortQueryServices — XP service table query (AGP, I2C, etc.) */
static NTSTATUS __stdcall vp_QueryServices(
    PVOID HwDeviceExtension,
    ULONG ServicesType,
    PVOID Interface)
{
    vp_log_hex("VP: QueryServices type=", ServicesType, " (stub)\r\n");
    return (NTSTATUS)0xC0000002L; /* STATUS_NOT_IMPLEMENTED */
}

/* VideoPortSetTrappedEmulatorPorts — VGA port trapping for V86 mode */
static NTSTATUS __stdcall vp_SetTrappedEmulatorPorts(
    PVOID HwDeviceExtension,
    ULONG NumAccessRanges,
    PVOID AccessRange)
{
    vp_log_hex("VP: SetTrappedEmulatorPorts n=", NumAccessRanges, " (stub)\r\n");
    return NO_ERROR;
}

/* VideoPortWritePortBufferUshort — burst 16-bit port write */
static void __stdcall vp_WritePortBufferUshort(PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    ULONG i;
    unsigned short port = (unsigned short)(ULONG)Port;
    for (i = 0; i < Count; i++) {
        _port_outw(port, Buffer[i]);
    }
}

/* VideoPortWriteRegisterBufferUchar — burst MMIO byte write */
static void __stdcall vp_WriteRegisterBufferUchar(PUCHAR Register, PUCHAR Buffer, ULONG Count)
{
    ULONG i;
    for (i = 0; i < Count; i++) {
        Register[i] = Buffer[i];
    }
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
   On Win98, uses VMM nested V86 execution:
     Begin_Nest_V86_Exec -> set Client_Reg_Struc -> Exec_Int 0x10 ->
     read Client_Reg_Struc -> End_Nest_Exec.
   VIDEO_X86_BIOS_ARGUMENTS layout:
     +0x00 Eax, +0x04 Ebx, +0x08 Ecx, +0x0C Edx,
     +0x10 Esi, +0x14 Edi, +0x18 Ebp
   NOTE: VMM nested V86 execution clobbers the ring-0 stack contents
   (caller's locals, return addresses) even though ESP is preserved.
   We use a static buffer for the BIOS arguments to avoid depending
   on stack-allocated data surviving the V86 call. The caller's
   stack frame is saved/restored in the ASM wrapper. */
static ULONG g_v86_bios_args[7]; /* static to survive stack clobber */

static VP_STATUS __stdcall vp_Int10(PVOID HwDeviceExtension, PVOID BiosArguments)
{
    PULONG args = (PULONG)BiosArguments;
    ULONG rc;

    if (!BiosArguments) return ERROR_INVALID_PARAMETER;

    vp_log_hex("VP: Int10 EAX=", args[0], "");
    vp_log_hex(" EBX=", args[1], "\r\n");

    /* Diagnostic: dump IVT entry for INT 10h.
       Real-mode IVT is at linear address 0x0000. Each entry is 4 bytes
       (segment:offset). INT 10h entry is at 0x10 * 4 = 0x40.
       In V86 mode, linear addresses below 1MB map to the V86 address space. */
    {
        volatile ULONG *ivt = (volatile ULONG *)0x00000000;
        ULONG ivt10 = ivt[0x10];
        USHORT seg = (USHORT)(ivt10 >> 16);
        USHORT off = (USHORT)(ivt10 & 0xFFFF);
        ULONG bios_linear = ((ULONG)seg << 4) + off;
        vp_log_hex("VP: IVT[10h]=", ivt10, "");
        vp_log_hex(" seg:off=", (ULONG)seg, ":");
        vp_log_hex("", (ULONG)off, "");
        vp_log_hex(" linear=", bios_linear, "\r\n");
        if (bios_linear >= 0xC0000 && bios_linear < 0x100000) {
            volatile UCHAR *rom = (volatile UCHAR *)bios_linear;
            vp_log_hex("VP: ROM[0]=", (ULONG)rom[0], "");
            vp_log_hex(" [1]=", (ULONG)rom[1], "");
            vp_log_hex(" [2]=", (ULONG)rom[2], "\r\n");
        } else if (bios_linear == 0) {
            VxD_Debug_Printf("VP: IVT[10h] is NULL! No BIOS handler.\r\n");
        } else {
            volatile UCHAR *handler = (volatile UCHAR *)bios_linear;
            vp_log_hex("VP: Handler[0]=", (ULONG)handler[0], "");
            vp_log_hex(" [1]=", (ULONG)handler[1], "");
            vp_log_hex(" [2]=", (ULONG)handler[2], "");
            vp_log_hex(" [3]=", (ULONG)handler[3], "\r\n");
        }
        /* Also check BIOS Data Area at 0x0449 (current video mode) */
        {
            volatile UCHAR *bda = (volatile UCHAR *)0x0449;
            vp_log_hex("VP: BDA[449h] video mode=", (ULONG)*bda, "\r\n");
        }
    }

    /* Copy to static buffer, call V86, copy back.
     * This is necessary because VMM V86 nesting clobbers the ring-0
     * stack, which may include the caller's stack-allocated BiosArgs. */
    vp_memcpy(g_v86_bios_args, args, 7 * sizeof(ULONG));
    rc = VxD_Exec_V86_Int10((PVOID)g_v86_bios_args);
    vp_memcpy(args, g_v86_bios_args, 7 * sizeof(ULONG));

    if (rc != 0) {
        VxD_Debug_Printf("VP: Int10 V86 exec FAILED\r\n");
        return ERROR_DEV_NOT_EXIST;
    }

    vp_log_hex("VP: Int10 returned EAX=", args[0], "");
    vp_log_hex(" EBX=", args[1], "\r\n");
    return NO_ERROR;
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

/* VideoPortGetVgaStatus: 2 params (HwDevExt, VgaStatus) = ret 8 */
static VP_STATUS __stdcall vp_GetVgaStatus(PVOID HwDev, PULONG VgaStatus)
{
    if (VgaStatus) *VgaStatus = 1; /* VGA is enabled */
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
    { "VideoPortCompareMemory",         (PVOID)vp_CompareMemory },
    { "VideoPortQueryServices",         (PVOID)vp_QueryServices },
    { "VideoPortSetTrappedEmulatorPorts",(PVOID)vp_SetTrappedEmulatorPorts },
    { "VideoPortWritePortBufferUshort", (PVOID)vp_WritePortBufferUshort },
    { "VideoPortWriteRegisterBufferUchar",(PVOID)vp_WriteRegisterBufferUchar },
    { "VideoPortGetVgaStatus",          (PVOID)vp_GetVgaStatus },
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

/* Minimal ntoskrnl.exe shim for vga.sys (4 imports) */
static void __stdcall ntos_KeBugCheckEx(ULONG Code, ULONG P1, ULONG P2, ULONG P3, ULONG P4) {
    vp_log_hex("VP: KeBugCheckEx code=", Code, "\r\n");
    for(;;) {} /* halt */
}
static ULONG g_ntos_KeTickCount = 0;
static void *ntos_memmove(void *dst, const void *src, ULONG n) {
    UCHAR *d = (UCHAR*)dst; const UCHAR *s = (const UCHAR*)src;
    if (d < s) { while (n--) *d++ = *s++; }
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
static ULONG ntos_except_handler3(void) { return 1; /* EXCEPTION_EXECUTE_HANDLER */ }

static const IMPORT_FUNC_ENTRY ntoskrnl_funcs[] = {
    { "KeBugCheckEx",       (PVOID)ntos_KeBugCheckEx },
    { "KeTickCount",        (PVOID)&g_ntos_KeTickCount },
    { "memmove",            (PVOID)ntos_memmove },
    { "_except_handler3",   (PVOID)ntos_except_handler3 },
    { NULL, NULL }
};

static PORT_DRIVER_SHIM ntoskrnl_shim = {
    "ntoskrnl.exe",
    ntoskrnl_funcs,
    0,
    NULL, NULL, NULL
};

static int g_video_shims_registered = 0;

void video_shim_register(void)
{
    if (g_video_shims_registered) return;
    g_video_shims_registered = 1;
    VxD_Debug_Printf("VP: Registering VIDEOPRT.SYS shim\r\n");
    register_port_driver(&videoport_shim);
    VxD_Debug_Printf("VP: Registering ntoskrnl.exe shim\r\n");
    register_port_driver(&ntoskrnl_shim);
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

#ifdef VGA_TEST_REAL
#include "VGA_EMBEDDED.H"
#endif

int video_test_miniport(void)
{
#ifdef VGA_TEST_REAL
    {
        PVOID entry_point = NULL;
        PVOID image_base = NULL;
        int rc;
        ULONG status;
        typedef ULONG (__stdcall *PFN_DRIVER_ENTRY)(PVOID, PVOID);
        PFN_DRIVER_ENTRY pfnDriverEntry;

        video_shim_register();

        /* Direct Int10 test 1: AH=0Fh (Get Current Video Mode).
         * Non-destructive. Returns AL=mode (0x03=text), AH=cols, BH=page. */
        {
            ULONG biosArgs[7];
            VP_STATUS ir;
            vp_memset(biosArgs, 0, sizeof(biosArgs));
            biosArgs[0] = 0x0F00; /* AH=0Fh */
            VxD_Debug_Printf("VP: === DIRECT INT10 TEST (AH=0Fh) ===\r\n");
            ir = vp_Int10(NULL, (PVOID)biosArgs);
            vp_log_hex("VP: rc=", ir, "");
            vp_log_hex(" EAX=", biosArgs[0], "");
            vp_log_hex(" EBX=", biosArgs[1], "\r\n");
            if (ir == NO_ERROR && biosArgs[0] != 0x0F00) {
                VxD_Debug_Printf("VP: BIOS modified regs -> V86 WORKS\r\n");
            } else if (ir == NO_ERROR) {
                VxD_Debug_Printf("VP: EAX unchanged, BIOS may not have executed\r\n");
            } else {
                VxD_Debug_Printf("VP: V86 exec failed\r\n");
            }
        }

        /* Direct Int10 test 2: AH=12h BL=10h (Get EGA Info).
         * Returns BL=memory, BH=mode, CL=switch, CH=feature bits. */
        {
            ULONG biosArgs[7];
            VP_STATUS ir;
            vp_memset(biosArgs, 0, sizeof(biosArgs));
            biosArgs[0] = 0x1200; /* AH=12h */
            biosArgs[1] = 0x0010; /* BL=10h */
            VxD_Debug_Printf("VP: === DIRECT INT10 TEST (AH=12h BL=10h) ===\r\n");
            ir = vp_Int10(NULL, (PVOID)biosArgs);
            vp_log_hex("VP: rc=", ir, "");
            vp_log_hex(" EAX=", biosArgs[0], "");
            vp_log_hex(" EBX=", biosArgs[1], "");
            vp_log_hex(" ECX=", biosArgs[2], "\r\n");
            if (ir == NO_ERROR && biosArgs[1] != 0x0010) {
                VxD_Debug_Printf("VP: *** V86 INT10 BIOS CONFIRMED ***\r\n");
            } else if (ir == NO_ERROR) {
                VxD_Debug_Printf("VP: EBX unchanged, BIOS likely NOT executing\r\n");
            }
        }

        VxD_Debug_Printf("VP: Loading vga.sys (real XP driver)...\r\n");
        rc = pe_load_image(vga_embedded_data, sizeof(vga_embedded_data),
                           NULL, &entry_point, &image_base);
        if (rc != 0) {
            vp_log_hex("VP: PE load failed rc=", (ULONG)rc, "\r\n");
            return rc;
        }
        vp_log_hex("VP: vga.sys loaded at ", (ULONG)image_base, "");
        vp_log_hex(" entry=", (ULONG)entry_point, "\r\n");

        pfnDriverEntry = (PFN_DRIVER_ENTRY)entry_point;
        VxD_Debug_Printf("VP: Calling DriverEntry...\r\n");
        status = pfnDriverEntry(NULL, NULL);
        vp_log_hex("VP: DriverEntry returned ", status, "\r\n");

        if (status != 0) {
            VxD_Debug_Printf("VP: DriverEntry FAILED\r\n");
            return -1;
        }
        VxD_Debug_Printf("VP: *** VGA.SYS INITIALIZED ***\r\n");
        return 0;
    }
#elif defined(VIDEO_TEST_BUILTIN)
    video_shim_register();
    return test_video_miniport();
#else
    VxD_Debug_Printf("VP: No test driver embedded\r\n");
    return -99;
#endif
}
