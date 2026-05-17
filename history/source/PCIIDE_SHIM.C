/*
 * PCIIDE_SHIM.C - PCIIDEX.SYS Port Driver Shim for Win98 VxD PE Loader
 *
 * Provides a PCIIDEX.SYS import resolution layer enabling XP PCI IDE
 * miniport drivers (INTELIDE.SYS, VIAIDE.SYS, CMDIDE.SYS) to run on
 * Win98 via the VxD PE loader (PELOAD.C).
 *
 * Architecture:
 *   - Registers "PCIIDEX.SYS" port driver shim
 *   - PciIdeXInitialize: stores miniport's GetControllerProperties callback,
 *     allocates extension, calls it to fill IDE_CONTROLLER_PROPERTIES
 *   - PciIdeXGetBusData/SetBusData: PCI config space read/write
 *   - Queries miniport for UDMA capabilities and logs supported modes
 *
 * Win98 bridge target: ESDI_506.PDR (Win98 IDE port driver)
 *   - Already supports BusMasterIDE for DMA
 *   - Our job: let XP miniports configure chipset timing registers
 *   (Not wired in Phase 1 — we query and log only)
 *
 * The critical miniport callback is PciIdeTransferModeSelect — it reports
 * what DMA modes the controller supports (UDMA33/66/100/133). Without this
 * info, Win98 falls back to PIO mode (slow).
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VxD wrapper)
 *
 * Calling convention: all functions __stdcall (NT kernel convention).
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

/* ================================================================
 * Inline port I/O via pragma aux (ring 0, no libc)
 * ================================================================ */

#ifdef __WATCOMC__
unsigned long  _port_ind(unsigned short port);
#pragma aux _port_ind = "in eax, dx" parm [dx] value [eax];
void _port_outd(unsigned short port, unsigned long val);
#pragma aux _port_outd = "out dx, eax" parm [dx] [eax];
#endif

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
 * Debug helpers
 * ================================================================ */

static void pciide_memset(void *dst, int val, ULONG n) {
    UCHAR *d = (UCHAR *)dst;
    while (n--) *d++ = (UCHAR)val;
}

static void pciide_ulong_to_hex(ULONG val, char *buf) {
    static const char hx[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 7; i >= 0; i--) buf[2 + (7 - i)] = hx[(val >> (i * 4)) & 0xF];
    buf[10] = 0;
}

static void pciide_log_hex(const char *prefix, ULONG v, const char *suffix) {
    char h[12];
    pciide_ulong_to_hex(v, h);
    VxD_Debug_Printf(prefix);
    VxD_Debug_Printf(h);
    VxD_Debug_Printf(suffix);
}

/* ================================================================
 * IDE_CONTROLLER_PROPERTIES Structure
 *
 * This is the wire-format struct that the miniport's
 * GetControllerProperties callback fills in. Offsets are fixed
 * and must match the XP DDK layout exactly.
 *
 * Layout (verified against XP DDK idex.h):
 *   +0x00: ULONG Size
 *   +0x04: ULONG ExtensionSize
 *   +0x08: PciIdeChannelEnabled function ptr
 *   +0x0C: PciIdeSyncAccessRequired function ptr
 *   +0x10: PciIdeTransferModeSelect function ptr (CRITICAL)
 *   +0x14: BOOLEAN IgnoreActiveBitForAtaDevice
 *   +0x15: pad[3]  (alignment to 0x18)
 *   +0x18: BOOLEAN AlwaysClearBusMasterInterrupt
 *   +0x19: pad[3]  (alignment to 0x1C)
 *   +0x1C: PciIdeUseDma function ptr
 *   +0x20: ULONG AlignmentRequirement
 *   +0x24: ULONG DefaultPIO
 *   +0x28: PciIdeUdmaModesSupported function ptr
 *   Total: 0x2C (44 bytes)
 * ================================================================ */

/* Function pointer typedefs for miniport callbacks.
 * Minimal-viable signatures — enough to call and interpret results.
 * Real XP DDK signatures are more complex but these suffice for
 * querying capabilities. */

/* PciIdeChannelEnabled(DevExt, Channel) -> IDE_CHANNEL_STATE enum */
typedef ULONG (__stdcall *PFN_CHANNEL_ENABLED)(PVOID, ULONG);

/* PciIdeSyncAccessRequired(DevExt) -> BOOLEAN */
typedef BOOLEAN (__stdcall *PFN_SYNC_ACCESS)(PVOID);

/* PciIdeTransferModeSelect(DevExt, TransferModeTimingTable, NumberOfDevices)
 * Returns NTSTATUS. The timing table is complex; we pass a zeroed buffer. */
typedef ULONG (__stdcall *PFN_TRANSFER_MODE_SELECT)(PVOID, PVOID, ULONG);

/* PciIdeUseDma(DevExt, CdbCommand, TargetSlave) -> BOOLEAN */
typedef BOOLEAN (__stdcall *PFN_USE_DMA)(PVOID, PVOID, ULONG);

/* PciIdeUdmaModesSupported(DevExt, UdmaModesBitmask*) -> NTSTATUS */
typedef ULONG (__stdcall *PFN_UDMA_MODES_SUPPORTED)(PVOID, PULONG);

/* The GetControllerProperties callback itself:
 * GetControllerProperties(DeviceExtension, ControllerProperties) -> NTSTATUS */
typedef ULONG (__stdcall *PFN_GET_CONTROLLER_PROPERTIES)(PVOID, PVOID);

typedef struct _IDE_CONTROLLER_PROPERTIES {
    ULONG                       Size;                           /* 0x00 */
    ULONG                       ExtensionSize;                  /* 0x04 */
    PFN_CHANNEL_ENABLED         PciIdeChannelEnabled;           /* 0x08 */
    PFN_SYNC_ACCESS             PciIdeSyncAccessRequired;       /* 0x0C */
    PFN_TRANSFER_MODE_SELECT    PciIdeTransferModeSelect;       /* 0x10 */
    BOOLEAN                     IgnoreActiveBitForAtaDevice;    /* 0x14 */
    UCHAR                       _pad1[3];                       /* 0x15-0x17 */
    BOOLEAN                     AlwaysClearBusMasterInterrupt;  /* 0x18 */
    UCHAR                       _pad2[3];                       /* 0x19-0x1B */
    PFN_USE_DMA                 PciIdeUseDma;                   /* 0x1C */
    ULONG                       AlignmentRequirement;           /* 0x20 */
    ULONG                       DefaultPIO;                     /* 0x24 */
    PFN_UDMA_MODES_SUPPORTED    PciIdeUdmaModesSupported;       /* 0x28 */
} IDE_CONTROLLER_PROPERTIES;

/* Compile-time layout verification */
typedef char ASSERT_IDE_PROPS_SIZE_IS_0x2C[
    (sizeof(IDE_CONTROLLER_PROPERTIES) == 0x2C) ? 1 : -1];
typedef char ASSERT_IDE_PROPS_CHANNEL_AT_0x08[
    ((ULONG)&((IDE_CONTROLLER_PROPERTIES*)0)->PciIdeChannelEnabled == 0x08) ? 1 : -1];
typedef char ASSERT_IDE_PROPS_XFERMODE_AT_0x10[
    ((ULONG)&((IDE_CONTROLLER_PROPERTIES*)0)->PciIdeTransferModeSelect == 0x10) ? 1 : -1];
typedef char ASSERT_IDE_PROPS_USEDMA_AT_0x1C[
    ((ULONG)&((IDE_CONTROLLER_PROPERTIES*)0)->PciIdeUseDma == 0x1C) ? 1 : -1];
typedef char ASSERT_IDE_PROPS_UDMA_AT_0x28[
    ((ULONG)&((IDE_CONTROLLER_PROPERTIES*)0)->PciIdeUdmaModesSupported == 0x28) ? 1 : -1];

/* ================================================================
 * PCI IDE Shim State
 * ================================================================ */

/* Fake DRIVER_OBJECT and REGISTRY_PATH for PciIdeXInitialize */
static UCHAR g_fake_driver_object[128];
static UCHAR g_fake_registry_path[64];  /* UNICODE_STRING: Length=0, Buf=NULL */

static struct {
    /* Miniport's GetControllerProperties callback */
    PFN_GET_CONTROLLER_PROPERTIES GetControllerProperties;
    /* Extension size requested by miniport */
    ULONG  ExtensionSize;
    /* Allocated device extension for miniport */
    PVOID  DeviceExtension;
    /* Filled by miniport */
    IDE_CONTROLLER_PROPERTIES ControllerProperties;
    /* PCI location (for Get/SetBusData) */
    ULONG  PciBus;
    ULONG  PciDev;
    ULONG  PciFunc;
    /* Initialization state */
    BOOLEAN Initialized;
} g_pciide_state;

/* ================================================================
 * PCI Config Space Access (x86 0xCF8/0xCFC mechanism 1)
 * ================================================================ */

static ULONG pciide_pci_read32(ULONG bus, ULONG dev, ULONG func, ULONG reg) {
    ULONG addr = 0x80000000UL | (bus << 16) | (dev << 11) |
                 (func << 8) | (reg & 0xFC);
    _port_outd(0xCF8, addr);
    return _port_ind(0xCFC);
}

static void pciide_pci_write32(ULONG bus, ULONG dev, ULONG func, ULONG reg, ULONG val) {
    ULONG addr = 0x80000000UL | (bus << 16) | (dev << 11) |
                 (func << 8) | (reg & 0xFC);
    _port_outd(0xCF8, addr);
    _port_outd(0xCFC, val);
}

/* ================================================================
 * PCIIDEX Export Implementations
 * ================================================================ */

/*
 * PciIdeXInitialize - 4 params (DriverObject, RegistryPath,
 *                               GetControllerProperties, ExtSize)
 *
 * Called by the miniport's DriverEntry. We:
 *   1. Store the GetControllerProperties callback
 *   2. Allocate a device extension of ExtSize bytes
 *   3. Call GetControllerProperties to fill IDE_CONTROLLER_PROPERTIES
 *   4. Log what we got
 */
static ULONG __stdcall pciide_XInitialize(
    PVOID DriverObject,
    PVOID RegistryPath,
    PFN_GET_CONTROLLER_PROPERTIES GetControllerProperties,
    ULONG ExtSize)
{
    ULONG status;

    VxD_Debug_Printf("PCIIDE: PciIdeXInitialize\r\n");
    pciide_log_hex("  GetControllerProperties=", (ULONG)GetControllerProperties, "\r\n");
    pciide_log_hex("  ExtSize=", ExtSize, "\r\n");

    if (!GetControllerProperties) {
        VxD_Debug_Printf("PCIIDE: ERROR - NULL GetControllerProperties\r\n");
        return STATUS_UNSUCCESSFUL;
    }

    g_pciide_state.GetControllerProperties = GetControllerProperties;
    g_pciide_state.ExtensionSize = ExtSize;

    /* Allocate device extension */
    if (ExtSize > 0) {
        ULONG pages = (ExtSize + PAGESIZE - 1) / PAGESIZE;
        g_pciide_state.DeviceExtension = VxD_PageAllocate(pages, PAGEFIXED);
        if (!g_pciide_state.DeviceExtension) {
            VxD_Debug_Printf("PCIIDE: ERROR - cannot allocate extension\r\n");
            return STATUS_UNSUCCESSFUL;
        }
        pciide_memset(g_pciide_state.DeviceExtension, 0, ExtSize);
        pciide_log_hex("  DeviceExtension=", (ULONG)g_pciide_state.DeviceExtension, "\r\n");
    } else {
        g_pciide_state.DeviceExtension = NULL;
    }

    /* Initialize controller properties struct */
    pciide_memset(&g_pciide_state.ControllerProperties, 0,
                  sizeof(IDE_CONTROLLER_PROPERTIES));
    g_pciide_state.ControllerProperties.Size = sizeof(IDE_CONTROLLER_PROPERTIES);

    /* Call miniport to fill in properties */
    VxD_Debug_Printf("PCIIDE: Calling GetControllerProperties...\r\n");
    status = GetControllerProperties(g_pciide_state.DeviceExtension,
                                     &g_pciide_state.ControllerProperties);
    pciide_log_hex("  GetControllerProperties returned ", status, "\r\n");

    if (status != STATUS_SUCCESS) {
        VxD_Debug_Printf("PCIIDE: WARNING - GetControllerProperties failed\r\n");
        return status;
    }

    /* Log what the miniport gave us */
    pciide_log_hex("  Props.Size=", g_pciide_state.ControllerProperties.Size, "\r\n");
    pciide_log_hex("  Props.ExtensionSize=",
                   g_pciide_state.ControllerProperties.ExtensionSize, "\r\n");
    pciide_log_hex("  Props.ChannelEnabled=",
                   (ULONG)g_pciide_state.ControllerProperties.PciIdeChannelEnabled, "\r\n");
    pciide_log_hex("  Props.TransferModeSelect=",
                   (ULONG)g_pciide_state.ControllerProperties.PciIdeTransferModeSelect, "\r\n");
    pciide_log_hex("  Props.UdmaModesSupported=",
                   (ULONG)g_pciide_state.ControllerProperties.PciIdeUdmaModesSupported, "\r\n");
    pciide_log_hex("  Props.DefaultPIO=",
                   g_pciide_state.ControllerProperties.DefaultPIO, "\r\n");
    VxD_Debug_Printf(g_pciide_state.ControllerProperties.IgnoreActiveBitForAtaDevice
                     ? "  IgnoreActiveBit=TRUE\r\n" : "  IgnoreActiveBit=FALSE\r\n");
    VxD_Debug_Printf(g_pciide_state.ControllerProperties.AlwaysClearBusMasterInterrupt
                     ? "  AlwaysClearBMInt=TRUE\r\n" : "  AlwaysClearBMInt=FALSE\r\n");

    g_pciide_state.Initialized = TRUE;
    VxD_Debug_Printf("PCIIDE: Initialization complete\r\n");

    return STATUS_SUCCESS;
}

/*
 * PciIdeXGetBusData - 4 params (DeviceExtension, Buffer, ConfigOffset, Length)
 * Reads PCI configuration space. Same mechanism as other shims.
 */
static ULONG __stdcall pciide_XGetBusData(
    PVOID DeviceExtension,
    PVOID Buffer,
    ULONG ConfigOffset,
    ULONG Length)
{
    ULONG i;
    UCHAR *buf = (UCHAR *)Buffer;

    pciide_log_hex("PCIIDE: GetBusData off=", ConfigOffset, "");
    pciide_log_hex(" len=", Length, "\r\n");

    if (!Buffer || Length == 0) return 0;

    for (i = 0; i < Length; i += 4) {
        ULONG regOff = (ConfigOffset + i) & 0xFC;
        ULONG val = pciide_pci_read32(g_pciide_state.PciBus,
                                       g_pciide_state.PciDev,
                                       g_pciide_state.PciFunc, regOff);
        ULONG byte_off = (ConfigOffset + i) & 3;
        ULONG j;
        for (j = 0; j < 4 && (i + j) < Length; j++) {
            buf[i + j] = (UCHAR)(val >> ((byte_off + j) * 8));
        }
    }
    return Length;
}

/*
 * PciIdeXSetBusData - 4 params (DeviceExtension, Buffer, ConfigOffset, Length)
 * Writes PCI configuration space.
 */
static ULONG __stdcall pciide_XSetBusData(
    PVOID DeviceExtension,
    PVOID Buffer,
    ULONG ConfigOffset,
    ULONG Length)
{
    ULONG i;
    UCHAR *buf = (UCHAR *)Buffer;

    pciide_log_hex("PCIIDE: SetBusData off=", ConfigOffset, "");
    pciide_log_hex(" len=", Length, "\r\n");

    if (!Buffer || Length == 0) return 0;

    for (i = 0; i < Length; i += 4) {
        ULONG regOff = (ConfigOffset + i) & 0xFC;
        ULONG val = 0;
        ULONG j;
        for (j = 0; j < 4 && (i + j) < Length; j++) {
            val |= ((ULONG)buf[i + j]) << (j * 8);
        }
        pciide_pci_write32(g_pciide_state.PciBus,
                           g_pciide_state.PciDev,
                           g_pciide_state.PciFunc, regOff, val);
    }
    return Length;
}

/* ================================================================
 * Import Function Table (PCIIDEX.SYS exports)
 * ================================================================ */

static const IMPORT_FUNC_ENTRY pciidex_funcs[] = {
    { "PciIdeXInitialize",  (PVOID)pciide_XInitialize },
    { "PciIdeXGetBusData",  (PVOID)pciide_XGetBusData },
    { "PciIdeXSetBusData",  (PVOID)pciide_XSetBusData },
    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Structure
 * ================================================================ */

static PORT_DRIVER_SHIM pciidex_shim = {
    "PCIIDEX.SYS",      /* dll_name */
    pciidex_funcs,       /* func_table */
    0,                   /* func_count (informational, uses null terminator) */
    NULL,                /* bridge_init */
    NULL,                /* bridge_io */
    NULL                 /* bridge_cleanup */
};

/* ================================================================
 * Public Registration
 * ================================================================ */

void pciide_shim_register(void)
{
    VxD_Debug_Printf("PCIIDE: Registering PCIIDEX.SYS shim\r\n");

    /* Initialize state */
    pciide_memset(&g_pciide_state, 0, sizeof(g_pciide_state));
    pciide_memset(g_fake_driver_object, 0, sizeof(g_fake_driver_object));
    pciide_memset(g_fake_registry_path, 0, sizeof(g_fake_registry_path));

    /* Default PCI location: bus 0, dev 31, func 1 (Intel ICH IDE) */
    g_pciide_state.PciBus  = 0;
    g_pciide_state.PciDev  = 31;
    g_pciide_state.PciFunc = 1;

    register_port_driver(&pciidex_shim);
    VxD_Debug_Printf("PCIIDE: Shim registered OK\r\n");
}

/* ================================================================
 * Test Entrypoint
 *
 * Exercises: call PciIdeXInitialize with fake DriverObject, then
 * query the miniport for UDMA capabilities via its callback.
 * ================================================================ */

void pciide_test(void)
{
    ULONG udma_modes = 0;
    ULONG channel_state;

    VxD_Debug_Printf("PCIIDE: === TEST BEGIN ===\r\n");

    if (!g_pciide_state.Initialized) {
        VxD_Debug_Printf("PCIIDE: Not initialized, nothing to test\r\n");
        VxD_Debug_Printf("PCIIDE: === TEST END ===\r\n");
        return;
    }

    /* Query UDMA modes if the miniport provides the callback */
    if (g_pciide_state.ControllerProperties.PciIdeUdmaModesSupported) {
        ULONG status;
        VxD_Debug_Printf("PCIIDE: Querying UDMA modes...\r\n");
        status = g_pciide_state.ControllerProperties.PciIdeUdmaModesSupported(
            g_pciide_state.DeviceExtension, &udma_modes);
        pciide_log_hex("  UdmaModesSupported returned status=", status, "\r\n");
        pciide_log_hex("  UDMA bitmask=", udma_modes, "\r\n");

        /* Decode UDMA modes */
        if (udma_modes & 0x01) VxD_Debug_Printf("  UDMA0 (16 MB/s)\r\n");
        if (udma_modes & 0x02) VxD_Debug_Printf("  UDMA1 (25 MB/s)\r\n");
        if (udma_modes & 0x04) VxD_Debug_Printf("  UDMA2 / UDMA33 (33 MB/s)\r\n");
        if (udma_modes & 0x08) VxD_Debug_Printf("  UDMA3 (44 MB/s)\r\n");
        if (udma_modes & 0x10) VxD_Debug_Printf("  UDMA4 / UDMA66 (66 MB/s)\r\n");
        if (udma_modes & 0x20) VxD_Debug_Printf("  UDMA5 / UDMA100 (100 MB/s)\r\n");
        if (udma_modes & 0x40) VxD_Debug_Printf("  UDMA6 / UDMA133 (133 MB/s)\r\n");
    } else {
        VxD_Debug_Printf("PCIIDE: No PciIdeUdmaModesSupported callback\r\n");
    }

    /* Query channel enabled state */
    if (g_pciide_state.ControllerProperties.PciIdeChannelEnabled) {
        VxD_Debug_Printf("PCIIDE: Querying channel states...\r\n");

        channel_state = g_pciide_state.ControllerProperties.PciIdeChannelEnabled(
            g_pciide_state.DeviceExtension, 0);
        pciide_log_hex("  Channel 0 (primary) state=", channel_state, "\r\n");

        channel_state = g_pciide_state.ControllerProperties.PciIdeChannelEnabled(
            g_pciide_state.DeviceExtension, 1);
        pciide_log_hex("  Channel 1 (secondary) state=", channel_state, "\r\n");
    } else {
        VxD_Debug_Printf("PCIIDE: No PciIdeChannelEnabled callback\r\n");
    }

    VxD_Debug_Printf("PCIIDE: === TEST END ===\r\n");
}
