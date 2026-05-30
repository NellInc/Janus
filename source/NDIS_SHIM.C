/*
 * NDIS_SHIM.C - NDIS Miniport Driver Shim for Win98 VxD PE Loader
 *
 * Provides an NDIS.SYS + HAL.dll import resolution layer enabling
 * NT4 NDIS miniport .sys files (e.g. ne2000.sys) to run on Win98
 * via the VxD PE loader (PELOAD.C).
 *
 * Architecture:
 *   - Registers two port driver shims: "NDIS.SYS" and "HAL.dll"
 *   - Provides a fake NDIS_MINIPORT_BLOCK with function pointers at
 *     hardcoded offsets (matching NT4 NDIS layout)
 *   - Stores miniport characteristics from NdisMRegisterMiniport
 *   - Provides packet pool, buffer pool, and memory allocation
 *   - Test entrypoint exercises: DriverEntry -> NdisMRegisterMiniport
 *     -> MiniportInitialize -> MiniportQueryInformation
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VxD wrapper)
 *
 * Calling convention: all functions called by the miniport use __stdcall
 * (NT kernel convention). The miniport's IAT was compiled by MSVC
 * assuming __stdcall for all NDIS exports.
 *
 * Known limitations (Phase 1):
 *   - No actual packet send/receive (just log and complete)
 *   - No Win98 NDIS 4.0 bridging (bypasses ndis.vxd entirely)
 *   - No interrupt handling (polling mode only, like ScsiPort)
 *   - No TCP/IP stack integration
 *   - Missing imports (NdisInitializeTimer, NdisSetTimer, etc.)
 *     will surface as PE_ERR_IMPORT_FAIL at pe_load_image time;
 *     add stubs as needed based on logged missing-function names
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
typedef ULONG              *PULONG_PTR;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

/* Define to bypass Safe_HwFindAdapter and call MiniportInitialize directly.
   Diagnostic: isolates whether crash is IDT manipulation or stack corruption. */
/* #define NDIS_DIRECT_CALL_TEST */

/* ================================================================
 * Inline port I/O via pragma aux (ring 0, no libc)
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
void _port_rep_insb(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_insb = "rep insb" parm [dx] [edi] [ecx] modify [edi ecx];
void _port_rep_insw(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_insw = "rep insw" parm [dx] [edi] [ecx] modify [edi ecx];
void _port_rep_outsb(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_outsb = "rep outsb" parm [dx] [esi] [ecx] modify [esi ecx];
void _port_rep_outsw(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_outsw = "rep outsw" parm [dx] [esi] [ecx] modify [esi ecx];
void _port_rep_insd(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_insd = "rep insd" parm [dx] [edi] [ecx] modify [edi ecx];
void _port_rep_outsd(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_outsd = "rep outsd" parm [dx] [esi] [ecx] modify [esi ecx];
void _port_stall(void);
#pragma aux _port_stall = "in al, 0x80" modify [al];
/* invlpg [eax] = 0F 01 38, wbinvd = 0F 09 — 486+ only but we run on 486+ always */
void _flush_tlb_page(void *addr);
#pragma aux _flush_tlb_page = 0x0F 0x01 0x38 parm [eax];
void _wbinvd(void);
#pragma aux _wbinvd = 0x0F 0x09;
unsigned long _get_cr3(void);
#pragma aux _get_cr3 = 0x0F 0x20 0xD8 value [eax]; /* mov eax, cr3 */
#endif

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
extern PVOID VxD_MapPhysToLinear(ULONG physAddr, ULONG nBytes);
extern ULONG VxD_GetPhysAddr(PVOID linearAddr);
extern void  VxD_Debug_Printf(const char *fmt, ...);

#define PAGESIZE    4096
#define PAGEFIXED   0x00000001

static void ndis_log_hex(const char *prefix, ULONG v, const char *suffix);

static ULONG ndis_va_to_pa(PVOID va)
{
    ULONG vaddr = (ULONG)va;
    ULONG pde_idx = vaddr >> 22;
    ULONG pte_idx = (vaddr >> 12) & 0x3FF;
    volatile ULONG *pde_ptr;
    volatile ULONG *pte_ptr;
    ULONG pde_val, pte_val;

    /* Win9x self-maps page tables at PDE index 0x3FE (not 0x3FF like NT).
       Page directory is at VA 0xFFBFE000.
       Page tables are at VA 0xFF800000 + (pageIndex * 4). */
    pde_ptr = (volatile ULONG *)(0xFFBFE000 + pde_idx * 4);
    pde_val = *pde_ptr;
    if (!(pde_val & 1)) {
        VxD_Debug_Printf("  va2pa: FAILED (PDE not present)\r\n");
        return 0;
    }

    pte_ptr = (volatile ULONG *)(0xFF800000 + (vaddr >> 12) * 4);
    pte_val = *pte_ptr;
    if (!(pte_val & 1)) {
        VxD_Debug_Printf("  va2pa: FAILED (PTE not present)\r\n");
        return 0;
    }

    return (pte_val & 0xFFFFF000) | (vaddr & 0xFFF);
}

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
extern int pe_load_image(const void *, unsigned long,
                         const IMPORT_FUNC_ENTRY *, void **, void **);

/* VPICD IRQ hookup for NDIS (ASM handlers in VXDWRAP_V4.ASM) */
extern ULONG VxD_Hook_NDIS_IRQ(ULONG irq_num);
extern ULONG VxD_Chain_NDIS_IRQ(ULONG irq_num);
extern ULONG g_ndis_isr_func;
extern ULONG g_ndis_dpc_func;
extern ULONG g_ndis_adapter_ctx;

/* ================================================================
 * Local helper functions (static to avoid link collisions)
 * ================================================================ */

static void ndis_memset(void *dst, int val, ULONG n) {
    UCHAR *d = (UCHAR *)dst;
    while (n--) *d++ = (UCHAR)val;
}

static void ndis_memcpy(void *dst, const void *src, ULONG n) {
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    while (n--) *d++ = *s++;
}

static int ndis_strcmp_nocase(const char *a, const char *b) {
    unsigned char ca, cb;
    for (;;) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += ('a' - 'A');
        if (cb >= 'A' && cb <= 'Z') cb += ('a' - 'A');
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0)  return 0;
    }
}

/* Debug helpers */
static void ndis_ulong_to_hex(ULONG val, char *buf) {
    static const char hx[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 7; i >= 0; i--) buf[2 + (7 - i)] = hx[(val >> (i * 4)) & 0xF];
    buf[10] = 0;
}

static void ndis_log_hex(const char *prefix, ULONG v, const char *suffix) {
    char h[12];
    ndis_ulong_to_hex(v, h);
    VxD_Debug_Printf(prefix);
    VxD_Debug_Printf(h);
    VxD_Debug_Printf(suffix);
}

/* ================================================================
 * NDIS Status Codes
 * ================================================================ */

typedef ULONG NDIS_STATUS;
typedef PVOID NDIS_HANDLE;

#define NDIS_STATUS_SUCCESS             0x00000000UL
#define NDIS_STATUS_PENDING             0x00000103UL
#define NDIS_STATUS_FAILURE             0xC0000001UL
#define NDIS_STATUS_RESOURCES           0xC000009AUL
#define NDIS_STATUS_NOT_SUPPORTED       0xC00000BBUL
#define NDIS_STATUS_ADAPTER_NOT_FOUND   0xC0010006UL
#define NDIS_STATUS_INVALID_LENGTH      0xC0010014UL
#define NDIS_STATUS_BUFFER_TOO_SHORT    0xC0010016UL

/* OID codes for testing */
#define OID_GEN_SUPPORTED_LIST          0x00010101UL
#define OID_GEN_HARDWARE_STATUS         0x00010102UL
#define OID_GEN_MEDIA_SUPPORTED         0x00010103UL
#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010EUL
#define OID_802_3_PERMANENT_ADDRESS     0x01010101UL
#define OID_802_3_CURRENT_ADDRESS       0x01010102UL

/* NdisMedium types */
#define NdisMedium802_3                 0

/* CM_RESOURCE types (used by NdisMPciAssignResources / NdisMQueryAdapterResources) */
#define CM_RESOURCE_PORT      1
#define CM_RESOURCE_INTERRUPT 2
#define CM_RESOURCE_MEMORY    3

/* ================================================================
 * NDIS_MINIPORT_BLOCK Layout (byte array with known offsets)
 *
 * Verified against Watcom DDK headers + ne2000.sys disassembly.
 * We use a byte array and access function pointers at fixed offsets.
 * Unused slots are filled with sentinel values (0xDEAD0000 | offset)
 * so accidental dereferences crash at diagnostic addresses.
 * These sentinels land in kernel space (>0xC0000000) and are almost
 * certainly unmapped, producing an immediate fault with the offset
 * encoded in the low 12 bits of the faulting address.
 * ================================================================ */

#define NDIS_MINIPORT_BLOCK_SIZE    0x400  /* NDIS 5.0 blocks are larger */

/* Offsets within the block */
#define NDIS_MB_ADAPTER_CONTEXT     0x0C
#define NDIS_MB_ETH_FILTER_DB       0xD8
#define NDIS_MB_PKT_INDICATE        0xE8
#define NDIS_MB_SEND_COMPLETE       0xEC
#define NDIS_MB_ETH_RX_INDICATE     0x164
#define NDIS_MB_ETH_RX_COMPLETE     0x170
#define NDIS_MB_STATUS_HANDLER      0x17C
#define NDIS_MB_STATUS_COMPLETE     0x180
#define NDIS_MB_TD_COMPLETE         0x184
#define NDIS_MB_QUERY_COMPLETE      0x188
#define NDIS_MB_SET_COMPLETE        0x18C

static UCHAR g_ndis_miniport_block[NDIS_MINIPORT_BLOCK_SIZE];
static UCHAR g_ndis_safe_page[4096];  /* zeroed page for safe pointer targets */

/* Fake NT kernel structures. NDIS 5.0 drivers may dereference these.
   Must be valid memory with zeroed fields so multi-level dereferences
   terminate safely (read 0, don't call 0). */
static UCHAR g_fake_device_object[256];
static UCHAR g_fake_driver_object[256];
static UCHAR g_fake_adapter_name[64]; /* UNICODE_STRING for adapter name */

/* Dummy Ethernet filter database handle (just needs to be non-NULL,
   passed as arg to EthRxIndicate/EthRxComplete handlers) */
static UCHAR g_ndis_eth_filter_db[64];

/* Accessor macros for the miniport block */
#define MB_PTR_AT(off) (*(PVOID *)(&g_ndis_miniport_block[(off)]))

/* ================================================================
 * Miniport Characteristics Storage
 *
 * NDIS 3.0 MINIPORT_CHARACTERISTICS layout (on the wire, as passed
 * to NdisMRegisterMiniport). Fields are in this exact order:
 *   UCHAR  MajorNdisVersion;      // offset 0
 *   UCHAR  MinorNdisVersion;      // offset 1
 *   USHORT Filler;                // offset 2 (padding)
 *   UINT   Reserved;              // offset 4 (reserved, must be 0)
 *   PVOID  CheckForHangHandler;   // offset 8
 *   PVOID  DisableInterruptHandler;   // offset 12
 *   PVOID  EnableInterruptHandler;    // offset 16
 *   PVOID  HaltHandler;               // offset 20
 *   PVOID  HandleInterruptHandler;    // offset 24
 *   PVOID  InitializeHandler;         // offset 28
 *   PVOID  ISRHandler;                // offset 32
 *   PVOID  QueryInformationHandler;   // offset 36
 *   PVOID  ReconfigureHandler;        // offset 40
 *   PVOID  ResetHandler;              // offset 44
 *   PVOID  SendHandler;               // offset 48
 *   PVOID  SetInformationHandler;     // offset 52
 *   PVOID  TransferDataHandler;       // offset 56
 *
 * NDIS 4.0 adds:
 *   PVOID  ReturnPacketHandler;       // offset 60
 *   PVOID  SendPacketsHandler;        // offset 64
 *   PVOID  AllocateCompleteHandler;   // offset 68
 *
 * Total: NDIS 3.0 = 60 bytes; NDIS 4.0 = 72 bytes.
 * ================================================================ */

/* Offsets into the raw MINIPORT_CHARACTERISTICS struct */
#define MC_MAJOR_VERSION    0
#define MC_MINOR_VERSION    1
#define MC_CHECK_HANG       8
#define MC_DISABLE_INT      12
#define MC_ENABLE_INT       16
#define MC_HALT             20
#define MC_HANDLE_INT       24
#define MC_INITIALIZE       28
#define MC_ISR              32
#define MC_QUERY_INFO       36
#define MC_RECONFIGURE      40
#define MC_RESET            44
#define MC_SEND             48
#define MC_SET_INFO         52
#define MC_TRANSFER_DATA    56
/* NDIS 4.0 extensions */
#define MC_RETURN_PACKET    60
#define MC_SEND_PACKETS     64
#define MC_ALLOC_COMPLETE   68

static struct {
    /* Stored callbacks (extracted from characteristics) */
    PVOID InitializeHandler;
    PVOID HaltHandler;
    PVOID QueryInformationHandler;
    PVOID SetInformationHandler;
    PVOID SendHandler;
    PVOID SendPacketsHandler;
    PVOID ReturnPacketHandler;
    PVOID CheckForHangHandler;
    PVOID ResetHandler;
    PVOID HandleInterruptHandler;
    PVOID ISRHandler;
    PVOID DisableInterruptHandler;
    PVOID EnableInterruptHandler;
    PVOID TransferDataHandler;
    PVOID ReconfigureHandler;
    PVOID AllocateCompleteHandler;
    /* Version info */
    UCHAR MajorVersion;
    UCHAR MinorVersion;
    /* Adapter context (set by NdisMSetAttributesEx) */
    PVOID AdapterContext;
    /* Wrapper handle (returned by NdisInitializeWrapper) */
    PVOID WrapperHandle;
    /* I/O port base (from NdisMRegisterIoPortRange) */
    USHORT IoPortBase;
    USHORT IoPortLength;
    PVOID  IoPortMapped;
    /* Interrupt */
    ULONG  InterruptVector;
    BOOLEAN InterruptRegistered;
    BOOLEAN VPICDHooked;
    /* PCI device location (from NdisMQueryAdapterResources / NdisMPciAssignResources scan) */
    ULONG  PciBus;
    ULONG  PciDev;
    ULONG  PciFunc;
    BOOLEAN PciFound;
    /* MMIO base (from NdisMMapIoSpace) */
    PVOID  MmioBase;
    /* DMA shared memory tracking (for PA readback diagnostics) */
    PVOID  LastSharedVA;
    ULONG  LastSharedPA;
    ULONG  LastSharedLen;
} g_ndis_miniport;

/* Forward declarations for test harness globals (defined in packet pool section) */
static volatile ULONG g_send_complete_flag = 0;
static volatile NDIS_STATUS g_send_complete_status = 0;
static UCHAR g_rx_capture_hdr[64];
static UCHAR g_rx_capture_data[1600];
static ULONG g_rx_capture_hdr_len = 0;
static ULONG g_rx_capture_data_len = 0;
static volatile ULONG g_rx_captured = 0;

/* ================================================================
 * Upcall Handlers (installed in MiniportBlock function pointer slots)
 *
 * These are called by the miniport through inline pointer dereference
 * (not through the IAT). They handle receive indication, status, etc.
 * ================================================================ */

static int g_ndis_log_count = 0;

/* EthRxIndicateHandler: miniport indicates a received Ethernet frame.
   Prototype matches NdisMEthIndicateReceive macro expansion (8 params):
   void (EthDB, MACContext, Address, HeaderBuffer, HeaderBufSize,
         LookaheadBuffer, LookaheadBufSize, PacketSize)
   Address is the 6-byte destination MAC (same pointer as HeaderBuffer). */
static void __stdcall ndis_EthRxIndicate(
    PVOID EthDB, PVOID MACContext,
    PUCHAR Address,
    PUCHAR HeaderBuffer, ULONG HeaderBufSize,
    PUCHAR LookaheadBuffer, ULONG LookaheadBufSize,
    ULONG PacketSize)
{
    (void)Address;  /* same pointer as HeaderBuffer; used by filter layer */
    ndis_log_hex("NDIS: EthRxIndicate hdrSz=", HeaderBufSize, "");
    ndis_log_hex(" pktSz=", PacketSize, "");
    ndis_log_hex(" laSize=", LookaheadBufSize, "\r\n");

    if (!g_rx_captured && HeaderBuffer && HeaderBufSize > 0) {
        ULONG hcopy = HeaderBufSize;
        ULONG dcopy = LookaheadBufSize;
        if (hcopy > sizeof(g_rx_capture_hdr)) hcopy = sizeof(g_rx_capture_hdr);
        if (dcopy > sizeof(g_rx_capture_data)) dcopy = sizeof(g_rx_capture_data);
        ndis_memcpy(g_rx_capture_hdr, HeaderBuffer, hcopy);
        g_rx_capture_hdr_len = hcopy;
        if (LookaheadBuffer && dcopy > 0) {
            ndis_memcpy(g_rx_capture_data, LookaheadBuffer, dcopy);
            g_rx_capture_data_len = dcopy;
        }
        g_rx_captured = 1;
        VxD_Debug_Printf("NDIS: *** RX FRAME CAPTURED ***\r\n");
    }
}

/* EthRxCompleteHandler: miniport signals all pending receives are done */
static void __stdcall ndis_EthRxComplete(PVOID EthDB)
{
    if (g_ndis_log_count < 20) {
        VxD_Debug_Printf("NDIS: EthRxComplete\r\n");
        g_ndis_log_count++;
    }
}

/* PacketIndicateHandler (NdisMIndicateReceivePacket path via MiniportBlock).
   Forward-declared; implemented below as ndis_MIndicateReceivePacket. */
static void __stdcall ndis_MIndicateReceivePacket(
    NDIS_HANDLE MiniportHandle, PVOID *PacketArray, ULONG NumberOfPackets);

static void __stdcall ndis_PacketIndicate(
    NDIS_HANDLE MiniportHandle, PVOID *PacketArray, ULONG NumberOfPackets)
{
    ndis_MIndicateReceivePacket(MiniportHandle, PacketArray, NumberOfPackets);
}

/* SendCompleteHandler (inline path from MiniportBlock) */
static void __stdcall ndis_SendCompleteHandler(
    NDIS_HANDLE MiniportHandle, PVOID Packet, NDIS_STATUS Status)
{
    ndis_log_hex("NDIS: SendComplete(inline) status=", Status, "\r\n");
    g_send_complete_flag = 1;
    g_send_complete_status = Status;
}

/* StatusHandler: miniport indicates link/media status change */
static void __stdcall ndis_StatusHandler(
    NDIS_HANDLE MiniportHandle, NDIS_STATUS GeneralStatus,
    PVOID StatusBuffer, ULONG StatusBufferSize)
{
    ndis_log_hex("NDIS: Status=", GeneralStatus, "\r\n");
}

/* StatusCompleteHandler */
static void __stdcall ndis_StatusCompleteHandler(NDIS_HANDLE MiniportHandle)
{
    VxD_Debug_Printf("NDIS: StatusComplete\r\n");
}

/* TransferDataCompleteHandler */
static void __stdcall ndis_TDCompleteHandler(
    NDIS_HANDLE MiniportHandle, PVOID Packet,
    NDIS_STATUS Status, ULONG BytesTransferred)
{
    ndis_log_hex("NDIS: TDComplete bytes=", BytesTransferred, "\r\n");
}

/* QueryInformationComplete */
static void __stdcall ndis_QueryCompleteHandler(
    NDIS_HANDLE MiniportHandle, NDIS_STATUS Status)
{
    ndis_log_hex("NDIS: QueryComplete status=", Status, "\r\n");
}

/* SetInformationComplete */
static void __stdcall ndis_SetCompleteHandler(
    NDIS_HANDLE MiniportHandle, NDIS_STATUS Status)
{
    ndis_log_hex("NDIS: SetComplete status=", Status, "\r\n");
}

/* Intrinsic to get current EBP value (for return address extraction) */
#ifdef __WATCOMC__
unsigned long _get_ebp_val(void);
#pragma aux _get_ebp_val = "mov eax, ebp" value [eax];
#endif

/* Stdcall no-op stubs for MB function pointer slots.
   CRITICAL: ndis_safe_noop (cdecl ret) causes stack corruption when called
   with stdcall args. Each stub must match the arg count for its slot. */
static volatile ULONG g_safe_noop_call_count = 0;
static ULONG ndis_safe_noop(void)
{
    g_safe_noop_call_count++;
    return 0;
}
static void __stdcall ndis_safe_noop_1arg(ULONG a) { g_safe_noop_call_count++; }
static void __stdcall ndis_safe_noop_2arg(ULONG a, ULONG b) { g_safe_noop_call_count++; }
static void __stdcall ndis_safe_noop_3arg(ULONG a, ULONG b, ULONG c) { g_safe_noop_call_count++; }
static ULONG __stdcall ndis_safe_ret0_1arg(ULONG a) { return 0; }
static ULONG __stdcall ndis_safe_ret0_2arg(ULONG a, ULONG b) { return 0; }
static ULONG __stdcall ndis_safe_ret0_3arg(ULONG a, ULONG b, ULONG c) { return 0; }

/* ResetCompleteHandler at MB+0xF4 — called by RTL8139 HandleInterrupt */
static void __stdcall ndis_ResetCompleteHandler(
    PVOID MiniportHandle, ULONG Status, ULONG AddressingReset)
{
    ndis_log_hex("NDIS: ResetComplete status=", Status, "\r\n");
}

/* ================================================================
 * MiniportBlock Initialization
 *
 * Fills all function pointer slots in the fake NDIS_MINIPORT_BLOCK.
 * Known handler offsets get our upcall functions; all other DWORD-
 * aligned slots get sentinel values (0xDEAD0000 | offset) so that
 * any accidental dereference traps at a diagnostic address.
 * ================================================================ */

static void ndis_init_miniport_block(void)
{
    ULONG off;
    ULONG *p;

    /* Fill strategy for NDIS 5.0 MINIPORT_BLOCK:
       - Data pointer slots (0x00-0xDF) → 0 (NULL). The driver must
         NULL-check before dereferencing (standard NT driver practice).
       - Function pointer slots (0xE0-0x198) → safe_func (0-arg noop)
       - Known handler/data slots → proper values (set below)
       Previous approaches (safe_page, self-referential) caused worse
       failures. Zero is safest — well-written drivers NULL-check. */
    {
        ULONG off;
        /* Zero entire block */
        for (off = 0; off < NDIS_MINIPORT_BLOCK_SIZE; off += 4) {
            *(ULONG *)(&g_ndis_miniport_block[off]) = 0;
        }
        /* Function pointer region: 0xE0-0x198.
           Default to 3-arg stdcall noop (safest for unknown slots —
           most NDIS callbacks take 2-3 args). */
        for (off = 0xE0; off <= 0x198; off += 4) {
            *(ULONG *)(&g_ndis_miniport_block[off]) = (ULONG)ndis_safe_noop_3arg;
        }
        /* Install correctly-typed handlers at known offsets */
        MB_PTR_AT(0xF4) = (PVOID)ndis_ResetCompleteHandler;
    }

    /* Set up fake NT kernel objects for multi-level dereferences.
       The driver reads DeviceObject/DriverObject from MB data slots and
       dereferences them. Point to zeroed structures (not safe_page,
       which is self-referential and causes infinite loops). */
    ndis_memset(g_fake_device_object, 0, sizeof(g_fake_device_object));
    ndis_memset(g_fake_driver_object, 0, sizeof(g_fake_driver_object));
    /* DEVICE_OBJECT+0x08 = DriverObject */
    *(PVOID *)(&g_fake_device_object[0x08]) = (PVOID)g_fake_driver_object;
    /* MB+0x18: DeviceObject (common NDIS 5.0 offset for FDO) */
    MB_PTR_AT(0x18) = (PVOID)g_fake_device_object;
    /* MB+0x1C: PhysicalDeviceObject */
    MB_PTR_AT(0x1C) = (PVOID)g_fake_device_object;
    /* MB+0x20: NextDeviceObject */
    MB_PTR_AT(0x20) = (PVOID)g_fake_device_object;

    /* Set up the Ethernet filter DB handle */
    ndis_memset(g_ndis_eth_filter_db, 0, sizeof(g_ndis_eth_filter_db));
    MB_PTR_AT(NDIS_MB_ETH_FILTER_DB) = (PVOID)g_ndis_eth_filter_db;

    /* Install our upcall handlers */
    MB_PTR_AT(NDIS_MB_ETH_RX_INDICATE) = (PVOID)ndis_EthRxIndicate;
    MB_PTR_AT(NDIS_MB_ETH_RX_COMPLETE) = (PVOID)ndis_EthRxComplete;
    MB_PTR_AT(NDIS_MB_PKT_INDICATE)    = (PVOID)ndis_PacketIndicate;
    MB_PTR_AT(NDIS_MB_SEND_COMPLETE)   = (PVOID)ndis_SendCompleteHandler;
    MB_PTR_AT(NDIS_MB_STATUS_HANDLER)  = (PVOID)ndis_StatusHandler;
    MB_PTR_AT(NDIS_MB_STATUS_COMPLETE) = (PVOID)ndis_StatusCompleteHandler;
    MB_PTR_AT(NDIS_MB_TD_COMPLETE)     = (PVOID)ndis_TDCompleteHandler;
    MB_PTR_AT(NDIS_MB_QUERY_COMPLETE)  = (PVOID)ndis_QueryCompleteHandler;
    MB_PTR_AT(NDIS_MB_SET_COMPLETE)    = (PVOID)ndis_SetCompleteHandler;

    /* AdapterContext starts NULL; set by NdisMSetAttributesEx */
    MB_PTR_AT(NDIS_MB_ADAPTER_CONTEXT) = NULL;
}

/* ================================================================
 * NDIS Configuration Registry (fake)
 *
 * ne2000.sys reads IoBaseAddress and InterruptNumber via
 * NdisReadConfiguration. We provide hardcoded ISA defaults.
 * ================================================================ */

/* NDIS_CONFIGURATION_PARAMETER types */
#define NdisParameterInteger    0
#define NdisParameterHexInteger 1
#define NdisParameterString     2

typedef struct _NDIS_CONFIGURATION_PARAMETER {
    ULONG ParameterType;
    union {
        ULONG IntegerData;
        /* String not implemented yet */
    } ParameterData;
} NDIS_CONFIGURATION_PARAMETER;

/* UNICODE_STRING equivalent (minimal) */
typedef struct _NDIS_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PUSHORT Buffer;
} NDIS_STRING;

/* Config entries we know ne2000.sys asks for.
   IRQ 10 chosen for QEMU default; typical real NE2000 ISA cards
   use IRQ 3, 5, or 10. Adjust to match your test hardware/VM. */
#define NDIS_CFG_MAX_ENTRIES 8
static struct {
    const char *keyword_ascii;   /* keyword in ASCII (for matching) */
    ULONG value;                 /* integer value to return */
} g_ndis_config[] = {
    { "IoBaseAddress",    0x300 },   /* Overridden with PCI BAR if detected */
    { "InterruptNumber",  10    },   /* Overridden with PCI IRQ if detected */
    { "BusType",          5     },   /* PCI = 5 (NdisInterfacePci) */
    { "BusNumber",        0     },
    { "CardType",         0     },
    { NULL, 0 }
};

static NDIS_CONFIGURATION_PARAMETER g_ndis_config_param;
static UCHAR g_ndis_config_handle_dummy[4]; /* non-NULL config handle */

/* ================================================================
 * Packet Pool and Buffer Pool
 *
 * NDIS_BUFFER is an MDL. Layout must match NT4 DDK exactly (from
 * NTKRNL.H in the backport repo). NDIS_PACKET has NDIS_PACKET_PRIVATE
 * at offset 0 with Head/Tail buffer chain pointers, followed by
 * MiniportReserved, WrapperReserved, and OOB data.
 * ================================================================ */

#define NDIS_PACKET_SIZE        128  /* Private(32) + Reserved(16) + OOB(32) + slack */
#define NDIS_PACKET_POOL_COUNT  32

/* NDIS_BUFFER == MDL (NT4 x86 layout, 28 bytes + PFN array) */
typedef struct _NDIS_BUFFER {
    struct _NDIS_BUFFER *Next;      /* +0x00 */
    USHORT Size;                    /* +0x04 */
    USHORT MdlFlags;                /* +0x06 */
    PVOID  Process;                 /* +0x08 (PEPROCESS, NULL for kernel) */
    PVOID  MappedSystemVa;          /* +0x0C */
    PVOID  StartVa;                 /* +0x10 */
    ULONG  ByteCount;               /* +0x14 */
    ULONG  ByteOffset;              /* +0x18 */
} NDIS_BUFFER;
/* sizeof = 0x1C (28), but we allocate a full page for safety */

/* MDL flags */
#define MDL_MAPPED_TO_SYSTEM_VA  0x0001

/* NDIS_PACKET_PRIVATE layout (offset 0 within NDIS_PACKET) */
#define PKT_PHYSICAL_COUNT  0x00
#define PKT_TOTAL_LENGTH    0x04
#define PKT_HEAD            0x08  /* PNDIS_BUFFER */
#define PKT_TAIL            0x0C  /* PNDIS_BUFFER */
#define PKT_POOL            0x10  /* PNDIS_PACKET_POOL */
#define PKT_COUNT           0x14
#define PKT_FLAGS           0x18
#define PKT_VALID_COUNTS    0x1C  /* BOOLEAN */
#define PKT_NDIS_FLAGS      0x1D  /* UCHAR */
#define PKT_OOB_OFFSET      0x1E  /* USHORT: offset to OOB data from packet start */
#define PKT_PRIVATE_SIZE    0x20  /* 32 bytes */

/* After Private: MiniportReserved(8), WrapperReserved(8) */
#define PKT_MINIPORT_RESERVED  0x20
#define PKT_WRAPPER_RESERVED   0x28

/* OOB data starts at offset 0x30 */
#define PKT_OOB_DATA        0x30
/* NDIS_PACKET_OOB_DATA: TimeToSend(8) + TimeReceived(8) + HeaderSize(4)
   + SizeMediaSpecificInfo(4) + MediaSpecificInformation(4) + Status(4) = 32 */
#define PKT_OOB_STATUS_OFFSET  0x1C  /* within OOB: Status field */

/* Accessor macros for packet fields */
#define PKT_PTR_AT(pkt, off)  (*(PVOID *)((PUCHAR)(pkt) + (off)))
#define PKT_ULONG_AT(pkt, off) (*(PULONG)((PUCHAR)(pkt) + (off)))
#define PKT_USHORT_AT(pkt, off) (*(PUSHORT)((PUCHAR)(pkt) + (off)))

typedef struct _NDIS_PACKET_POOL {
    UCHAR  packets[NDIS_PACKET_POOL_COUNT * NDIS_PACKET_SIZE];
    UCHAR  in_use[NDIS_PACKET_POOL_COUNT];
    ULONG  count;
} NDIS_PACKET_POOL;

static NDIS_PACKET_POOL *g_packet_pool = NULL;
static UCHAR g_buffer_pool_handle[4]; /* non-NULL buffer pool handle */

/* ================================================================
 * NDIS Function Implementations
 * ================================================================ */

/*
 * NdisInitializeWrapper / NdisMInitializeWrapper
 *
 * Called first by DriverEntry. Returns a "wrapper handle" which is
 * just our miniport block pointer. The miniport passes this handle
 * to NdisMRegisterMiniport.
 *
 * Prototype: VOID NdisInitializeWrapper(
 *   OUT PNDIS_HANDLE NdisWrapperHandle,
 *   IN  PVOID       SystemSpecific1,    // RegistryPath
 *   IN  PVOID       SystemSpecific2,    // Context (unused)
 *   IN  PVOID       SystemSpecific3     // unused
 * );
 */
static void __stdcall ndis_InitializeWrapper(
    PVOID *NdisWrapperHandle,
    PVOID SystemSpecific1,
    PVOID SystemSpecific2,
    PVOID SystemSpecific3)
{
    VxD_Debug_Printf("NDIS: NdisInitializeWrapper\r\n");

    /* Initialize the miniport block with handlers and sentinels */
    ndis_init_miniport_block();

    /* Return our miniport block as the "wrapper handle" */
    g_ndis_miniport.WrapperHandle = (PVOID)g_ndis_miniport_block;
    if (NdisWrapperHandle) {
        *NdisWrapperHandle = (PVOID)g_ndis_miniport_block;
    }
}

/*
 * NdisMRegisterMiniport
 *
 * Called by DriverEntry after NdisInitializeWrapper. The miniport
 * passes its MINIPORT_CHARACTERISTICS struct containing all callback
 * function pointers.
 *
 * Prototype: NDIS_STATUS NdisMRegisterMiniport(
 *   IN NDIS_HANDLE NdisWrapperHandle,
 *   IN PNDIS_MINIPORT_CHARACTERISTICS MiniportCharacteristics,
 *   IN UINT CharacteristicsLength
 * );
 */
static NDIS_STATUS __stdcall ndis_MRegisterMiniport(
    NDIS_HANDLE NdisWrapperHandle,
    PVOID MiniportCharacteristics,
    ULONG CharacteristicsLength)
{
    PUCHAR mc = (PUCHAR)MiniportCharacteristics;

    VxD_Debug_Printf("NDIS: NdisMRegisterMiniport\r\n");
    ndis_log_hex("  CharLen=", CharacteristicsLength, "\r\n");

    if (!MiniportCharacteristics || CharacteristicsLength < 60) {
        VxD_Debug_Printf("NDIS: RegisterMiniport: bad characteristics!\r\n");
        return NDIS_STATUS_FAILURE;
    }

    /* Extract version */
    g_ndis_miniport.MajorVersion = mc[MC_MAJOR_VERSION];
    g_ndis_miniport.MinorVersion = mc[MC_MINOR_VERSION];
    ndis_log_hex("  NDIS version=", (ULONG)g_ndis_miniport.MajorVersion, "");
    ndis_log_hex(".", (ULONG)g_ndis_miniport.MinorVersion, "\r\n");

    /* Extract NDIS 3.0 callbacks (always present) */
    g_ndis_miniport.CheckForHangHandler    = *(PVOID *)(mc + MC_CHECK_HANG);
    g_ndis_miniport.DisableInterruptHandler = *(PVOID *)(mc + MC_DISABLE_INT);
    g_ndis_miniport.EnableInterruptHandler  = *(PVOID *)(mc + MC_ENABLE_INT);
    g_ndis_miniport.HaltHandler            = *(PVOID *)(mc + MC_HALT);
    g_ndis_miniport.HandleInterruptHandler = *(PVOID *)(mc + MC_HANDLE_INT);
    g_ndis_miniport.InitializeHandler      = *(PVOID *)(mc + MC_INITIALIZE);
    g_ndis_miniport.ISRHandler             = *(PVOID *)(mc + MC_ISR);
    g_ndis_miniport.QueryInformationHandler = *(PVOID *)(mc + MC_QUERY_INFO);
    g_ndis_miniport.ReconfigureHandler     = *(PVOID *)(mc + MC_RECONFIGURE);
    g_ndis_miniport.ResetHandler           = *(PVOID *)(mc + MC_RESET);
    g_ndis_miniport.SendHandler            = *(PVOID *)(mc + MC_SEND);
    g_ndis_miniport.SetInformationHandler  = *(PVOID *)(mc + MC_SET_INFO);
    g_ndis_miniport.TransferDataHandler    = *(PVOID *)(mc + MC_TRANSFER_DATA);

    /* Extract NDIS 4.0 callbacks if present */
    if (CharacteristicsLength >= 72 && g_ndis_miniport.MajorVersion >= 4) {
        g_ndis_miniport.ReturnPacketHandler   = *(PVOID *)(mc + MC_RETURN_PACKET);
        g_ndis_miniport.SendPacketsHandler    = *(PVOID *)(mc + MC_SEND_PACKETS);
        g_ndis_miniport.AllocateCompleteHandler = *(PVOID *)(mc + MC_ALLOC_COMPLETE);
    } else {
        g_ndis_miniport.ReturnPacketHandler   = NULL;
        g_ndis_miniport.SendPacketsHandler    = NULL;
        g_ndis_miniport.AllocateCompleteHandler = NULL;
    }

    /* Populate MiniportBlock function pointer fields that NT NDIS normally
       fills from the characteristics. The miniport (or our shim) may
       call through these directly. */
    MB_PTR_AT(0x154) = g_ndis_miniport.DisableInterruptHandler;
    MB_PTR_AT(0x158) = g_ndis_miniport.EnableInterruptHandler;
    MB_PTR_AT(0x15C) = g_ndis_miniport.SendPacketsHandler;

    ndis_log_hex("  Initialize=", (ULONG)g_ndis_miniport.InitializeHandler, "\r\n");
    ndis_log_hex("  Send=",       (ULONG)g_ndis_miniport.SendHandler, "\r\n");
    ndis_log_hex("  QueryInfo=",  (ULONG)g_ndis_miniport.QueryInformationHandler, "\r\n");
    ndis_log_hex("  ISR=",        (ULONG)g_ndis_miniport.ISRHandler, "\r\n");
    ndis_log_hex("  DisableInt=", (ULONG)g_ndis_miniport.DisableInterruptHandler, "\r\n");
    ndis_log_hex("  EnableInt=",  (ULONG)g_ndis_miniport.EnableInterruptHandler, "\r\n");

    return NDIS_STATUS_SUCCESS;
}

/*
 * NdisMSetAttributesEx / NdisMSetAttributes
 *
 * Miniport calls this from MiniportInitialize to register its
 * adapter context and attributes.
 *
 * Prototype: VOID NdisMSetAttributesEx(
 *   IN NDIS_HANDLE MiniportAdapterHandle,
 *   IN NDIS_HANDLE MiniportAdapterContext,
 *   IN UINT        CheckForHangTimeInSeconds,
 *   IN ULONG       AttributeFlags
 * );
 */
static void __stdcall ndis_MSetAttributesEx(
    NDIS_HANDLE MiniportAdapterHandle,
    NDIS_HANDLE MiniportAdapterContext,
    ULONG CheckForHangTimeInSeconds,
    ULONG AttributeFlags,
    ULONG AdapterType)            /* NDIS_INTERFACE_TYPE — was MISSING! */
{
    (void)AdapterType;
    VxD_Debug_Printf("NDIS: NdisMSetAttributesEx\r\n");
    ndis_log_hex("  AdapterContext=", (ULONG)MiniportAdapterContext, "\r\n");
    ndis_log_hex("  Flags=", AttributeFlags, "\r\n");

    /* Store adapter context in our state and in the miniport block */
    g_ndis_miniport.AdapterContext = MiniportAdapterContext;
    MB_PTR_AT(NDIS_MB_ADAPTER_CONTEXT) = MiniportAdapterContext;
}

/*
 * NdisMRegisterIoPortRange
 *
 * Maps an I/O port range for the miniport. For ISA, the virtual
 * address is the same as the physical port number.
 *
 * Prototype: NDIS_STATUS NdisMRegisterIoPortRange(
 *   OUT PVOID *PortOffset,
 *   IN  NDIS_HANDLE MiniportAdapterHandle,
 *   IN  UINT InitialPort,
 *   IN  UINT NumberOfPorts
 * );
 */
static NDIS_STATUS __stdcall ndis_MRegisterIoPortRange(
    PVOID *PortOffset,
    NDIS_HANDLE MiniportAdapterHandle,
    ULONG InitialPort,
    ULONG NumberOfPorts)
{
    ndis_log_hex("NDIS: NdisMRegisterIoPortRange base=", InitialPort, "");
    ndis_log_hex(" count=", NumberOfPorts, "\r\n");

    /* For ISA, the port offset is simply the base I/O address.
       The miniport adds register offsets to this base. */
    if (PortOffset) {
        *PortOffset = (PVOID)InitialPort;
    }

    g_ndis_miniport.IoPortBase = (USHORT)InitialPort;
    g_ndis_miniport.IoPortLength = (USHORT)NumberOfPorts;
    g_ndis_miniport.IoPortMapped = (PVOID)InitialPort;

    /* Pre-mask IRQ 10 at the PIC BEFORE the miniport touches the NIC.
       The NE2000 init sequence enables NIC interrupts, which fires
       IRQ 10 before our NdisMRegisterInterrupt shim can mask it. */
    {
        UCHAR pic_mask = _port_inb(0xA1);
        _port_outb(0xA1, pic_mask | 0x04);  /* bit 2 = IRQ 10 */
        VxD_Debug_Printf("NDIS: Pre-masked IRQ10 at PIC\r\n");
    }

    return NDIS_STATUS_SUCCESS;
}

/*
 * NdisMDeregisterIoPortRange
 */
static void __stdcall ndis_MDeregisterIoPortRange(
    NDIS_HANDLE MiniportAdapterHandle,
    ULONG InitialPort,
    ULONG NumberOfPorts,
    PVOID PortOffset)
{
    VxD_Debug_Printf("NDIS: NdisMDeregisterIoPortRange\r\n");
    /* No-op: nothing to unmap for ISA ports */
}

/*
 * NdisMRegisterInterrupt
 *
 * Registers the miniport's ISR with the interrupt controller.
 * VPICD hookup deferred (GPFs at Device_Init). Polling mode for now.
 */
/*
 * NDIS_MINIPORT_INTERRUPT layout (must match NT DDK):
 *   0x00  PKINTERRUPT InterruptObject
 *   0x04  KSPIN_LOCK  DpcCountLock
 *   0x08  PVOID       Reserved
 *   0x0C  PVOID       MiniportIsr  (W_ISR_HANDLER)
 *   0x10  PVOID       MiniportDpc  (W_HANDLE_INTERRUPT_HANDLER)
 *   0x14  KDPC        InterruptDpc (32 bytes on x86)
 *   0x34  PVOID       Miniport     (back-pointer to NDIS_MINIPORT_BLOCK)
 *   0x38  UCHAR       DpcCount
 *   0x39  BOOLEAN     Filler1
 *   0x3A  KEVENT      DpcsCompletedEvent (varies)
 *   ...   BOOLEAN     SharedInterrupt
 *   ...   BOOLEAN     IsrRequested
 */
#define NDIS_INTERRUPT_SIZE 0x60

static NDIS_STATUS __stdcall ndis_MRegisterInterrupt(
    PVOID NdisInterruptHandle,  /* IN/OUT: PNDIS_MINIPORT_INTERRUPT */
    NDIS_HANDLE MiniportAdapterHandle,
    ULONG InterruptVector,
    ULONG InterruptLevel,
    BOOLEAN RequestISR,
    BOOLEAN SharedInterrupt,
    ULONG InterruptMode)
{
    PUCHAR intobj;

    ndis_log_hex("NDIS: NdisMRegisterInterrupt vec=", InterruptVector, "");
    ndis_log_hex(" level=", InterruptLevel, "\r\n");

    g_ndis_miniport.InterruptVector = InterruptVector;
    g_ndis_miniport.InterruptRegistered = TRUE;

    /* Zero the NDIS_MINIPORT_INTERRUPT struct so no garbage pointers remain.
       Key fields: ISR at +0x0C, HandleInterrupt at +0x10, Miniport at +0x34 */
    if (NdisInterruptHandle) {
        ndis_memset(NdisInterruptHandle, 0, NDIS_INTERRUPT_SIZE);
        *(PVOID *)((PUCHAR)NdisInterruptHandle + 0x0C) = g_ndis_miniport.ISRHandler;
        *(PVOID *)((PUCHAR)NdisInterruptHandle + 0x10) = g_ndis_miniport.HandleInterruptHandler;
        *(PVOID *)((PUCHAR)NdisInterruptHandle + 0x34) = (PVOID)g_ndis_miniport_block;
    }
    /* Mask this IRQ at the PIC to prevent unhandled interrupts during
       MiniportInitialize. VPICD hookup happens after MiniportInitialize
       returns (outside Safe_HwFindAdapter) because VMM service calls
       (int 0x20) conflict with the IDT fault-catching wrapper. */
    {
        USHORT pic_port;
        UCHAR pic_mask, irq_bit;
        if (InterruptVector < 8) {
            pic_port = 0x21;
            irq_bit = (UCHAR)(1 << InterruptVector);
        } else {
            pic_port = 0xA1;
            irq_bit = (UCHAR)(1 << (InterruptVector - 8));
        }
        pic_mask = _port_inb(pic_port);
        _port_outb(pic_port, pic_mask | irq_bit);
        ndis_log_hex("NDIS: Masked IRQ", InterruptVector, " at PIC (VPICD hookup deferred)\r\n");
    }
    return NDIS_STATUS_SUCCESS;
}

/*
 * NdisMDeregisterInterrupt
 */
static void __stdcall ndis_MDeregisterInterrupt(PVOID NdisInterruptHandle)
{
    VxD_Debug_Printf("NDIS: NdisMDeregisterInterrupt\r\n");
    g_ndis_miniport.InterruptRegistered = FALSE;
    /* TODO: Unhook VPICD */
}

/*
 * NdisAllocateMemory
 *
 * Allocates memory. PhysicalAddress constraint is ignored for now.
 * Memory comes from VxD_PageAllocate.
 *
 * Prototype: NDIS_STATUS NdisAllocateMemory(
 *   OUT PVOID *VirtualAddress,
 *   IN  UINT Length,
 *   IN  UINT MemoryFlags,
 *   IN  NDIS_PHYSICAL_ADDRESS HighestAcceptableAddress
 * );
 *
 * Note: HighestAcceptableAddress is NDIS_PHYSICAL_ADDRESS (LARGE_INTEGER),
 * a 64-bit value passed by value. On x86 __stdcall it occupies two
 * DWORDs on the stack. We must consume both to keep the stack balanced.
 */
static NDIS_STATUS __stdcall ndis_AllocateMemory(
    PVOID *VirtualAddress,
    ULONG Length,
    ULONG MemoryFlags,
    ULONG HighAddrLow,
    ULONG HighAddrHigh)
{
    PVOID mem;
    ULONG nPages;

    ndis_log_hex("NDIS: NdisAllocateMemory size=", Length, "\r\n");

    if (!VirtualAddress || Length == 0) return NDIS_STATUS_FAILURE;

    nPages = (Length + PAGESIZE - 1) / PAGESIZE;
    mem = VxD_PageAllocate(nPages, PAGEFIXED);
    if (!mem) {
        VxD_Debug_Printf("NDIS: AllocateMemory FAILED!\r\n");
        return NDIS_STATUS_RESOURCES;
    }

    ndis_memset(mem, 0, nPages * PAGESIZE);
    *VirtualAddress = mem;

    ndis_log_hex("  -> VA=", (ULONG)mem, "\r\n");
    return NDIS_STATUS_SUCCESS;
}

/*
 * NdisFreeMemory
 */
static void __stdcall ndis_FreeMemory(
    PVOID VirtualAddress,
    ULONG Length,
    ULONG MemoryFlags)
{
    ndis_log_hex("NDIS: NdisFreeMemory VA=", (ULONG)VirtualAddress, "\r\n");
    if (VirtualAddress) {
        VxD_PageFree(VirtualAddress);
    }
}

/*
 * NdisAllocateMemoryWithTag — same as NdisAllocateMemory but with pool tag.
 * Pool tags are an NT debugging feature; we ignore the tag.
 */
static NDIS_STATUS __stdcall ndis_AllocateMemoryWithTag(
    PVOID *VirtualAddress,
    ULONG Length,
    ULONG Tag)
{
    PVOID mem;
    ULONG nPages;

    ndis_log_hex("NDIS: NdisAllocateMemoryWithTag size=", Length, "");
    ndis_log_hex(" tag=", Tag, "\r\n");

    if (!VirtualAddress || Length == 0) return NDIS_STATUS_FAILURE;

    nPages = (Length + PAGESIZE - 1) / PAGESIZE;
    mem = VxD_PageAllocate(nPages, PAGEFIXED);
    if (!mem) return NDIS_STATUS_RESOURCES;

    ndis_memset(mem, 0, nPages * PAGESIZE);
    *VirtualAddress = mem;
    return NDIS_STATUS_SUCCESS;
}

/*
 * NdisMAllocateSharedMemory — allocates physically contiguous DMA memory.
 * In our VxD context, VxD_PageAllocate with PAGEFIXED returns physically
 * contiguous pages suitable for bus-master DMA.
 */
extern PVOID VxD_PageAllocateDMA(ULONG nPages, ULONG *pPhysAddr);

/* NdisMAllocateSharedMemory — PhysicalAddress is PNDIS_PHYSICAL_ADDRESS
   (pointer to LARGE_INTEGER, 8 bytes). NOT two separate ULONGs. */
static void __stdcall ndis_MAllocateSharedMemory(
    NDIS_HANDLE MiniportAdapterHandle,
    ULONG Length,
    BOOLEAN Cached,
    PVOID *VirtualAddress,
    PVOID PhysicalAddress)  /* PNDIS_PHYSICAL_ADDRESS = PLARGE_INTEGER */
{
    PVOID mem;
    ULONG nPages;
    ULONG physAddr = 0;
    PULONG pPhys = (PULONG)PhysicalAddress;

    ndis_log_hex("NDIS: NdisMAllocateSharedMemory size=", Length, "\r\n");

    if (!VirtualAddress || !pPhys) return;

    nPages = (Length + PAGESIZE - 1) / PAGESIZE;
    mem = VxD_PageAllocateDMA(nPages, &physAddr);
    if (!mem) {
        *VirtualAddress = NULL;
        pPhys[0] = 0;
        pPhys[1] = 0;
        VxD_Debug_Printf("NDIS: AllocateSharedMemory FAILED!\r\n");
        return;
    }

    /* Override PA with CR3 page table walk — VxD_PageAllocateDMA fallback
       computes wrong PA (VA-0xC0000000 doesn't match real page mappings). */
    {
        ULONG realPA = ndis_va_to_pa(mem);
        if (realPA) {
            ndis_log_hex("NDIS: CR3-walk PA=", realPA, "");
            ndis_log_hex(" (was ", physAddr, ")\r\n");
            physAddr = realPA;
        }
    }

    *VirtualAddress = mem;
    pPhys[0] = physAddr;  /* LowPart */
    pPhys[1] = 0;         /* HighPart */
    ndis_log_hex("  -> VA=", (ULONG)mem, "");
    ndis_log_hex(" PA=", physAddr, "\r\n");

    g_ndis_miniport.LastSharedVA = mem;
    g_ndis_miniport.LastSharedPA = physAddr;
    g_ndis_miniport.LastSharedLen = Length;
}

/*
 * NdisMFreeSharedMemory
 */
/* NdisMFreeSharedMemory — PhysicalAddress is NDIS_PHYSICAL_ADDRESS
   (LARGE_INTEGER, 8 bytes by value on stack: LowPart + HighPart). */
static void __stdcall ndis_MFreeSharedMemory(
    NDIS_HANDLE MiniportAdapterHandle,
    ULONG Length,
    BOOLEAN Cached,
    PVOID VirtualAddress,
    ULONG PhysicalAddressLow,
    ULONG PhysicalAddressHigh)
{
    ndis_log_hex("NDIS: NdisMFreeSharedMemory VA=", (ULONG)VirtualAddress, "\r\n");
    if (VirtualAddress) VxD_PageFree(VirtualAddress);
}

/*
 * NdisMAllocateMapRegisters / NdisMFreeMapRegisters
 * DMA map registers for scatter/gather. In our flat ring-0 context
 * where physical == virtual, these are no-ops (the DMA address is
 * the virtual address). Real scatter/gather would need ISA DMA
 * controller programming, but PCI bus-master NICs use their own DMA.
 */
static NDIS_STATUS __stdcall ndis_MAllocateMapRegisters(
    NDIS_HANDLE MiniportAdapterHandle,
    ULONG DmaChannel,
    BOOLEAN DmaSize,
    ULONG BaseMapRegistersNeeded,
    ULONG MaximumPhysicalMapping)
{
    ndis_log_hex("NDIS: NdisMAllocateMapRegisters n=", BaseMapRegistersNeeded, "");
    ndis_log_hex(" maxMap=", MaximumPhysicalMapping, "\r\n");
    return NDIS_STATUS_SUCCESS;
}

static void __stdcall ndis_MFreeMapRegisters(NDIS_HANDLE MiniportAdapterHandle)
{
    VxD_Debug_Printf("NDIS: NdisMFreeMapRegisters\r\n");
}

/*
 * NdisMQueryAdapterResources — returns PCI resources from registry/PnP.
 * Similar to NdisMPciAssignResources but with a different output format.
 * Uses full CM_RESOURCE_LIST (not stripped).
 */
static void __stdcall ndis_MQueryAdapterResources(
    NDIS_STATUS *Status,
    NDIS_HANDLE WrapperConfigurationContext,
    PVOID ResourceList,        /* OUT: PCM_RESOURCE_LIST */
    PULONG BufferSize)
{
    ULONG bus, dev, func, cfgAddr, val;
    USHORT vendor_id, device_id;
    ULONG bar0, bar1, irq_line;
    PUCHAR res;
    int desc_count;
    BOOLEAN found = FALSE;
    ULONG needed;

    VxD_Debug_Printf("NDIS: NdisMQueryAdapterResources\r\n");
    if (!Status || !BufferSize) return;

    /* Scan PCI bus for network controller (class 0x02) */
    for (bus = 0; bus < 4 && !found; bus++) {
        for (dev = 0; dev < 32 && !found; dev++) {
            for (func = 0; func < 8 && !found; func++) {
                cfgAddr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8);
                _port_outd(0xCF8, cfgAddr);
                val = _port_ind(0xCFC);
                vendor_id = (USHORT)(val & 0xFFFF);
                device_id = (USHORT)(val >> 16);
                if (vendor_id == 0xFFFF || vendor_id == 0) continue;

                _port_outd(0xCF8, cfgAddr | 0x08);
                val = _port_ind(0xCFC);
                if ((val >> 24) == 0x02) {
                    _port_outd(0xCF8, cfgAddr | 0x10);
                    bar0 = _port_ind(0xCFC);
                    _port_outd(0xCF8, cfgAddr | 0x14);
                    bar1 = _port_ind(0xCFC);
                    _port_outd(0xCF8, cfgAddr | 0x3C);
                    irq_line = _port_ind(0xCFC) & 0xFF;
                    g_ndis_miniport.PciBus = bus;
                    g_ndis_miniport.PciDev = dev;
                    g_ndis_miniport.PciFunc = func;
                    g_ndis_miniport.PciFound = TRUE;
                    ndis_log_hex("NDIS: QAR PCI bus=", bus, "");
                    ndis_log_hex(" dev=", dev, "");
                    ndis_log_hex(" func=", func, "\r\n");
                    ndis_log_hex("NDIS: QAR BAR0=", bar0, "");
                    ndis_log_hex(" BAR1=", bar1, "");
                    ndis_log_hex(" IRQ=", irq_line, "\r\n");
                    found = TRUE;
                }
            }
        }
    }

    if (!found) {
        VxD_Debug_Printf("NDIS: QAR no PCI NIC found!\r\n");
        *Status = NDIS_STATUS_FAILURE;
        return;
    }

    /* NdisMQueryAdapterResources returns NDIS_RESOURCE_LIST which is
       CM_PARTIAL_RESOURCE_LIST (NOT full CM_RESOURCE_LIST):
         +0x00 USHORT Version (=1)
         +0x02 USHORT Revision (=1)
         +0x04 ULONG  Count (descriptors)
         +0x08 CM_PARTIAL_RESOURCE_DESCRIPTOR[]:
           each 16 bytes: Type(1) ShareDisp(1) Flags(2) + union(12) */
    desc_count = 0;
    if (bar0 & 1) desc_count++;   /* I/O port from BAR0 */
    if (bar1 & ~0xF) desc_count++; /* Memory from BAR1 */
    if (irq_line > 0 && irq_line < 255) desc_count++; /* IRQ */

    needed = 0x08 + desc_count * 16;
    if (*BufferSize < needed || !ResourceList) {
        *BufferSize = needed;
        *Status = NDIS_STATUS_RESOURCES;
        ndis_log_hex("NDIS: QueryAdapterResources need=", needed, "\r\n");
        return;
    }

    res = (PUCHAR)ResourceList;
    ndis_memset(res, 0, needed);
    *(USHORT *)(res + 0x00) = 1;    /* Version */
    *(USHORT *)(res + 0x02) = 1;    /* Revision */
    *(ULONG *)(res + 0x04) = (ULONG)desc_count;

    {
        int di = 0;
        PUCHAR d;

        if (bar0 & 1) {
            d = res + 0x08 + di * 16;
            d[0] = CM_RESOURCE_PORT;
            d[1] = 1;   /* ShareDisposition = Shared */
            *(USHORT *)(d + 2) = 1; /* Flags = IO */
            *(ULONG *)(d + 4) = bar0 & 0xFFFC;
            *(ULONG *)(d + 12) = 256;
            di++;
        }
        if (bar1 & ~0xF) {
            d = res + 0x08 + di * 16;
            d[0] = CM_RESOURCE_MEMORY;
            d[1] = 1;
            *(ULONG *)(d + 4) = bar1 & ~0xF;
            *(ULONG *)(d + 12) = 256;
            di++;
        }
        if (irq_line > 0 && irq_line < 255) {
            d = res + 0x08 + di * 16;
            d[0] = CM_RESOURCE_INTERRUPT;
            d[1] = 1;
            *(USHORT *)(d + 2) = 1; /* Flags = LevelSensitive */
            *(ULONG *)(d + 4) = irq_line;
            *(ULONG *)(d + 8) = irq_line;
            *(ULONG *)(d + 12) = 0xFFFFFFFF;
            di++;
        }
    }

    *BufferSize = needed;
    *Status = NDIS_STATUS_SUCCESS;
    ndis_log_hex("NDIS: QueryAdapterResources OK, ", (ULONG)desc_count, " descs");
    ndis_log_hex(" size=", needed, "\r\n");

    /* Hex dump the resource list for debugging */
    {
        ULONG di;
        for (di = 0; di < needed && di < 96; di += 4) {
            if (di % 16 == 0) ndis_log_hex("  QAR+", di, ": ");
            ndis_log_hex("", *(ULONG *)(res + di), " ");
            if (di % 16 == 12) VxD_Debug_Printf("\r\n");
        }
        if (needed % 16 != 0) VxD_Debug_Printf("\r\n");
    }
}

/*
 * NdisMSendComplete
 *
 * Called by the miniport to indicate a send has completed.
 */
static void __stdcall ndis_MSendComplete(
    NDIS_HANDLE MiniportAdapterHandle,
    PVOID Packet,
    NDIS_STATUS Status)
{
    ndis_log_hex("NDIS: NdisMSendComplete status=", Status, "\r\n");
    g_send_complete_status = Status;
    g_send_complete_flag = 1;
}

/*
 * NdisMMapIoSpace
 *
 * Maps physical memory-mapped I/O into virtual address space.
 *
 * Prototype: NDIS_STATUS NdisMMapIoSpace(
 *   OUT PVOID *VirtualAddress,
 *   IN  NDIS_HANDLE MiniportAdapterHandle,
 *   IN  NDIS_PHYSICAL_ADDRESS PhysicalAddress,
 *   IN  ULONG Length
 * );
 *
 * NDIS_PHYSICAL_ADDRESS is LARGE_INTEGER (8 bytes by value on the stack).
 * Must consume both DWORDs to keep __stdcall stack balanced.
 */
static NDIS_STATUS __stdcall ndis_MMapIoSpace(
    PVOID *VirtualAddress,
    NDIS_HANDLE MiniportAdapterHandle,
    ULONG PhysicalAddressLow,
    ULONG PhysicalAddressHigh,
    ULONG Length)
{
    PVOID mapped;

    ndis_log_hex("NDIS: NdisMMapIoSpace phys=", PhysicalAddressLow, "");
    ndis_log_hex(" len=", Length, "\r\n");

    if (!VirtualAddress) return NDIS_STATUS_FAILURE;

    g_ndis_miniport.MmioBase = NULL;  /* will set below on success */

    mapped = VxD_MapPhysToLinear(PhysicalAddressLow, Length);

    if (!mapped) {
        /* VxD_MapPhysToLinear may fail for ISA adapter memory that
           doesn't have a real backing device. Allocate a zeroed dummy
           page so the driver can safely read from it. */
        ULONG nPages = (Length + 4095) / 4096;
        mapped = VxD_PageAllocate(nPages, 1);
        if (mapped) {
            ndis_memset(mapped, 0, nPages * 4096);
            ndis_log_hex("  -> dummy VA=", (ULONG)mapped, "\r\n");
        } else {
            VxD_Debug_Printf("NDIS: MapIoSpace FAILED!\r\n");
            return NDIS_STATUS_RESOURCES;
        }
    }

    *VirtualAddress = mapped;
    g_ndis_miniport.MmioBase = mapped;
    ndis_log_hex("  -> VA=", (ULONG)mapped, "\r\n");

    /* Verify MMIO mapping: read first 4 bytes and compare with port I/O */
    if (g_ndis_miniport.IoPortBase && Length >= 4) {
        ULONG mmio_val = *(ULONG *)mapped;
        ULONG pio_val = _port_ind(g_ndis_miniport.IoPortBase);
        ndis_log_hex("NDIS: MMIO verify: mmio[0]=", mmio_val, "");
        ndis_log_hex(" pio[0]=", pio_val, "\r\n");
        if (mmio_val == pio_val && mmio_val != 0) {
            VxD_Debug_Printf("NDIS: MMIO VERIFIED OK\r\n");
        } else if (mmio_val == 0) {
            VxD_Debug_Printf("NDIS: *** MMIO reads ZERO (mapping may be dummy) ***\r\n");
        } else {
            VxD_Debug_Printf("NDIS: *** MMIO/PIO MISMATCH ***\r\n");
        }
    }

    return NDIS_STATUS_SUCCESS;
}

/*
 * NdisMUnmapIoSpace
 */
static void __stdcall ndis_MUnmapIoSpace(
    NDIS_HANDLE MiniportAdapterHandle,
    PVOID VirtualAddress,
    ULONG Length)
{
    VxD_Debug_Printf("NDIS: NdisMUnmapIoSpace (no-op)\r\n");
    /* Can't easily unmap in VxD; leaks virtual range but harmless */
}

/*
 * NdisReadPciSlotInformation
 *
 * Reads PCI configuration space. Same mechanism as ScsiPort's
 * sp_GetBusData (x86 ports 0xCF8/0xCFC).
 */
static ULONG __stdcall ndis_ReadPciSlotInformation(
    NDIS_HANDLE NdisAdapterHandle,
    ULONG SlotNumber,
    ULONG Offset,
    PVOID Buffer,
    ULONG Length)
{
    ULONG devNum, funcNum, cfgAddr, regOff;
    ULONG i;
    UCHAR *buf = (UCHAR *)Buffer;

    ndis_log_hex("NDIS: ReadPciSlot slot=", SlotNumber, "");
    ndis_log_hex(" off=", Offset, "");
    ndis_log_hex(" len=", Length, "");
    ndis_log_hex(" PciFound=", (ULONG)g_ndis_miniport.PciFound, "\r\n");

    if (!Buffer || Length == 0) return 0;

    if (g_ndis_miniport.PciFound && SlotNumber == 0) {
        devNum  = g_ndis_miniport.PciDev;
        funcNum = g_ndis_miniport.PciFunc;
        ndis_log_hex("  -> remap dev=", devNum, "");
        ndis_log_hex(" func=", funcNum, "\r\n");
    } else {
        devNum  = SlotNumber & 0x1F;
        funcNum = (SlotNumber >> 5) & 0x07;
    }

    for (i = 0; i < Length; i += 4) {
        ULONG busNum = g_ndis_miniport.PciFound ? g_ndis_miniport.PciBus : 0;
        regOff = (Offset + i) & 0xFC;
        cfgAddr = 0x80000000UL | (busNum << 16) |
                  (devNum << 11) | (funcNum << 8) | regOff;
        _port_outd(0xCF8, cfgAddr);
        {
            ULONG val = _port_ind(0xCFC);
            ULONG byte_off = (Offset + i) & 3;
            ULONG j;
            /* Handle unaligned reads correctly */
            for (j = 0; j < 4 && (i + j) < Length; j++) {
                buf[i + j] = (UCHAR)(val >> ((byte_off + j) * 8));
            }
        }
    }
    return Length;
}

/*
 * NdisWritePciSlotInformation
 */
static ULONG __stdcall ndis_WritePciSlotInformation(
    NDIS_HANDLE NdisAdapterHandle,
    ULONG SlotNumber,
    ULONG Offset,
    PVOID Buffer,
    ULONG Length)
{
    ULONG devNum, funcNum, cfgAddr, regOff;
    ULONG i;
    UCHAR *buf = (UCHAR *)Buffer;

    ndis_log_hex("NDIS: WritePciSlot slot=", SlotNumber, "");
    ndis_log_hex(" off=", Offset, "");
    ndis_log_hex(" len=", Length, "\r\n");

    if (!Buffer || Length == 0) return 0;

    if (g_ndis_miniport.PciFound && SlotNumber == 0) {
        devNum  = g_ndis_miniport.PciDev;
        funcNum = g_ndis_miniport.PciFunc;
    } else {
        devNum  = SlotNumber & 0x1F;
        funcNum = (SlotNumber >> 5) & 0x07;
    }

    for (i = 0; i < Length; i += 4) {
        ULONG busNum = g_ndis_miniport.PciFound ? g_ndis_miniport.PciBus : 0;
        ULONG val = 0;
        ULONG j;
        regOff = (Offset + i) & 0xFC;
        for (j = 0; j < 4 && (i + j) < Length; j++) {
            val |= ((ULONG)buf[i + j]) << (j * 8);
        }
        cfgAddr = 0x80000000UL | (busNum << 16) |
                  (devNum << 11) | (funcNum << 8) | regOff;
        _port_outd(0xCF8, cfgAddr);
        _port_outd(0xCFC, val);
    }
    return Length;
}

/*
 * NdisReadMcaPosInformation
 *
 * ne2000.sys checks whether it's on a MicroChannel bus.
 * Return failure: this is ISA/PCI, not MCA.
 */
static NDIS_STATUS __stdcall ndis_ReadMcaPosInformation(
    NDIS_HANDLE NdisAdapterHandle,
    PULONG ChannelNumber,
    PVOID PosData)
{
    VxD_Debug_Printf("NDIS: ReadMcaPosInformation -> NOT_SUPPORTED\r\n");
    return NDIS_STATUS_NOT_SUPPORTED;
}

/*
 * NdisOpenConfiguration
 *
 * Opens a handle to the adapter's registry configuration.
 * We return a dummy non-NULL handle.
 */
static void __stdcall ndis_OpenConfiguration(
    NDIS_STATUS *Status,
    NDIS_HANDLE *ConfigurationHandle,
    NDIS_HANDLE WrapperConfigurationContext)
{
    VxD_Debug_Printf("NDIS: NdisOpenConfiguration\r\n");
    if (ConfigurationHandle) {
        *ConfigurationHandle = (NDIS_HANDLE)g_ndis_config_handle_dummy;
    }
    if (Status) {
        *Status = NDIS_STATUS_SUCCESS;
    }
}

/*
 * NdisCloseConfiguration
 */
static void __stdcall ndis_CloseConfiguration(NDIS_HANDLE ConfigurationHandle)
{
    VxD_Debug_Printf("NDIS: NdisCloseConfiguration\r\n");
}

/*
 * NdisReadConfiguration
 *
 * Reads a named configuration value. We match against our hardcoded
 * config table. The keyword is an NDIS_STRING (UNICODE_STRING), so
 * we must do a basic Unicode-to-ASCII comparison.
 *
 * Prototype: VOID NdisReadConfiguration(
 *   OUT PNDIS_STATUS Status,
 *   OUT PNDIS_CONFIGURATION_PARAMETER *ParameterValue,
 *   IN  NDIS_HANDLE ConfigurationHandle,
 *   IN  PNDIS_STRING Keyword,
 *   IN  NDIS_PARAMETER_TYPE ParameterType
 * );
 */
static void __stdcall ndis_ReadConfiguration(
    NDIS_STATUS *Status,
    PVOID *ParameterValue,   /* OUT: pointer to NDIS_CONFIGURATION_PARAMETER */
    NDIS_HANDLE ConfigurationHandle,
    PVOID Keyword,           /* PNDIS_STRING (UNICODE_STRING) */
    ULONG ParameterType)
{
    NDIS_STRING *kw;
    USHORT *wbuf;
    USHORT wlen;
    int i, match;
    char ascii_kw[64];

    if (!Status) return;

    if (!Keyword || !ParameterValue) {
        *Status = NDIS_STATUS_FAILURE;
        return;
    }

    /* Convert UNICODE_STRING keyword to ASCII for matching */
    kw = (NDIS_STRING *)Keyword;
    wbuf = kw->Buffer;
    wlen = kw->Length / 2;  /* Length is in bytes, chars are 2 bytes */
    if (wlen > 62) wlen = 62;

    for (i = 0; i < (int)wlen; i++) {
        ascii_kw[i] = (char)(wbuf[i] & 0xFF);
    }
    ascii_kw[wlen] = '\0';

    VxD_Debug_Printf("NDIS: NdisReadConfiguration key=\"");
    VxD_Debug_Printf(ascii_kw);
    VxD_Debug_Printf("\"\r\n");

    /* Search our config table */
    match = -1;
    for (i = 0; g_ndis_config[i].keyword_ascii != NULL; i++) {
        if (ndis_strcmp_nocase(ascii_kw, g_ndis_config[i].keyword_ascii) == 0) {
            match = i;
            break;
        }
    }

    if (match >= 0) {
        g_ndis_config_param.ParameterType = NdisParameterInteger;
        g_ndis_config_param.ParameterData.IntegerData = g_ndis_config[match].value;
        *ParameterValue = (PVOID)&g_ndis_config_param;
        *Status = NDIS_STATUS_SUCCESS;
        ndis_log_hex("  -> value=", g_ndis_config[match].value, "\r\n");
    } else {
        *ParameterValue = NULL;
        *Status = NDIS_STATUS_FAILURE;
        VxD_Debug_Printf("  -> NOT FOUND\r\n");
    }
}

/*
 * NdisAllocatePacketPool / NdisAllocatePacketPoolEx
 *
 * Allocates a pool of NDIS_PACKET structures.
 */
static void __stdcall ndis_AllocatePacketPool(
    NDIS_STATUS *Status,
    NDIS_HANDLE *PoolHandle,
    ULONG NumberOfDescriptors,
    ULONG ProtocolReservedLength)
{
    ndis_log_hex("NDIS: NdisAllocatePacketPool n=", NumberOfDescriptors, "");
    ndis_log_hex(" rsv=", ProtocolReservedLength, "\r\n");

    if (!Status || !PoolHandle) return;

    if (!g_packet_pool) {
        g_packet_pool = (NDIS_PACKET_POOL *)VxD_PageAllocate(
            (sizeof(NDIS_PACKET_POOL) + PAGESIZE - 1) / PAGESIZE, PAGEFIXED);
        if (!g_packet_pool) {
            *Status = NDIS_STATUS_RESOURCES;
            *PoolHandle = NULL;
            return;
        }
        ndis_memset(g_packet_pool, 0, sizeof(NDIS_PACKET_POOL));
        g_packet_pool->count = NDIS_PACKET_POOL_COUNT;
    }

    *PoolHandle = (NDIS_HANDLE)g_packet_pool;
    *Status = NDIS_STATUS_SUCCESS;
}

/*
 * NdisFreePacketPool
 */
static void __stdcall ndis_FreePacketPool(NDIS_HANDLE PoolHandle)
{
    VxD_Debug_Printf("NDIS: NdisFreePacketPool\r\n");
    /* Don't actually free; we may reuse */
}

/*
 * NdisAllocatePacket
 */
static void __stdcall ndis_AllocatePacket(
    NDIS_STATUS *Status,
    PVOID *Packet,       /* OUT: PNDIS_PACKET */
    NDIS_HANDLE PoolHandle)
{
    NDIS_PACKET_POOL *pool = (NDIS_PACKET_POOL *)PoolHandle;
    ULONG i;

    if (!Status || !Packet) return;

    if (!pool) {
        *Status = NDIS_STATUS_FAILURE;
        *Packet = NULL;
        return;
    }

    for (i = 0; i < pool->count; i++) {
        if (!pool->in_use[i]) {
            PUCHAR pkt;
            pool->in_use[i] = 1;
            pkt = &pool->packets[i * NDIS_PACKET_SIZE];
            ndis_memset(pkt, 0, NDIS_PACKET_SIZE);
            /* Set Pool pointer in NDIS_PACKET_PRIVATE */
            PKT_PTR_AT(pkt, PKT_POOL) = (PVOID)pool;
            /* OOB data offset from packet start */
            PKT_USHORT_AT(pkt, PKT_OOB_OFFSET) = (USHORT)PKT_OOB_DATA;
            *Packet = (PVOID)pkt;
            *Status = NDIS_STATUS_SUCCESS;
            return;
        }
    }

    *Status = NDIS_STATUS_RESOURCES;
    *Packet = NULL;
}

/*
 * NdisFreePacket
 */
static void __stdcall ndis_FreePacket(PVOID Packet)
{
    ULONG idx;
    if (!g_packet_pool || !Packet) return;

    idx = ((ULONG)Packet - (ULONG)g_packet_pool->packets) / NDIS_PACKET_SIZE;
    if (idx < g_packet_pool->count) {
        g_packet_pool->in_use[idx] = 0;
    }
}

/*
 * NdisAllocateBufferPool
 */
static void __stdcall ndis_AllocateBufferPool(
    NDIS_STATUS *Status,
    NDIS_HANDLE *PoolHandle,
    ULONG NumberOfDescriptors)
{
    VxD_Debug_Printf("NDIS: NdisAllocateBufferPool\r\n");
    if (Status) *Status = NDIS_STATUS_SUCCESS;
    if (PoolHandle) *PoolHandle = (NDIS_HANDLE)g_buffer_pool_handle;
}

/*
 * NdisFreeBufferPool
 */
static void __stdcall ndis_FreeBufferPool(NDIS_HANDLE PoolHandle)
{
    VxD_Debug_Printf("NDIS: NdisFreeBufferPool\r\n");
}

/*
 * NdisAllocateBuffer
 *
 * Allocates an NDIS_BUFFER (MDL) describing a region of memory.
 */
static void __stdcall ndis_AllocateBuffer(
    NDIS_STATUS *Status,
    PVOID *Buffer,           /* OUT: PNDIS_BUFFER (MDL) */
    NDIS_HANDLE PoolHandle,
    PVOID VirtualAddress,
    ULONG Length)
{
    NDIS_BUFFER *buf;

    if (!Status || !Buffer) return;

    buf = (NDIS_BUFFER *)VxD_PageAllocate(1, PAGEFIXED);
    if (!buf) {
        *Status = NDIS_STATUS_RESOURCES;
        *Buffer = NULL;
        return;
    }

    ndis_memset(buf, 0, PAGESIZE);
    buf->Next = NULL;
    buf->MdlFlags = MDL_MAPPED_TO_SYSTEM_VA;
    buf->Process = NULL;
    buf->MappedSystemVa = VirtualAddress;
    buf->StartVa = (PVOID)((ULONG)VirtualAddress & ~0xFFFUL);
    buf->ByteCount = Length;
    buf->ByteOffset = (ULONG)VirtualAddress & 0xFFFUL;

    /* Fill PFN array at offset 0x1C (immediately after the MDL fixed fields).
       NT drivers read MDL->PfnArray[i] to get physical page frame numbers
       for DMA. Without this, the PFN is 0 and DMA targets PA 0. */
    {
        ULONG nPages = ((ULONG)VirtualAddress + Length + PAGESIZE - 1) / PAGESIZE
                     - ((ULONG)VirtualAddress / PAGESIZE);
        PULONG pfn = (PULONG)((PUCHAR)buf + 0x1C);
        ULONG i;
        ULONG baseVa = (ULONG)VirtualAddress & ~0xFFFUL;
        for (i = 0; i < nPages && i < (PAGESIZE - 0x1C) / sizeof(ULONG); i++) {
            ULONG pageVa = baseVa + (i * PAGESIZE);
            ULONG physAddr = ndis_va_to_pa((PVOID)pageVa);
            if (!physAddr) {
                physAddr = VxD_GetPhysAddr((PVOID)pageVa);
            }
            pfn[i] = physAddr >> 12;  /* PFN = physical page frame number */
        }
        buf->Size = (USHORT)(sizeof(NDIS_BUFFER) + nPages * sizeof(ULONG));
        ndis_log_hex("NDIS: AllocBuffer VA=", (ULONG)VirtualAddress, "");
        ndis_log_hex(" PFN[0]=", pfn[0], "");
        ndis_log_hex(" PA=", pfn[0] << 12, "\r\n");
    }

    *Buffer = (PVOID)buf;
    *Status = NDIS_STATUS_SUCCESS;
}

/*
 * NdisFreeBuffer
 */
static void __stdcall ndis_FreeBuffer(PVOID Buffer)
{
    if (Buffer) {
        VxD_PageFree(Buffer);
    }
}

/*
 * NdisTerminateWrapper
 *
 * Called if DriverEntry fails after NdisInitializeWrapper.
 */
static void __stdcall ndis_TerminateWrapper(
    NDIS_HANDLE NdisWrapperHandle,
    PVOID SystemSpecific)
{
    VxD_Debug_Printf("NDIS: NdisTerminateWrapper\r\n");
}

/*
 * NdisMIndicateStatus / NdisMIndicateStatusComplete
 *
 * Called by miniport to indicate status changes (link up/down, etc.)
 * These go through the import table, not inline dereference.
 */
static void __stdcall ndis_MIndicateStatus(
    NDIS_HANDLE MiniportHandle,
    NDIS_STATUS GeneralStatus,
    PVOID StatusBuffer,
    ULONG StatusBufferSize)
{
    ndis_log_hex("NDIS: NdisMIndicateStatus=", GeneralStatus, "\r\n");
}

static void __stdcall ndis_MIndicateStatusComplete(NDIS_HANDLE MiniportHandle)
{
    VxD_Debug_Printf("NDIS: NdisMIndicateStatusComplete\r\n");
}

/*
 * NdisMQueryInformationComplete
 *
 * Called by miniport when an async query completes.
 */
static void __stdcall ndis_MQueryInformationComplete(
    NDIS_HANDLE MiniportHandle,
    NDIS_STATUS Status)
{
    ndis_log_hex("NDIS: NdisMQueryInformationComplete status=", Status, "\r\n");
}

/*
 * NdisMSetInformationComplete
 */
static void __stdcall ndis_MSetInformationComplete(
    NDIS_HANDLE MiniportHandle,
    NDIS_STATUS Status)
{
    ndis_log_hex("NDIS: NdisMSetInformationComplete status=", Status, "\r\n");
}

/*
 * NdisMTransferDataComplete
 */
static void __stdcall ndis_MTransferDataComplete(
    NDIS_HANDLE MiniportHandle,
    PVOID Packet,
    NDIS_STATUS Status,
    ULONG BytesTransferred)
{
    ndis_log_hex("NDIS: NdisMTransferDataComplete bytes=", BytesTransferred, "\r\n");
}

/*
 * NdisMIndicateReceivePacket
 *
 * NDIS 4.0 packet-array receive indication (goes through import).
 */
static void __stdcall ndis_MIndicateReceivePacket(
    NDIS_HANDLE MiniportHandle,
    PVOID *PacketArray,
    ULONG NumberOfPackets)
{
    ULONG i;
    ndis_log_hex("NDIS: NdisMIndicateReceivePacket n=", NumberOfPackets, "\r\n");

    for (i = 0; i < NumberOfPackets && !g_rx_captured; i++) {
        PVOID pkt = PacketArray[i];
        NDIS_BUFFER *buf;
        PUCHAR data;
        ULONG len;

        if (!pkt) continue;

        /* Get first buffer from packet: Head is at PKT_HEAD offset */
        buf = (NDIS_BUFFER *)PKT_PTR_AT(pkt, PKT_HEAD);
        if (!buf) continue;

        data = (PUCHAR)buf->MappedSystemVa;
        len = buf->ByteCount;
        if (!data || len == 0) continue;

        ndis_log_hex("NDIS: RxPkt buf VA=", (ULONG)data, "");
        ndis_log_hex(" len=", len, "\r\n");

        /* Capture: first 14 bytes are Ethernet header, rest is payload */
        {
            ULONG hcopy = (len >= 14) ? 14 : len;
            ULONG dcopy = (len > 14) ? len - 14 : 0;
            if (hcopy > sizeof(g_rx_capture_hdr)) hcopy = sizeof(g_rx_capture_hdr);
            if (dcopy > sizeof(g_rx_capture_data)) dcopy = sizeof(g_rx_capture_data);
            ndis_memcpy(g_rx_capture_hdr, data, hcopy);
            g_rx_capture_hdr_len = hcopy;
            if (dcopy > 0) {
                ndis_memcpy(g_rx_capture_data, data + 14, dcopy);
                g_rx_capture_data_len = dcopy;
            }
            g_rx_captured = 1;
            VxD_Debug_Printf("NDIS: *** RX FRAME CAPTURED (IndicateReceivePacket) ***\r\n");
        }

        /* Call ReturnPacket if driver registered one */
        if (g_ndis_miniport.ReturnPacketHandler) {
            typedef void (__stdcall *PFN_RETURN_PKT)(NDIS_HANDLE, PVOID);
            PFN_RETURN_PKT pfnReturn = (PFN_RETURN_PKT)g_ndis_miniport.ReturnPacketHandler;
            pfnReturn(g_ndis_miniport.AdapterContext, pkt);
        }
    }
}

/*
 * NdisMRegisterAdapterShutdownHandler / NdisMDeregisterAdapterShutdownHandler
 *
 * Miniport registers a shutdown callback. We just store it.
 */
static PVOID g_ndis_shutdown_handler = NULL;
static PVOID g_ndis_shutdown_context = NULL;

static void __stdcall ndis_MRegisterAdapterShutdownHandler(
    NDIS_HANDLE MiniportHandle,
    PVOID ShutdownContext,
    PVOID ShutdownHandler)
{
    VxD_Debug_Printf("NDIS: RegisterAdapterShutdownHandler\r\n");
    g_ndis_shutdown_handler = ShutdownHandler;
    g_ndis_shutdown_context = ShutdownContext;
}

static void __stdcall ndis_MDeregisterAdapterShutdownHandler(
    NDIS_HANDLE MiniportHandle)
{
    VxD_Debug_Printf("NDIS: DeregisterAdapterShutdownHandler\r\n");
    g_ndis_shutdown_handler = NULL;
    g_ndis_shutdown_context = NULL;
}

/*
 * NdisMSynchronizeWithInterrupt
 *
 * Calls a function synchronized with the ISR (raise IRQL equivalent).
 * In our single-threaded VxD context, just call the function directly.
 */
static BOOLEAN __stdcall ndis_MSynchronizeWithInterrupt(
    PVOID NdisInterruptHandle,
    PVOID SynchronizeFunction,
    PVOID SynchronizeContext)
{
    typedef BOOLEAN (__stdcall *SYNC_FUNC)(PVOID);
    SYNC_FUNC fn = (SYNC_FUNC)SynchronizeFunction;

    VxD_Debug_Printf("NDIS: SynchronizeWithInterrupt\r\n");
    if (fn) {
        return fn(SynchronizeContext);
    }
    return FALSE;
}

/*
 * NdisStallExecution
 *
 * Stalls for a given number of microseconds.
 * Use port 0x80 reads (each ~1us on ISA bus).
 */
static void __stdcall ndis_StallExecution(ULONG MicrosecondsToStall)
{
    if (MicrosecondsToStall > 0x10000000UL) {
        ndis_log_hex("NDIS: StallExec SUSPECT arg=", MicrosecondsToStall, " (MB fallback?)\r\n");
        return;
    }
    {
        ULONG i;
        for (i = 0; i < MicrosecondsToStall; i++) {
            _port_stall();
        }
    }
}

/*
 * NdisMoveMemory
 */
static void __stdcall ndis_MoveMemory(PVOID Dest, PVOID Src, ULONG Length)
{
    ndis_memcpy(Dest, Src, Length);
}

/*
 * NdisZeroMemory
 */
static void __stdcall ndis_ZeroMemory(PVOID Dest, ULONG Length)
{
    ndis_memset(Dest, 0, Length);
}

/*
 * NdisEqualMemory
 */
static BOOLEAN __stdcall ndis_EqualMemory(PVOID Src1, PVOID Src2, ULONG Length)
{
    UCHAR *a = (UCHAR *)Src1;
    UCHAR *b = (UCHAR *)Src2;
    ULONG i;
    for (i = 0; i < Length; i++) {
        if (a[i] != b[i]) return FALSE;
    }
    return TRUE;
}

/*
 * NdisWriteErrorLogEntry
 *
 * Miniport logs an error. We just print it.
 */
static void __stdcall ndis_WriteErrorLogEntry(
    NDIS_HANDLE MiniportHandle,
    ULONG ErrorCode,
    ULONG NumberOfErrorValues)
{
    ndis_log_hex("NDIS: WriteErrorLogEntry code=", ErrorCode, "");
    ndis_log_hex(" nvals=", NumberOfErrorValues, "\r\n");
}

/*
 * NdisReadNetworkAddress
 *
 * Returns the network-level (configured) MAC address.
 * If no override is configured, return NDIS_STATUS_FAILURE and the
 * miniport falls back to the hardware address.
 */
static void __stdcall ndis_ReadNetworkAddress(
    NDIS_STATUS *Status,
    PVOID *NetworkAddress,
    PULONG NetworkAddressLength,
    NDIS_HANDLE ConfigurationHandle)
{
    VxD_Debug_Printf("NDIS: NdisReadNetworkAddress -> no override\r\n");
    if (Status) *Status = NDIS_STATUS_FAILURE;
    if (NetworkAddress) *NetworkAddress = NULL;
    if (NetworkAddressLength) *NetworkAddressLength = 0;
}

/*
 * Timer structure layout (shared by NdisInitializeTimer and NdisMInitializeTimer)
 *
 * NDIS_MINIPORT_TIMER (WinXP, total ~0x54 bytes):
 *   +0x00: KTIMER (0x28 bytes)
 *   +0x28: KDPC (0x1C bytes)
 *   +0x44: TimerFunction (PVOID)
 *   +0x48: TimerContext (PVOID)
 *   +0x4C: Miniport (PNDIS_MINIPORT_BLOCK)
 *   +0x50: NextDeferredTimer (PNDIS_MINIPORT_TIMER)
 */
#define NDIS_TIMER_SIZE     0x54
#define NDIS_TIMER_FUNC     0x44
#define NDIS_TIMER_CTX      0x48
#define NDIS_TIMER_MINIPORT 0x4C
#define NDIS_TIMER_NEXT     0x50

/*
 * NdisInitializeTimer / NdisSetTimer / NdisCancelTimer
 */
static UCHAR g_ndis_timer_dummy[32]; /* non-NULL timer object */

static void __stdcall ndis_InitializeTimer(
    PVOID NdisTimer,    /* OUT: PNDIS_MINIPORT_TIMER */
    NDIS_HANDLE MiniportAdapterHandle,
    PVOID TimerFunction,
    PVOID FunctionContext)
{
    VxD_Debug_Printf("NDIS: NdisInitializeTimer\r\n");
}

static void __stdcall ndis_SetTimer(PVOID NdisTimer, ULONG MillisecondsToDelay)
{
    ndis_log_hex("NDIS: NdisSetTimer ms=", MillisecondsToDelay,
                 " (STUB: timer will NOT fire!)\r\n");
    /* TODO: Wire to VTD_Set_Global_Time_Out or similar */
}

static void __stdcall ndis_CancelTimer(PVOID NdisTimer, BOOLEAN *TimerCancelled)
{
    VxD_Debug_Printf("NDIS: NdisCancelTimer (stub)\r\n");
    if (TimerCancelled) *TimerCancelled = TRUE;
}

/*
 * NdisMInitializeTimer / NdisMSetPeriodicTimer / NdisMCancelTimer
 *
 * NDIS 4.0/5.0 miniport timer. Initializes the NDIS_MINIPORT_TIMER
 * structure so the driver doesn't jump through uninitialized pointers.
 */
static ULONG g_timer_init_count = 0;
static void __stdcall ndis_MInitializeTimer(
    PVOID NdisTimer,
    NDIS_HANDLE MiniportAdapterHandle,
    PVOID TimerFunction,
    PVOID FunctionContext)
{
    g_timer_init_count++;
    ndis_log_hex("NDIS: NdisMInitializeTimer timer=", (ULONG)NdisTimer, "");
    ndis_log_hex(" func=", (ULONG)TimerFunction, "");
    ndis_log_hex(" ctx=", (ULONG)FunctionContext, "\r\n");

    if (NdisTimer) {
        /* Zero 0x50 bytes of the timer structure. XP NDIS_MINIPORT_TIMER:
           KTIMER (0x28) + KDPC (0x20) + func/ctx/miniport/next (0x10) = 0x58
           Use 0x50 to be conservative (timer4 at +0xB8 + 0x50 = +0x108,
           well within the 0xB98-byte adapter context allocation). */
        ndis_memset(NdisTimer, 0, 0x50);
    }
}

static void __stdcall ndis_MSetPeriodicTimer(
    PVOID NdisTimer, ULONG MillisecondsPeriod)
{
    ndis_log_hex("NDIS: NdisMSetPeriodicTimer ms=", MillisecondsPeriod,
                 " (STUB)\r\n");
}

static void __stdcall ndis_MCancelTimer(PVOID NdisTimer, BOOLEAN *TimerCancelled)
{
    VxD_Debug_Printf("NDIS: NdisMCancelTimer (stub)\r\n");
    if (TimerCancelled) *TimerCancelled = TRUE;
}

/*
 * NdisReadEisaSlotInformation
 *
 * Some miniports check for EISA bus. Return failure.
 */
static NDIS_STATUS __stdcall ndis_ReadEisaSlotInformation(
    NDIS_HANDLE NdisAdapterHandle,
    PULONG SlotNumber,
    PVOID EisaData)
{
    VxD_Debug_Printf("NDIS: NdisReadEisaSlotInformation -> NOT_SUPPORTED\r\n");
    return NDIS_STATUS_NOT_SUPPORTED;
}

/* ================================================================
 * HAL.dll Function Implementations
 *
 * The 9 HAL.dll imports are simple port I/O wrappers. NT miniports
 * call these instead of ScsiPort's port functions.
 * ================================================================ */

static ULONG g_hal_io_count = 0;

static UCHAR __stdcall hal_ReadPortUchar(PUCHAR Port)
{
    UCHAR val = _port_inb((unsigned short)(ULONG)Port);
    if (g_hal_io_count < 500) {
        ndis_log_hex("HAL:INb ", (ULONG)Port, "");
        ndis_log_hex("=", (ULONG)val, "\r\n");
        g_hal_io_count++;
    }
    return val;
}

static USHORT __stdcall hal_ReadPortUshort(PUSHORT Port)
{
    return _port_inw((unsigned short)(ULONG)Port);
}

static void __stdcall hal_ReadPortBufferUchar(PUCHAR Port, PUCHAR Buffer, ULONG Count)
{
    if (g_hal_io_count < 200) {
        ndis_log_hex("HAL:INbuf ", (ULONG)Port, "");
        ndis_log_hex(" cnt=", Count, "\r\n");
        g_hal_io_count++;
    }
    _port_rep_insb((unsigned short)(ULONG)Port, Buffer, Count);
}

static void __stdcall hal_ReadPortBufferUshort(PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    _port_rep_insw((unsigned short)(ULONG)Port, Buffer, Count);
}

static void __stdcall hal_ReadPortBufferUlong(PULONG Port, PULONG Buffer, ULONG Count)
{
    _port_rep_insd((unsigned short)(ULONG)Port, Buffer, Count);
}

static void __stdcall hal_WritePortUchar(PUCHAR Port, UCHAR Value)
{
    if (g_hal_io_count < 500) {
        ndis_log_hex("HAL:OUTb ", (ULONG)Port, "");
        ndis_log_hex("=", (ULONG)Value, "\r\n");
        g_hal_io_count++;
    }
    _port_outb((unsigned short)(ULONG)Port, Value);
}

static void __stdcall hal_WritePortUshort(PUSHORT Port, USHORT Value)
{
    _port_outw((unsigned short)(ULONG)Port, Value);
}

static void __stdcall hal_WritePortBufferUchar(PUCHAR Port, PUCHAR Buffer, ULONG Count)
{
    _port_rep_outsb((unsigned short)(ULONG)Port, Buffer, Count);
}

static void __stdcall hal_WritePortBufferUshort(PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    _port_rep_outsw((unsigned short)(ULONG)Port, Buffer, Count);
}

static void __stdcall hal_WritePortBufferUlong(PULONG Port, PULONG Buffer, ULONG Count)
{
    _port_rep_outsd((unsigned short)(ULONG)Port, Buffer, Count);
}

static ULONG __stdcall hal_ReadPortUlong(PULONG Port)
{
    ULONG val = _port_ind((unsigned short)(ULONG)Port);
    if (g_hal_io_count < 500) {
        ndis_log_hex("HAL:INd ", (ULONG)Port, "");
        ndis_log_hex("=", val, "\r\n");
        g_hal_io_count++;
    }
    return val;
}

static void __stdcall hal_WritePortUlong(PULONG Port, ULONG Value)
{
    if (g_hal_io_count < 500) {
        ndis_log_hex("HAL:OUTd ", (ULONG)Port, "");
        ndis_log_hex("=", Value, "\r\n");
        g_hal_io_count++;
    }
    _port_outd((unsigned short)(ULONG)Port, Value);
}

/*
 * KeStallExecutionProcessor
 *
 * Stalls for N microseconds. Same as NdisStallExecution.
 */
static void __stdcall hal_KeStallExecutionProcessor(ULONG Microseconds)
{
    ULONG i;
    for (i = 0; i < Microseconds; i++) {
        _port_stall();
    }
}

/* ================================================================
 * Additional NDIS functions needed by ne2000.sys
 * ================================================================ */

/* DPC-level spinlock stubs (single-CPU VxD, no preemption) */
static void __stdcall ndis_DprAcquireSpinLock(PVOID SpinLock) { (void)SpinLock; }
static void __stdcall ndis_DprReleaseSpinLock(PVOID SpinLock) { (void)SpinLock; }

/* EthFilterDpr receive indication — internal NDIS filter function.
   rtl8029.sys calls this instead of using the MiniportBlock macro.
   Route to our EthRxIndicate/Complete handlers. */
/* NT DDK EthFilterDprIndicateReceive has 8 params:
   Filter, MacReceiveContext, Address(6-byte dst MAC),
   HeaderBuffer, HeaderBufferSize, LookAheadBuffer,
   LookAheadBufferSize, PacketSize */
static void __stdcall ndis_EthFilterDprIndicateReceive(
    PVOID EthFilter, PVOID MacReceiveContext, PUCHAR Address,
    PUCHAR HeaderBuffer, ULONG HeaderBufferSize,
    PVOID LookAheadBuffer, ULONG LookAheadBufferSize,
    ULONG PacketSize)
{
    ndis_log_hex("NDIS: EthFilterDprIndRcv hdrSz=", HeaderBufferSize, "");
    ndis_log_hex(" pktSz=", PacketSize, "");
    ndis_log_hex(" laSize=", LookAheadBufferSize, "\r\n");

    if (!g_rx_captured && HeaderBuffer && HeaderBufferSize > 0) {
        ULONG hcopy = HeaderBufferSize;
        ULONG dcopy = LookAheadBufferSize;
        if (hcopy > sizeof(g_rx_capture_hdr)) hcopy = sizeof(g_rx_capture_hdr);
        if (dcopy > sizeof(g_rx_capture_data)) dcopy = sizeof(g_rx_capture_data);
        ndis_memcpy(g_rx_capture_hdr, HeaderBuffer, hcopy);
        g_rx_capture_hdr_len = hcopy;
        if (LookAheadBuffer && dcopy > 0) {
            ndis_memcpy(g_rx_capture_data, (PUCHAR)LookAheadBuffer, dcopy);
            g_rx_capture_data_len = dcopy;
        }
        g_rx_captured = 1;
        VxD_Debug_Printf("NDIS: *** RX FRAME CAPTURED (DprPath) ***\r\n");
    }
}

static void __stdcall ndis_EthFilterDprIndicateReceiveComplete(PVOID EthFilter)
{
    VxD_Debug_Printf("NDIS: EthFilterDprIndRcvComplete\r\n");
}

/*
 * NdisMPciAssignResources — returns PCI resource assignments.
 *
 * Scans PCI bus for the network adapter, reads its BAR0 and IRQ,
 * and builds a minimal CM_RESOURCE_LIST.
 *
 * CM_RESOURCE_LIST layout (simplified, x86):
 *   +0x00 ULONG Count (=1)
 *   +0x04 CM_FULL_RESOURCE_DESCRIPTOR:
 *     +0x00 ULONG InterfaceType (=5, PCIBus)
 *     +0x04 ULONG BusNumber
 *     +0x08 CM_PARTIAL_RESOURCE_LIST:
 *       +0x00 USHORT Version (=1)
 *       +0x02 USHORT Revision (=1)
 *       +0x04 ULONG  Count (number of descriptors)
 *       +0x08 CM_PARTIAL_RESOURCE_DESCRIPTOR[]:
 *         +0x00 UCHAR  Type (1=Port, 2=Interrupt, 3=Memory)
 *         +0x01 UCHAR  ShareDisposition
 *         +0x02 USHORT Flags
 *         +0x04 ULONG  Start.LowPart
 *         +0x08 ULONG  Start.HighPart
 *         +0x0C ULONG  Length (or Level/Vector/Affinity for IRQ)
 * Each CM_PARTIAL_RESOURCE_DESCRIPTOR is 16 bytes (0x10).
 */
/* CM_RESOURCE_* defined earlier in file */
/* NdisMPciAssignResources returns a stripped format (no InterfaceType/BusNumber/Version):
   +0x00: Count (ULONG, always 1)
   +0x04: DescriptorCount (ULONG)
   +0x08: Descriptors[] (16 bytes each) */
#define CM_RES_HEADER_SIZE    8   /* Count + DescriptorCount */
#define CM_RES_DESC_SIZE      16
#define CM_RES_MAX_DESCS      4

static UCHAR g_ndis_pci_resources[CM_RES_HEADER_SIZE + CM_RES_MAX_DESCS * CM_RES_DESC_SIZE];

static NDIS_STATUS __stdcall ndis_MPciAssignResources(
    NDIS_HANDLE MiniportAdapterHandle,
    ULONG SlotNumber,
    PVOID *AssignedResources)
{
    ULONG bus, dev, func, cfgAddr, val;
    USHORT vendor_id, device_id;
    ULONG bar0, irq_line;
    PUCHAR res;
    int desc_count;
    BOOLEAN found = FALSE;

    VxD_Debug_Printf("NDIS: NdisMPciAssignResources\r\n");
    if (!AssignedResources) return NDIS_STATUS_FAILURE;

    /* Scan PCI bus for network controller (class 0x02) */
    for (bus = 0; bus < 4 && !found; bus++) {
        for (dev = 0; dev < 32 && !found; dev++) {
            for (func = 0; func < 8 && !found; func++) {
                cfgAddr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8);
                _port_outd(0xCF8, cfgAddr);
                val = _port_ind(0xCFC);
                vendor_id = (USHORT)(val & 0xFFFF);
                device_id = (USHORT)(val >> 16);
                if (vendor_id == 0xFFFF || vendor_id == 0) continue;

                _port_outd(0xCF8, cfgAddr | 0x08);
                val = _port_ind(0xCFC);
                if ((val >> 24) == 0x02) {  /* Class 0x02 = Network */
                    ndis_log_hex("NDIS: PCI NIC VID=", (ULONG)vendor_id, "");
                    ndis_log_hex(" DID=", (ULONG)device_id, "");
                    ndis_log_hex(" bus=", bus, "");
                    ndis_log_hex(" dev=", dev, "\r\n");

                    /* Read BAR0 */
                    _port_outd(0xCF8, cfgAddr | 0x10);
                    bar0 = _port_ind(0xCFC);

                    /* Read IRQ line */
                    _port_outd(0xCF8, cfgAddr | 0x3C);
                    irq_line = _port_ind(0xCFC) & 0xFF;

                    ndis_log_hex("  BAR0=", bar0, "");
                    ndis_log_hex(" IRQ=", irq_line, "\r\n");

                    g_ndis_miniport.PciBus = bus;
                    g_ndis_miniport.PciDev = dev;
                    g_ndis_miniport.PciFunc = func;
                    g_ndis_miniport.PciFound = TRUE;

                    /* Update config with PCI-detected values so NdisReadConfiguration
                       returns correct IoBaseAddress/InterruptNumber for PCI NICs. */
                    if ((bar0 & 1) && (bar0 & ~3) != 0) {
                        g_ndis_config[0].value = bar0 & ~3;
                        ndis_log_hex("NDIS: Config IoBaseAddress updated to ", bar0 & ~3, "\r\n");
                    }
                    if (irq_line != 0 && irq_line != 0xFF) {
                        g_ndis_config[1].value = irq_line;
                        ndis_log_hex("NDIS: Config IRQ updated to ", irq_line, "\r\n");
                    }

                    found = TRUE;
                }
            }
        }
    }

    if (!found) {
        VxD_Debug_Printf("NDIS: No PCI NIC found\r\n");
        *AssignedResources = NULL;
        return NDIS_STATUS_FAILURE;
    }

    /* Build stripped resource list (NdisMPciAssignResources format):
       +0x00: Count (always 1)
       +0x04: DescriptorCount
       +0x08: Descriptors[] */
    res = g_ndis_pci_resources;
    ndis_memset(res, 0, sizeof(g_ndis_pci_resources));
    desc_count = 0;

    *(ULONG *)(res + 0) = 1;        /* Count (resource lists, always 1) */
    /* DescriptorCount at +4 will be set after counting */

    /* I/O port resource (BAR0 with bit 0 = I/O space indicator) */
    if (bar0 & 1) {
        ULONG io_base = bar0 & 0xFFFC;
        PUCHAR d = res + CM_RES_HEADER_SIZE + desc_count * CM_RES_DESC_SIZE;
        d[0] = CM_RESOURCE_PORT;     /* Type */
        d[1] = 1;                    /* ShareDisposition = Shared */
        *(USHORT *)(d + 2) = 1;     /* Flags = IO */
        *(ULONG *)(d + 4) = io_base; /* Start.LowPart */
        *(ULONG *)(d + 8) = 0;      /* Start.HighPart */
        *(ULONG *)(d + 12) = 32;    /* Length (NE2000 uses 32 I/O ports) */
        desc_count++;
        ndis_log_hex("  Res: PORT base=", io_base, "");
        ndis_log_hex(" len=", 32, "\r\n");
    }

    /* Interrupt resource */
    if (irq_line > 0 && irq_line < 255) {
        PUCHAR d = res + CM_RES_HEADER_SIZE + desc_count * CM_RES_DESC_SIZE;
        d[0] = CM_RESOURCE_INTERRUPT; /* Type */
        d[1] = 1;                     /* ShareDisposition = Shared */
        *(USHORT *)(d + 2) = 1;      /* Flags = LevelSensitive */
        *(ULONG *)(d + 4) = irq_line; /* Level */
        *(ULONG *)(d + 8) = irq_line; /* Vector */
        *(ULONG *)(d + 12) = 0xFFFFFFFF; /* Affinity = all CPUs */
        desc_count++;
        ndis_log_hex("  Res: IRQ=", irq_line, "\r\n");
    }

    /* Set descriptor count at +0x04 */
    *(ULONG *)(res + 4) = (ULONG)desc_count;

    *AssignedResources = (PVOID)res;

    /* Also store in MiniportBlock.Resources (offset 0x6C) — some drivers
       read resources from the block rather than the output parameter. */
    MB_PTR_AT(0x6C) = (PVOID)res;

    ndis_log_hex("NDIS: PciAssignResources OK, ", (ULONG)desc_count, " descriptors\r\n");
    return NDIS_STATUS_SUCCESS;
}

static void __stdcall ndis_QueryBuffer(PVOID NdisBuffer, PVOID *VirtualAddress, PULONG Length)
{
    /* NdisBuffer is our fake MDL: StartVa at +0x10, ByteCount at +0x14, ByteOffset at +0x18 */
    if (NdisBuffer) {
        PUCHAR mdl = (PUCHAR)NdisBuffer;
        ULONG startVa = *(PULONG)(mdl + 0x10);
        ULONG byteCount = *(PULONG)(mdl + 0x14);
        ULONG byteOffset = *(PULONG)(mdl + 0x18);
        if (VirtualAddress) *VirtualAddress = (PVOID)(startVa + byteOffset);
        if (Length) *Length = byteCount;
    } else {
        if (VirtualAddress) *VirtualAddress = NULL;
        if (Length) *Length = 0;
    }
}

static void __stdcall ndis_QueryBufferOffset(PVOID NdisBuffer, PULONG Offset, PULONG Length)
{
    if (NdisBuffer) {
        PUCHAR mdl = (PUCHAR)NdisBuffer;
        if (Offset) *Offset = *(PULONG)(mdl + 0x18);
        if (Length) *Length = *(PULONG)(mdl + 0x14);
    } else {
        if (Offset) *Offset = 0;
        if (Length) *Length = 0;
    }
}

static ULONG __stdcall ndis_BufferToSpanPages(PVOID NdisBuffer)
{
    /* Return 1 for any buffer (single contiguous page assumption) */
    if (!NdisBuffer) return 0;
    return 1;
}

static ULONG __stdcall ndis_SystemProcessorCount(void)
{
    return 1;
}

/*
 * NdisQueryPacket — returns packet metadata from NDIS_PACKET_PRIVATE.
 * Any output pointer may be NULL.
 */
static void __stdcall ndis_QueryPacket(
    PVOID Packet,
    PULONG PhysicalBufferCount,
    PULONG BufferCount,
    PVOID *FirstBuffer,
    PULONG TotalPacketLength)
{
    if (!Packet) return;
    if (PhysicalBufferCount) *PhysicalBufferCount = PKT_ULONG_AT(Packet, PKT_PHYSICAL_COUNT);
    if (BufferCount) *BufferCount = PKT_ULONG_AT(Packet, PKT_COUNT);
    if (FirstBuffer) *FirstBuffer = PKT_PTR_AT(Packet, PKT_HEAD);
    if (TotalPacketLength) *TotalPacketLength = PKT_ULONG_AT(Packet, PKT_TOTAL_LENGTH);
}

/*
 * NdisGetFirstBufferFromPacket — fast path for first buffer info.
 */
static void __stdcall ndis_GetFirstBufferFromPacket(
    PVOID Packet,
    PVOID *FirstBuffer,
    PVOID *FirstBufferVA,
    PULONG FirstBufferLength,
    PULONG TotalBufferLength)
{
    NDIS_BUFFER *buf;
    if (!Packet) return;
    buf = (NDIS_BUFFER *)PKT_PTR_AT(Packet, PKT_HEAD);
    if (FirstBuffer) *FirstBuffer = (PVOID)buf;
    if (buf) {
        if (FirstBufferVA) *FirstBufferVA = buf->MappedSystemVa;
        if (FirstBufferLength) *FirstBufferLength = buf->ByteCount;
    } else {
        if (FirstBufferVA) *FirstBufferVA = NULL;
        if (FirstBufferLength) *FirstBufferLength = 0;
    }
    if (TotalBufferLength) *TotalBufferLength = PKT_ULONG_AT(Packet, PKT_TOTAL_LENGTH);
}

/*
 * NdisGetNextBuffer — walk the MDL chain.
 */
static void __stdcall ndis_GetNextBuffer(
    PVOID CurrentBuffer,
    PVOID *NextBuffer)
{
    if (!NextBuffer) return;
    if (CurrentBuffer) {
        *NextBuffer = (PVOID)((NDIS_BUFFER *)CurrentBuffer)->Next;
    } else {
        *NextBuffer = NULL;
    }
}

/*
 * NdisChainBufferAtBack — append buffer to packet's buffer chain.
 * Updates Head/Tail/TotalLength/Count/PhysicalCount in Private.
 */
static void ndis_chain_buffer_to_packet(PVOID Packet, NDIS_BUFFER *Buffer)
{
    NDIS_BUFFER *tail;
    ULONG total_len, count;

    if (!Packet || !Buffer) return;
    Buffer->Next = NULL;

    tail = (NDIS_BUFFER *)PKT_PTR_AT(Packet, PKT_TAIL);
    if (tail) {
        tail->Next = Buffer;
    } else {
        PKT_PTR_AT(Packet, PKT_HEAD) = (PVOID)Buffer;
    }
    PKT_PTR_AT(Packet, PKT_TAIL) = (PVOID)Buffer;

    total_len = PKT_ULONG_AT(Packet, PKT_TOTAL_LENGTH) + Buffer->ByteCount;
    count = PKT_ULONG_AT(Packet, PKT_COUNT) + 1;
    PKT_ULONG_AT(Packet, PKT_TOTAL_LENGTH) = total_len;
    PKT_ULONG_AT(Packet, PKT_COUNT) = count;
    PKT_ULONG_AT(Packet, PKT_PHYSICAL_COUNT) = count;
}

/* __stdcall wrapper for the import table */
static void __stdcall ndis_ChainBufferAtBack(PVOID Packet, PVOID Buffer)
{
    ndis_chain_buffer_to_packet(Packet, (NDIS_BUFFER *)Buffer);
}

static void __stdcall ndis_ChainBufferAtFront(PVOID Packet, PVOID Buffer)
{
    NDIS_BUFFER *buf = (NDIS_BUFFER *)Buffer;
    NDIS_BUFFER *old_head;
    if (!Packet || !buf) return;

    old_head = (NDIS_BUFFER *)PKT_PTR_AT(Packet, PKT_HEAD);
    buf->Next = old_head;
    PKT_PTR_AT(Packet, PKT_HEAD) = (PVOID)buf;
    if (!old_head) {
        PKT_PTR_AT(Packet, PKT_TAIL) = (PVOID)buf;
    }
    PKT_ULONG_AT(Packet, PKT_TOTAL_LENGTH) += buf->ByteCount;
    PKT_ULONG_AT(Packet, PKT_COUNT) += 1;
    PKT_ULONG_AT(Packet, PKT_PHYSICAL_COUNT) += 1;
}

static void __stdcall ndis_UnchainBufferAtFront(PVOID Packet, PVOID *Buffer)
{
    NDIS_BUFFER *head;
    if (!Packet || !Buffer) return;
    head = (NDIS_BUFFER *)PKT_PTR_AT(Packet, PKT_HEAD);
    if (head) {
        PKT_PTR_AT(Packet, PKT_HEAD) = (PVOID)head->Next;
        if (!head->Next) PKT_PTR_AT(Packet, PKT_TAIL) = NULL;
        PKT_ULONG_AT(Packet, PKT_TOTAL_LENGTH) -= head->ByteCount;
        PKT_ULONG_AT(Packet, PKT_COUNT) -= 1;
        PKT_ULONG_AT(Packet, PKT_PHYSICAL_COUNT) -= 1;
        head->Next = NULL;
        *Buffer = (PVOID)head;
    } else {
        *Buffer = NULL;
    }
}

static void __stdcall ndis_UnchainBufferAtBack(PVOID Packet, PVOID *Buffer)
{
    NDIS_BUFFER *head, *prev, *cur;
    if (!Packet || !Buffer) return;
    head = (NDIS_BUFFER *)PKT_PTR_AT(Packet, PKT_HEAD);
    if (!head) { *Buffer = NULL; return; }
    if (!head->Next) {
        PKT_PTR_AT(Packet, PKT_HEAD) = NULL;
        PKT_PTR_AT(Packet, PKT_TAIL) = NULL;
        PKT_ULONG_AT(Packet, PKT_TOTAL_LENGTH) -= head->ByteCount;
        PKT_ULONG_AT(Packet, PKT_COUNT) -= 1;
        PKT_ULONG_AT(Packet, PKT_PHYSICAL_COUNT) -= 1;
        head->Next = NULL;
        *Buffer = (PVOID)head;
        return;
    }
    prev = head;
    cur = head->Next;
    while (cur->Next) { prev = cur; cur = cur->Next; }
    prev->Next = NULL;
    PKT_PTR_AT(Packet, PKT_TAIL) = (PVOID)prev;
    PKT_ULONG_AT(Packet, PKT_TOTAL_LENGTH) -= cur->ByteCount;
    PKT_ULONG_AT(Packet, PKT_COUNT) -= 1;
    PKT_ULONG_AT(Packet, PKT_PHYSICAL_COUNT) -= 1;
    *Buffer = (PVOID)cur;
}

/*
 * NdisGetPacketFlags / NdisSetPacketFlags
 */
static ULONG __stdcall ndis_GetPacketFlags(PVOID Packet)
{
    if (!Packet) return 0;
    return PKT_ULONG_AT(Packet, PKT_FLAGS);
}

static void __stdcall ndis_SetPacketFlags(PVOID Packet, ULONG Flags)
{
    if (Packet) PKT_ULONG_AT(Packet, PKT_FLAGS) = Flags;
}

/* ================================================================
 * NDIS Import Function Table (for NDIS.SYS resolution)
 * ================================================================ */

static const IMPORT_FUNC_ENTRY ndis_funcs[] = {
    { "NdisInitializeWrapper",              (PVOID)ndis_InitializeWrapper },
    { "NdisMInitializeWrapper",             (PVOID)ndis_InitializeWrapper },
    { "NdisMRegisterMiniport",              (PVOID)ndis_MRegisterMiniport },
    { "NdisMSetAttributes",                 (PVOID)ndis_MSetAttributesEx },
    { "NdisMSetAttributesEx",               (PVOID)ndis_MSetAttributesEx },
    { "NdisMRegisterIoPortRange",           (PVOID)ndis_MRegisterIoPortRange },
    { "NdisMDeregisterIoPortRange",         (PVOID)ndis_MDeregisterIoPortRange },
    { "NdisMRegisterInterrupt",             (PVOID)ndis_MRegisterInterrupt },
    { "NdisMDeregisterInterrupt",           (PVOID)ndis_MDeregisterInterrupt },
    { "NdisAllocateMemory",                 (PVOID)ndis_AllocateMemory },
    { "NdisFreeMemory",                     (PVOID)ndis_FreeMemory },
    { "NdisMSendComplete",                  (PVOID)ndis_MSendComplete },
    { "NdisMMapIoSpace",                    (PVOID)ndis_MMapIoSpace },
    { "NdisMUnmapIoSpace",                  (PVOID)ndis_MUnmapIoSpace },
    { "NdisReadPciSlotInformation",         (PVOID)ndis_ReadPciSlotInformation },
    { "NdisWritePciSlotInformation",        (PVOID)ndis_WritePciSlotInformation },
    { "NdisReadMcaPosInformation",          (PVOID)ndis_ReadMcaPosInformation },
    { "NdisReadEisaSlotInformation",        (PVOID)ndis_ReadEisaSlotInformation },
    { "NdisOpenConfiguration",              (PVOID)ndis_OpenConfiguration },
    { "NdisCloseConfiguration",             (PVOID)ndis_CloseConfiguration },
    { "NdisReadConfiguration",              (PVOID)ndis_ReadConfiguration },
    { "NdisReadNetworkAddress",             (PVOID)ndis_ReadNetworkAddress },
    { "NdisAllocatePacketPool",             (PVOID)ndis_AllocatePacketPool },
    { "NdisAllocatePacketPoolEx",           (PVOID)ndis_AllocatePacketPool },
    { "NdisFreePacketPool",                 (PVOID)ndis_FreePacketPool },
    { "NdisAllocatePacket",                 (PVOID)ndis_AllocatePacket },
    { "NdisFreePacket",                     (PVOID)ndis_FreePacket },
    { "NdisAllocateBufferPool",             (PVOID)ndis_AllocateBufferPool },
    { "NdisFreeBufferPool",                 (PVOID)ndis_FreeBufferPool },
    { "NdisAllocateBuffer",                 (PVOID)ndis_AllocateBuffer },
    { "NdisFreeBuffer",                     (PVOID)ndis_FreeBuffer },
    { "NdisTerminateWrapper",               (PVOID)ndis_TerminateWrapper },
    { "NdisMIndicateStatus",                (PVOID)ndis_MIndicateStatus },
    { "NdisMIndicateStatusComplete",        (PVOID)ndis_MIndicateStatusComplete },
    { "NdisMQueryInformationComplete",      (PVOID)ndis_MQueryInformationComplete },
    { "NdisMSetInformationComplete",        (PVOID)ndis_MSetInformationComplete },
    { "NdisMTransferDataComplete",          (PVOID)ndis_MTransferDataComplete },
    { "NdisMIndicateReceivePacket",         (PVOID)ndis_MIndicateReceivePacket },
    { "NdisMRegisterAdapterShutdownHandler",(PVOID)ndis_MRegisterAdapterShutdownHandler },
    { "NdisMDeregisterAdapterShutdownHandler",(PVOID)ndis_MDeregisterAdapterShutdownHandler },
    { "NdisMSynchronizeWithInterrupt",      (PVOID)ndis_MSynchronizeWithInterrupt },
    { "NdisStallExecution",                 (PVOID)ndis_StallExecution },
    { "NdisMoveMemory",                     (PVOID)ndis_MoveMemory },
    { "NdisZeroMemory",                     (PVOID)ndis_ZeroMemory },
    { "NdisEqualMemory",                    (PVOID)ndis_EqualMemory },
    { "NdisWriteErrorLogEntry",             (PVOID)ndis_WriteErrorLogEntry },
    { "NdisInitializeTimer",                (PVOID)ndis_InitializeTimer },
    { "NdisSetTimer",                       (PVOID)ndis_SetTimer },
    { "NdisCancelTimer",                    (PVOID)ndis_CancelTimer },
    { "NdisMInitializeTimer",               (PVOID)ndis_MInitializeTimer },
    { "NdisMSetPeriodicTimer",              (PVOID)ndis_MSetPeriodicTimer },
    { "NdisMCancelTimer",                   (PVOID)ndis_MCancelTimer },
    { "NdisQueryBuffer",                    (PVOID)ndis_QueryBuffer },
    { "NdisQueryBufferOffset",              (PVOID)ndis_QueryBufferOffset },
    { "NDIS_BUFFER_TO_SPAN_PAGES",          (PVOID)ndis_BufferToSpanPages },
    { "NdisSystemProcessorCount",           (PVOID)ndis_SystemProcessorCount },
    { "NdisDprAcquireSpinLock",             (PVOID)ndis_DprAcquireSpinLock },
    { "NdisDprReleaseSpinLock",             (PVOID)ndis_DprReleaseSpinLock },
    { "NdisAllocateSpinLock",               (PVOID)ndis_DprAcquireSpinLock },
    { "NdisFreeSpinLock",                   (PVOID)ndis_DprReleaseSpinLock },
    { "NdisAcquireSpinLock",                (PVOID)ndis_DprAcquireSpinLock },
    { "NdisReleaseSpinLock",                (PVOID)ndis_DprReleaseSpinLock },
    { "EthFilterDprIndicateReceive",        (PVOID)ndis_EthFilterDprIndicateReceive },
    { "EthFilterDprIndicateReceiveComplete",(PVOID)ndis_EthFilterDprIndicateReceiveComplete },
    { "NdisMPciAssignResources",            (PVOID)ndis_MPciAssignResources },
    { "NdisQueryPacket",                    (PVOID)ndis_QueryPacket },
    { "NdisGetFirstBufferFromPacket",       (PVOID)ndis_GetFirstBufferFromPacket },
    { "NdisGetFirstBufferFromPacketSafe",   (PVOID)ndis_GetFirstBufferFromPacket },
    { "NdisGetNextBuffer",                  (PVOID)ndis_GetNextBuffer },
    { "NdisChainBufferAtBack",              (PVOID)ndis_ChainBufferAtBack },
    { "NdisChainBufferAtFront",             (PVOID)ndis_ChainBufferAtFront },
    { "NdisUnchainBufferAtFront",           (PVOID)ndis_UnchainBufferAtFront },
    { "NdisUnchainBufferAtBack",            (PVOID)ndis_UnchainBufferAtBack },
    { "NdisGetPacketFlags",                 (PVOID)ndis_GetPacketFlags },
    { "NdisSetPacketFlags",                 (PVOID)ndis_SetPacketFlags },
    { "NdisAllocateMemoryWithTag",          (PVOID)ndis_AllocateMemoryWithTag },
    { "NdisMAllocateSharedMemory",          (PVOID)ndis_MAllocateSharedMemory },
    { "NdisMFreeSharedMemory",              (PVOID)ndis_MFreeSharedMemory },
    { "NdisMAllocateMapRegisters",          (PVOID)ndis_MAllocateMapRegisters },
    { "NdisMFreeMapRegisters",              (PVOID)ndis_MFreeMapRegisters },
    { "NdisMQueryAdapterResources",         (PVOID)ndis_MQueryAdapterResources },
    { "NdisReadPcmciaAttributeMemory",     (PVOID)ndis_safe_ret0_3arg },
    { NULL, NULL }
};

/* ================================================================
 * HAL Import Function Table (for HAL.dll resolution)
 * ================================================================ */

static const IMPORT_FUNC_ENTRY hal_funcs[] = {
    { "READ_PORT_UCHAR",            (PVOID)hal_ReadPortUchar },
    { "READ_PORT_USHORT",           (PVOID)hal_ReadPortUshort },
    { "READ_PORT_BUFFER_UCHAR",     (PVOID)hal_ReadPortBufferUchar },
    { "READ_PORT_BUFFER_USHORT",    (PVOID)hal_ReadPortBufferUshort },
    { "READ_PORT_BUFFER_ULONG",     (PVOID)hal_ReadPortBufferUlong },
    { "WRITE_PORT_UCHAR",           (PVOID)hal_WritePortUchar },
    { "WRITE_PORT_USHORT",          (PVOID)hal_WritePortUshort },
    { "WRITE_PORT_BUFFER_UCHAR",    (PVOID)hal_WritePortBufferUchar },
    { "WRITE_PORT_BUFFER_USHORT",   (PVOID)hal_WritePortBufferUshort },
    { "WRITE_PORT_BUFFER_ULONG",    (PVOID)hal_WritePortBufferUlong },
    { "READ_PORT_ULONG",            (PVOID)hal_ReadPortUlong },
    { "WRITE_PORT_ULONG",           (PVOID)hal_WritePortUlong },
    { "KeStallExecutionProcessor",  (PVOID)hal_KeStallExecutionProcessor },
    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Structures (for register_port_driver)
 * ================================================================ */

static PORT_DRIVER_SHIM ndis_shim = {
    "NDIS.SYS",        /* dll_name */
    ndis_funcs,         /* func_table */
    0,                  /* func_count (informational, uses null terminator) */
    NULL,               /* bridge_init */
    NULL,               /* bridge_io */
    NULL                /* bridge_cleanup */
};

static PORT_DRIVER_SHIM hal_shim = {
    "HAL.dll",          /* dll_name */
    hal_funcs,          /* func_table */
    0,                  /* func_count */
    NULL,               /* bridge_init */
    NULL,               /* bridge_io */
    NULL                /* bridge_cleanup */
};

/* ================================================================
 * Public Registration
 * ================================================================ */

void ndis_shim_register(void)
{
    VxD_Debug_Printf("NDIS: Registering NDIS.SYS shim\r\n");
    register_port_driver(&ndis_shim);

    VxD_Debug_Printf("NDIS: Registering HAL.dll shim\r\n");
    register_port_driver(&hal_shim);
}

/* ================================================================
 * Test Entrypoint
 *
 * Exercises the full miniport init path:
 *   1. Register NDIS + HAL shims
 *   2. PE-load ne2000.sys
 *   3. Call DriverEntry (which calls NdisInitializeWrapper +
 *      NdisMRegisterMiniport)
 *   4. Call MiniportInitialize with our miniport block as handle
 *   5. Call MiniportQueryInformation(OID_GEN_SUPPORTED_LIST)
 *   6. Report results
 *
 * The ne2000.sys image must be provided as an embedded byte array
 * (same pattern as ATAPI_EMBEDDED.H / SYM_HI_EMBEDDED.H).
 * Define NDIS_TEST_EMBEDDED to include the embedded image.
 * ================================================================ */

#ifdef NDIS_TEST_RTL8139
#include "RTL8139_EMBEDDED.H"
#define NE2000_IMAGE      rtl8139_embedded_data
#define NE2000_IMAGE_SIZE sizeof(rtl8139_embedded_data)
#define NDIS_TEST_EMBEDDED 1
#define NDIS_DRIVER_NAME "rtl8139.sys"
#elif defined(NDIS_TEST_NE2000_XP)
#include "NE2000_XP_EMBEDDED.H"
#define NE2000_IMAGE      ne2000_xp_embedded_data
#define NE2000_IMAGE_SIZE sizeof(ne2000_xp_embedded_data)
#define NDIS_TEST_EMBEDDED 1
#define NDIS_DRIVER_NAME "ne2000.sys"
#elif defined(NDIS_TEST_RTL8029)
#include "RTL8029_EMBEDDED.H"
#define NE2000_IMAGE      rtl8029_embedded_data
#define NE2000_IMAGE_SIZE sizeof(rtl8029_embedded_data)
#define NDIS_TEST_EMBEDDED 1
#define NDIS_DRIVER_NAME "rtl8029.sys"
#elif defined(NDIS_TEST_EMBEDDED)
#include "NE2000_EMBEDDED.H"
#define NE2000_IMAGE      ne2000_embedded_data
#define NE2000_IMAGE_SIZE sizeof(ne2000_embedded_data)
#define NDIS_DRIVER_NAME "ne2000.sys"
#endif

/* DriverEntry prototype for NT kernel drivers:
   NTSTATUS DriverEntry(PVOID DriverObject, PVOID RegistryPath); */
typedef ULONG (__stdcall *PFN_DRIVER_ENTRY)(PVOID, PVOID);

/* MiniportInitialize prototype:
   NDIS_STATUS MiniportInitialize(
     OUT PNDIS_STATUS OpenErrorStatus,
     OUT PUINT SelectedMediumIndex,
     IN  PNDIS_MEDIUM MediumArray,
     IN  UINT MediumArraySize,
     IN  NDIS_HANDLE MiniportAdapterHandle,
     IN  NDIS_HANDLE WrapperConfigurationContext); */
typedef NDIS_STATUS (__stdcall *PFN_MINIPORT_INIT)(
    NDIS_STATUS *, ULONG *, ULONG *, ULONG,
    NDIS_HANDLE, NDIS_HANDLE);

/* MiniportQueryInformation prototype:
   NDIS_STATUS MiniportQueryInformation(
     IN  NDIS_HANDLE MiniportAdapterContext,
     IN  NDIS_OID Oid,
     IN  PVOID InformationBuffer,
     IN  ULONG InformationBufferLength,
     OUT PULONG BytesWritten,
     OUT PULONG BytesNeeded); */
typedef NDIS_STATUS (__stdcall *PFN_MINIPORT_QUERY)(
    NDIS_HANDLE, ULONG, PVOID, ULONG, PULONG, PULONG);

int ndis_test_ne2000(void)
{
#ifdef NDIS_TEST_EMBEDDED
    PVOID entry_point = NULL;
    PVOID image_base = NULL;
    PFN_DRIVER_ENTRY pfnDriverEntry;
    PFN_MINIPORT_INIT pfnInit;
    PFN_MINIPORT_QUERY pfnQuery;
    NDIS_STATUS status;
    ULONG driver_status;
    int rc;

    /* Fake DriverObject and RegistryPath (miniport ignores these;
       it gets its info from NdisInitializeWrapper) */
    UCHAR fake_driver_object[16];
    UCHAR fake_registry_path[16];

    /* MiniportInitialize parameters */
    NDIS_STATUS open_error_status;
    ULONG selected_medium_index;
    ULONG medium_array[1];
    UCHAR query_buffer[256];
    ULONG bytes_written, bytes_needed;

    VxD_Debug_Printf("NDIS: PCI pre-scan starting...\r\n");
    {
        ULONG bus, dev, func, cfgAddr, val;
        for (bus = 0; bus < 4; bus++)
            for (dev = 0; dev < 32; dev++)
                for (func = 0; func < 8; func++) {
                    cfgAddr = 0x80000000 | (bus << 16) | (dev << 11) | (func << 8);
                    _port_outd(0xCF8, cfgAddr);
                    val = _port_ind(0xCFC);
                    if ((val & 0xFFFF) == 0xFFFF || (val & 0xFFFF) == 0) continue;
                    _port_outd(0xCF8, cfgAddr | 0x08);
                    val = _port_ind(0xCFC);
                    if ((val >> 24) == 0x02) {
                        ULONG bar0, irq;
                        _port_outd(0xCF8, cfgAddr | 0x10);
                        bar0 = _port_ind(0xCFC);
                        _port_outd(0xCF8, cfgAddr | 0x3C);
                        irq = _port_ind(0xCFC) & 0xFF;
                        if (bar0 & 1) {
                            g_ndis_config[0].value = bar0 & ~3;
                            g_ndis_config[1].value = irq;
                            ndis_log_hex("NDIS: PCI NIC pre-scan: IO=", bar0 & ~3, "");
                            ndis_log_hex(" IRQ=", irq, "\r\n");
                        }
                        goto pci_done;
                    }
                }
        pci_done: ;
    }

    VxD_Debug_Printf("=== NDIS NE2000 TEST BEGIN ===\r\n");

    /* Step 1: Register shims */
    ndis_shim_register();

    /* Step 2: PE-load ne2000.sys
       Pass NULL as fallback table; both DLLs resolved via shim registry.
       If this returns PE_ERR_IMPORT_FAIL (-10), check the debug log for
       the missing function name and add a stub to ndis_funcs[]. */
    VxD_Debug_Printf("NDIS: Loading ne2000.sys...\r\n");
    rc = pe_load_image(NE2000_IMAGE, NE2000_IMAGE_SIZE,
                       NULL, &entry_point, &image_base);
    if (rc != 0) {
        ndis_log_hex("NDIS: pe_load_image FAILED rc=", (ULONG)rc, "\r\n");
        return rc;
    }
    ndis_log_hex("NDIS: ne2000.sys loaded at ", (ULONG)image_base, "");
    ndis_log_hex(" entry=", (ULONG)entry_point, "\r\n");

    /* Step 3: Call DriverEntry
       This triggers: NdisInitializeWrapper + NdisMRegisterMiniport */
    VxD_Debug_Printf("NDIS: Calling DriverEntry...\r\n");
    ndis_memset(fake_driver_object, 0, sizeof(fake_driver_object));
    ndis_memset(fake_registry_path, 0, sizeof(fake_registry_path));

    pfnDriverEntry = (PFN_DRIVER_ENTRY)entry_point;
    driver_status = pfnDriverEntry((PVOID)fake_driver_object,
                                   (PVOID)fake_registry_path);
    ndis_log_hex("NDIS: DriverEntry returned ", driver_status, "\r\n");

    if (driver_status != 0) {
        VxD_Debug_Printf("NDIS: DriverEntry FAILED!\r\n");
        return -1;
    }

    /* Verify we got the callbacks */
    if (!g_ndis_miniport.InitializeHandler) {
        VxD_Debug_Printf("NDIS: No InitializeHandler registered!\r\n");
        return -2;
    }

    /* Step 4: Call MiniportInitialize
       The handle is our miniport block. WrapperConfigurationContext
       is used by NdisOpenConfiguration (we pass our block again).
       The miniport will call NdisMSetAttributesEx to store its
       AdapterContext; from then on, we pass AdapterContext (not the
       handle) to miniport callbacks like Query/Set/Send. */
    /* Dump key miniport block offsets to diagnose post-init crashes */
    {
        ULONG di;
        VxD_Debug_Printf("NDIS: MB dump (key offsets):\r\n");
        for (di = 0; di < 0x1A0; di += 4) {
            ULONG val = *(ULONG *)(&g_ndis_miniport_block[di]);
            ULONG safe = (ULONG)g_ndis_safe_page;
            if (val != safe && val != 0) {
                ndis_log_hex("  MB+", di, "=");
                ndis_log_hex("", val, "\r\n");
            }
        }
    }
    VxD_Debug_Printf("NDIS: Calling MiniportInitialize...\r\n");

    open_error_status = 0;
    selected_medium_index = 0;
    medium_array[0] = NdisMedium802_3;  /* Ethernet */

#ifdef NDIS_DIRECT_CALL_TEST
    /* Direct call to MiniportInitialize, bypassing Safe_HwFindAdapter.
       Diagnostic: if this crashes, the fault is in MiniportInitialize itself
       (stack corruption, bad MB dereference). If Safe_HwFindAdapter crashes
       but this doesn't, the IDT manipulation is the problem. */
    pfnInit = (PFN_MINIPORT_INIT)g_ndis_miniport.InitializeHandler;
    VxD_Debug_Printf("NDIS: DIRECT CALL (no Safe_HwFindAdapter)\r\n");
    status = pfnInit(
        &open_error_status,
        &selected_medium_index,
        medium_array,
        1,
        (NDIS_HANDLE)g_ndis_miniport_block,
        (NDIS_HANDLE)g_ndis_miniport_block
    );
#else
    /* Use Safe_HwFindAdapter to wrap MiniportInitialize with IDT fault
       catching. Both have 6 __stdcall params; Safe_HwFindAdapter takes
       (func_ptr, p1..p6) and returns 0xDEAD0001 on page fault / GPF. */
    {
        extern ULONG Safe_HwFindAdapter(PVOID, PVOID, PVOID, PVOID, PVOID, PVOID, PVOID);
        extern ULONG g_fault_code, g_fault_eip, g_fault_addr;

        pfnInit = (PFN_MINIPORT_INIT)g_ndis_miniport.InitializeHandler;
        status = Safe_HwFindAdapter(
            (PVOID)pfnInit,
            (PVOID)&open_error_status,
            (PVOID)&selected_medium_index,
            (PVOID)medium_array,
            (PVOID)1,
            (PVOID)g_ndis_miniport_block,
            (PVOID)g_ndis_miniport_block
        );

        if (status == 0xDEAD0001UL) {
            VxD_Debug_Printf("NDIS: *** MiniportInitialize CRASHED ***\r\n");
            ndis_log_hex("  FAULT code=", g_fault_code, "\r\n");
            ndis_log_hex("  FAULT eip=",  g_fault_eip, "\r\n");
            ndis_log_hex("  FAULT addr=", g_fault_addr, " (CR2)\r\n");
            ndis_log_hex("  safe_noop calls=", g_safe_noop_call_count, "\r\n");
            return -3;
        }
    }
#endif

    ndis_log_hex("NDIS: MiniportInitialize returned ", status, "\r\n");
    ndis_log_hex("  SelectedMedium=", selected_medium_index, "\r\n");

    if (status != NDIS_STATUS_SUCCESS) {
        ndis_log_hex("NDIS: MiniportInitialize FAILED! OpenErr=",
                     open_error_status, "\r\n");
        return -3;
    }

    /* VPICD hookup: deferred to future work. Both VPICD_Virtualize_IRQ and
       VPICD_Call_When_Hw_Int crash at both Device_Init AND Init_Complete
       (GPF at EIP=0xF532). PCI IRQ steering owns IRQ 11 and its
       virtualization state prevents re-hooking. Polling mode (which was
       proven for ARP TX+RX) is the working path. */
    VxD_Debug_Printf("NDIS: Polling mode (VPICD hookup deferred)\r\n");

    /* Step 5: Call MiniportQueryInformation(OID_GEN_SUPPORTED_LIST)
       Uses AdapterContext, not the miniport block handle */
    if (!g_ndis_miniport.QueryInformationHandler) {
        VxD_Debug_Printf("NDIS: No QueryInformationHandler!\r\n");
        return -4;
    }

    VxD_Debug_Printf("NDIS: Querying OID_GEN_SUPPORTED_LIST...\r\n");
    ndis_memset(query_buffer, 0, sizeof(query_buffer));
    bytes_written = 0;
    bytes_needed = 0;

    pfnQuery = (PFN_MINIPORT_QUERY)g_ndis_miniport.QueryInformationHandler;
    status = pfnQuery(
        g_ndis_miniport.AdapterContext,  /* MiniportAdapterContext */
        OID_GEN_SUPPORTED_LIST,
        (PVOID)query_buffer,
        sizeof(query_buffer),
        &bytes_written,
        &bytes_needed
    );

    ndis_log_hex("NDIS: QueryInfo returned ", status, "");
    ndis_log_hex(" written=", bytes_written, "");
    ndis_log_hex(" needed=", bytes_needed, "\r\n");

    if (status == NDIS_STATUS_SUCCESS && bytes_written > 0) {
        ULONG *oids = (ULONG *)query_buffer;
        ULONG n_oids = bytes_written / 4;
        ULONG i;
        ndis_log_hex("NDIS: Supported OIDs (", n_oids, "):\r\n");
        for (i = 0; i < n_oids && i < 16; i++) {
            ndis_log_hex("  OID: ", oids[i], "\r\n");
        }
    }

    /* ============================================================
     * Step 6: Query MAC address (OID_802_3_CURRENT_ADDRESS)
     * ============================================================ */
    {
        UCHAR mac_addr[6];
        ndis_memset(mac_addr, 0, 6);
        bytes_written = 0;
        bytes_needed = 0;
        status = pfnQuery(
            g_ndis_miniport.AdapterContext,
            OID_802_3_CURRENT_ADDRESS,
            (PVOID)mac_addr,
            6,
            &bytes_written,
            &bytes_needed
        );
        ndis_log_hex("NDIS: QueryMAC status=", status, "");
        ndis_log_hex(" written=", bytes_written, "\r\n");
        if (status == NDIS_STATUS_SUCCESS) {
            VxD_Debug_Printf("NDIS: MAC=");
            { ULONG mi;
              for (mi = 0; mi < 6; mi++) {
                  ndis_log_hex("", (ULONG)mac_addr[mi], mi < 5 ? ":" : "\r\n");
              }
            }
        }

    /* ============================================================
     * Step 7: Set packet filter (required before send/receive)
     * ============================================================ */
    {
        ULONG packet_filter = 0x0B;  /* DIRECTED | MULTICAST | BROADCAST */
        typedef NDIS_STATUS (__stdcall *PFN_MINIPORT_SET)(
            NDIS_HANDLE, ULONG, PVOID, ULONG, PULONG, PULONG);
        PFN_MINIPORT_SET pfnSet;

        if (g_ndis_miniport.SetInformationHandler) {
            pfnSet = (PFN_MINIPORT_SET)g_ndis_miniport.SetInformationHandler;
            bytes_written = 0;
            bytes_needed = 0;
            status = pfnSet(
                g_ndis_miniport.AdapterContext,
                OID_GEN_CURRENT_PACKET_FILTER,
                (PVOID)&packet_filter,
                sizeof(packet_filter),
                &bytes_written,
                &bytes_needed
            );
            ndis_log_hex("NDIS: SetPacketFilter status=", status, "\r\n");
        }
    }

    /* ============================================================
     * Step 8: Send an ARP request for 10.0.2.2 (QEMU slirp gateway)
     * ============================================================ */
    {
        typedef NDIS_STATUS (__stdcall *PFN_MINIPORT_SEND)(
            NDIS_HANDLE, PVOID, ULONG);
        typedef void (__stdcall *PFN_MINIPORT_SEND_PACKETS)(
            NDIS_HANDLE, PVOID *, ULONG);
        PFN_MINIPORT_SEND pfnSend;
        PFN_MINIPORT_SEND_PACKETS pfnSendPkts;

        PVOID pkt_pool_handle = NULL;
        PVOID buf_pool_handle = NULL;
        PVOID send_packet = NULL;
        PVOID send_buffer_ndis = NULL;
        PVOID frame_mem = NULL;
        PUCHAR frame;
        ULONG frame_len;

        ndis_log_hex("NDIS: SendHandler=",        (ULONG)g_ndis_miniport.SendHandler, "\r\n");
        ndis_log_hex("NDIS: SendPacketsHandler=",  (ULONG)g_ndis_miniport.SendPacketsHandler, "\r\n");

        pfnSend = (PFN_MINIPORT_SEND)g_ndis_miniport.SendHandler;
        pfnSendPkts = (PFN_MINIPORT_SEND_PACKETS)g_ndis_miniport.SendPacketsHandler;

        if (!pfnSend && !pfnSendPkts) {
            VxD_Debug_Printf("NDIS: No send handler registered!\r\n");
            goto test_done;
        }

        /* Allocate packet + buffer pools for our test frame */
        ndis_AllocatePacketPool(&status, &pkt_pool_handle, 4, 0);
        if (status != NDIS_STATUS_SUCCESS) {
            VxD_Debug_Printf("NDIS: PacketPool alloc failed!\r\n");
            goto test_done;
        }
        ndis_AllocateBufferPool(&status, &buf_pool_handle, 4);

        /* Allocate one packet */
        ndis_AllocatePacket(&status, &send_packet, pkt_pool_handle);
        if (status != NDIS_STATUS_SUCCESS || !send_packet) {
            VxD_Debug_Printf("NDIS: Packet alloc failed!\r\n");
            goto test_done;
        }

        /* Allocate frame memory (ARP request = 42 bytes, pad to 60 min Ethernet) */
        frame_len = 60;  /* minimum Ethernet frame */
        frame_mem = VxD_PageAllocate(1, PAGEFIXED);
        if (!frame_mem) {
            VxD_Debug_Printf("NDIS: Frame alloc failed!\r\n");
            goto test_done;
        }
        ndis_memset(frame_mem, 0, 4096);
        frame = (PUCHAR)frame_mem;

        /* Build ARP request:
           Ethernet: dst=FF:FF:FF:FF:FF:FF src=<our MAC> type=0x0806
           ARP: hw=1(Eth) proto=0x0800 hlen=6 plen=4 op=1(Request)
                sender MAC=<our MAC> sender IP=10.0.2.15 (QEMU default)
                target MAC=00:00:00:00:00:00 target IP=10.0.2.2 (gateway) */
        /* Ethernet header (14 bytes) */
        ndis_memset(frame, 0xFF, 6);         /* dst: broadcast */
        ndis_memcpy(frame + 6, mac_addr, 6); /* src: our MAC */
        frame[12] = 0x08; frame[13] = 0x06;  /* ethertype: ARP */

        /* ARP payload (28 bytes, starting at offset 14) */
        frame[14] = 0x00; frame[15] = 0x01;  /* hardware type: Ethernet */
        frame[16] = 0x08; frame[17] = 0x00;  /* protocol type: IPv4 */
        frame[18] = 6;                        /* hardware addr len */
        frame[19] = 4;                        /* protocol addr len */
        frame[20] = 0x00; frame[21] = 0x01;  /* operation: ARP request */
        ndis_memcpy(frame + 22, mac_addr, 6);/* sender MAC */
        frame[28] = 10; frame[29] = 0;       /* sender IP: 10.0.2.15 */
        frame[30] = 2;  frame[31] = 15;
        ndis_memset(frame + 32, 0, 6);       /* target MAC: 00:00:00:00:00:00 */
        frame[38] = 10; frame[39] = 0;       /* target IP: 10.0.2.2 */
        frame[40] = 2;  frame[41] = 2;
        /* bytes 42-59 are zero padding to minimum Ethernet frame size */

        VxD_Debug_Printf("NDIS: ARP frame built (60 bytes)\r\n");

        /* Allocate NDIS buffer wrapping our frame */
        ndis_AllocateBuffer(&status, &send_buffer_ndis, buf_pool_handle,
                            frame_mem, frame_len);
        if (status != NDIS_STATUS_SUCCESS || !send_buffer_ndis) {
            VxD_Debug_Printf("NDIS: Buffer alloc failed!\r\n");
            goto test_done;
        }

        /* Chain buffer to packet */
        ndis_chain_buffer_to_packet(send_packet, (NDIS_BUFFER *)send_buffer_ndis);

        ndis_log_hex("NDIS: Packet @", (ULONG)send_packet, "");
        ndis_log_hex(" Head=", (ULONG)PKT_PTR_AT(send_packet, PKT_HEAD), "");
        ndis_log_hex(" TotalLen=", PKT_ULONG_AT(send_packet, PKT_TOTAL_LENGTH), "\r\n");

        /* Clear send completion flag */
        g_send_complete_flag = 0;
        g_send_complete_status = 0;

        /* Pre-TX DMA diagnostic: if shared memory was allocated, verify its
           contents are visible from the physical address side. The driver will
           memcpy our frame into its DMA buffer before programming TSAD. */
        if (g_ndis_miniport.LastSharedVA && g_ndis_miniport.LastSharedPA) {
            PVOID pa_view;
            ndis_log_hex("NDIS: Pre-TX DMA buf VA=", (ULONG)g_ndis_miniport.LastSharedVA, "");
            ndis_log_hex(" PA=", g_ndis_miniport.LastSharedPA, "\r\n");
            pa_view = VxD_MapPhysToLinear(g_ndis_miniport.LastSharedPA, PAGESIZE);
            if (pa_view) {
                ULONG va0 = *(volatile ULONG *)g_ndis_miniport.LastSharedVA;
                ULONG pa0 = *(volatile ULONG *)pa_view;
                ndis_log_hex("NDIS: Pre-TX VA[0]=", va0, "");
                ndis_log_hex(" PA[0]=", pa0, "\r\n");
            }
        }

        /* Call the appropriate send handler */
        VxD_Debug_Printf("NDIS: === SENDING ARP REQUEST ===\r\n");

        if (pfnSendPkts) {
            /* NDIS 4.0 path: MiniportSendPackets */
            PVOID pkt_array[1];
            pkt_array[0] = send_packet;
            VxD_Debug_Printf("NDIS: Calling MiniportSendPackets...\r\n");
            pfnSendPkts(g_ndis_miniport.AdapterContext, pkt_array, 1);
            VxD_Debug_Printf("NDIS: MiniportSendPackets returned\r\n");
        } else {
            /* NDIS 3.0 path: MiniportSend */
            VxD_Debug_Printf("NDIS: Calling MiniportSend...\r\n");
            status = pfnSend(g_ndis_miniport.AdapterContext, send_packet, 0);
            ndis_log_hex("NDIS: MiniportSend returned ", status, "\r\n");
            if (status == NDIS_STATUS_SUCCESS) {
                if (!g_send_complete_flag) {
                    g_send_complete_flag = 1;
                    g_send_complete_status = NDIS_STATUS_SUCCESS;
                }
            } else if (status == NDIS_STATUS_PENDING) {
                VxD_Debug_Printf("NDIS: Send PENDING (async completion)\r\n");
            } else {
                VxD_Debug_Printf("NDIS: *** SEND FAILED ***\r\n");
                goto test_done;
            }
        }

        /* Wait for send completion (if async) */
        if (!g_send_complete_flag) {
            ULONG wait;
            VxD_Debug_Printf("NDIS: Waiting for send completion...\r\n");
            for (wait = 0; wait < 1000 && !g_send_complete_flag; wait++) {
                ULONG s;
                for (s = 0; s < 1000; s++) _port_stall();
            }
        }

        if (g_send_complete_flag) {
            ndis_log_hex("NDIS: SEND COMPLETE status=", g_send_complete_status, "\r\n");
            if (g_send_complete_status == NDIS_STATUS_SUCCESS) {
                VxD_Debug_Printf("NDIS: *** ARP SEND SUCCESS ***\r\n");
            }
        } else {
            VxD_Debug_Printf("NDIS: Send completion timeout\r\n");
        }

    /* ============================================================
     * Step 9: Poll for ARP reply
     * ============================================================ */
#ifdef NDIS_TEST_RTL8139
        /* RTL8139 TX verification: check ISR for TOK and read TSD0.
           TOK = bit 2 in ISR (0xC03E). TSD0 at 0xC010 has OWN+TOK bits. */
        if (g_ndis_miniport.IoPortBase) {
            USHORT nic_base = g_ndis_miniport.IoPortBase;
            USHORT isr_now = _port_inw(nic_base + 0x3E);
            ULONG tsd0 = _port_ind(nic_base + 0x10);
            ULONG tsad0 = _port_ind(nic_base + 0x20);
            ndis_log_hex("NDIS: Post-TX ISR=", (ULONG)isr_now, "");
            ndis_log_hex(" TSD0=", tsd0, "\r\n");
            ndis_log_hex("NDIS: TSAD0(DMA addr)=", tsad0, "\r\n");
            /* TSD bit 13 = TOK, bit 15 = OWN (cleared when TX done) */
            if (tsd0 & 0x2000) VxD_Debug_Printf("NDIS: TSD0 TOK=1 (TX completed!)\r\n");
            if (!(tsd0 & 0x8000)) VxD_Debug_Printf("NDIS: TSD0 OWN cleared (HW done)\r\n");

            /* Post-TX DMA readback: map TSAD0 address and check contents.
               This is the ACTUAL PA the NIC DMA'd from. */
            if (tsad0) {
                PVOID tsad_view = VxD_MapPhysToLinear(tsad0 & 0xFFFFF000, PAGESIZE);
                if (tsad_view) {
                    ULONG off = tsad0 & 0xFFF;
                    PUCHAR dm = (PUCHAR)tsad_view + off;
                    ndis_log_hex("NDIS: TSAD0-DMA[0..3]=", *(ULONG*)dm, "");
                    ndis_log_hex(" [4..7]=", *(ULONG*)(dm+4), "\r\n");
                    if (*(ULONG*)dm == 0) {
                        VxD_Debug_Printf("NDIS: *** DMA BUFFER IS ZEROS AT TSAD0 ADDR ***\r\n");
                    } else {
                        VxD_Debug_Printf("NDIS: DMA buffer has data at TSAD0 addr\r\n");
                    }
                }
                if (g_ndis_miniport.LastSharedPA) {
                    if (tsad0 == g_ndis_miniport.LastSharedPA ||
                        (tsad0 >= g_ndis_miniport.LastSharedPA &&
                         tsad0 < g_ndis_miniport.LastSharedPA + g_ndis_miniport.LastSharedLen)) {
                        VxD_Debug_Printf("NDIS: TSAD0 is within our SharedMem range\r\n");
                    } else {
                        ndis_log_hex("NDIS: *** TSAD0 OUTSIDE SharedMem! Expected PA=", g_ndis_miniport.LastSharedPA, "");
                        ndis_log_hex(" len=", g_ndis_miniport.LastSharedLen, " ***\r\n");
                    }
                }
            }

            /* Clear TX interrupt if set */
            if (isr_now & 0x0004) _port_outw(nic_base + 0x3E, 0x0004);
            /* Wait 50ms for QEMU slirp to process ARP and send reply */
            { ULONG w; for (w = 0; w < 50000; w++) _port_stall(); }
            /* Check if ROK appeared after delay */
            isr_now = _port_inw(nic_base + 0x3E);
            ndis_log_hex("NDIS: After 50ms ISR=", (ULONG)isr_now, "\r\n");
        }
#endif
        VxD_Debug_Printf("NDIS: === POLLING FOR ARP REPLY ===\r\n");
        g_rx_captured = 0;

        if (g_ndis_miniport.HandleInterruptHandler && g_ndis_miniport.IoPortBase) {
            typedef void (__stdcall *PFN_HANDLE_INT)(NDIS_HANDLE);
            typedef BOOLEAN (__stdcall *PFN_ISR)(BOOLEAN *, PVOID);
            PFN_HANDLE_INT pfnHandleInt = (PFN_HANDLE_INT)g_ndis_miniport.HandleInterruptHandler;
            PFN_ISR pfnISR = (PFN_ISR)g_ndis_miniport.ISRHandler;
            USHORT nic_base = g_ndis_miniport.IoPortBase;
            ULONG poll;
            ULONG rx_found = 0;

#ifdef NDIS_TEST_RTL8139
            /* RTL8139: ISR at iobase+0x3E (IntrStatus, 16-bit).
               Do NOT unmask PIC IRQ — that causes Win98 to dispatch the
               interrupt, calling the driver's ISR which clears our pending ROK.
               Just poll the ISR register directly (works in QEMU). */
            ndis_log_hex("NDIS: RTL8139 polling ISR at port ", (ULONG)(nic_base + 0x3E), "");
            ndis_log_hex(" current=", (ULONG)_port_inw(nic_base + 0x3E), "\r\n");

            for (poll = 0; poll < 500 && !g_rx_captured; poll++) {
                USHORT isr_val = _port_inw(nic_base + 0x3E);

                if (isr_val & 0x0001) {  /* ROK: Receive OK */
                    if (!rx_found) {
                        ndis_log_hex("NDIS: RTL8139 ISR=", (ULONG)isr_val, " (ROK detected)\r\n");
                        rx_found = 1;
                    }

                    /* Try HandleInterrupt first (fixed: MB+0xF4 now __stdcall).
                       Falls back to direct ring read if g_rx_captured stays 0. */
                    pfnHandleInt(g_ndis_miniport.AdapterContext);

                    /* If HandleInterrupt didn't capture via indicate path,
                       do direct RX ring buffer read as fallback.
                       RTL8139 RX ring: shared memory VA + CAPR offset.
                       Each frame: [status:16][length:16][data...][pad to DWORD] */
                    if (!g_rx_captured && g_ndis_miniport.LastSharedVA) {
                        ULONG rbstart_pa = _port_ind(nic_base + 0x30);
                        ULONG ring_va_offset = rbstart_pa - g_ndis_miniport.LastSharedPA;
                        PUCHAR ring = (PUCHAR)g_ndis_miniport.LastSharedVA + ring_va_offset;
                        USHORT capr = _port_inw(nic_base + 0x38);
                        USHORT cbr = _port_inw(nic_base + 0x3A);
                        ULONG ring_offset = (capr + 16) & 0xFFFF;
                        USHORT rx_status, rx_len;
                        PUCHAR frame_ptr;

                        ndis_log_hex("NDIS: RX CAPR=", (ULONG)capr, "");
                        ndis_log_hex(" CBR=", (ULONG)cbr, "\r\n");

                        rx_status = *(USHORT *)(ring + ring_offset);
                        rx_len = *(USHORT *)(ring + ring_offset + 2);
                        ndis_log_hex("NDIS: RX status=", (ULONG)rx_status, "");
                        ndis_log_hex(" len=", (ULONG)rx_len, "\r\n");

                        if ((rx_status & 0x0001) && rx_len >= 14 && rx_len < 1600) {
                            frame_ptr = ring + ring_offset + 4;
                            {
                                ULONG hcopy = 14;
                                ULONG dcopy = rx_len - 14 - 4;
                                if (dcopy > sizeof(g_rx_capture_data)) dcopy = sizeof(g_rx_capture_data);
                                ndis_memcpy(g_rx_capture_hdr, frame_ptr, hcopy);
                                g_rx_capture_hdr_len = hcopy;
                                ndis_memcpy(g_rx_capture_data, frame_ptr + 14, dcopy);
                                g_rx_capture_data_len = dcopy;
                                g_rx_captured = 1;
                                VxD_Debug_Printf("NDIS: *** RX FRAME CAPTURED (direct ring read) ***\r\n");
                                ndis_log_hex("NDIS: RX dst=", *(ULONG*)frame_ptr, "");
                                ndis_log_hex(" ethertype=", (ULONG)(frame_ptr[12] << 8 | frame_ptr[13]), "\r\n");
                            }
                        } else {
                            ndis_log_hex("NDIS: RX bad status/len at offset ", ring_offset, "\r\n");
                        }
                    }

                    /* Acknowledge all pending interrupts */
                    _port_outw(nic_base + 0x3E, 0xFFFF);
                }

                if (isr_val & 0x0010) {  /* RxOvw: RX buffer overflow */
                    ndis_log_hex("NDIS: RTL8139 RxOvw ISR=", (ULONG)isr_val, "\r\n");
                    _port_outw(nic_base + 0x3E, 0xFFFF);
                }

                /* Small delay between polls (~1ms) */
                { ULONG d; for (d = 0; d < 1000; d++) _port_stall(); }
            }

            /* Disable NIC IMR after polling */
            _port_outw(nic_base + 0x3C, 0x0000);

#else  /* NE2000 / RTL8029 */
            /* NE2000: ISR at iobase+0x07 (page 0, 8-bit).
               Bit 0 = PRX (packet received).
               Enable all interrupt sources in IMR so ISR reflects status. */
            _port_outb(nic_base, 0x22);       /* CR: page 0, no DMA, start */
            _port_outb(nic_base + 0x0F, 0xFF);/* IMR: enable all interrupt sources */

            for (poll = 0; poll < 500 && !g_rx_captured; poll++) {
                UCHAR isr_val = _port_inb(nic_base + 0x07);

                if (isr_val & 0x01) {  /* PRX: packet received */
                    if (!rx_found) {
                        ndis_log_hex("NDIS: NIC ISR=", (ULONG)isr_val, " (RX detected)\r\n");
                        rx_found = 1;
                    }

                    /* Call miniport's HandleInterrupt (DPC-level handler).
                       This will read the NIC buffer and call EthFilterDprIndicateReceive
                       or EthRxIndicate to deliver the frame. */
                    pfnHandleInt(g_ndis_miniport.AdapterContext);

                    /* Acknowledge the interrupt at the NIC */
                    _port_outb(nic_base + 0x07, 0xFF);  /* clear all ISR bits */
                }

                if (isr_val & 0x10) {  /* OVW: overrun */
                    ndis_log_hex("NDIS: NIC ISR overrun=", (ULONG)isr_val, "\r\n");
                    _port_outb(nic_base + 0x07, 0x10);  /* clear OVW */
                }

                /* Small delay between polls (~1ms) */
                { ULONG d; for (d = 0; d < 1000; d++) _port_stall(); }
            }

            /* Re-mask NIC interrupts */
            _port_outb(nic_base + 0x0F, 0x00);
#endif  /* NDIS_TEST_RTL8139 */

            ndis_log_hex("NDIS: Poll complete, iterations=", poll, "\r\n");

            if (g_rx_captured) {
                ULONG hi;
                VxD_Debug_Printf("NDIS: *** ARP REPLY RECEIVED ***\r\n");
                ndis_log_hex("  Hdr len=", g_rx_capture_hdr_len, "");
                ndis_log_hex(" Data len=", g_rx_capture_data_len, "\r\n");

                /* Dump first 14 bytes of header (Ethernet) */
                VxD_Debug_Printf("  Hdr: ");
                for (hi = 0; hi < g_rx_capture_hdr_len && hi < 14; hi++) {
                    ndis_log_hex("", (ULONG)g_rx_capture_hdr[hi], " ");
                }
                VxD_Debug_Printf("\r\n");

                /* Check if it's an ARP reply (ethertype 0x0806, opcode 0x0002) */
                if (g_rx_capture_hdr_len >= 14 &&
                    g_rx_capture_hdr[12] == 0x08 && g_rx_capture_hdr[13] == 0x06) {
                    VxD_Debug_Printf("NDIS: Frame is ARP\r\n");
                    if (g_rx_capture_data_len >= 8 &&
                        g_rx_capture_data[6] == 0x00 && g_rx_capture_data[7] == 0x02) {
                        VxD_Debug_Printf("NDIS: *** ARP REPLY CONFIRMED (op=2) ***\r\n");
                    }
                }

                /* Dump first 28 bytes of data (ARP payload) */
                VxD_Debug_Printf("  Data: ");
                for (hi = 0; hi < g_rx_capture_data_len && hi < 28; hi++) {
                    ndis_log_hex("", (ULONG)g_rx_capture_data[hi], " ");
                }
                VxD_Debug_Printf("\r\n");
            } else {
                VxD_Debug_Printf("NDIS: No RX frame captured (timeout)\r\n");
            }
        } else {
            VxD_Debug_Printf("NDIS: No HandleInterruptHandler, skipping RX poll\r\n");
        }
    }  /* end send/receive block */
    }  /* end MAC query block */

test_done:
    VxD_Debug_Printf("=== NDIS NE2000 TEST COMPLETE ===\r\n");
    return 0;

#else
    VxD_Debug_Printf("NDIS: Test not available (NE2000_EMBEDDED.H not included)\r\n");
    return -99;
#endif
}
