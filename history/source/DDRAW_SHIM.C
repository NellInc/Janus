/*
 * DDRAW_SHIM.C - DirectDraw/Direct3D HAL Callback Provider for Win98 VxD
 *
 * Provides DirectDraw Hardware Abstraction Layer callbacks for display
 * drivers loaded via the VxD PE loader (PELOAD.C). Unlike port driver
 * shims (ScsiPort, NDIS) which resolve PE IAT imports, this module
 * publishes a DD_HALINFO structure that DirectDraw's runtime calls into.
 *
 * Architecture:
 *   - VideoPort shim calls ddraw_shim_init() after HwInitialize succeeds,
 *     passing framebuffer base, dimensions, and pitch
 *   - ddraw_shim_get_halinfo() returns the populated DD_HALINFO with
 *     callback tables pointing to our stub/software implementations
 *   - DirectDraw callbacks use __stdcall (single LPDDHAL_xxxDATA arg)
 *   - D3D callbacks are accessed via GetDriverInfo GUID queries
 *
 * DirectDraw HAL callback contract:
 *   - Each callback: DWORD __stdcall Cb(PVOID lpData)
 *   - Set lpData->ddRVal for the DD return code (DD_OK, DDERR_*)
 *   - Return DDHAL_DRIVER_HANDLED (1) if we processed it
 *   - Return DDHAL_DRIVER_NOTHANDLED (0) to let DDraw emulate
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VideoPort shim, VxD wrapper)
 */

/* ================================================================
 * Basic type definitions (match PELOAD.C / NTMINI_V5.C)
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

typedef ULONG               DWORD;
typedef LONG                HRESULT;
typedef ULONG               FLATPTR;  /* linear address as integer */

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

/* ================================================================
 * DirectDraw constants
 * ================================================================ */

#define DD_OK                       0x00000000UL
#define DDERR_UNSUPPORTED           0x80004001UL

#define DDHAL_DRIVER_HANDLED        1UL
#define DDHAL_DRIVER_NOTHANDLED     0UL

/* Capability flags (DDCORECAPS.dwCaps at offset 4) */
#define DDCAPS_BLT                  0x00000040UL
#define DDCAPS_BLTCOLORFILL         0x08000000UL
#define DDCAPS_COLORKEY             0x00000400UL

/* Surface callback flags */
#define DDHAL_SURFCB32_LOCK             0x00000004UL
#define DDHAL_SURFCB32_UNLOCK           0x00000008UL
#define DDHAL_SURFCB32_BLT              0x00000020UL
#define DDHAL_SURFCB32_FLIP             0x00000040UL
#define DDHAL_SURFCB32_DESTROYSURFACE   0x00000001UL
#define DDHAL_SURFCB32_GETBLTSTATUS     0x00000100UL
#define DDHAL_SURFCB32_GETFLIPSTATUS    0x00000200UL
#define DDHAL_SURFCB32_SETCOLORKEY      0x00000400UL
#define DDHAL_SURFCB32_UPDATEOVERLAY    0x00001000UL

/* DD callback flags */
#define DDHAL_CB32_CREATESURFACE        0x00000002UL
#define DDHAL_CB32_WAITFORVERTICALBLANK 0x00000004UL
#define DDHAL_CB32_GETSCANLINE          0x00000080UL

#define VIDMEM_ISLINEAR     0x00000001UL

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
extern void  VxD_Debug_Printf(const char *fmt, ...);

#define PAGESIZE    4096
#define PAGEFIXED   0x00000001

/* ================================================================
 * Local helpers
 * ================================================================ */

static void dd_memset(void *dst, int val, ULONG n) {
    UCHAR *d = (UCHAR *)dst;
    while (n--) *d++ = (UCHAR)val;
}

static void dd_memcpy(void *dst, const void *src, ULONG n) {
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    while (n--) *d++ = *s++;
}

static void dd_log_hex(const char *prefix, ULONG v, const char *suffix) {
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
 * Opaque structures: access by offset (same pattern as NDIS_SHIM)
 *
 * We define structures as byte arrays and access fields at known
 * DDK offsets. This avoids header dependencies and matches the
 * opaque-blob approach used throughout this project.
 * ================================================================ */

/* --- DDHAL callback data: field offsets within each struct --- */

/* DDHAL_CREATESURFACEDATA */
#define CSDATA_DWSCNT       0x0C    /* DWORD dwSCnt */
#define CSDATA_DDRVAL       0x10    /* HRESULT ddRVal */

/* DDHAL_LOCKDATA */
#define LOCKDATA_LPSURFDATA 0x18    /* PVOID lpSurfData (output) */
#define LOCKDATA_DDRVAL     0x1C    /* HRESULT ddRVal */

/* DDHAL_UNLOCKDATA */
#define UNLOCKDATA_DDRVAL   0x08    /* HRESULT ddRVal */

/* DDHAL_BLTDATA */
#define BLTDATA_RCDEST_L    0x08    /* DWORD rDest.left */
#define BLTDATA_RCDEST_T    0x0C    /* DWORD rDest.top */
#define BLTDATA_RCDEST_R    0x10    /* DWORD rDest.right */
#define BLTDATA_RCDEST_B    0x14    /* DWORD rDest.bottom */
#define BLTDATA_LPSRC       0x18    /* PVOID lpDDSrcSurface */
#define BLTDATA_RCSRC_L     0x1C    /* DWORD rSrc.left */
#define BLTDATA_RCSRC_T     0x20    /* DWORD rSrc.top */
#define BLTDATA_RCSRC_R     0x24    /* DWORD rSrc.right */
#define BLTDATA_RCSRC_B     0x28    /* DWORD rSrc.bottom */
#define BLTDATA_DWFLAGS     0x2C    /* DWORD dwFlags */
#define BLTDATA_FILLCOLOR   0x34    /* DWORD bltFX.dwFillColor */
#define BLTDATA_DDRVAL      0x38    /* HRESULT ddRVal */

/* DDHAL_FLIPDATA */
#define FLIPDATA_DDRVAL     0x10    /* HRESULT ddRVal */

/* DDHAL_GETBLTSTATUSDATA / GETFLIPSTATUSDATA */
#define STATUSDATA_DDRVAL   0x0C    /* HRESULT ddRVal */

/* DDHAL_WAITFORVERTICALBLANKDATA */
#define VBLANK_BISINVB      0x08    /* DWORD bIsInVB (output) */
#define VBLANK_DWSCANLINE   0x0C    /* DWORD dwScanLine (output) */
#define VBLANK_DDRVAL       0x10    /* HRESULT ddRVal */

/* DDHAL_GETSCANLINEDATA */
#define SCANLINE_DWSCANLINE 0x04    /* DWORD dwScanLine (output) */
#define SCANLINE_DDRVAL     0x08    /* HRESULT ddRVal */

/* DDHAL_SETCOLORKEYDATA */
#define COLORKEY_LO         0x0C    /* DWORD ckNew.low */
#define COLORKEY_HI         0x10    /* DWORD ckNew.high */
#define COLORKEY_DDRVAL     0x14    /* HRESULT ddRVal */

/* DDHAL_UPDATEOVERLAYDATA */
#define OVERLAY_DDRVAL      0x2C    /* HRESULT ddRVal */

/* DDHAL_GETDRIVERINFODATA */
#define GDI_GUID_DATA1      0x08    /* DWORD guidInfo.Data1 */
#define GDI_GUID_DATA2      0x0C    /* USHORT guidInfo.Data2 */
#define GDI_GUID_DATA3      0x0E    /* USHORT guidInfo.Data3 */
#define GDI_EXPECTEDSIZE    0x18    /* DWORD dwExpectedSize */
#define GDI_ACTUALSIZE      0x20    /* DWORD dwActualSize */
#define GDI_DDRVAL          0x24    /* HRESULT ddRVal */

/* Accessor macros */
#define DD_DWORD_AT(base, off) (*(DWORD *)((PUCHAR)(base) + (off)))
#define DD_PVOID_AT(base, off) (*(PVOID *)((PUCHAR)(base) + (off)))
#define DD_LONG_AT(base, off)  (*(LONG *)((PUCHAR)(base) + (off)))

/* --- DDCORECAPS: opaque blob, 0x180 bytes --- */
#define DDCORECAPS_SIZE     0x180
#define DCCAPS_DWSIZE       0x00
#define DCCAPS_DWCAPS       0x04
#define DCCAPS_VIDMEMTOTAL  0x3C
#define DCCAPS_VIDMEMFREE   0x40

/* --- VIDMEM heap entry: 16 bytes --- */
#define VIDMEM_ENTRY_SIZE   16
#define VIDMEM_DWFLAGS      0x00
#define VIDMEM_FPSTART      0x04
#define VIDMEM_FPEND        0x08

/* --- DD_HALINFO layout offsets ---
 * dwSize(4) + VIDMEMINFO(varies) + ddCaps(opaque) + GetDriverInfo(4) + dwFlags(4)
 * We build this as a flat buffer. */
#define HALINFO_TOTAL_SIZE  0x200

/* VIDMEMINFO within DD_HALINFO (starts at offset 4) */
#define VMI_FPPRIMARY       0x04
#define VMI_DWFLAGS         0x08
#define VMI_DISPLAYWIDTH    0x0C
#define VMI_DISPLAYHEIGHT   0x10
#define VMI_LDISPLAYPITCH   0x14
/* DDPIXELFORMAT inlined at offset 0x18; preserve field order if
   replacing with nested struct in future. */
#define VMI_PF_DWSIZE       0x18
#define VMI_PF_DWFLAGS      0x1C
#define VMI_PF_DWFOURCC     0x20
#define VMI_PF_BITCOUNT     0x24
#define VMI_PF_RMASK        0x28
#define VMI_PF_GMASK        0x2C
#define VMI_PF_BMASK        0x30
#define VMI_PF_AMASK        0x34
#define VMI_NUMHEAPS        0x50
#define VMI_PVMLIST         0x54

/* DDCORECAPS blob starts after VIDMEMINFO */
#define HALINFO_CAPS_OFF    0x58
/* GetDriverInfo pointer after caps blob */
#define HALINFO_GDI_OFF     (HALINFO_CAPS_OFF + DDCORECAPS_SIZE)

/* ================================================================
 * State
 * ================================================================ */

static struct {
    FLATPTR     fbBase;
    ULONG       fbSize;
    ULONG       width;
    ULONG       height;
    ULONG       bpp;
    ULONG       pitch;
    ULONG       heapStart;
    BOOLEAN     initialized;
} g_dd;

static UCHAR g_halinfo_buf[HALINFO_TOTAL_SIZE];
static UCHAR g_vidmem_entry[VIDMEM_ENTRY_SIZE];

/* Callback tables as flat arrays (dwSize + dwFlags + function pointers) */
#define DDCB_SIZE       40  /* dwSize(4)+dwFlags(4)+8 ptrs(32) */
#define SURFCB_SIZE     52  /* dwSize(4)+dwFlags(4)+11 ptrs(44) */
#define PALCB_SIZE      12  /* dwSize(4)+dwFlags(4)+1 ptr(4) */

static UCHAR g_dd_cb[DDCB_SIZE];
static UCHAR g_surf_cb[SURFCB_SIZE];
static UCHAR g_pal_cb[PALCB_SIZE];

/* Callback table offsets: function pointers start at offset 8 */
#define CB_DWSIZE   0x00
#define CB_DWFLAGS  0x04
#define CB_FUNC0    0x08

/* DD callbacks: func indices (in pointer slots from CB_FUNC0) */
#define DDCB_DESTROYDRIVER      0
#define DDCB_CREATESURFACE      1
#define DDCB_SETCOLORKEY        2
#define DDCB_SETMODE            3
#define DDCB_WAITVBLANK         4
#define DDCB_CANCREATESURFACE   5
#define DDCB_CREATEPALETTE      6
#define DDCB_GETSCANLINE        7

/* Surface callbacks: func indices */
#define SCBI_DESTROYSURFACE     0
#define SCBI_FLIP               1
#define SCBI_SETCLIPLIST        2
#define SCBI_LOCK               3
#define SCBI_UNLOCK             4
#define SCBI_BLT                5
#define SCBI_SETCOLORKEY        6
#define SCBI_ADDATTACHED        7
#define SCBI_GETBLTSTATUS       8
#define SCBI_GETFLIPSTATUS      9
#define SCBI_UPDATEOVERLAY      10

/* ================================================================
 * DirectDraw HAL Callbacks
 * ================================================================ */

static DWORD __stdcall DDHal_CreateSurface(PVOID lpData)
{
    dd_log_hex("DDRAW: CreateSurface cnt=",
               DD_DWORD_AT(lpData, CSDATA_DWSCNT), "\r\n");
    DD_DWORD_AT(lpData, CSDATA_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;  /* let DDraw manage heap */
}

static DWORD __stdcall DDHal_WaitForVerticalBlank(PVOID lpData)
{
    DD_DWORD_AT(lpData, VBLANK_BISINVB) = 0;
    DD_DWORD_AT(lpData, VBLANK_DWSCANLINE) = 0;
    DD_DWORD_AT(lpData, VBLANK_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDHal_GetScanLine(PVOID lpData)
{
    DD_DWORD_AT(lpData, SCANLINE_DWSCANLINE) = 0;
    DD_DWORD_AT(lpData, SCANLINE_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDSurf_DestroySurface(PVOID lpData)
{
    VxD_Debug_Printf("DDRAW: DestroySurface\r\n");
    DD_DWORD_AT(lpData, 0x08) = DD_OK;  /* ddRVal at offset 8 */
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDSurf_Lock(PVOID lpData)
{
    DD_PVOID_AT(lpData, LOCKDATA_LPSURFDATA) = (PVOID)g_dd.fbBase;
    DD_DWORD_AT(lpData, LOCKDATA_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDSurf_Unlock(PVOID lpData)
{
    DD_DWORD_AT(lpData, UNLOCKDATA_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDSurf_Blt(PVOID lpData)
{
    ULONG dstX, dstY, w, h, row, bytesPerPixel;
    PVOID srcSurf;

    bytesPerPixel = g_dd.bpp >> 3;
    if (bytesPerPixel == 0) bytesPerPixel = 1;

    dstX = DD_DWORD_AT(lpData, BLTDATA_RCDEST_L);
    dstY = DD_DWORD_AT(lpData, BLTDATA_RCDEST_T);
    w = DD_DWORD_AT(lpData, BLTDATA_RCDEST_R) - dstX;
    h = DD_DWORD_AT(lpData, BLTDATA_RCDEST_B) - dstY;
    srcSurf = DD_PVOID_AT(lpData, BLTDATA_LPSRC);

    if (srcSurf == NULL) {
        /* Color fill */
        ULONG fillVal = DD_DWORD_AT(lpData, BLTDATA_FILLCOLOR);
        for (row = 0; row < h; row++) {
            PUCHAR dst = (PUCHAR)(g_dd.fbBase +
                         (dstY + row) * g_dd.pitch + dstX * bytesPerPixel);
            ULONG x;
            for (x = 0; x < w; x++) {
                if (bytesPerPixel == 1) dst[x] = (UCHAR)fillVal;
                else if (bytesPerPixel == 2) ((PUSHORT)dst)[x] = (USHORT)fillVal;
                else ((PULONG)dst)[x] = fillVal;
            }
        }
    } else {
        /* Source copy (software memcpy fallback) */
        ULONG srcX = DD_DWORD_AT(lpData, BLTDATA_RCSRC_L);
        ULONG srcY = DD_DWORD_AT(lpData, BLTDATA_RCSRC_T);
        for (row = 0; row < h; row++) {
            PUCHAR dstRow = (PUCHAR)(g_dd.fbBase +
                            (dstY + row) * g_dd.pitch + dstX * bytesPerPixel);
            PUCHAR srcRow = (PUCHAR)(g_dd.fbBase +
                            (srcY + row) * g_dd.pitch + srcX * bytesPerPixel);
            dd_memcpy(dstRow, srcRow, w * bytesPerPixel);
        }
    }

    DD_DWORD_AT(lpData, BLTDATA_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDSurf_Flip(PVOID lpData)
{
    /* No page flip hardware; return NOTHANDLED so DDraw uses blit fallback */
    DD_DWORD_AT(lpData, FLIPDATA_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD __stdcall DDSurf_GetBltStatus(PVOID lpData)
{
    DD_DWORD_AT(lpData, STATUSDATA_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDSurf_GetFlipStatus(PVOID lpData)
{
    DD_DWORD_AT(lpData, STATUSDATA_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDSurf_SetColorKey(PVOID lpData)
{
    dd_log_hex("DDRAW: SetColorKey lo=",
               DD_DWORD_AT(lpData, COLORKEY_LO), "");
    dd_log_hex(" hi=", DD_DWORD_AT(lpData, COLORKEY_HI), "\r\n");
    DD_DWORD_AT(lpData, COLORKEY_DDRVAL) = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD __stdcall DDSurf_UpdateOverlay(PVOID lpData)
{
    VxD_Debug_Printf("DDRAW: UpdateOverlay (no HW overlay)\r\n");
    DD_DWORD_AT(lpData, OVERLAY_DDRVAL) = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_HANDLED;
}

/* ================================================================
 * Direct3D: GetDriverInfo (GUID-based extension query)
 *
 * D3D HAL is discovered via this callback. We log the requested GUID
 * and return NOTHANDLED, causing D3D to fall back to software (RGB).
 * ================================================================ */

static DWORD __stdcall DDHal_GetDriverInfo(PVOID lpData)
{
    dd_log_hex("DDRAW: GetDriverInfo GUID={",
               DD_DWORD_AT(lpData, GDI_GUID_DATA1), "-");
    dd_log_hex("", (ULONG)(*(USHORT *)((PUCHAR)lpData + GDI_GUID_DATA2)), "-");
    dd_log_hex("", (ULONG)(*(USHORT *)((PUCHAR)lpData + GDI_GUID_DATA3)), "}\r\n");

    DD_DWORD_AT(lpData, GDI_ACTUALSIZE) = 0;
    DD_DWORD_AT(lpData, GDI_DDRVAL) = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* ================================================================
 * Pixel format table
 * ================================================================ */

static void dd_set_pixfmt(ULONG bpp) {
    DWORD flags = 0x00000040UL; /* DDPF_RGB */
    DWORD r = 0, g = 0, b = 0;

    if (bpp == 16) {
        r = 0x0000F800UL; g = 0x000007E0UL; b = 0x0000001FUL;
    } else if (bpp == 32) {
        r = 0x00FF0000UL; g = 0x0000FF00UL; b = 0x000000FFUL;
    } else if (bpp == 8) {
        flags = 0x00000060UL;  /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
    }

    DD_DWORD_AT(g_halinfo_buf, VMI_PF_DWSIZE) = 32;
    DD_DWORD_AT(g_halinfo_buf, VMI_PF_DWFLAGS) = flags;
    DD_DWORD_AT(g_halinfo_buf, VMI_PF_BITCOUNT) = bpp;
    DD_DWORD_AT(g_halinfo_buf, VMI_PF_RMASK) = r;
    DD_DWORD_AT(g_halinfo_buf, VMI_PF_GMASK) = g;
    DD_DWORD_AT(g_halinfo_buf, VMI_PF_BMASK) = b;
}

/* ================================================================
 * Initialization and Public API
 * ================================================================ */

/*
 * ddraw_shim_init - Called by VideoPort after HwInitialize succeeds.
 *
 * @framebuffer: linear address of the mapped framebuffer
 * @width:       display width in pixels
 * @height:      display height in pixels
 * @bpp:         bits per pixel (8, 16, 24, 32)
 * @pitch:       bytes per scanline
 * @vram_size:   total video memory (0 = auto: pitch*height*2)
 */
void ddraw_shim_init(PVOID framebuffer, ULONG width, ULONG height,
                     ULONG bpp, ULONG pitch, ULONG vram_size)
{
    PUCHAR caps;

    VxD_Debug_Printf("DDRAW: Initializing shim\r\n");
    dd_log_hex("  fb=", (ULONG)framebuffer, "");
    dd_log_hex(" w=", width, "");
    dd_log_hex(" h=", height, "");
    dd_log_hex(" bpp=", bpp, "");
    dd_log_hex(" pitch=", pitch, "\r\n");

    g_dd.fbBase = (FLATPTR)framebuffer;
    g_dd.width = width;
    g_dd.height = height;
    g_dd.bpp = bpp;
    g_dd.pitch = pitch;
    g_dd.heapStart = pitch * height;
    if (vram_size == 0) vram_size = pitch * height * 2;
    g_dd.fbSize = vram_size;

    /* Video memory heap entry */
    dd_memset(g_vidmem_entry, 0, VIDMEM_ENTRY_SIZE);
    DD_DWORD_AT(g_vidmem_entry, VIDMEM_DWFLAGS) = VIDMEM_ISLINEAR;
    DD_DWORD_AT(g_vidmem_entry, VIDMEM_FPSTART) = g_dd.fbBase + g_dd.heapStart;
    DD_DWORD_AT(g_vidmem_entry, VIDMEM_FPEND) = g_dd.fbBase + vram_size - 1;

    /* Build DD_HALINFO blob */
    dd_memset(g_halinfo_buf, 0, HALINFO_TOTAL_SIZE);
    DD_DWORD_AT(g_halinfo_buf, 0x00) = HALINFO_TOTAL_SIZE;  /* dwSize */

    /* VIDMEMINFO */
    DD_DWORD_AT(g_halinfo_buf, VMI_FPPRIMARY) = g_dd.fbBase;
    DD_DWORD_AT(g_halinfo_buf, VMI_DISPLAYWIDTH) = width;
    DD_DWORD_AT(g_halinfo_buf, VMI_DISPLAYHEIGHT) = height;
    DD_LONG_AT(g_halinfo_buf, VMI_LDISPLAYPITCH) = (LONG)pitch;
    DD_DWORD_AT(g_halinfo_buf, VMI_NUMHEAPS) = 1;
    DD_PVOID_AT(g_halinfo_buf, VMI_PVMLIST) = (PVOID)g_vidmem_entry;
    dd_set_pixfmt(bpp);

    /* DDCORECAPS (opaque blob at HALINFO_CAPS_OFF) */
    caps = g_halinfo_buf + HALINFO_CAPS_OFF;
    DD_DWORD_AT(caps, DCCAPS_DWSIZE) = DDCORECAPS_SIZE;
    DD_DWORD_AT(caps, DCCAPS_DWCAPS) = DDCAPS_BLT | DDCAPS_BLTCOLORFILL;
    DD_DWORD_AT(caps, DCCAPS_VIDMEMTOTAL) = vram_size;
    DD_DWORD_AT(caps, DCCAPS_VIDMEMFREE) = vram_size - g_dd.heapStart;

    /* GetDriverInfo pointer */
    DD_PVOID_AT(g_halinfo_buf, HALINFO_GDI_OFF) = (PVOID)DDHal_GetDriverInfo;

    /* DD callback table */
    dd_memset(g_dd_cb, 0, DDCB_SIZE);
    DD_DWORD_AT(g_dd_cb, CB_DWSIZE) = DDCB_SIZE;
    DD_DWORD_AT(g_dd_cb, CB_DWFLAGS) = DDHAL_CB32_CREATESURFACE
                                      | DDHAL_CB32_WAITFORVERTICALBLANK
                                      | DDHAL_CB32_GETSCANLINE;
    DD_PVOID_AT(g_dd_cb, CB_FUNC0 + DDCB_CREATESURFACE * 4) = (PVOID)DDHal_CreateSurface;
    DD_PVOID_AT(g_dd_cb, CB_FUNC0 + DDCB_WAITVBLANK * 4) = (PVOID)DDHal_WaitForVerticalBlank;
    DD_PVOID_AT(g_dd_cb, CB_FUNC0 + DDCB_GETSCANLINE * 4) = (PVOID)DDHal_GetScanLine;

    /* Surface callback table */
    dd_memset(g_surf_cb, 0, SURFCB_SIZE);
    DD_DWORD_AT(g_surf_cb, CB_DWSIZE) = SURFCB_SIZE;
    DD_DWORD_AT(g_surf_cb, CB_DWFLAGS) = DDHAL_SURFCB32_DESTROYSURFACE
                                        | DDHAL_SURFCB32_LOCK
                                        | DDHAL_SURFCB32_UNLOCK
                                        | DDHAL_SURFCB32_BLT
                                        | DDHAL_SURFCB32_FLIP
                                        | DDHAL_SURFCB32_GETBLTSTATUS
                                        | DDHAL_SURFCB32_GETFLIPSTATUS
                                        | DDHAL_SURFCB32_SETCOLORKEY
                                        | DDHAL_SURFCB32_UPDATEOVERLAY;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_DESTROYSURFACE * 4) = (PVOID)DDSurf_DestroySurface;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_FLIP * 4) = (PVOID)DDSurf_Flip;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_LOCK * 4) = (PVOID)DDSurf_Lock;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_UNLOCK * 4) = (PVOID)DDSurf_Unlock;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_BLT * 4) = (PVOID)DDSurf_Blt;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_GETBLTSTATUS * 4) = (PVOID)DDSurf_GetBltStatus;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_GETFLIPSTATUS * 4) = (PVOID)DDSurf_GetFlipStatus;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_SETCOLORKEY * 4) = (PVOID)DDSurf_SetColorKey;
    DD_PVOID_AT(g_surf_cb, CB_FUNC0 + SCBI_UPDATEOVERLAY * 4) = (PVOID)DDSurf_UpdateOverlay;

    /* Palette callback table (no HW palette accel) */
    dd_memset(g_pal_cb, 0, PALCB_SIZE);
    DD_DWORD_AT(g_pal_cb, CB_DWSIZE) = PALCB_SIZE;

    g_dd.initialized = TRUE;
    VxD_Debug_Printf("DDRAW: Shim initialized\r\n");
}

/* Return pointer to the DD_HALINFO blob. NULL if not yet initialized. */
PVOID ddraw_shim_get_halinfo(void)
{
    if (!g_dd.initialized) return NULL;
    return (PVOID)g_halinfo_buf;
}

/* Return pointer to the DD callback table */
PVOID ddraw_shim_get_dd_callbacks(void)
{
    return (PVOID)g_dd_cb;
}

/* Return pointer to the surface callback table */
PVOID ddraw_shim_get_surface_callbacks(void)
{
    return (PVOID)g_surf_cb;
}

/* Return pointer to the palette callback table */
PVOID ddraw_shim_get_palette_callbacks(void)
{
    return (PVOID)g_pal_cb;
}

/*
 * ddraw_shim_register - Symmetry with other shim modules.
 *
 * Unlike port driver shims (ScsiPort, NDIS) this does NOT register
 * with the PE loader's IAT resolver. It prepares internal state only.
 * VideoPort calls ddraw_shim_init() later with framebuffer parameters.
 */
void ddraw_shim_register(void)
{
    VxD_Debug_Printf("DDRAW: Shim registered (callback provider)\r\n");
    dd_memset(&g_dd, 0, sizeof(g_dd));
}
