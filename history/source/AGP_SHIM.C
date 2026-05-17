/*
 * AGP_SHIM.C - AGPLIB.SYS Port Driver Shim for Win98 VxD PE Loader
 *
 * Provides an AGPLIB.SYS import resolution layer enabling XP AGP miniport
 * drivers (UAGP35.SYS for Intel, AGPGART.SYS for generic) to run on Win98
 * via the VxD PE loader (PELOAD.C).
 *
 * Architecture:
 *   - Registers "AGPLIB.SYS" port driver shim
 *   - Implements AgpInitializeTarget/Master (reads AGP cap from PCI config)
 *   - Implements AgpSetRate (writes AGP command register)
 *   - Stubs remaining exports (alloc, map, flush) for Phase 2
 *
 * Win98 bridge target: VGARTD.VXD (AGP GART management)
 *   - VGARTD_Get_Info: get AGP aperture base/size
 *   - VGARTD_Alloc_Range: allocate AGP memory range
 *   - VGARTD_Free_Range: free range
 *   (Not wired in Phase 1 — stubs only)
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
 * Debug helpers
 * ================================================================ */

static void agp_ulong_to_hex(ULONG val, char *buf) {
    static const char hx[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 7; i >= 0; i--) buf[2 + (7 - i)] = hx[(val >> (i * 4)) & 0xF];
    buf[10] = 0;
}

static void agp_log_hex(const char *prefix, ULONG v, const char *suffix) {
    char h[12];
    agp_ulong_to_hex(v, h);
    VxD_Debug_Printf(prefix);
    VxD_Debug_Printf(h);
    VxD_Debug_Printf(suffix);
}

/* ================================================================
 * AGP Capability Constants
 * ================================================================ */

/* PCI capability IDs */
#define PCI_CAP_ID_AGP          0x02

/* AGP capability register offsets (from cap base) */
#define AGP_CAP_ID              0x00  /* capability ID (byte) */
#define AGP_CAP_NEXT            0x01  /* next cap pointer (byte) */
#define AGP_CAP_VERSION         0x02  /* AGP version (byte: major.minor) */
#define AGP_CAP_STATUS          0x04  /* status register (dword) */
#define AGP_CAP_COMMAND         0x08  /* command register (dword) */

/* AGP rate bits (in status and command registers) */
#define AGP_RATE_1X             0x01
#define AGP_RATE_2X             0x02
#define AGP_RATE_4X             0x04
#define AGP_RATE_8X             0x08  /* AGP 3.0 only */

/* AGP command register bits */
#define AGP_CMD_AGP_ENABLE      0x00000100UL

/* ================================================================
 * AGP State
 * ================================================================ */

static struct {
    /* PCI location of host bridge (bus 0, dev 0, func 0 typically) */
    ULONG  PciBus;
    ULONG  PciDev;
    ULONG  PciFunc;
    /* AGP capability offset in PCI config space */
    ULONG  AgpCapOffset;
    BOOLEAN AgpCapFound;
    /* Parsed from AGP status register */
    ULONG  SupportedRates;  /* bitmask: AGP_RATE_1X | 2X | 4X | 8X */
    ULONG  CurrentRate;
    ULONG  AgpVersion;      /* major<<4 | minor */
    /* Aperture info (from VGARTD or PCI BAR) */
    ULONG  ApertureBase;
    ULONG  ApertureSize;
} g_agp_state;

/* ================================================================
 * PCI Config Space Access (x86 0xCF8/0xCFC mechanism 1)
 * ================================================================ */

static ULONG agp_pci_read32(ULONG bus, ULONG dev, ULONG func, ULONG reg) {
    ULONG addr = 0x80000000UL | (bus << 16) | (dev << 11) |
                 (func << 8) | (reg & 0xFC);
    _port_outd(0xCF8, addr);
    return _port_ind(0xCFC);
}

static void agp_pci_write32(ULONG bus, ULONG dev, ULONG func, ULONG reg, ULONG val) {
    ULONG addr = 0x80000000UL | (bus << 16) | (dev << 11) |
                 (func << 8) | (reg & 0xFC);
    _port_outd(0xCF8, addr);
    _port_outd(0xCFC, val);
}

static UCHAR agp_pci_read8(ULONG bus, ULONG dev, ULONG func, ULONG reg) {
    ULONG val = agp_pci_read32(bus, dev, func, reg & 0xFC);
    return (UCHAR)(val >> ((reg & 3) * 8));
}

/* ================================================================
 * AGP Capability Discovery
 *
 * Walks the PCI capabilities linked list starting at offset 0x34
 * looking for capability ID 0x02 (AGP).
 * ================================================================ */

static BOOLEAN agp_find_capability(ULONG bus, ULONG dev, ULONG func) {
    ULONG status;
    UCHAR cap_ptr, cap_id;
    int max_walk = 48;  /* prevent infinite loop on broken hardware */

    /* Check capabilities list bit in PCI status register */
    status = agp_pci_read32(bus, dev, func, 0x04);
    if (!((status >> 16) & 0x0010)) {
        VxD_Debug_Printf("AGP: No capabilities list in PCI status\r\n");
        return FALSE;
    }

    /* Read capabilities pointer at offset 0x34 */
    cap_ptr = agp_pci_read8(bus, dev, func, 0x34);
    cap_ptr &= 0xFC;  /* must be dword-aligned */

    while (cap_ptr != 0 && max_walk-- > 0) {
        cap_id = agp_pci_read8(bus, dev, func, cap_ptr);
        if (cap_id == PCI_CAP_ID_AGP) {
            g_agp_state.AgpCapOffset = (ULONG)cap_ptr;
            g_agp_state.AgpCapFound = TRUE;
            agp_log_hex("AGP: Found AGP capability at offset ", (ULONG)cap_ptr, "\r\n");
            return TRUE;
        }
        cap_ptr = agp_pci_read8(bus, dev, func, cap_ptr + 1);
        cap_ptr &= 0xFC;
    }

    VxD_Debug_Printf("AGP: AGP capability not found\r\n");
    return FALSE;
}

/* ================================================================
 * AGPLIB Export Implementations
 * ================================================================ */

/*
 * AgpInitializeTarget - 1 param (AgpExtension)
 * Called during miniport init to probe the AGP target (host bridge).
 * We scan bus 0 dev 0 func 0 for the AGP capability.
 */
static ULONG __stdcall agp_InitializeTarget(PVOID AgpExtension) {
    ULONG status_reg;

    VxD_Debug_Printf("AGP: AgpInitializeTarget\r\n");

    /* Host bridge is typically at 0:0:0 */
    g_agp_state.PciBus  = 0;
    g_agp_state.PciDev  = 0;
    g_agp_state.PciFunc = 0;

    if (!agp_find_capability(0, 0, 0)) {
        return STATUS_UNSUCCESSFUL;
    }

    /* Read AGP version */
    g_agp_state.AgpVersion = (ULONG)agp_pci_read8(
        0, 0, 0, g_agp_state.AgpCapOffset + AGP_CAP_VERSION);
    agp_log_hex("AGP: Version ", g_agp_state.AgpVersion, "\r\n");

    /* Read AGP status for supported rates */
    status_reg = agp_pci_read32(0, 0, 0,
        g_agp_state.AgpCapOffset + AGP_CAP_STATUS);
    g_agp_state.SupportedRates = status_reg & 0x0F;
    agp_log_hex("AGP: Supported rates mask ", g_agp_state.SupportedRates, "\r\n");

    return STATUS_SUCCESS;
}

/*
 * AgpInitializeMaster - 2 params (AgpExtension, AgpCapabilities*)
 * Called after target init to read the master (graphics card) capabilities.
 * AgpCapabilities is an output struct; we fill it with supported rates.
 */
static ULONG __stdcall agp_InitializeMaster(PVOID AgpExtension, PVOID AgpCapabilities) {
    VxD_Debug_Printf("AGP: AgpInitializeMaster\r\n");

    if (AgpCapabilities) {
        /* AgpCapabilities struct: first ULONG is supported rate mask */
        *(PULONG)AgpCapabilities = g_agp_state.SupportedRates;
    }

    return STATUS_SUCCESS;
}

/*
 * AgpSetRate - 2 params (AgpExtension, AgpRate)
 * Sets the AGP transfer rate by writing the AGP command register.
 * Rate values: 1=1x, 2=2x, 4=4x, 8=8x
 */
static ULONG __stdcall agp_SetRate(PVOID AgpExtension, ULONG AgpRate) {
    ULONG cmd_reg;

    agp_log_hex("AGP: AgpSetRate rate=", AgpRate, "\r\n");

    if (!g_agp_state.AgpCapFound) {
        VxD_Debug_Printf("AGP: ERROR - no AGP capability found\r\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* Verify requested rate is supported */
    if (!(AgpRate & g_agp_state.SupportedRates)) {
        agp_log_hex("AGP: ERROR - rate not supported, supported=",
                    g_agp_state.SupportedRates, "\r\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* Read current command register, set rate bits and enable */
    cmd_reg = agp_pci_read32(g_agp_state.PciBus, g_agp_state.PciDev,
                             g_agp_state.PciFunc,
                             g_agp_state.AgpCapOffset + AGP_CAP_COMMAND);
    cmd_reg &= ~0x0FUL;            /* clear rate bits */
    cmd_reg |= (AgpRate & 0x0F);   /* set new rate */
    cmd_reg |= AGP_CMD_AGP_ENABLE; /* ensure AGP is enabled */

    agp_pci_write32(g_agp_state.PciBus, g_agp_state.PciDev,
                    g_agp_state.PciFunc,
                    g_agp_state.AgpCapOffset + AGP_CAP_COMMAND, cmd_reg);

    g_agp_state.CurrentRate = AgpRate;
    agp_log_hex("AGP: Rate set, cmd_reg=", cmd_reg, "\r\n");

    return STATUS_SUCCESS;
}

/*
 * AgpAllocateMemory - 4 params (AgpExtension, Pages, Type, MemoryContext*)
 * Stub: would allocate GART pages via VGARTD_Alloc_Range.
 */
static ULONG __stdcall agp_AllocateMemory(PVOID AgpExtension, ULONG Pages,
                                           ULONG Type, PVOID MemoryContext) {
    agp_log_hex("AGP: AgpAllocateMemory pages=", Pages, " [STUB]\r\n");
    return STATUS_UNSUCCESSFUL;
}

/*
 * AgpFreeMemory - 2 params (AgpExtension, MemoryContext)
 */
static ULONG __stdcall agp_FreeMemory(PVOID AgpExtension, PVOID MemoryContext) {
    VxD_Debug_Printf("AGP: AgpFreeMemory [STUB]\r\n");
    return STATUS_SUCCESS;
}

/*
 * AgpMapMemory - 4 params (AgpExtension, MemoryContext, Offset, Range*)
 * Stub: would map allocated pages into AGP aperture.
 */
static ULONG __stdcall agp_MapMemory(PVOID AgpExtension, PVOID MemoryContext,
                                      ULONG Offset, PVOID Range) {
    agp_log_hex("AGP: AgpMapMemory offset=", Offset, " [STUB]\r\n");
    return STATUS_UNSUCCESSFUL;
}

/*
 * AgpUnMapMemory - 4 params (AgpExtension, MemoryContext, Offset, Range)
 */
static ULONG __stdcall agp_UnMapMemory(PVOID AgpExtension, PVOID MemoryContext,
                                        ULONG Offset, ULONG Range) {
    VxD_Debug_Printf("AGP: AgpUnMapMemory [STUB]\r\n");
    return STATUS_SUCCESS;
}

/*
 * AgpFlushPages - 2 params (AgpExtension, MemoryContext)
 * Stub: would flush GART TLB entries.
 */
static ULONG __stdcall agp_FlushPages(PVOID AgpExtension, PVOID MemoryContext) {
    VxD_Debug_Printf("AGP: AgpFlushPages [STUB]\r\n");
    return STATUS_SUCCESS;
}

/*
 * AgpSetAperture - 3 params (AgpExtension, Base, Size)
 * Stub: would configure AGP aperture base and size.
 */
static ULONG __stdcall agp_SetAperture(PVOID AgpExtension, ULONG Base, ULONG Size) {
    agp_log_hex("AGP: AgpSetAperture base=", Base, "");
    agp_log_hex(" size=", Size, " [STUB]\r\n");
    g_agp_state.ApertureBase = Base;
    g_agp_state.ApertureSize = Size;
    return STATUS_SUCCESS;
}

/*
 * AgpFlushChipsetCaches - 1 param (AgpExtension)
 * Stub: would issue WBINVD or chipset-specific flush.
 */
static ULONG __stdcall agp_FlushChipsetCaches(PVOID AgpExtension) {
    VxD_Debug_Printf("AGP: AgpFlushChipsetCaches [STUB]\r\n");
    return STATUS_SUCCESS;
}

/* ================================================================
 * Import Function Table (AGPLIB.SYS exports)
 * ================================================================ */

static const IMPORT_FUNC_ENTRY agplib_funcs[] = {
    { "AgpInitializeTarget",    (PVOID)agp_InitializeTarget },
    { "AgpInitializeMaster",    (PVOID)agp_InitializeMaster },
    { "AgpSetRate",             (PVOID)agp_SetRate },
    { "AgpAllocateMemory",      (PVOID)agp_AllocateMemory },
    { "AgpFreeMemory",          (PVOID)agp_FreeMemory },
    { "AgpMapMemory",           (PVOID)agp_MapMemory },
    { "AgpUnMapMemory",         (PVOID)agp_UnMapMemory },
    { "AgpFlushPages",          (PVOID)agp_FlushPages },
    { "AgpSetAperture",         (PVOID)agp_SetAperture },
    { "AgpFlushChipsetCaches",  (PVOID)agp_FlushChipsetCaches },
    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Structure
 * ================================================================ */

static PORT_DRIVER_SHIM agplib_shim = {
    "AGPLIB.SYS",       /* dll_name */
    agplib_funcs,        /* func_table */
    0,                   /* func_count (informational, uses null terminator) */
    NULL,                /* bridge_init */
    NULL,                /* bridge_io */
    NULL                 /* bridge_cleanup */
};

/* ================================================================
 * Public Registration
 * ================================================================ */

void agp_shim_register(void)
{
    VxD_Debug_Printf("AGP: Registering AGPLIB.SYS shim\r\n");
    register_port_driver(&agplib_shim);
    VxD_Debug_Printf("AGP: Shim registered OK\r\n");
}
