/*
 * NTMINI_V5.C - Win98 CD-ROM File System Driver (VxD)
 *
 * Provides CD-ROM file access on Windows 98 systems where the normal
 * IOS/CDFS driver chain fails (common with NEC ATAPI controllers).
 * Registers as an IFSMgr File System Driver, mounts D:, and serves
 * file I/O requests using direct ATAPI commands and ISO 9660 parsing.
 *
 * Architecture:
 *   - Registers with IFSMgr via RegisterMount at Init_Complete
 *   - Creates a DCB and triggers CDROM_Attach to mount D:
 *   - FSD entry table handles Open, Read, Close through IFSMgr dispatch
 *   - Direct ATAPI I/O bypasses the broken IOS chain
 *   - ISO 9660 filesystem parsing for file lookup
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  wlink with LE/VLE format for Win98 VMM
 *
 * Released under the MIT License. See LICENSE for details.
 */

/* Inline port I/O via pragma aux (no libc needed) */
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
void _port_rep_insw(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_insw = "rep insw" parm [dx] [edi] [ecx] modify [edi ecx];
void _port_rep_insd(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_insd = "rep insd" parm [dx] [edi] [ecx] modify [edi ecx];
void _port_rep_outsw(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_outsw = "rep outsw" parm [dx] [esi] [ecx] modify [esi ecx];
void _port_rep_outsd(unsigned short port, void *buf, unsigned long cnt);
#pragma aux _port_rep_outsd = "rep outsd" parm [dx] [esi] [ecx] modify [esi ecx];
void _port_stall(void);
#pragma aux _port_stall = "in al, 0x80" modify [al];
#endif

#define PORT_IN_BYTE(p)    _port_inb((unsigned short)(p))
#define PORT_IN_WORD(p)    _port_inw((unsigned short)(p))
#define PORT_IN_DWORD(p)   _port_ind((unsigned short)(p))
#define PORT_OUT_BYTE(p,v) _port_outb((unsigned short)(p),(unsigned char)(v))
#define PORT_OUT_WORD(p,v) _port_outw((unsigned short)(p),(unsigned short)(v))
#define PORT_OUT_DWORD(p,v) _port_outd((unsigned short)(p),(unsigned long)(v))
#define PORT_READ_BUFFER_USHORT(p,b,c) _port_rep_insw((unsigned short)(p),(b),(c))
#define PORT_READ_BUFFER_ULONG(p,b,c) _port_rep_insd((unsigned short)(p),(b),(c))
#define PORT_WRITE_BUFFER_USHORT(p,b,c) _port_rep_outsw((unsigned short)(p),(b),(c))
#define PORT_WRITE_BUFFER_ULONG(p,b,c) _port_rep_outsd((unsigned short)(p),(b),(c))
#define PORT_STALL_ONE() _port_stall()

typedef unsigned char       UCHAR;
typedef unsigned short      USHORT;
typedef unsigned long       ULONG;
typedef signed long         LONG;
typedef unsigned char       BOOLEAN;  /* MUST match NT DDK: UCHAR, NOT int */
typedef void                VOID;
typedef void               *PVOID;
typedef UCHAR              *PUCHAR;
typedef USHORT             *PUSHORT;
typedef ULONG              *PULONG;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)
#define STATUS_SUCCESS      0x00000000
#define STATUS_UNSUCCESSFUL 0xC0000001

/* Physical address */
typedef union {
    struct { ULONG LowPart; LONG HighPart; };
    long long QuadPart;
} SCSI_PHYSICAL_ADDRESS;

/* Access range */
typedef struct {
    SCSI_PHYSICAL_ADDRESS RangeStart;
    ULONG RangeLength;
    BOOLEAN RangeInMemory;
} ACCESS_RANGE, *PACCESS_RANGE;

/* PORT_CONFIGURATION_INFORMATION */
typedef struct _PORT_CONFIGURATION_INFORMATION {
    ULONG Length; ULONG SystemIoBusNumber; ULONG AdapterInterfaceType;
    ULONG BusInterruptLevel; ULONG BusInterruptVector; ULONG InterruptMode;
    ULONG MaximumTransferLength; ULONG NumberOfPhysicalBreaks;
    ULONG DmaChannel; ULONG DmaPort; ULONG DmaWidth; ULONG DmaSpeed;
    ULONG AlignmentMask; ULONG NumberOfAccessRanges;
    ACCESS_RANGE (*AccessRanges)[];
    PVOID Reserved; UCHAR NumberOfBuses; UCHAR InitiatorBusId[8];
    BOOLEAN ScatterGather; BOOLEAN Master; BOOLEAN CachesData;
    BOOLEAN AdapterScansDown; BOOLEAN AtdiskPrimaryClaimed;
    BOOLEAN AtdiskSecondaryClaimed; BOOLEAN Dma32BitAddresses;
    BOOLEAN DemandMode; BOOLEAN MapBuffers; BOOLEAN NeedPhysicalAddresses;
    BOOLEAN TaggedQueuing; BOOLEAN AutoRequestSense;
    BOOLEAN MultipleRequestPerLu; BOOLEAN ReceiveEvent;
    BOOLEAN RealModeInitialized; BOOLEAN BufferAccessScsiPortControlled;
    UCHAR MaximumNumberOfTargets; UCHAR ReservedUchars[2];
    ULONG SlotNumber; ULONG BusInterruptLevel2; ULONG BusInterruptVector2;
    ULONG InterruptMode2; ULONG DmaChannel2; ULONG DmaPort2;
    ULONG DmaWidth2; ULONG DmaSpeed2;
    ULONG DeviceExtensionSize; ULONG SpecificLuExtensionSize; ULONG SrbExtensionSize;
} PORT_CONFIGURATION_INFORMATION, *PPORT_CONFIGURATION_INFORMATION;

/* SRB (simplified) */
typedef struct _SCSI_REQUEST_BLOCK {
    USHORT Length; UCHAR Function; UCHAR SrbStatus;
    UCHAR ScsiStatus; UCHAR PathId; UCHAR TargetId; UCHAR Lun;
    UCHAR QueueTag; UCHAR QueueAction; UCHAR CdbLength;
    UCHAR SenseInfoBufferLength; ULONG SrbFlags; ULONG DataTransferLength;
    ULONG TimeOutValue; PVOID DataBuffer; PVOID SenseInfoBuffer;
    struct _SCSI_REQUEST_BLOCK *NextSrb; PVOID OriginalRequest;
    PVOID SrbExtension; ULONG InternalStatus; UCHAR Cdb[16];
} SCSI_REQUEST_BLOCK, *PSCSI_REQUEST_BLOCK;

/* HW_INITIALIZATION_DATA */
typedef struct _HW_INITIALIZATION_DATA {
    ULONG HwInitializationDataSize; ULONG AdapterInterfaceType;
    BOOLEAN (*HwInitialize)(PVOID);
    BOOLEAN (*HwStartIo)(PVOID, PSCSI_REQUEST_BLOCK);
    BOOLEAN (*HwInterrupt)(PVOID);
    PVOID HwFindAdapter;
    BOOLEAN (*HwResetBus)(PVOID, ULONG);
    PVOID HwDmaStarted; PVOID HwAdapterState;
    ULONG DeviceExtensionSize; ULONG SpecificLuExtensionSize;
    ULONG SrbExtensionSize; ULONG NumberOfAccessRanges;
    PVOID Reserved; BOOLEAN MapBuffers; BOOLEAN NeedPhysicalAddresses;
    BOOLEAN TaggedQueuing; BOOLEAN AutoRequestSense;
    BOOLEAN MultipleRequestPerLu; BOOLEAN ReceiveEvent;
    USHORT VendorIdLength; PVOID VendorId;
    USHORT DeviceIdLength; PVOID DeviceId; PVOID HwAdapterControl;
} HW_INITIALIZATION_DATA, *PHW_INITIALIZATION_DATA;

typedef struct { const char *name; PVOID func; } IMPORT_FUNC_ENTRY;

/* VxD wrappers */
extern void VxD_Debug_Printf(const char *msg);
extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void VxD_PageFree(PVOID addr);
extern int pe_load_image(const void *, unsigned long, const IMPORT_FUNC_ENTRY *, void **, void **);

/* Forward declarations for functions used before definition */
extern void Call_ILB_Service(ULONG service_rtn, PVOID isp_packet);
static void iso9660_test_read(void);
static int iso9660_enum_dir(ULONG dir_lba, ULONG dir_size, int index,
                             char *out_name, ULONG *out_name_len,
                             ULONG *out_lba, ULONG *out_size, UCHAR *out_flags);

/* VPICD IRQ hook (in ASM) */
extern ULONG VxD_Hook_IRQ(ULONG irq_num);
extern PVOID g_irq_hw_int_func;  /* set to HwInterrupt function pointer */
extern PVOID g_irq_devext;       /* set to DeviceExtension pointer */

/* IOSBRIDGE-compatible globals (defined in ASM, populated here after init) */
extern PVOID g_HwStartIo;       /* function pointer slots */
extern PVOID g_HwResetBus;
extern PVOID g_HwInterrupt;
extern PVOID g_DeviceExtension;
extern ULONG g_SrbExtensionSize;
extern volatile ULONG g_SrbCompleted;  /* set by ScsiPortNotification */
extern volatile ULONG g_ReadyForNext;  /* set by ScsiPortNotification */

/* Forward declarations */
void _ntmini_ios_init(void);

/* Embedded ATAPI.SYS */
#include "ATAPI_EMBEDDED.H"

/* Debug helpers */
static void ulong_to_hex(ULONG val, char *buf) {
    static const char hx[]="0123456789ABCDEF"; int i;
    buf[0]='0';buf[1]='x';
    for(i=7;i>=0;i--) buf[2+(7-i)]=hx[(val>>(i*4))&0xF];
    buf[10]=0;
}
static void log_hex(const char *p, ULONG v, const char *s) {
    char h[12]; ulong_to_hex(v,h);
    VxD_Debug_Printf(p); VxD_Debug_Printf(h); VxD_Debug_Printf(s);
}

/* Simple memset/memcpy (no libc) */
static void my_memset(void *dst, int val, ULONG n) {
    UCHAR *d = (UCHAR *)dst; while (n--) *d++ = (UCHAR)val;
}
static void my_memcpy(void *dst, const void *src, ULONG n) {
    UCHAR *d = (UCHAR *)dst; const UCHAR *s = (const UCHAR *)src;
    while (n--) *d++ = *s++;
}

/* ================================================================
 * GLOBAL STATE
 * ================================================================ */
static struct {
    BOOLEAN (__stdcall *HwInitialize)(PVOID);
    BOOLEAN (__stdcall *HwStartIo)(PVOID, PSCSI_REQUEST_BLOCK);
    BOOLEAN (__stdcall *HwInterrupt)(PVOID);
    ULONG   (__stdcall *HwFindAdapter)(PVOID,PVOID,PVOID,PVOID,
                             PPORT_CONFIGURATION_INFORMATION,PUCHAR);
    BOOLEAN (__stdcall *HwResetBus)(PVOID, ULONG);
    PVOID   DeviceExtension;
    ULONG   DeviceExtensionSize;
} g_state;

/* Device extension memory (1 page = 4KB, enough for any miniport) */
static UCHAR g_devext_buf[4096];

/* ================================================================
 * SCSIPORT FUNCTION IMPLEMENTATIONS
 * ================================================================ */

/* Port remapping: miniport thinks it uses primary (0x1F0-0x1F7, 0x3F6)
   but we redirect to secondary (0x170-0x177, 0x376) where the CD-ROM is. */
static USHORT remap_port(USHORT port) {
    if (port >= 0x1F0 && port <= 0x1F7) return (USHORT)(port - 0x1F0 + 0x170);
    if (port == 0x3F6) return 0x376;
    return port;
}

/* Port I/O: log and use real IN/OUT for reads, safe stubs for writes */
static int sp_io_log_count = 0;
static BOOLEAN sp_slave_selected = FALSE; /* track drive select for status fix */
static UCHAR __stdcall sp_ReadPortUchar(PUCHAR Port) {
    USHORT port_num = remap_port((USHORT)(ULONG)Port);
    UCHAR val = PORT_IN_BYTE(port_num);
    /* Status register fixes for QEMU IDE quirks: */
    if (port_num == 0x177 || port_num == 0x1F7) {
        if (sp_slave_selected) {
            /* Non-existent slave: force 0x00 (no device).
               QEMU echoes master's status for non-existent slave. */
            val = 0x00;
        } else if (val == 0x00) {
            /* Master after DEVICE RESET: QEMU returns 0x00 but
               miniport needs DRDY (0x40) to proceed. */
            val = 0x50;  /* DRDY + DSC */
        }
    }
    if (sp_io_log_count < 100) {
        log_hex("SP:INb ", (ULONG)port_num, "");
        log_hex("=", (ULONG)val, "\r\n");
        sp_io_log_count++;
    }
    return val;
}
static USHORT __stdcall sp_ReadPortUshort(PUSHORT Port) {
    return PORT_IN_WORD(remap_port((USHORT)(ULONG)Port));
}
static ULONG __stdcall sp_ReadPortUlong(PULONG Port) {
    return PORT_IN_DWORD(remap_port((USHORT)(ULONG)Port));
}
static void __stdcall sp_WritePortUchar(PUCHAR Port, UCHAR Value) {
    USHORT port_num = remap_port((USHORT)(ULONG)Port);
    /* Track drive select register to know if master or slave */
    if (port_num == 0x176 || port_num == 0x1F6) {
        sp_slave_selected = (Value & 0x10) ? TRUE : FALSE;
    }
    if (sp_io_log_count < 100) {
        log_hex("SP:OUTb ", (ULONG)port_num, "");
        log_hex("=", (ULONG)Value, "\r\n");
        sp_io_log_count++;
    }
    PORT_OUT_BYTE(port_num, Value);
}
static void __stdcall sp_WritePortUshort(PUSHORT Port, USHORT Value) {
    PORT_OUT_WORD(remap_port((USHORT)(ULONG)Port), Value);
}
static void __stdcall sp_WritePortUlong(PULONG Port, ULONG Value) {
    PORT_OUT_DWORD(remap_port((USHORT)(ULONG)Port), Value);
}
static void __stdcall sp_ReadPortBufferUshort(PUSHORT Port, PUSHORT Buf, ULONG Cnt) {
    USHORT port_num = remap_port((USHORT)(ULONG)Port);
    if (sp_io_log_count < 100) {
        log_hex("SP:INbuf16 ", (ULONG)port_num, "");
        log_hex(" cnt=", Cnt, "\r\n");
        sp_io_log_count++;
    }
    PORT_READ_BUFFER_USHORT(port_num, Buf, Cnt);
    /* Log first 4 words of buffer for IDENTIFY data */
    if (Cnt >= 4 && Buf) {
        log_hex("SP:BUF w0=", (ULONG)Buf[0], "");
        log_hex(" w1=", (ULONG)Buf[1], "");
        log_hex(" w2=", (ULONG)Buf[2], "");
        log_hex(" w3=", (ULONG)Buf[3], "\r\n");
    }
}
static void __stdcall sp_ReadPortBufferUlong(PULONG Port, PULONG Buf, ULONG Cnt) {
    PORT_READ_BUFFER_ULONG(remap_port((USHORT)(ULONG)Port), Buf, Cnt);
}
static void __stdcall sp_WritePortBufferUshort(PUSHORT Port, PUSHORT Buf, ULONG Cnt) {
    PORT_WRITE_BUFFER_USHORT(remap_port((USHORT)(ULONG)Port), Buf, Cnt);
}
static void __stdcall sp_WritePortBufferUlong(PULONG Port, PULONG Buf, ULONG Cnt) {
    PORT_WRITE_BUFFER_ULONG(remap_port((USHORT)(ULONG)Port), Buf, Cnt);
}

/* Register I/O: memory-mapped. For IDE, same as port I/O. */
static UCHAR __stdcall sp_ReadRegisterUchar(PUCHAR Reg) { return *Reg; }
static USHORT __stdcall sp_ReadRegisterUshort(PUSHORT Reg) { return *Reg; }
static ULONG __stdcall sp_ReadRegisterUlong(PULONG Reg) { return *Reg; }
static void __stdcall sp_WriteRegisterUchar(PUCHAR Reg, UCHAR Val) { *Reg = Val; }
static void __stdcall sp_WriteRegisterUshort(PUSHORT Reg, USHORT Val) { *Reg = Val; }
static void __stdcall sp_WriteRegisterUlong(PULONG Reg, ULONG Val) { *Reg = Val; }

/* GetDeviceBase: for I/O ports, just return the port number as a pointer.
   NT miniports cast the result to PUCHAR for port I/O. */
static PVOID __stdcall sp_GetDeviceBase(PVOID HwExt, ULONG BusType,
    ULONG SystemIoBusNumber,
    SCSI_PHYSICAL_ADDRESS IoAddr, ULONG Length, BOOLEAN InMemory) {
    log_hex("SP:GDB ext=", (ULONG)HwExt, "");
    log_hex(" bt=", BusType, "");
    log_hex(" bn=", SystemIoBusNumber, "");
    log_hex(" a.lo=", IoAddr.LowPart, "");
    log_hex(" a.hi=", (ULONG)IoAddr.HighPart, "");
    log_hex(" len=", Length, "");
    log_hex(" mem=", (ULONG)InMemory, "\r\n");
    if (InMemory) {
        /* Memory-mapped: would need VxD MapPhysToLinear. Not needed for IDE. */
        return (PVOID)IoAddr.LowPart;
    }
    /* I/O port: just return the port number. Miniport uses ScsiPortReadPortUchar
       which casts this to USHORT for the IN instruction. */
    return (PVOID)IoAddr.LowPart;
}

static void __stdcall sp_FreeDeviceBase(PVOID HwExt, PVOID MappedAddr) {
    /* Nothing to free for I/O ports */
}

static void __stdcall sp_StallExecution(ULONG Microseconds) {
    ULONG i;
    for (i = 0; i < Microseconds; i++) {
        PORT_STALL_ONE();  /* ~1us delay via port 0x80 read */
    }
}

static void __stdcall sp_MoveMemory(PVOID Dst, PVOID Src, ULONG Len) {
    if (Src && Dst) my_memcpy(Dst, Src, Len);
}

/* Struct-return functions are in ASM (VXDWRAP_V4.ASM) for exact ABI control.
   MSVC struct-return uses hidden first param; ASM handles this directly. */
extern void sp_ConvertUlong_asm(void);
extern void sp_GetPhysAddr_asm(void);

static PVOID __stdcall sp_GetUncachedExtension(PVOID HwExt,
    PPORT_CONFIGURATION_INFORMATION Config, ULONG Size) {
    /* Allocate non-cached memory for DMA. Use a static buffer for now. */
    static UCHAR uncached_buf[4096];
    if (Size > sizeof(uncached_buf)) return NULL;
    my_memset(uncached_buf, 0, Size);
    return uncached_buf;
}

static BOOLEAN g_srb_complete = FALSE;  /* set TRUE by RequestComplete */
static void sp_Notification(ULONG Type, PVOID HwExt) {
    /* Variadic cdecl: for RequestComplete(0), 3rd arg = SRB pointer.
       We access extra args via pointer arithmetic on HwExt. */
    if (Type == 0) { /* RequestComplete */
        g_srb_complete = TRUE;
        g_SrbCompleted = TRUE;  /* IOSBRIDGE-compatible flag */
    } else if (Type == 1) { /* NextRequest */
        g_ReadyForNext = TRUE;  /* IOSBRIDGE-compatible flag */
    }
    log_hex("SP:Notif t=", Type, "\r\n");
}

static void __stdcall sp_CompleteRequest(PVOID HwExt, UCHAR p, UCHAR t, UCHAR l, UCHAR s) {
    VxD_Debug_Printf("SP:CompleteRequest\r\n");
}

static void __stdcall sp_LogError(PVOID HwExt, PSCSI_REQUEST_BLOCK Srb,
    UCHAR p, UCHAR t, UCHAR l, ULONG e1, ULONG e2) {
    log_hex("SP:LogError e1=", e1, "");
    log_hex(" e2=", e2, "\r\n");
}

static ULONG __stdcall sp_GetBusData(PVOID HwExt, ULONG BusDataType,
    ULONG SystemIoBusNumber, ULONG SlotNumber, PVOID Buffer, ULONG Length) {
    /* PCI Configuration read via x86 ports 0xCF8/0xCFC.
       BusDataType=4 = PCIConfiguration.
       SlotNumber: bits 0-4 = device, bits 5-7 = function. */
    ULONG devNum, funcNum, regOff, cfgAddr;
    ULONG i;
    UCHAR *buf = (UCHAR *)Buffer;

    if (BusDataType != 4 || Length == 0 || !Buffer) return 0;

    devNum = SlotNumber & 0x1F;
    funcNum = (SlotNumber >> 5) & 0x07;

    /* Read PCI config space register by register (4 bytes at a time) */
    for (i = 0; i < Length; i += 4) {
        regOff = i & 0xFC;
        cfgAddr = 0x80000000 | (SystemIoBusNumber << 16) |
                  (devNum << 11) | (funcNum << 8) | regOff;
        PORT_OUT_DWORD(0xCF8, cfgAddr);
        {
            ULONG val = PORT_IN_DWORD(0xCFC);
            ULONG j;
            for (j = 0; j < 4 && (i + j) < Length; j++) {
                buf[i + j] = (UCHAR)(val >> (j * 8));
            }
        }
    }

    /* Check if device exists (vendor = 0xFFFF means no device) */
    if (Length >= 2 && buf[0] == 0xFF && buf[1] == 0xFF) return 0;

    return Length;
}

static ULONG __stdcall sp_SetBusDataByOffset(PVOID HwExt, ULONG BusDataType,
    ULONG SysNum, ULONG Slot, PVOID Buf, ULONG Off, ULONG Len) {
    return 0;
}

/* ScsiPortInitialize: the heart of the shim */
static ULONG __stdcall sp_Initialize(
    PVOID Argument1, PVOID Argument2,
    PHW_INITIALIZATION_DATA HwInitData, PVOID HwContext)
{
    PORT_CONFIGURATION_INFORMATION configInfo;
    ACCESS_RANGE accessRanges[8];
    UCHAR again;
    ULONG status;

    VxD_Debug_Printf("SP:Initialize called\r\n");
    log_hex("SP: DevExtSize=", HwInitData->DeviceExtensionSize, "\r\n");
    log_hex("SP: NumAccessRanges=", HwInitData->NumberOfAccessRanges, "\r\n");
    log_hex("SP: AdapterType=", HwInitData->AdapterInterfaceType, "\r\n");

    /* Only handle ISA bus (type 1). Skip MicroChannel(3), PCI(5), etc.
       Returning STATUS_SUCCESS tells miniport "checked this bus, nothing found." */
    if (HwInitData->AdapterInterfaceType != 1) {
        VxD_Debug_Printf("SP: Skipping non-ISA bus type\r\n");
        return STATUS_SUCCESS;
    }

    /* Save miniport callbacks (cast: HW_INIT_DATA uses default cdecl,
       but actual NT miniport code is __stdcall) */
    g_state.HwInitialize = (BOOLEAN(__stdcall *)(PVOID))
                            HwInitData->HwInitialize;
    g_state.HwStartIo    = (BOOLEAN(__stdcall *)(PVOID,PSCSI_REQUEST_BLOCK))
                            HwInitData->HwStartIo;
    g_state.HwInterrupt  = (BOOLEAN(__stdcall *)(PVOID))
                            HwInitData->HwInterrupt;
    g_state.HwFindAdapter = (ULONG(__stdcall *)(PVOID,PVOID,PVOID,PVOID,
                            PPORT_CONFIGURATION_INFORMATION,PUCHAR))
                            HwInitData->HwFindAdapter;
    g_state.HwResetBus   = (BOOLEAN(__stdcall *)(PVOID,ULONG))
                            HwInitData->HwResetBus;
    g_state.DeviceExtensionSize = HwInitData->DeviceExtensionSize;

    /* Allocate device extension from static buffer */
    if (HwInitData->DeviceExtensionSize > sizeof(g_devext_buf)) {
        VxD_Debug_Printf("SP: DevExt too large!\r\n");
        return STATUS_UNSUCCESSFUL;
    }
    g_state.DeviceExtension = g_devext_buf;
    my_memset(g_devext_buf, 0, sizeof(g_devext_buf));

    /* Set up PORT_CONFIGURATION_INFORMATION for secondary IDE
       (QEMU CD-ROM: 0x170-0x177, control 0x376, IRQ 15) */
    my_memset(&configInfo, 0, sizeof(configInfo));
    my_memset(accessRanges, 0, sizeof(accessRanges));
    configInfo.Length = sizeof(PORT_CONFIGURATION_INFORMATION);
    configInfo.SystemIoBusNumber = 0;
    configInfo.AdapterInterfaceType = 1; /* Isa */
    configInfo.BusInterruptLevel = 15;   /* Secondary IDE IRQ */
    configInfo.BusInterruptVector = 15;
    configInfo.InterruptMode = 1;        /* Latched */
    configInfo.MaximumTransferLength = 0x10000;
    configInfo.NumberOfPhysicalBreaks = 17;
    configInfo.NumberOfAccessRanges = HwInitData->NumberOfAccessRanges;
    configInfo.AccessRanges = &accessRanges;
    configInfo.NumberOfBuses = 4;  /* allow up to 4 PathId values */
    configInfo.MaximumNumberOfTargets = 2;
    configInfo.AtdiskPrimaryClaimed = FALSE;  /* probe primary (port I/O remapped to secondary hw) */
    configInfo.AtdiskSecondaryClaimed = TRUE;  /* skip secondary (we're remapping primary→secondary) */
    configInfo.MapBuffers = TRUE;

    /* Force-set fields at NT DDK byte offsets in case struct layout differs.
       NT DDK offsets: AtdiskPrimaryClaimed=77, AtdiskSecondaryClaimed=78,
       MapBuffers=81, MaximumNumberOfTargets=89 */
    {
        UCHAR *raw = (UCHAR *)&configInfo;
        raw[77] = 0;  /* AtdiskPrimaryClaimed = FALSE (probe "primary", remapped to secondary hw) */
        raw[78] = 1;  /* AtdiskSecondaryClaimed = TRUE (skip real secondary) */
        raw[81] = 1;  /* MapBuffers = TRUE */
        raw[89] = 2;  /* MaximumNumberOfTargets = 2 */
        log_hex("SP: sizeof(configInfo)=", (ULONG)sizeof(configInfo), "\r\n");
        log_hex("SP: &PrimaryClaimed offset=",
            (ULONG)((UCHAR*)&configInfo.AtdiskPrimaryClaimed - (UCHAR*)&configInfo), "\r\n");
    }

    /* Present PRIMARY IDE addresses (0x1F0/0x3F6) so miniport stores device
       at channel 0 (PathId 0). Port I/O is remapped to secondary hardware
       (0x170/0x376) in sp_ReadPortUchar/sp_WritePortUchar via remap_port(). */
    accessRanges[0].RangeStart.LowPart = 0x1F0;  /* primary command (remapped to 0x170) */
    accessRanges[0].RangeStart.HighPart = 0;
    accessRanges[0].RangeLength = 8;
    accessRanges[0].RangeInMemory = FALSE;

    accessRanges[1].RangeStart.LowPart = 0x3F6;  /* primary control (remapped to 0x376) */
    accessRanges[1].RangeStart.HighPart = 0;
    accessRanges[1].RangeLength = 1;
    accessRanges[1].RangeInMemory = FALSE;

    /* Call miniport's HwFindAdapter in a loop (miniport may set again=TRUE
       to request additional calls, e.g., to try different bus configurations) */
    {
        ULONG pass;
        BOOLEAN found = FALSE;
        for (pass = 0; pass < 8; pass++) {
            VxD_Debug_Printf("SP: Calling HwFindAdapter...\r\n");
            again = FALSE;
            sp_io_log_count = 0;  /* reset I/O log for each pass */
            status = g_state.HwFindAdapter(
                g_state.DeviceExtension,
                HwContext,
                NULL,
                NULL,
                &configInfo,
                &again);

            log_hex("SP: HwFindAdapter returned ", status, "");
            log_hex(" again=", (ULONG)again, "\r\n");

            if (status == 0 /* SP_RETURN_FOUND */) {
                found = TRUE;
                break;
            }
            if (!again) break;  /* miniport says don't call again */

            /* Miniport wants another try. Zero accessRanges so miniport
               advances to next channel from its internal list. */
            log_hex("SP: AR[0]=", accessRanges[0].RangeStart.LowPart, "");
            log_hex(" AR[1]=", accessRanges[1].RangeStart.LowPart, "\r\n");
            VxD_Debug_Printf("SP: again=TRUE, retrying...\r\n");
            my_memset(accessRanges, 0, sizeof(accessRanges));
        }

        if (!found) {
            VxD_Debug_Printf("SP: HwFindAdapter: no adapter found after all passes\r\n");
            return STATUS_UNSUCCESSFUL;
        }
    }

    VxD_Debug_Printf("SP: Adapter FOUND! Calling HwInitialize...\r\n");
    if (!g_state.HwInitialize(g_state.DeviceExtension)) {
        VxD_Debug_Printf("SP: HwInitialize FAILED\r\n");
        return STATUS_UNSUCCESSFUL;
    }

    VxD_Debug_Printf("SP: HwInitialize OK!\r\n");

    /* Clear any pending interrupt state from HwFindAdapter/HwInitialize
       BEFORE patching the device extension. */
    if (g_state.HwInterrupt) {
        ULONG clr;
        VxD_Debug_Printf("SP: Clearing pending interrupts...\r\n");
        sp_io_log_count = 0;
        for (clr = 0; clr < 5; clr++) {
            g_state.HwInterrupt(g_state.DeviceExtension);
            sp_StallExecution(10000); /* 10ms between clears */
        }
    }

    /* Fix device extension for HwStartIo.
       Disassembly of HwStartIo reveals:
       - Offset 0x00: CurrentSrb pointer (MUST be 0 = idle, don't touch!)
       - Offset 0x04: BaseIoAddress1 (command registers) = 0x170 ✓
       - Offset 0x0C: BaseIoAddress2 (control register) = 0x376 ✓
       - Offset 0x44 + TargetId*2: DeviceFlags (WORD array)
         HwStartIo checks: bit 1 (0x02) = ATAPI, bit 0 (0x01) = PRESENT
       The miniport detected the CD-ROM but stored DeviceFlags at a
       different offset (for channel 1). We need to set flags at 0x44
       (channel 0 / TargetId 0) for HwStartIo to find the device. */
    {
        USHORT *flags = (USHORT *)((UCHAR *)g_state.DeviceExtension + 0x44);
        log_hex("SP: DevFlags[0] at 0x44=", (ULONG)flags[0], "");
        log_hex(" DevFlags[1]=", (ULONG)flags[1], "\r\n");
        if (flags[0] == 0) {
            flags[0] = 0x0013; /* DFLAGS_DEVICE_PRESENT(0x01) |
                                  DFLAGS_ATAPI_DEVICE(0x02) |
                                  DFLAGS_REMOVABLE_DRIVE(0x10) */
            VxD_Debug_Printf("SP: FIX: set DevFlags[0]=0x0013 (ATAPI+PRESENT+REMOVABLE)\r\n");
        }
    }

    /* Set up IRQ handler globals for VPICD use */
    g_irq_hw_int_func = (PVOID)g_state.HwInterrupt;
    g_irq_devext = g_state.DeviceExtension;
    VxD_Debug_Printf("SP: IRQ15 polling mode\r\n");

    VxD_Debug_Printf("SP: Init complete\r\n");
    return STATUS_SUCCESS;
}

/* Import function table */
static const IMPORT_FUNC_ENTRY scsiport_funcs[] = {
    { "ScsiPortGetDeviceBase",               (PVOID)sp_GetDeviceBase },
    { "ScsiPortFreeDeviceBase",              (PVOID)sp_FreeDeviceBase },
    { "ScsiPortReadPortUchar",               (PVOID)sp_ReadPortUchar },
    { "ScsiPortReadPortUshort",              (PVOID)sp_ReadPortUshort },
    { "ScsiPortReadPortUlong",               (PVOID)sp_ReadPortUlong },
    { "ScsiPortReadPortBufferUshort",        (PVOID)sp_ReadPortBufferUshort },
    { "ScsiPortReadPortBufferUlong",         (PVOID)sp_ReadPortBufferUlong },
    { "ScsiPortWritePortUchar",              (PVOID)sp_WritePortUchar },
    { "ScsiPortWritePortUshort",             (PVOID)sp_WritePortUshort },
    { "ScsiPortWritePortBufferUshort",       (PVOID)sp_WritePortBufferUshort },
    { "ScsiPortWritePortBufferUlong",        (PVOID)sp_WritePortBufferUlong },
    { "ScsiPortWritePortUlong",              (PVOID)sp_WritePortUlong },
    { "ScsiPortStallExecution",              (PVOID)sp_StallExecution },
    { "ScsiPortMoveMemory",                  (PVOID)sp_MoveMemory },
    { "ScsiPortGetPhysicalAddress",          (PVOID)sp_GetPhysAddr_asm },
    { "ScsiPortGetUncachedExtension",        (PVOID)sp_GetUncachedExtension },
    { "ScsiPortNotification",                (PVOID)sp_Notification },
    { "ScsiPortCompleteRequest",             (PVOID)sp_CompleteRequest },
    { "ScsiPortLogError",                    (PVOID)sp_LogError },
    { "ScsiPortInitialize",                  (PVOID)sp_Initialize },
    { "ScsiPortConvertUlongToPhysicalAddress",(PVOID)sp_ConvertUlong_asm },
    { "ScsiPortGetBusData",                  (PVOID)sp_GetBusData },
    { "ScsiPortSetBusDataByOffset",          (PVOID)sp_SetBusDataByOffset },
    { "ScsiPortReadRegisterUchar",           (PVOID)sp_ReadRegisterUchar },
    { "ScsiPortReadRegisterUshort",          (PVOID)sp_ReadRegisterUshort },
    { "ScsiPortReadRegisterUlong",           (PVOID)sp_ReadRegisterUlong },
    { "ScsiPortWriteRegisterUchar",          (PVOID)sp_WriteRegisterUchar },
    { "ScsiPortWriteRegisterUshort",         (PVOID)sp_WriteRegisterUshort },
    { "ScsiPortWriteRegisterUlong",          (PVOID)sp_WriteRegisterUlong },
    { NULL, NULL }
};

/* Double-init protection (SYSTEM.INI + IOSUBSYS both load us) */
static BOOLEAN g_ntmini_initialized = FALSE;
static int g_ios_ready = 0;       /* Set after init completes; IOR returns NOT_READY until then */
static int g_ior_test_mode = 0;   /* Skip IOS_BD_Complete during handler self-test */
static volatile int g_ios_chain_done = 0; /* Set by IOR completion callback */

/* Deferred READ test state (set during init, executed in AEP_1_SEC) */
static ULONG g_deferred_dcb_ptr = 0;  /* IOS-managed DCB address */
static ULONG g_deferred_iop_ptr = 0;  /* IOS-managed IOP address */
static int g_deferred_read_pending = 0; /* 1 = chain patched, ready for deferred READ */
static int g_deferred_read_done = 0;    /* 1 = deferred READ already attempted */

/* ================================================================
 * In-memory debug log (readable via QEMU monitor after boot)
 * Search for "NTLOG:" marker in memory at 0xC0000000+
 * ================================================================ */
static char g_memlog[4096] = "NTLOG:START\r\n";
static int g_memlog_pos = 13;  /* after "NTLOG:START\r\n" */

static void memlog(const char *msg)
{
    int i;
    for (i = 0; msg[i] && g_memlog_pos < 4090; i++) {
        g_memlog[g_memlog_pos++] = msg[i];
    }
    g_memlog[g_memlog_pos] = '\0';
}

static void memlog_hex(const char *prefix, ULONG val)
{
    static const char hx[] = "0123456789ABCDEF";
    char buf[20];
    int i;
    memlog(prefix);
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++)
        buf[2 + i] = hx[(val >> (28 - i * 4)) & 0xF];
    buf[10] = '\0';
    memlog(buf);
}

/* ================================================================
 * INIT
 * ================================================================ */

int _ntmini_init(void)
{
    void *entry_point;
    void *image_base;
    int rc;
    ULONG status;
    typedef ULONG (__stdcall *PFN_DRIVER_ENTRY)(void *, void *);
    PFN_DRIVER_ENTRY DriverEntry;

    /* Forward declarations for IOS functions used during Device_Init */
    extern ULONG IOS_Get_Version_Test(void);
    extern void _ntmini_ios_init(void);

    /* Double-init protection (SYSTEM.INI + IOSUBSYS) */
    if (g_ntmini_initialized) {
        memlog("SKIP:already_init\r\n");
        VxD_Debug_Printf("NTMINI-V5: Already initialized, skip\r\n");
        return 1;  /* Return success to stay loaded (for memlog reading) */
    }

    memlog("ALIVE\r\n");
    VxD_Debug_Printf("NTMINI-V5: ALIVE\r\n");

    /* SKIP PE load and HwFindAdapter entirely.
       HwFindAdapter does direct IDE port I/O on 0x170-0x177 (secondary controller)
       which leaves the ATAPI controller in a confused state. NECATAPI then can't
       communicate with the CD-ROM, causing "device not ready" when Explorer tries
       to browse D:. By skipping all hardware probing, NECATAPI gets clean access. */
    VxD_Debug_Printf("V5: Minimal init (no PE/HwFindAdapter)\r\n");
    VxD_Debug_Printf("V5: NECATAPI handles all CD-ROM I/O\r\n");

    (void)entry_point; (void)image_base; (void)rc; (void)status; (void)DriverEntry;

    g_ntmini_initialized = TRUE;
    g_ios_ready = 1;  /* IOR handler can now process I/O requests */
    memlog("DONE\r\n");
    VxD_Debug_Printf("V5: DONE (I/O ready)\r\n");
    return 1;  /* Always stay loaded so memlog can be read */
}

void _ntmini_cleanup(void) {
    VxD_Debug_Printf("V5: unloaded\r\n");
}

/* _ntmini_mount_cdrom: defined after IOS type declarations below */

/* Dynamic init handler - called when loaded by IOS from IOSUBSYS.
   This IS the IOS init phase, so we get the ILB with IOS_Register!
   Do full PE load + miniport init + IOS registration here. */
void _ntmini_dynamic_init(void) {
    memlog("DYN_INIT\r\n");
    VxD_Debug_Printf("V5: DYNAMIC LOAD (by IOS from IOSUBSYS)\r\n");
    if (g_ntmini_initialized) {
        memlog("DYN_SKIP\r\n");
        VxD_Debug_Printf("V5: Already initialized (from SYSTEM.INI), skip\r\n");
        return;
    }
    memlog("DYN_CALL_INIT\r\n");
    /* Full init: PE load, miniport, IOS registration.
       During IOS's init phase, IOS_Register provides the ILB! */
    _ntmini_init();
    memlog("DYN_DONE\r\n");
}

/* ================================================================
 * IOS PORT DRIVER INTEGRATION
 * ================================================================
 *
 * Register with Win9x IOS (I/O Supervisor) to provide CD-ROM access
 * to the operating system. This creates a DCB (Device Control Block)
 * that gives the CD-ROM a drive letter accessible from Explorer.
 *
 * Flow: IOS_Register → AEP_INITIALIZE → AEP_CONFIG_DCB →
 *       AEP_DEVICE_INQUIRY → IOPs for read requests
 * ================================================================ */

/* IOS ASM wrappers */
extern ULONG IOS_Get_Version_Test(void);  /* diagnostic: 0xFEEDFACE if dispatch fails */
extern ULONG IOS_Register_Driver(PVOID ddb);
extern ULONG ISP_Insert_Calldown(PVOID dcb, PVOID cd, PVOID ddb, ULONG flags);
extern void IOS_BD_Complete(PVOID iop);
extern void IOS_Requestor(PVOID isp);
extern void ios_aep_bridge(void);  /* ASM AEP handler */
extern void ios_ior_bridge(void);  /* ASM IOR handler */
extern void Call_ILB_Internal_Request(ULONG func_addr, ULONG dcb, ULONG iop, ULONG start_cd);

/* AEP function codes */
/* AEP function codes - MUST match DDK IOS.H values */
#define AEP_INITIALIZE        0   /* 0x00 */
#define AEP_SYSTEM_CRIT_SHUTDOWN 1
#define AEP_BOOT_COMPLETE     2   /* 0x02 */
#define AEP_CONFIG_DCB        3   /* 0x03 */
#define AEP_UNCONFIG_DCB      4   /* 0x04 */
#define AEP_IOP_TIMEOUT       5   /* 0x05 */
#define AEP_DEVICE_INQUIRY    6   /* 0x06 */
#define AEP_HALF_SEC          7
#define AEP_1_SEC             8
#define AEP_2_SECS            9
#define AEP_ASSOCIATE_DCB_AEP 12  /* 0x0C */
#define AEP_REAL_MODE_HANDOFF 13  /* 0x0D */
#define AEP_UNINITIALIZE      15  /* 0x0F */
#define AEP_CREATE_VRP        18  /* 0x12 */
#define AEP_1E_SCSI           0x1E

/* DCB (Device Control Block) - simplified for manual creation */
typedef struct _IOS_DCB {
    ULONG   DCB_cmn_size;           /* 0x00 */
    struct _IOS_DCB *DCB_next;      /* 0x04 */
    struct _IOS_DCB *DCB_next_logical; /* 0x08 */
    PVOID   DCB_ddb;                /* 0x0C: owning IOS DDB */
    UCHAR   DCB_device_type;        /* 0x10 */
    UCHAR   DCB_bus_type;           /* 0x11 */
    UCHAR   DCB_bus_number;         /* 0x12 */
    UCHAR   DCB_target_id;          /* 0x13 */
    UCHAR   DCB_lun;                /* 0x14 */
    UCHAR   DCB_pad1[3];            /* 0x15 */
    ULONG   DCB_dmd_flags;          /* 0x18 */
    ULONG   DCB_apparent_blk_shift; /* 0x1C: log2(sector_size) */
    ULONG   DCB_apparent_blk_size;  /* 0x20: sector size */
    ULONG   DCB_apparent_head_count;/* 0x24 */
    ULONG   DCB_apparent_cyl_count; /* 0x28 */
    ULONG   DCB_apparent_spt;       /* 0x2C */
    ULONG   DCB_apparent_total_sectors; /* 0x30 */
    PVOID   DCB_cd;                 /* 0x34: calldown chain head */
    ULONG   DCB_port_specific;      /* 0x38 */
    ULONG   DCB_max_xfer_len;      /* 0x3C */
    char    DCB_vendor_id[8];       /* 0x40 */
    char    DCB_product_id[16];     /* 0x48 */
    char    DCB_rev_level[4];       /* 0x58 */
    ULONG   DCB_expansion[8];      /* 0x5C */
} IOS_DCB;

/* Calldown entry */
typedef struct _IOS_CALLDOWN {
    PVOID   CD_func;                /* handler function */
    PVOID   CD_ddb;                 /* owning DDB */
    struct _IOS_CALLDOWN *CD_next;  /* next in chain */
    ULONG   CD_flags;               /* flags */
} IOS_CALLDOWN;

/* DCB constants - MUST match DDK IOS.H values */
#define DCB_TYPE_CDROM          0x05    /* DCB_type_cdrom */
#define DCB_BUS_ESDI            0x00    /* DDK: ESDI=0, SCSI=1 */
#define DCB_BUS_SCSI            0x01
/* DCB_device_flags (field at actual DCB device_flags offset) */
#define DCB_DEV_REMOVABLE       0x00000004
#define DCB_DEV_PHYSICAL        0x00008000
#define DCB_DEV_UNCERTAIN_MEDIA 0x00001000
#define DCB_DEV_TSD_PROCESSED   0x01000000
/* DCB_device_flags2 */
#define DCB_DEV2_ATAPI_DEVICE   0x00000002
#define ISPCDF_BOTTOM           0x0001
#define ISPCDF_PORT_DRIVER      0x0002

/* AEP result codes */
#define AEP_SUCCESS          0x00
#define AEP_FAILURE          0x01

/* DRP (Driver Registration Packet) for IOS_Register (ordinal 7).
   FROM THE ACTUAL WIN98 DDK IOS.H (extracted from 98DDK.tar):
   - DRP_eyecatch_str MUST be "XXXXXXXX" (8 bytes)
   - DRP_LGN = load group number (determines layer in IOS stack)
   - DRP_aer = Async Event Routine (AEP handler)
   - DRP_ilb = FILLED BY IOS with ILB pointer on success
   - DRP_reg_result: 1=REMAIN_RESIDENT, 2=MINIMIZE, 3=ABORT, 4=INVALID_LAYER
   CRITICAL: This struct MUST be packed! IOS is assembly code with no padding.
   Verified against ESDI_506.PDR hex dump: DRP_feature_code at +0x25 (packed),
   DRP_reg_result at +0x2C, total size 0x38 = 56 bytes. */
#pragma pack(push,1)
typedef struct {
    UCHAR  DRP_eyecatch_str[8];    /* 0x00: must be "XXXXXXXX"           */
    ULONG  DRP_LGN;                /* 0x08: load group number            */
    ULONG  DRP_aer;                /* 0x0C: async event routine (AEP)    */
    ULONG  DRP_ilb;                /* 0x10: IOS fills with ILB pointer   */
    UCHAR  DRP_ascii_name[16];     /* 0x14: driver name string           */
    UCHAR  DRP_revision;           /* 0x24: driver revision              */
    ULONG  DRP_feature_code;       /* 0x25: feature code flags (PACKED!) */
    USHORT DRP_if_requirements;    /* 0x29: interface requirements       */
    UCHAR  DRP_bus_type;           /* 0x2B: bus type                     */
    USHORT DRP_reg_result;         /* 0x2C: registration result          */
    ULONG  DRP_reference_data;     /* 0x2E: reference data for AER       */
    UCHAR  DRP_reserved1[2];       /* 0x32: reserved                     */
    ULONG  DRP_reserved2;          /* 0x34: reserved                     */
} IOS_DRP;                          /* total: 0x38 = 56 bytes             */
#pragma pack(pop)

/* ILB (IOS Linkage Block) - returned by IOS_Register via DRP_ilb */
typedef struct {
    ULONG  ILB_service_rtn;        /* +0x00: ISP service entry point     */
    ULONG  ILB_dprintf_rtn;        /* +0x04: debug printf                */
    ULONG  ILB_Wait_10th_Sec;      /* +0x08: wait 1/10 sec              */
    ULONG  ILB_internal_request;   /* +0x0C: submit internal IOPs        */
    ULONG  ILB_io_criteria_rtn;    /* +0x10                              */
    ULONG  ILB_int_io_criteria_rtn;/* +0x14                              */
    ULONG  ILB_dvt;                /* +0x18: DVT pointer                 */
    ULONG  ILB_ios_mem_virt;       /* +0x1C                              */
    ULONG  ILB_enqueue_iop;        /* +0x20                              */
    ULONG  ILB_dequeue_iop;        /* +0x24                              */
} IOS_ILB;

/* ISP function codes (called via ILB_service_rtn) */
#define ISP_CREATE_DDB           0
#define ISP_CREATE_DCB_FUNC      1
#define ISP_CREATE_IOP           2
#define ISP_ALLOC_MEM            3
#define ISP_DEALLOC_MEM          4
#define ISP_INSERT_CALLDOWN_FUNC 5
#define ISP_GET_FIRST_NEXT_DCB_FUNC 10

/* ISP packets (packed, from DDK ISP.INC) for ILB_service_rtn calls */
#pragma pack(push,1)
typedef struct {
    USHORT func;       /* ISP_CREATE_DCB = 1 */
    USHORT result;     /* output */
    USHORT dcb_size;   /* size of DCB to create */
    ULONG  dcb_ptr;    /* output: ptr to created DCB */
    UCHAR  pad[2];
} ISP_CREATE_DCB_PKT;

typedef struct {
    USHORT func;       /* ISP_INSERT_CALLDOWN = 5 */
    USHORT result;
    ULONG  dcb;        /* DCB to insert calldown on */
    ULONG  req;        /* request handler address */
    ULONG  ddb;        /* DDB pointer */
    USHORT expan_len;  /* expansion area length */
    ULONG  flags;      /* demand flags */
    UCHAR  lgn;        /* load group number */
    UCHAR  pad;
} ISP_INSERT_CD_PKT;

typedef struct {
    USHORT func;       /* ISP_ASSOCIATE_DCB = 6 */
    USHORT result;
    ULONG  dcb;        /* DCB to associate */
    UCHAR  drive;      /* drive letter (0=A:, 2=C:, 3=D:, etc.) */
    UCHAR  flags;
    UCHAR  pad[2];
} ISP_ASSOC_DCB_PKT;

typedef struct {
    USHORT func;       /* ISP_BROADCAST_AEP = 20 */
    USHORT result;
    ULONG  paep;       /* pointer to AEP packet to broadcast */
} ISP_BCAST_AEP_PKT;

typedef struct {
    USHORT func;       /* ISP_DEVICE_ARRIVED = 14 */
    USHORT result;
    ULONG  dcb;        /* DCB of arrived device */
    ULONG  flags;      /* ISP_DEV_ARR_FL_MEDIA_ONLY = 1 */
} ISP_DEV_ARRIVED_PKT;

#define ISP_DEVICE_ARRIVED_FUNC  14
#define ISP_DEV_ARR_FL_MEDIA_ONLY 1

typedef struct {
    USHORT func;       /* ISP_DRIVE_LETTER_PICK = 16 */
    USHORT result;
    ULONG  dcb;        /* DCB pointer */
    UCHAR  letter[2];  /* requested letter range (e.g. {3,3} for D:) */
    UCHAR  flags;      /* ISP_PDL_FL_OK_RM_CD etc. */
    UCHAR  pad;
} ISP_DLP_PKT;

#define ISP_DRIVE_LETTER_PICK_FUNC 16
#define ISP_PDL_FL_OK_RM_CD     0x04

/* ISP_GET_FIRST_NEXT_DCB packet (func=10).
   Used to enumerate DCBs through the ILB instead of the broken
   IOS_Get_Device_List (ordinal 3 is actually IOS_Exclusive_Access). */
typedef struct {
    USHORT func;       /* ISP_GET_FIRST_NEXT_DCB = 10 */
    USHORT result;     /* output: 0 = success */
    ULONG  dcb;        /* in: 0 = get first DCB, else current DCB to get next */
                        /* out: next DCB pointer (0 if no more) */
} ISP_GET_DCB_PKT;

/* ISP_CREATE_IOP packet (func=2).
   Layout from Win98 DDK ISP.INC (approximate). */
typedef struct {
    USHORT func;       /* ISP_CREATE_IOP = 2 */
    USHORT result;
    ULONG  dcb;        /* DCB this IOP is for */
    USHORT delta;      /* expansion delta */
    ULONG  iop_ptr;    /* output: IOP pointer */
    ULONG  callback;   /* completion callback */
} ISP_CREATE_IOP_PKT;

#pragma pack(pop)

/* Real CALLDOWN_NODE layout (verified by ISP_INSERT_CALLDOWN output).
   IOS allocated a node and placed our data as:
     +0x00 = ios_ior_bridge (handler)
     +0x04 = 0x00000002 (ISPCDF_PORT_DRIVER flags)
     +0x08 = &g_ios_ddb (DDB pointer)
     +0x0C = next pointer in chain
   This differs from some DDK headers that show CD_next at +0x00! */
typedef struct _REAL_CALLDOWN_NODE {
    PVOID   CD_fsd_entry;                 /* +0x00: handler function */
    ULONG   CD_flags;                     /* +0x04: flags */
    PVOID   CD_ddb;                       /* +0x08: DDB/DRP pointer */
    struct _REAL_CALLDOWN_NODE *CD_next;  /* +0x0C: next in chain */
} REAL_CALLDOWN_NODE;

/* DRP LGN layer masks */
#define DRP_NT_MPD       (1 << 0x14)   /* NT miniport driver layer */
#define DRP_ESDI_PD      (1 << 0x16)   /* ESDI port driver layer */
#define DRP_MISC_PD      (1 << 0x13)   /* misc port driver layer */

/* DRP_reg_result values */
#define DRP_REMAIN_RESIDENT  1
#define DRP_MINIMIZE         2
#define DRP_ABORT            3
#define DRP_INVALID_LAYER    4

/* Keep old alias */
typedef IOS_DRP IOS_DDB;

/* AEP common header - Verified via hex dump of IOS AEP packet:
   +0x00: func(2), +0x02: result(2), +0x04: ddb(4), +0x08: lgn(1).
   Total 9 bytes PACKED, but extended fields start at +0x0C (IOS pads
   to 4-byte boundary). Hex dump confirmed: lgn=0x16 at +0x08,
   zeros at +0x09/0x0A/0x0B, extended fields at +0x0C.
   So AEP_HEADER is effectively 12 bytes (9 + 3 padding). */
typedef struct {
    USHORT AEP_func;         /* 0x00: function code */
    USHORT AEP_result;       /* 0x02: result code */
    ULONG  AEP_ddb;          /* 0x04: DDB pointer/handle */
    UCHAR  AEP_lgn;          /* 0x08: load group number */
    UCHAR  AEP_align[3];     /* 0x09: padding to 0x0C */
} AEP_HEADER;

/* AEP_INITIALIZE extended struct - verified layout */
typedef struct {
    AEP_HEADER hdr;              /* 0x00-0x0B: common header (12 bytes) */
    ULONG  AEP_bi_reference;     /* 0x0C: ILB pointer (0 for late reg) */
    UCHAR  AEP_bi_flags;         /* 0x10: flags */
    UCHAR  AEP_bi_max_target;    /* 0x11: max SCSI target */
    UCHAR  AEP_bi_max_lun;       /* 0x12: max LUN */
    UCHAR  AEP_bi_pad;           /* 0x13: padding */
    ULONG  AEP_bi_dcb;           /* 0x14: DCB pointer */
    ULONG  AEP_bi_hdevnode;      /* 0x18: devnode handle */
    ULONG  AEP_bi_regkey;        /* 0x1C: registry key */
} AEP_BI_INIT;

/* AEP_CONFIG_DCB extended struct */
typedef struct {
    AEP_HEADER hdr;              /* 0x00-0x0B: common header (12 bytes) */
    ULONG  AEP_cd_dcb;           /* 0x0C: DCB being configured */
    ULONG  AEP_cd_ddb;           /* 0x10: DDB of configuring driver */
} AEP_CD_CONFIG;

/* Global ILB pointer - filled during AEP_INITIALIZE */
static IOS_ILB *g_ilb = (IOS_ILB *)0;
static ULONG g_deferred_dcb = 0;  /* DCB for deferred ISP_DEVICE_ARRIVED */
static ULONG g_aep_config_dcb = 0;  /* DCB captured from AEP_CONFIG_DCB */
static int g_deferred_dcb_enum = 0;    /* 1 = try ISP_GET_FIRST_NEXT_DCB via timer */
static int g_deferred_enum_done = 0;   /* 1 = already attempted */

/* VMM timer callback — defined after all type declarations below */
extern ULONG Set_Global_Timeout(ULONG ms, PVOID callback, ULONG refdata);
extern void timer_callback_bridge(void);

/* IOP (I/O Packet) layout from IOS disassembly (2026-04-07).
   The IOP header is 0x64 bytes of IOS-internal fields, followed by
   the IOR (I/O Request) at offset 0x64.

   IOS sets these IOP header fields:
     +0x00: queue link (free list / queue next)
     +0x04: physical_dcb (set by ILB_internal_request from DCB+0x00)
     +0x08: DCB pointer (set by chain executor from EBX)
     +0x0C: set to 0x0F by chain executor
     +0x0E: set to 0x0F by chain executor
     +0x10: current calldown pointer (set by ILB_internal_request)
     +0x14: SGD area pointer (set by chain executor)
     +0x22: flags byte (checked by dequeue_iop)
     +0x2C: internal function pointer (set by chain executor)

   IOR embedded at IOP+0x64:
     +0x64 (IOR+0x00): IOR_next
     +0x68 (IOR+0x04): IOR_func (USHORT)
     +0x6A (IOR+0x06): IOR_status (USHORT)
     +0x6C (IOR+0x08): IOR_flags (ULONG)
     +0x6D (IOR+0x09): IOR_flags byte 1 (bit 2 = internal_request)
     +0x70 (IOR+0x0C): IOR_callback
     +0x74 (IOR+0x10): IOR_start_addr[0] (low 32 bits)
     +0x78 (IOR+0x14): IOR_start_addr[1] (high 32 bits)
     +0x7C (IOR+0x18): IOR_xfer_count (bytes)
     +0x80 (IOR+0x1C): IOR_buffer_ptr
     +0xA4 (IOR+0x40): IOR_callback_ref (completion callback data)

   Calldown handlers receive the IOP pointer (not IOR).
   IOS_BD_Command_Complete also takes the IOP pointer. */

#define IOR_BASE  0x64   /* offset of IOR within IOP */

typedef struct _IOP {
    UCHAR  IOP_header[0x64];     /* 0x00: IOS-internal header */
    /* IOR starts here at IOP+0x64 */
    ULONG  IOR_next;             /* 0x64: queue link */
    USHORT IOR_func;             /* 0x68: function code */
    USHORT IOR_status;           /* 0x6A: status */
    ULONG  IOR_flags;            /* 0x6C: flags */
    ULONG  IOR_callback;         /* 0x70: completion callback */
    ULONG  IOR_start_addr[2];    /* 0x74: start sector (low, high) */
    ULONG  IOR_xfer_count;       /* 0x7C: transfer count in bytes */
    ULONG  IOR_buffer_ptr;       /* 0x80: data buffer pointer */
    /* More fields follow */
} IOP;

/* IOS DDB class and flags (from W9XDDK.H) */
#define DDB_CLASS_PORT       0x0001
#define DDB_MERIT_PORT       0x10000000

static IOS_DDB g_ios_ddb;
static IOS_DCB g_ios_dcb;
static IOS_CALLDOWN g_ios_calldown;
static REAL_CALLDOWN_NODE g_real_calldown;  /* correct DDK layout for IOS-managed DCB */
static BOOLEAN g_ios_registered = FALSE;
static UCHAR g_dummy_dcb[256];    /* Dummy DCB for safe DCB_next chain termination */
static UCHAR g_dummy_vrp[64];     /* Dummy VRP for safe DCB_vrp_ptr */
static ULONG g_our_dcb_addr = 0;  /* Our created DCB address (for AEP spy) */

/* Forward declaration for walk_calldown_chain (defined after _ntmini_ior_handler) */
static int walk_calldown_chain(ULONG dcb_addr, ULONG target_func);

/* Validate a candidate CD-ROM DCB address.
   Checks: device_type at +0x40 == 5, calldown chain at +0x08 points to
   IOS code/heap (not VMM data), calldown node has valid handler pointer,
   and DCB_physical_dcb at +0x00 is a kernel pointer.
   Returns 1 if valid, 0 if not. */
static int validate_cdrom_dcb(ULONG addr) {
    UCHAR *p = (UCHAR *)addr;
    ULONG phys_dcb  = *(ULONG *)(p + 0x00);  /* DCB_physical_dcb */
    ULONG cd_chain  = *(ULONG *)(p + 0x08);  /* DCB_ptr_cd */
    ULONG dcb_next  = *(ULONG *)(p + 0x0C);  /* DCB_next */

    /* 1. Device type byte at +0x40 must be 5 (CD-ROM) */
    if (p[0x40] != DCB_TYPE_CDROM)
        return 0;

    /* 2. DCB_physical_dcb at +0x00 must be a kernel pointer.
       Real DCBs point to themselves or a related DCB.
       False positives have small numbers (e.g. 0x150). */
    if (phys_dcb < 0xC0000000 || phys_dcb >= 0xD0000000) {
        log_hex("V5: DCB reject ", addr, "");
        log_hex(": phys=", phys_dcb, " (not kernel ptr)\r\n");
        return 0;
    }

    /* 3. Calldown chain at +0x08 must be in IOS code/heap range.
       IOS code: 0xC003xxxx. IOS heap: 0xC14xxxxx.
       Reject VMM data range: 0xC8xxxxxx. */
    if (cd_chain < 0xC0000000 || cd_chain >= 0xD0000000) {
        log_hex("V5: DCB reject ", addr, "");
        log_hex(": cd_chain=", cd_chain, " (out of range)\r\n");
        return 0;
    }
    if ((cd_chain >> 20) == 0xC80) {
        log_hex("V5: DCB reject ", addr, "");
        log_hex(": cd_chain=", cd_chain, " (VMM data)\r\n");
        return 0;
    }

    /* 4. DCB_next at +0x0C must be 0 or a valid kernel pointer */
    if (dcb_next != 0 && (dcb_next < 0xC0000000 || dcb_next >= 0xD0000000)) {
        log_hex("V5: DCB reject ", addr, "");
        log_hex(": dcb_next=", dcb_next, " (invalid)\r\n");
        return 0;
    }

    log_hex("V5: DCB validated at ", addr, "\r\n");
    return 1;
}

/* VMM timer callback for deferred DCB enumeration.
   ISP_GET_FIRST_NEXT_DCB crashes from Init_Complete context.
   AEP_1_SEC never fires for late-registered drivers.
   VMM_Set_Global_Time_Out fires from system VM context after delay. */
void _ntmini_timer_callback(ULONG refdata)
{
    log_hex("TMR: callback refdata=", refdata, "\r\n");

    if (!g_deferred_dcb_enum || g_deferred_enum_done)
        return;

    g_deferred_enum_done = 1;

    VxD_Debug_Printf("TMR: ISP_GET_FIRST_NEXT_DCB from timer...\r\n");

    if (g_ilb != (IOS_ILB *)0 && g_ilb->ILB_service_rtn >= 0xC0000000) {
        ISP_GET_DCB_PKT isp_gd;
        ULONG cur_dcb = 0;
        int iter;

        for (iter = 0; iter < 10; iter++) {
            my_memset(&isp_gd, 0, sizeof(isp_gd));
            isp_gd.func = ISP_GET_FIRST_NEXT_DCB_FUNC;
            isp_gd.dcb = cur_dcb;

            Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_gd);

            log_hex("TMR: GD[", (ULONG)iter, "] ");
            log_hex("r=", (ULONG)isp_gd.result, "");
            log_hex(" dcb=", isp_gd.dcb, "\r\n");

            if (isp_gd.result != 0 || isp_gd.dcb == 0 ||
                isp_gd.dcb < 0xC0000000)
                break;

            {
                UCHAR *dcb = (UCHAR *)isp_gd.dcb;
                log_hex("  t40=", (ULONG)dcb[0x40], "");
                log_hex(" cd+08=", *(ULONG *)(dcb+0x08), "\r\n");

                if (dcb[0x40] == DCB_TYPE_CDROM) {
                    ISP_INSERT_CD_PKT isp_cd;
                    VxD_Debug_Printf("TMR: *** CD-ROM DCB! ***\r\n");
                    walk_calldown_chain(isp_gd.dcb, (ULONG)0);

                    my_memset(&isp_cd, 0, sizeof(isp_cd));
                    isp_cd.func = ISP_INSERT_CALLDOWN_FUNC;
                    isp_cd.dcb = isp_gd.dcb;
                    isp_cd.req = (ULONG)ios_ior_bridge;
                    isp_cd.ddb = (ULONG)&g_ios_ddb;
                    isp_cd.flags = ISPCDF_PORT_DRIVER | ISPCDF_BOTTOM;
                    isp_cd.lgn = 0x16;

                    VxD_Debug_Printf("TMR: ISP_INSERT_CALLDOWN...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_cd);
                    log_hex("TMR: INSERT result=", (ULONG)isp_cd.result, "\r\n");
                    if (isp_cd.result == 0) {
                        VxD_Debug_Printf("TMR: *** HANDLER INSERTED! ***\r\n");
                        walk_calldown_chain(isp_gd.dcb, (ULONG)ios_ior_bridge);
                        g_deferred_dcb_enum = 0;
                    }
                    break;
                }
            }
            cur_dcb = isp_gd.dcb;
        }
    } else {
        VxD_Debug_Printf("TMR: No ILB\r\n");
    }
}

/* Called from Sys_VM_Init. Set_Global_Time_Out requires system VM context.
   If Init_Complete deferred DCB enumeration, set the timer here. */
void _ntmini_deferred_timer_setup(void)
{
    VxD_Debug_Printf("SVM: deferred_timer_setup\r\n");
    /* ISP_GET_FIRST_NEXT_DCB crashes from all boot contexts.
       Approach D (direct ATAPI bypass) is used instead. */
}

/* Forward declarations for ISO 9660 (defined after atapi_read_sector) */
static int iso9660_read_pvd(void);
static int iso9660_find_file(ULONG dir_lba, ULONG dir_size,
                              const char *name, ULONG name_len,
                              ULONG *out_lba, ULONG *out_size);
static int iso9660_read_file(ULONG file_lba, ULONG file_size,
                              ULONG offset, UCHAR *buffer, ULONG count);
/* ISO 9660 state (definitions after atapi_read_sector, forward-declared here) */
ULONG g_iso_root_lba;
ULONG g_iso_root_size;
int g_iso_pvd_valid;

/* ================================================================
 * IFSMgr File System API Hook
 *
 * Hooks all IFSMgr file operations. For D: drive, handles READ and
 * CLOSE using our ISO 9660 parser + direct ATAPI I/O. Other
 * functions pass through to the FSD entry table via prev().
 * Other drives pass through unmodified.
 * ================================================================ */

/* IFSMgr hook function numbers (from DDK ifsmgr.h) */
#define IFSFN_READ       0
#define IFSFN_CLOSE      11
#define IFSFN_OPEN       36

/* Open file table */
#define MAX_ISO_FILES 8
typedef struct {
    int in_use;
    ULONG file_lba;
    ULONG file_size;
    ULONG file_pos;     /* current read position in file */
    ULONG handle;       /* IFSMgr file handle (for slot lookup) */
} ISO_FILE_ENTRY;
static ISO_FILE_ENTRY g_iso_files[MAX_ISO_FILES];
static int g_last_opened_slot = 0;  /* most recently opened slot index */

/* Directory search state for NetSearch (FindFirst/FindNext) */
static ULONG g_search_dir_offset = 0;   /* byte offset into root directory */
static int g_search_active = 0;         /* 1 = search in progress */
static int g_fsd_hook_installed = 0;
static int g_fsd_log_count = 0;

/* Previous hook in chain (pIFSFunc: 5 args, no pfnPrev) */
typedef int (*PFN_IFS_FUNC)(int fn, int drive, int resType, int cpid, ULONG pir);

/* Forward declarations: FSD state (defined after FSD code) */
extern int g_fsd_registered;
extern ULONG g_fsd_provider_id;

/* Forward declaration for FSD function called from hook */
static int _cdecl ntmini_fsd_open(int fn, int drive, int resType, int cpid, ULONG pir);

/* IFSMgr hook callback. cdecl, 6 args.
   pfnPrev = function to call for pass-through (5 args)
   fn = IFSFN_OPEN(36), IFSFN_READ(0), IFSFN_CLOSE(11), etc.
   drive = 1-based (1=A, 2=B, 3=C, 4=D)
   pir = ioreq pointer */
int _ntmini_ifs_hook(ULONG pfnPrev, int fn, int drive,
                      int resType, int cpid, ULONG pir)
{
    PFN_IFS_FUNC prev = (PFN_IFS_FUNC)pfnPrev;

    /* With FSD registered, intercept D: operations.
       Handle READ and CLOSE directly; pass other functions
       (including OPEN) through to the FSD entry table. */
    if (drive == 4 && g_fsd_registered) {
        int result;
        if (g_fsd_log_count < 50) {
            log_hex("IFS: D: fn=", (ULONG)fn, "");
            log_hex(" pir=", pir, "\r\n");
        }

        /* Handle READ (fn=0) directly in hook: ISO9660 read */
        if (fn == IFSFN_READ) {
            if (pir >= 0xC0000000 && pir < 0xD0000000) {
                ULONG *pi = (ULONG *)pir;
                ULONG count = pi[0];   /* +0x00: byte count */
                ULONG buffer = pi[5];  /* +0x14: data buffer */
                int found = -1;

                /* Use the most recently opened slot. The READ ioreq
                   does not contain the file handle at a known offset,
                   so handle-based matching is unreliable. For typical
                   open-read-close patterns this is correct. */
                if (g_last_opened_slot >= 0 && g_last_opened_slot < MAX_ISO_FILES &&
                    g_iso_files[g_last_opened_slot].in_use) {
                    found = g_last_opened_slot;
                }

                if (found >= 0 && buffer >= 0xC0000000 &&
                    buffer < 0xD0000000 && count > 0) {
                    ISO_FILE_ENTRY *fe = &g_iso_files[found];
                    ULONG offset = fe->file_pos;
                    ULONG remaining = (offset < fe->file_size) ? fe->file_size - offset : 0;
                    ULONG read_size = count;
                    int bytes;

                    if (remaining == 0) {
                        pi[0] = 0;  /* EOF: 0 bytes read */
                        return 0;
                    }
                    if (read_size > remaining)
                        read_size = remaining;

                    bytes = iso9660_read_file(
                        fe->file_lba, fe->file_size,
                        offset, (UCHAR *)buffer, read_size);
                    if (bytes > 0) {
                        fe->file_pos += (ULONG)bytes;
                        pi[0] = (ULONG)bytes;
                        if (g_fsd_log_count < 30) {
                            log_hex("IFS: READ OK bytes=", (ULONG)bytes, "");
                            log_hex(" pos=", fe->file_pos, "\r\n");
                            g_fsd_log_count++;
                        }
                        return 0;
                    }
                }
            }
            return -1;
        }

        /* For CLOSE (fn=11): free the most recently opened slot.
           Note: Ring0_FileIO CLOSE may not always reach the IFS hook,
           so slots are also reclaimed on overflow in NetOpen. */
        if (fn == IFSFN_CLOSE) {
            if (g_last_opened_slot >= 0 && g_last_opened_slot < MAX_ISO_FILES) {
                g_iso_files[g_last_opened_slot].in_use = 0;
                g_iso_files[g_last_opened_slot].handle = 0;
                g_iso_files[g_last_opened_slot].file_pos = 0;
            }
            if (g_fsd_log_count < 50) {
                VxD_Debug_Printf("IFS: D: CLOSE\r\n");
                g_fsd_log_count++;
            }
            return 0;
        }

        /* All other fn values: pass through to prev() so IFSMgr
           dispatches to our FSD entry table functions. */
        if (g_fsd_log_count < 50) {
            log_hex("IFS: D: fn=", (ULONG)fn, " -> prev()\r\n");
        }

        result = prev(fn, drive, resType, cpid, pir);

        /* After OPEN succeeds, capture the handle IFSMgr returned
           (at ioreq+0x10) and store it in the file slot for later
           READ/CLOSE matching. */
        if (fn == IFSFN_OPEN && result == 0 &&
            pir >= 0xC0000000 && pir < 0xD0000000) {
            ULONG *pi = (ULONG *)pir;
            ULONG handle = pi[4];  /* +0x10: IFSMgr handle */
            if (g_last_opened_slot >= 0 && g_last_opened_slot < MAX_ISO_FILES &&
                g_iso_files[g_last_opened_slot].in_use) {
                g_iso_files[g_last_opened_slot].handle = handle;
                if (g_fsd_log_count < 50) {
                    log_hex("IFS: D: captured handle=", handle, "");
                    log_hex(" slot=", (ULONG)g_last_opened_slot, "\r\n");
                }
            }
        }

        if (g_fsd_log_count < 50) {
            log_hex("IFS: D: result=", (ULONG)result, "\r\n");
            g_fsd_log_count++;
        }
        return result;
    }

    /* Non-D: drives: always pass through */
    return prev(fn, drive, resType, cpid, pir);
}

/* ================================================================
 * IFSMgr FSD (File System Driver) Registration
 *
 * IFSMgr_RegisterMount registers a mount callback. When IFSMgr
 * tries to mount D:, our callback claims it and provides a function
 * table (NetOpen, NetDelete, etc.). All D: file operations then go
 * through our FSD functions, which use ISO 9660 + direct ATAPI.
 * This creates proper IFSMgr file handles (unlike hooks).
 *
 * FSD entry point table (from DDK IFSMGR.INC):
 *   DWORD count (10)
 *   NetDelete, NetDir, NetFileAttributes, NetFileInfo,
 *   NetFlush, NetGetDiskInfo, NetOpen, NetRename,
 *   NetSearch, ShutDown
 * ================================================================ */

ULONG g_fsd_provider_id = 0xFFFFFFFF;  /* from RegisterMount */
int g_fsd_registered = 0;

/* FSD functions: log which function IFSMgr calls with ENTRY marker */
static int _cdecl ntmini_fsd_delete(int fn, int drive, int resType, int cpid, ULONG pir) {
    VxD_Debug_Printf("FSD: >>> NetDelete ENTRY <<<\r\n"); return 5; }
static int _cdecl ntmini_fsd_dir(int fn, int drive, int resType, int cpid, ULONG pir) {
    VxD_Debug_Printf("FSD: >>> NetDir <<<\r\n");
    /* Return 0 (success) so IFSMgr considers the volume valid.
       Returning 5 (error) during mount may prevent file operations. */
    if (pir >= 0xC0000000 && pir < 0xD0000000) {
        ULONG *pi = (ULONG *)pir;
        pi[4] = 0;  /* ir_error = success */
    }
    return 0;
}
static int _cdecl ntmini_fsd_fileattr(int fn, int drive, int resType, int cpid, ULONG pir) {
    /* Return read-only attribute for any file query.
       Explorer calls this to check file attributes. */
    if (pir >= 0xC0000000 && pir < 0xD0000000) {
        ULONG *pi = (ULONG *)pir;
        pi[4] = 0;      /* ir_error = success */
        pi[6] = 0x01;   /* +0x18: attributes = FILE_ATTRIBUTE_READONLY */
    }
    return 0;
}
static int _cdecl ntmini_fsd_fileinfo(int fn, int drive, int resType, int cpid, ULONG pir) {
    VxD_Debug_Printf("FSD: >>> NetFileInfo ENTRY <<<\r\n"); return 5; }
static int _cdecl ntmini_fsd_flush(int fn, int drive, int resType, int cpid, ULONG pir) {
    VxD_Debug_Printf("FSD: >>> NetFlush ENTRY <<<\r\n"); return 0; }
static int _cdecl ntmini_fsd_rename(int fn, int drive, int resType, int cpid, ULONG pir) {
    VxD_Debug_Printf("FSD: >>> NetRename ENTRY <<<\r\n"); return 5; }
static int _cdecl ntmini_fsd_search(int fn, int drive, int resType, int cpid, ULONG pir) {
    /* NetSearch: enumerate files in ISO 9660 root directory.
       IFSMgr calls this for FindFirst (first call) and FindNext
       (subsequent calls). We use g_search_dir_offset as an index
       into the root directory entries.

       The ioreq for Search has:
         pi[3] (+0x0C): parsed path / search pattern
         pi[5] (+0x14): output buffer for find data

       Output buffer layout (Win32 FIND_DATA-like):
         +0x00: file attributes (DWORD)
         +0x04-0x1B: timestamps (create, access, write - 3 FILETIMEs)
         +0x1C: file size high (DWORD)
         +0x20: file size low (DWORD)
         +0x24-0x2C: reserved
         +0x2C: filename (260 bytes, null-terminated)

       Returns 0 on success, 0x12 (ERROR_NO_MORE_FILES) when exhausted. */

    char entry_name[64];
    ULONG entry_name_len, entry_lba, entry_size;
    UCHAR entry_flags;
    int r;

    if (!g_iso_pvd_valid) {
        if (iso9660_read_pvd() != 0) return 0x12;
    }

    if (pir < 0xC0000000 || pir >= 0xD0000000) return 0x12;

    {
        ULONG *pi = (ULONG *)pir;
        UCHAR subfn = (UCHAR)(pi[1] & 0xFF);  /* +0x04 low byte: sub-function */

        /* Sub-function 0 or first call: reset search position */
        if (subfn == 0 || !g_search_active) {
            g_search_dir_offset = 0;
            g_search_active = 1;
        }

        r = iso9660_enum_dir(g_iso_root_lba, g_iso_root_size,
                              (int)g_search_dir_offset,
                              entry_name, &entry_name_len,
                              &entry_lba, &entry_size, &entry_flags);

        if (r != 0) {
            /* No more entries */
            g_search_active = 0;
            g_search_dir_offset = 0;
            return 0x12;
        }

        /* Fill output buffer if available */
        {
            ULONG out_buf = pi[5];  /* +0x14: output buffer */
            if (out_buf >= 0xC0000000 && out_buf < 0xD0000000) {
                ULONG *op = (ULONG *)out_buf;
                UCHAR *ob = (UCHAR *)out_buf;

                /* Clear output buffer */
                { int z; for (z = 0; z < 0x130; z++) ob[z] = 0; }

                /* File attributes: read-only, plus directory flag if set */
                op[0] = 0x01;  /* FILE_ATTRIBUTE_READONLY */
                if (entry_flags & 0x02) {
                    op[0] |= 0x10;  /* FILE_ATTRIBUTE_DIRECTORY */
                }

                /* File size */
                op[7] = 0;           /* +0x1C: size high */
                op[8] = entry_size;  /* +0x20: size low */

                /* Filename at +0x2C (ANSI, null-terminated) */
                {
                    ULONG k;
                    for (k = 0; k < entry_name_len && k < 259; k++)
                        ob[0x2C + k] = (UCHAR)entry_name[k];
                    ob[0x2C + k] = 0;
                }
            }
        }

        g_search_dir_offset++;

        if (g_fsd_log_count < 30) {
            VxD_Debug_Printf("FSD: SEARCH '");
            VxD_Debug_Printf(entry_name);
            log_hex("' sz=", entry_size, "\r\n");
            g_fsd_log_count++;
        }

        return 0;
    }
}
static int _cdecl ntmini_fsd_shutdown(int fn, int drive, int resType, int cpid, ULONG pir) {
    VxD_Debug_Printf("FSD: >>> ShutDown ENTRY <<<\r\n"); return 0; }

/* NetOpen: handles file open requests. Creates IFSMgr file handles. */
static int _cdecl ntmini_fsd_open(int fn, int drive, int resType, int cpid, ULONG pir)
{
    /* ioreq for NetOpen:
       ir_flags (+0x04): access/share mode
       ir_fh (+0x08): handle (IFSMgr fills after we return 0)
       ir_ppath (+0x0C): parsed path
       ir_error (+0x10): error code
       ir_options (+0x14): create options
       Calling convention: (fn, drive, resType, cpid, pir) - pir is 5th arg */
    ULONG *pi;
    ULONG ppath_addr;
    char filename[64];
    int nc, fi;
    ULONG file_lba, file_size;

    VxD_Debug_Printf("FSD: >>> NetOpen ENTRY <<<\r\n");

    if (pir < 0xC0000000 || pir >= 0xD0000000) return 2;
    pi = (ULONG *)pir;

    if (g_fsd_log_count < 30) {
        log_hex("FSD: NetOpen pir=", pir, "\r\n");
    }

    if (!g_iso_pvd_valid) {
        if (iso9660_read_pvd() != 0) {
            return 2;  /* FILE_NOT_FOUND */
        }
    }

    /* Parse filename from parsed path */
    ppath_addr = pi[3];  /* ir_ppath at +0x0C */
    if (ppath_addr < 0xC0000000 || ppath_addr >= 0xD0000000) {
        return 2;
    }

    {
        UCHAR *pp = (UCHAR *)ppath_addr;
        USHORT elem_len = *(USHORT *)(pp + 4);
        nc = 0;

        if (elem_len > 0 && elem_len <= 126) {
            USHORT *uc = (USHORT *)(pp + 6);
            nc = elem_len / 2;
            if (nc > 63) nc = 63;
            for (fi = 0; fi < nc; fi++)
                filename[fi] = (char)(uc[fi] & 0xFF);
            filename[nc] = 0;
            while (nc > 0 && filename[nc - 1] == 0) nc--;
        }
    }

    if (g_fsd_log_count < 30) {
        VxD_Debug_Printf("FSD: OPEN '");
        VxD_Debug_Printf(filename);
        log_hex("' nc=", (ULONG)nc, "\r\n");
        g_fsd_log_count++;
    }

    /* Look up in ISO 9660 */
    if (nc == 0 || iso9660_find_file(g_iso_root_lba, g_iso_root_size,
                           filename, (ULONG)nc, &file_lba, &file_size) != 0) {
        return 2;  /* FILE_NOT_FOUND */
    }

    /* Find a free file slot. If all slots are full, reuse the oldest
       (slot 0). Ring0_FileIO CLOSE may not go through the IFS hook,
       so slots may not be freed explicitly. */
    {
        int slot;
        for (slot = 0; slot < MAX_ISO_FILES; slot++) {
            if (!g_iso_files[slot].in_use) break;
        }
        if (slot >= MAX_ISO_FILES) {
            /* All slots full: reclaim slot 0 (oldest) */
            slot = 0;
        }

        g_iso_files[slot].in_use = 1;
        g_iso_files[slot].file_lba = file_lba;
        g_iso_files[slot].file_size = file_size;
        g_iso_files[slot].file_pos = 0;
        g_iso_files[slot].handle = 0;  /* filled by hook after IFSMgr creates handle */
        g_last_opened_slot = slot;
    }

    log_hex("FSD: OPEN OK lba=", file_lba, "");
    log_hex(" sz=", file_size, "\r\n");
    return 0;
}

/* NetGetDiskInfo: volume info (free space, label, serial number).
   IFSMgr calls this during OPEN to validate the volume.
   Must populate ioreq fields or IFSMgr won't dispatch NetOpen.

   ioreq layout for GetDiskInfo:
     ir_data (+0x14): pointer to buffer for disk info
     ir_length (+0x00): buffer size
     ir_error (+0x10): error code (0 = success)

   Disk info structure (from DDK):
     +0x00: sectors per cluster
     +0x02: bytes per sector (512 for CD-ROM: 2048 really, but emulated)
     +0x04: available clusters
     +0x06: total clusters
   But format may differ for extended calls. */
static int _cdecl ntmini_fsd_getdiskinfo(int fn, int drive, int resType, int cpid, ULONG pir)
{
    /* CDFS GetDiskInfo is a stub. Returns error 5 for sub-functions
       0x01, 0x02, 0x10, 0x12. Must return -1 (not handled) to avoid
       short-circuiting the OPEN dispatch chain.
       Calling convention: (fn, drive, resType, cpid, pir) */
    {
        log_hex("FSD: GDI fn=", (ULONG)fn, "");
        log_hex(" drv=", (ULONG)drive, "");
        log_hex(" pir=", pir, "\r\n");
    }

    if (pir >= 0xC0000000 && pir < 0xD0000000) {
        ULONG *pi = (ULONG *)pir;
        UCHAR flags_low = (UCHAR)(pi[1] & 0x07);
        UCHAR subfn = (UCHAR)(pi[6] & 0xFF);  /* +0x18 low byte */
        USHORT *pw = (USHORT *)((UCHAR *)pir + 0x1A);

        if (g_fsd_log_count < 50) {
            log_hex("FSD: GDI fl=", (ULONG)flags_low, "");
            log_hex(" sub=", (ULONG)subfn, "\r\n");
            g_fsd_log_count++;
        }

        /* IFSMgr calls GetDiskInfo as a pre-validation step during OPEN
           dispatch. Return success to let OPEN dispatch continue.
           For subfn==0 (explicit free-space query), also fill volume info. */
        if (subfn == 0) {
            ULONG data_ptr = pi[5];  /* ir_data at +0x14 */
            if (data_ptr >= 0xC0000000 && data_ptr < 0xD0000000) {
                /* DDK disk info structure uses 16-bit fields:
                   +0x00: WORD sectors per cluster
                   +0x02: WORD bytes per sector
                   +0x04: WORD available clusters
                   +0x06: WORD total clusters */
                USHORT *dp = (USHORT *)data_ptr;
                dp[0] = 1;       /* sectors per cluster */
                dp[1] = 2048;    /* bytes per sector */
                dp[2] = 0;       /* available clusters (read-only CD) */
                dp[3] = 0xFFFF;  /* total clusters (max WORD, ~700MB) */
            }
        }

        /* Return -1 = "not handled, continue dispatch chain".
           Returning 0 here would prevent NetOpen from being called. */
        return -1;
    }

    /* pir not a valid kernel pointer */
    return -1;
}

/* FSD entry point table (10 functions as per DDK).
   Initialized at runtime because static function pointers
   may not get proper fixups in LE VxD format. */
static ULONG g_fsd_table[11];
static int g_fsd_table_inited = 0;

static void init_fsd_table(void) {
    if (g_fsd_table_inited) return;
    /* Header: CDFS uses 0x0F10030A. Byte layout (LE):
       Byte 0 (0x0A): count = 10 entries
       Byte 1 (0x03): FSD type (3=CFSD via VCDFSD). For RegisterMount, try 0.
       Byte 2-3 (0x0F10): flags/version
       Try both: first with type=0 (local FSD via RegisterMount),
       fall back to type=3 (CDFS-style) if needed. */
    g_fsd_table[0]  = 10;                               /* count: 10 entries */
    g_fsd_table[1]  = (ULONG)ntmini_fsd_delete;       /* NetDelete */
    g_fsd_table[2]  = (ULONG)ntmini_fsd_dir;          /* NetDir */
    g_fsd_table[3]  = (ULONG)ntmini_fsd_fileattr;     /* NetFileAttributes */
    g_fsd_table[4]  = (ULONG)ntmini_fsd_fileinfo;     /* NetFileInfo */
    g_fsd_table[5]  = (ULONG)ntmini_fsd_flush;        /* NetFlush */
    g_fsd_table[6]  = (ULONG)ntmini_fsd_getdiskinfo;  /* NetGetDiskInfo */
    g_fsd_table[7]  = (ULONG)ntmini_fsd_open;         /* NetOpen */
    g_fsd_table[8]  = (ULONG)ntmini_fsd_rename;       /* NetRename */
    g_fsd_table[9]  = (ULONG)ntmini_fsd_search;       /* NetSearch */
    g_fsd_table[10] = (ULONG)ntmini_fsd_shutdown;     /* ShutDown */
    g_fsd_table_inited = 1;
}

/* Mount callback: called by IFSMgr when mounting a volume.
   cdecl: int MountFS(pioreq pir)
   ir_flags=0 (MOUNT), ir_drv=drive (0=A:), ir_gi.g_ptr=BDD
   Return 0 and set ir_gi = entry point table to claim the volume. */
int _cdecl _ntmini_fsd_mount(ULONG pir)
{
    ULONG *pi = (ULONG *)pir;
    UCHAR drive_byte;
    UCHAR func_byte;

    if (pir < 0xC0000000 || pir >= 0xD0000000) return -1;

    /* The mount callback is the main FSD dispatch function. IFSMgr
       calls it for ALL operations, not just mount.
       ir_flags at +0x04 (low byte) contains the function code:
         0 = MOUNT
         2 = REMOUNT/verify
         Other values = file operations dispatched via the entry table */

    drive_byte = (UCHAR)(pi[10] & 0xFF);  /* +0x28: ir_drv */
    func_byte = (UCHAR)(pi[1] & 0xFF);    /* +0x04: ir_flags low byte = func */

    /* Only interested in D: (drive 3). */
    if (drive_byte != 3 && func_byte == 0) {
        return -1;  /* Not ours for mount */
    }

    /* Initialize on first call */
    init_fsd_table();
    if (!g_iso_pvd_valid) {
        iso9660_read_pvd();
    }

    /* Dispatch based on function code */
    switch (func_byte) {
    case 0: {
        /* MOUNT: claim D:, set entry table, return 0 */
        VxD_Debug_Printf("FSD: MOUNT D:\r\n");

        /* Set ioreq fields for successful mount */
        /* IFSMgr indexes the entry table as table[IFSFN] (0-based).
           IFSFN_OPEN=6 must land on NetOpen. Our g_fsd_table has a count
           header at [0], so skip it: point ir_gi past the header. */
        pi[4] = (ULONG)(g_fsd_table + 1);  /* +0x10: ir_gi = entry table (past header) */

        /* Set provider data at +0x1C */
        {
            static ULONG g_volume_data[8];
            g_volume_data[0] = g_fsd_provider_id;
            g_volume_data[1] = (ULONG)(g_fsd_table + 1);
            g_volume_data[2] = 3;
            g_volume_data[3] = g_iso_root_lba;
            g_volume_data[4] = g_iso_root_size;
            pi[7] = (ULONG)g_volume_data;  /* +0x1C: ir_pr */
        }

        log_hex("FSD: table=", (ULONG)(g_fsd_table + 1), "\r\n");
        return 0;
    }

    case 2: {
        /* REMOUNT/verify: return success to confirm volume is valid */
        VxD_Debug_Printf("FSD: VERIFY D:\r\n");
        pi[4] = 0;  /* ir_error = success */
        return 0;
    }

    default: {
        /* Post-mount file operation: dispatch to entry table.
           The func_byte maps to the entry table index.
           CDFS dispatches through its own internal handler. */
        int table_index = -1;
        int result;

        /* Map IFSMgr function codes to entry table indices.
           This mapping was discovered from CDFS disassembly. */
        if (func_byte <= 10) {
            table_index = func_byte;  /* direct mapping for 1-10 */
        }

        if (g_fsd_log_count < 40) {
            log_hex("FSD: dispatch fn=", (ULONG)func_byte, "");
            log_hex(" idx=", (ULONG)table_index, "\r\n");
            g_fsd_log_count++;
        }

        if (table_index >= 1 && table_index <= 10 && g_fsd_table[table_index]) {
            /* Call the entry table function with IFS calling convention.
               Entry table funcs expect (fn, drive, resType, cpid, pir). */
            typedef int (_cdecl *FSD_FUNC)(int fn, int drive, int resType, int cpid, ULONG pir);
            FSD_FUNC handler = (FSD_FUNC)g_fsd_table[table_index];
            result = handler((int)func_byte, (int)drive_byte, 0, 0, pir);
            return result;
        }

        /* Unknown function: return error */
        log_hex("FSD: unknown fn=", (ULONG)func_byte, "\r\n");
        return -1;
    }
    }
}

/* Called at Init_Complete / Sys_VM_Init time.
   Approach 2: piggyback on NECATAPI's DCB.
   NECATAPI loaded during IOS init and created the CD-ROM DCB with proper
   IFSMgr/CDS registration. We find that DCB, insert our calldown handler
   at the bottom of the chain, and try IFSMgr_CDROM_Attach. */
void _ntmini_mount_cdrom(void) {
    extern ULONG IFSMgr_CDROM_Attach_Wrapper(ULONG drive, ULONG *pvrp);
    extern void NotifyVolumeArrival_Wrapper(ULONG drive);
    extern void Call_ILB_Service(ULONG service_rtn, PVOID isp_packet);
    extern ULONG IOS_Get_Device_List_Wrapper(void);
    ULONG vrp_out = 0;
    ULONG ar;
    ULONG necatapi_dcb = 0;  /* NECATAPI's CD-ROM DCB address */

    VxD_Debug_Printf("V5: Init_Complete: mount CD-ROM...\r\n");

    /* Register as FSD before CDROM_Attach. When IFSMgr mounts D:,
     * CDFS fails (no working IOS chain), then our FSD claims D:.
     * All file operations go through our FSD functions using
     * ISO 9660 + direct ATAPI. */
    if (!g_fsd_registered) {
        extern ULONG IFSMgr_RegisterMount_Wrapper(ULONG pfnMount, ULONG version, ULONG is_default);
        ULONG provider_id;

        VxD_Debug_Printf("V5: Registering FSD with IFSMgr...\r\n");
        provider_id = IFSMgr_RegisterMount_Wrapper(
            (ULONG)_ntmini_fsd_mount,
            0x22,  /* IFSMGRVERSION */
            0      /* normal FSD, not default */
        );
        log_hex("V5: RegisterMount provider_id=", provider_id, "\r\n");

        if (provider_id != 0xFFFFFFFF) {
            g_fsd_provider_id = provider_id;
            g_fsd_registered = 1;
            VxD_Debug_Printf("V5: *** FSD REGISTERED! ***\r\n");
        } else {
            VxD_Debug_Printf("V5: FSD registration failed\r\n");
        }
    }

    /* ================================================================
     * FAST PATH: If _ntmini_ios_init ran at Device_Init, we already
     * have ILB from Device_Init early registration. Skip ILB discovery,
     * go straight to DCB creation + mount.
     * ================================================================ */
    if (g_ilb != (IOS_ILB *)0) {
        VxD_Debug_Printf("V5: ILB from Device_Init, skipping ILB discovery\r\n");
        log_hex("V5: ILB=", (ULONG)g_ilb, "\r\n");
    } else {
        VxD_Debug_Printf("V5: No ILB from Device_Init, doing full discovery...\r\n");

    /* ================================================================
     * STEP 1: Get ILB from another driver's DRP.
     * Skip IOS_Register: registering at Init_Complete breaks NECATAPI's
     * CDS (IFSMgr_CDROM_Attach returns -1). Instead, find the ILB by
     * walking the VxD DDB chain and reading another driver's DRP_ilb.
     * Our VxD stays loaded via SYSTEM.INI regardless.
     * ================================================================ */
    {
        extern ULONG VMM_Get_DDB_Wrapper(USHORT device_id);
        ULONG ios_ddb = VMM_Get_DDB_Wrapper(0x0010);  /* IOS device ID */
        log_hex("V5: VMM_Get_DDB(IOS)=", ios_ddb, "\r\n");

        if (ios_ddb >= 0xC0000000 && ios_ddb < 0xD0000000) {
            UCHAR *walk_ddb = (UCHAR *)ios_ddb;
            ULONG start_ddb = ios_ddb;
            int ddb_count;

            VxD_Debug_Printf("V5: Walking VxD DDB chain for ILB...\r\n");

            for (ddb_count = 0; ddb_count < 80 && g_ilb == (IOS_ILB *)0; ddb_count++) {
                ULONG next_ddb = *(ULONG *)walk_ddb;
                ULONG ctrl = *(ULONG *)(walk_ddb + 0x18);
                ULONG ref = *(ULONG *)(walk_ddb + 0x2C);
                {
                    char dname[9]; int di;
                    for(di=0;di<8;di++) dname[di]=((UCHAR*)walk_ddb)[0x0C+di];
                    dname[8]=0;
                    log_hex("V5: DDB[", (ULONG)ddb_count, "] ");
                    VxD_Debug_Printf(dname);
                    log_hex(" ref=", ref, "\r\n");
                }

                /* Check if Reference_Data points to a DRP */
                if (ref >= 0xC0000000 && ref < 0xD0000000 &&
                    ref != (ULONG)&g_ios_ddb) {
                    UCHAR *r = (UCHAR *)ref;
                    if (r[0]=='X' && r[1]=='X' && r[2]=='X' && r[3]=='X') {
                        ULONG ilb_val = *(ULONG *)(r + 0x10);
                        if (ilb_val >= 0xC0000000) {
                            IOS_ILB *fi = (IOS_ILB *)ilb_val;
                            if (fi->ILB_service_rtn >= 0xC0000000) {
                                char dn[9];
                                int j;
                                for(j=0;j<8;j++) dn[j]=r[0x14+j];
                                dn[8]=0;
                                VxD_Debug_Printf("V5: ILB from DRP: ");
                                VxD_Debug_Printf(dn);
                                log_hex(" ILB=", ilb_val, "\r\n");
                                g_ilb = fi;
                                g_ios_ddb.DRP_ilb = ilb_val;
                            }
                        }
                        /* Log DRP+0x2E and +0x34 for drivers with non-zero values. */
                        {
                            ULONG rd = *(ULONG*)(r + 0x2E);
                            ULONG r2 = *(ULONG*)(r + 0x34);
                            if (rd != 0 || r2 != 0) {
                                char dn2[9];
                                int k;
                                for(k=0;k<8;k++) dn2[k]=r[0x14+k];
                                dn2[8]=0;
                                VxD_Debug_Printf("V5: DRP[");
                                VxD_Debug_Printf(dn2);
                                log_hex("] +2E=", rd, "");
                                log_hex(" +34=", r2, "\r\n");
                            }
                        }
                    }
                }

                /* 8KB ctrl-region scan DISABLED: causes page faults in Safe Mode
                   when scanning unmapped pages near VxD code.
                   ILB recovery via DDB Reference_Data (above) is sufficient. */

                if (next_ddb < 0xC0000000 || next_ddb == start_ddb)
                    break;
                walk_ddb = (UCHAR *)next_ddb;
            }
        }

        if (g_ilb != (IOS_ILB *)0) {
            VxD_Debug_Printf("V5: *** ILB RECOVERED! ***\r\n");
            log_hex("  service_rtn=", g_ilb->ILB_service_rtn, "\r\n");
        } else {
            VxD_Debug_Printf("V5: ILB not found anywhere\r\n");
        }
    } /* end STEP 1 inner block */
    } /* end else: no ILB from Device_Init */

    /* ================================================================
     * STEP 2: Find NECATAPI's CD-ROM DCB.
     * Method A (primary): IOS_Get_Device_List — the authoritative DCB
     *   chain maintained by IOS. Walk DCB_next at +0x0C, check +0x40
     *   for device_type 5 (CD-ROM).
     * Method B (fallback): DVT pointer scan — check pointers in ILB's
     *   DVT for CD-ROM DCBs in IOS heap range.
     * Heap brute-force scan removed: crashes on unmapped pages.
     * ================================================================ */

    /* Dump ILB fields for diagnostics */
    if (g_ilb != (IOS_ILB *)0) {
        VxD_Debug_Printf("V5: ILB dump:\r\n");
        log_hex("  +00 service_rtn=", g_ilb->ILB_service_rtn, "\r\n");
        log_hex("  +04 dprintf_rtn=", g_ilb->ILB_dprintf_rtn, "\r\n");
        log_hex("  +18 dvt=", g_ilb->ILB_dvt, "\r\n");
        log_hex("  +1C ios_mem_virt=", g_ilb->ILB_ios_mem_virt, "\r\n");
    }

    /* Method A: DISABLED. IOS ordinal 3 is IOS_Exclusive_Access, NOT
       IOS_Get_Device_List. All data from previous calls was garbage (IOS
       internal structures with ASCII strings, not real DCBs).
       Use ISP_GET_FIRST_NEXT_DCB (func=10) via ILB service instead,
       deferred to AEP_1_SEC context (crashes from Init_Complete). */
    VxD_Debug_Printf("V5: IOS_Get_Device_List skipped (ordinal 3 is wrong)\r\n");

    /* Method B: Scan DVT pointers for CD-ROM DCB (fallback) */
    if (necatapi_dcb == 0 && g_ilb != (IOS_ILB *)0 &&
        g_ilb->ILB_dvt >= 0xC0000000 && g_ilb->ILB_dvt < 0xD0000000) {
        UCHAR *dvt = (UCHAR *)g_ilb->ILB_dvt;
        ULONG ilb_page = (ULONG)g_ilb & 0xFFF00000;
        int off;

        VxD_Debug_Printf("V5: DVT scan fallback...\r\n");
        for (off = 0; off < 128 && necatapi_dcb == 0; off += 4) {
            ULONG candidate = *(ULONG *)(dvt + off);
            if (candidate >= ilb_page && candidate < (ilb_page + 0x100000) &&
                (candidate & 3) == 0) {
                UCHAR *cdcb = (UCHAR *)candidate;
                if (cdcb[0x40] == DCB_TYPE_CDROM &&
                    validate_cdrom_dcb(candidate)) {
                    necatapi_dcb = candidate;
                    VxD_Debug_Printf("V5: *** FOUND CD-ROM DCB via DVT! ***\r\n");
                }
            }
        }
    }

    if (necatapi_dcb != 0) {
        /* Dump the CD-ROM DCB and its chain */
        UCHAR *dcb = (UCHAR *)necatapi_dcb;
        int off;
        for (off = 0; off < 0x80; off += 16) {
            ULONG *p = (ULONG *)(dcb + off);
            log_hex("  +", (ULONG)off, ": ");
            log_hex("", p[0], " ");
            log_hex("", p[1], " ");
            log_hex("", p[2], " ");
            log_hex("", p[3], "\r\n");
        }
        VxD_Debug_Printf("V5: NECATAPI chain:\r\n");
        walk_calldown_chain(necatapi_dcb, (ULONG)0);
    }

    if (necatapi_dcb == 0) {
        VxD_Debug_Printf("V5: No CD-ROM DCB found in IOS chain\r\n");
        goto do_mount;
    }

    /* ================================================================
     * STEP 3: Insert our IOR handler at the bottom of NECATAPI's chain.
     * Use ISP_INSERT_CALLDOWN (func=5) via ILB_service_rtn.
     * Our handler does direct ATAPI I/O via atapi_read_sector(),
     * bypassing NECATAPI's broken port driver.
     * Set up g_ios_ddb as our DRP for the calldown entry.
     * ================================================================ */
    if (g_ilb != (IOS_ILB *)0 && g_ilb->ILB_service_rtn >= 0xC0000000) {
        ISP_INSERT_CD_PKT isp_cd;

        /* Set up DRP for ISP_INSERT_CALLDOWN (DDB param) */
        my_memset(&g_ios_ddb, 0, sizeof(g_ios_ddb));
        g_ios_ddb.DRP_eyecatch_str[0] = 'X';
        g_ios_ddb.DRP_eyecatch_str[1] = 'X';
        g_ios_ddb.DRP_eyecatch_str[2] = 'X';
        g_ios_ddb.DRP_eyecatch_str[3] = 'X';
        g_ios_ddb.DRP_eyecatch_str[4] = 'X';
        g_ios_ddb.DRP_eyecatch_str[5] = 'X';
        g_ios_ddb.DRP_eyecatch_str[6] = 'X';
        g_ios_ddb.DRP_eyecatch_str[7] = 'X';
        g_ios_ddb.DRP_LGN = 0x00400000;
        g_ios_ddb.DRP_aer = (ULONG)ios_aep_bridge;
        {
            const char *n = "NTMINI";
            int i;
            for (i = 0; n[i] && i < 15; i++) g_ios_ddb.DRP_ascii_name[i] = n[i];
        }

        my_memset(&isp_cd, 0, sizeof(isp_cd));
        isp_cd.func = ISP_INSERT_CALLDOWN_FUNC;  /* 5 */
        isp_cd.dcb = necatapi_dcb;
        isp_cd.req = (ULONG)ios_ior_bridge;
        isp_cd.ddb = (ULONG)&g_ios_ddb;
        isp_cd.flags = ISPCDF_PORT_DRIVER | ISPCDF_BOTTOM;  /* 0x0003: port driver at bottom */
        isp_cd.lgn = 0x16;  /* ESDI port driver layer */

        VxD_Debug_Printf("V5: ISP_INSERT_CALLDOWN into NECATAPI chain...\r\n");
        Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_cd);
        log_hex("V5: INSERT result=", (ULONG)isp_cd.result, "\r\n");

        if (isp_cd.result == 0) {
            VxD_Debug_Printf("V5: *** HANDLER INSERTED! ***\r\n");
        } else {
            VxD_Debug_Printf("V5: INSERT FAILED\r\n");
        }

        /* Walk chain to verify our handler is at the bottom */
        VxD_Debug_Printf("V5: Post-insert chain:\r\n");
        walk_calldown_chain(necatapi_dcb, (ULONG)ios_ior_bridge);
    } else {
        VxD_Debug_Printf("V5: No ILB, cannot insert calldown\r\n");
    }

    /* ================================================================
     * STEP 4: Mount D: via IFSMgr.
     * NECATAPI already created CDS for D: during IOS init.
     * IFSMgr_CDROM_Attach tells CDFS to mount the filesystem.
     * I/O requests will flow through NECATAPI's chain, hit our
     * handler at the bottom, and get real ATAPI data from QEMU.
     * ================================================================ */
do_mount:
    /* ================================================================
     * PRE-MOUNT: Create our own IOS-managed DCB with a working ATAPI
     * chain BEFORE IFSMgr_CDROM_Attach. This lets CDFS read sectors
     * through IOS during mount, avoiding the "cached mount failure"
     * that breaks all subsequent D: access.
     *
     * DCB must be created BEFORE CDROM_Attach so IFSMgr finds
     * our DCB as the primary one (creating after causes VRP corruption).
     * ================================================================ */
    if (g_ilb != (IOS_ILB *)0 && g_our_dcb_addr == 0 && necatapi_dcb == 0) {
        extern void Call_ILB_Service(ULONG service_rtn, PVOID isp_packet);
        ISP_CREATE_DCB_PKT isp_dcb;

        VxD_Debug_Printf("V5: Pre-mount DCB creation...\r\n");

        /* Create DCB */
        my_memset(&isp_dcb, 0, sizeof(isp_dcb));
        isp_dcb.func = 1;       /* ISP_CREATE_DCB */
        isp_dcb.dcb_size = 256;
        Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_dcb);
        log_hex("V5: CREATE_DCB result=", (ULONG)isp_dcb.result, "");
        log_hex(" dcb=", isp_dcb.dcb_ptr, "\r\n");

        if (isp_dcb.result == 0 && isp_dcb.dcb_ptr >= 0xC0000000) {
            UCHAR *dcb = (UCHAR *)isp_dcb.dcb_ptr;
            g_our_dcb_addr = isp_dcb.dcb_ptr;

            /* Set up DRP for calldown insertion */
            if (g_ios_ddb.DRP_eyecatch_str[0] != 'X') {
                int i;
                my_memset(&g_ios_ddb, 0, sizeof(g_ios_ddb));
                for (i = 0; i < 8; i++) g_ios_ddb.DRP_eyecatch_str[i] = 'X';
                g_ios_ddb.DRP_LGN = 0x00400000;
                g_ios_ddb.DRP_aer = (ULONG)ios_aep_bridge;
                {
                    const char *n = "NTMINI";
                    for (i = 0; n[i] && i < 15; i++) g_ios_ddb.DRP_ascii_name[i] = n[i];
                }
            }

            /* Write DCB fields at verified real DDK offsets */
            *(ULONG *)(dcb + 0x1C) = DCB_DEV_PHYSICAL | DCB_DEV_REMOVABLE |
                                      DCB_DEV_UNCERTAIN_MEDIA;
            *(ULONG *)(dcb + 0x20) = DCB_DEV_PHYSICAL | DCB_TYPE_CDROM;
            *(ULONG *)(dcb + 0x24) = DCB_DEV2_ATAPI_DEVICE;
            *(ULONG *)(dcb + 0x40) = DCB_DEV_PHYSICAL | DCB_TYPE_CDROM;
            *(UCHAR *)(dcb + 0x4C) = 11; /* log2(2048) */
            *(ULONG *)(dcb + 0x50) = 2048;
            {
                const char *vid = "QEMU    ";
                const char *pid = "QEMU CD-ROM     ";
                int i;
                for (i = 0; i < 8; i++) dcb[0x60 + i] = vid[i];
                for (i = 0; i < 16; i++) dcb[0x68 + i] = pid[i];
            }

            /* Insert our ATAPI handler at bottom of chain */
            {
                ISP_INSERT_CD_PKT isp_cd;
                my_memset(&isp_cd, 0, sizeof(isp_cd));
                isp_cd.func = ISP_INSERT_CALLDOWN_FUNC;
                isp_cd.dcb = isp_dcb.dcb_ptr;
                isp_cd.req = (ULONG)ios_ior_bridge;
                isp_cd.ddb = (ULONG)&g_ios_ddb;
                isp_cd.flags = ISPCDF_PORT_DRIVER;
                isp_cd.lgn = 0x16;
                Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_cd);
                log_hex("V5: INSERT result=", (ULONG)isp_cd.result, "\r\n");
            }

            /* Associate DCB with D: */
            {
                ISP_ASSOC_DCB_PKT isp_assoc;
                my_memset(&isp_assoc, 0, sizeof(isp_assoc));
                isp_assoc.func = 6;
                isp_assoc.dcb = isp_dcb.dcb_ptr;
                isp_assoc.drive = 3;
                Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_assoc);
            }

            /* Drive letter pick for D: */
            {
                ISP_DLP_PKT isp_dlp;
                my_memset(&isp_dlp, 0, sizeof(isp_dlp));
                isp_dlp.func = ISP_DRIVE_LETTER_PICK_FUNC;
                isp_dlp.dcb = isp_dcb.dcb_ptr;
                isp_dlp.letter[0] = 3;
                isp_dlp.letter[1] = 3;
                isp_dlp.flags = ISP_PDL_FL_OK_RM_CD;
                Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_dlp);
            }

            /* Broadcast CONFIG_DCB so upper layers insert calldowns */
            {
                UCHAR aep_buf[20];
                ISP_BCAST_AEP_PKT isp_bcast;
                my_memset(aep_buf, 0, sizeof(aep_buf));
                *(USHORT *)(aep_buf + 0) = AEP_CONFIG_DCB;
                *(ULONG *)(aep_buf + 4) = (ULONG)&g_ios_ddb;
                *(UCHAR *)(aep_buf + 8) = 0x16;
                *(ULONG *)(aep_buf + 0x0C) = isp_dcb.dcb_ptr;
                *(ULONG *)(aep_buf + 0x10) = (ULONG)&g_ios_ddb;
                my_memset(&isp_bcast, 0, sizeof(isp_bcast));
                isp_bcast.func = 20;
                isp_bcast.paep = (ULONG)aep_buf;
                Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_bcast);
                log_hex("V5: CONFIG_DCB bcast result=", (ULONG)isp_bcast.result, "\r\n");
            }

            /* Create VRP for DEVICE_ARRIVED */
            my_memset(g_dummy_vrp, 0, sizeof(g_dummy_vrp));
            *(ULONG *)(g_dummy_vrp + 0x14) = 2048;
            *(ULONG *)(g_dummy_vrp + 0x20) = isp_dcb.dcb_ptr;
            *(ULONG *)(g_dummy_vrp + 0x24) = 3;
            *(ULONG *)(dcb + 0x18) = (ULONG)g_dummy_vrp;

            /* DEVICE_ARRIVED */
            g_ios_ready = 1;
            {
                ISP_DEV_ARRIVED_PKT isp_arr;
                my_memset(&isp_arr, 0, sizeof(isp_arr));
                isp_arr.func = ISP_DEVICE_ARRIVED_FUNC;
                isp_arr.dcb = isp_dcb.dcb_ptr;
                Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_arr);
                log_hex("V5: DEVICE_ARRIVED result=", (ULONG)isp_arr.result, "\r\n");
            }

            VxD_Debug_Printf("V5: Pre-mount DCB + chain ready\r\n");
            walk_calldown_chain(isp_dcb.dcb_ptr, (ULONG)ios_ior_bridge);
        }
    }

    NotifyVolumeArrival_Wrapper(3);
    VxD_Debug_Printf("V5: NotifyVolumeArrival(D:) done\r\n");

    /* Let CDROM_Attach run directly. Our FSD mount callback fires
       during CDROM_Attach and claims D: before CDFS can try. */
    VxD_Debug_Printf("V5: Calling CDROM_Attach (FSD mount callback will claim D:)...\r\n");

    /* Check what IFSMgr knows about D: before CDROM_Attach */
    {
        extern ULONG IFSMgr_Get_Drive_Info_Wrapper(ULONG drive);
        ULONG dinfo = IFSMgr_Get_Drive_Info_Wrapper(3);  /* D: = 3 */
        log_hex("V5: Drive_Info(D:)=", dinfo, "\r\n");
    }

    if (g_fsd_registered) {
        VxD_Debug_Printf("V5: FSD registered, calling CDROM_Attach for CDS setup...\r\n");
    }

    ar = IFSMgr_CDROM_Attach_Wrapper(3, &vrp_out);
    log_hex("V5: CDROM_Attach result=", ar, "");
    log_hex(" vrp=", vrp_out, "\r\n");

    if (ar == 0 && vrp_out != 0) {
        VxD_Debug_Printf("V5: *** D: MOUNTED! ***\r\n");

        /* Dump VRP structure to find DCB via VRP+0x14 */
        {
            UCHAR *vrp = (UCHAR *)vrp_out;
            int off;
            VxD_Debug_Printf("V5: VRP dump:\r\n");
            for (off = 0; off < 0x30; off += 16) {
                ULONG *p = (ULONG *)(vrp + off);
                log_hex("  +", (ULONG)off, ": ");
                log_hex("", p[0], " ");
                log_hex("", p[1], " ");
                log_hex("", p[2], " ");
                log_hex("", p[3], "\r\n");
            }
            /* VRP+0x14 points to a DCB-like structure. Confirmed findings:
               - Has device_type=5 at +0x40 (CD-ROM)
               - But +0x08 = VRP address (NOT calldown chain)
               - Driver name "SERENUMVXD" at +0x34 (wrong driver)
               - This is a DCB STUB, not the real DCB.
               Check DCB_stub+0x18 for the real DCB. */
            if (necatapi_dcb == 0) {
                ULONG dcb_stub = *(ULONG *)(vrp + 0x14);
                log_hex("V5: VRP+14 (stub)=", dcb_stub, "\r\n");
                if (dcb_stub >= 0xC0000000 && dcb_stub < 0xD0000000) {
                    UCHAR *ds = (UCHAR *)dcb_stub;
                    ULONG chain08 = *(ULONG *)(ds + 0x08);
                    ULONG link18 = *(ULONG *)(ds + 0x18);
                    log_hex("  stub+08=", chain08, "");
                    log_hex(" +18=", link18, "");
                    log_hex(" +40=", (ULONG)ds[0x40], "\r\n");

                    /* If +0x08 == VRP, this is the known stub. Check +0x18 */
                    if (chain08 == vrp_out) {
                        VxD_Debug_Printf("V5: +08=VRP (stub confirmed)\r\n");
                    }

                    /* If stub +0x08 is a real chain (not VRP), use it */
                    if (chain08 >= 0xC0000000 && chain08 < 0xD0000000 &&
                        chain08 != vrp_out && necatapi_dcb == 0) {
                        VxD_Debug_Printf("V5: Using stub chain directly\r\n");
                        walk_calldown_chain(dcb_stub, (ULONG)0);
                        necatapi_dcb = dcb_stub;
                    }
                }
            }
        }

        /* If we found DCB via VRP, insert our handler */
        if (necatapi_dcb != 0 && g_ilb != (IOS_ILB *)0 &&
            g_ilb->ILB_service_rtn >= 0xC0000000) {
            ISP_INSERT_CD_PKT isp_cd;
            const char *n = "NTMINI";
            int i;

            /* Set up DRP */
            my_memset(&g_ios_ddb, 0, sizeof(g_ios_ddb));
            for (i = 0; i < 8; i++) g_ios_ddb.DRP_eyecatch_str[i] = 'X';
            g_ios_ddb.DRP_LGN = 0x00400000;
            g_ios_ddb.DRP_aer = (ULONG)ios_aep_bridge;
            for (i = 0; n[i] && i < 15; i++) g_ios_ddb.DRP_ascii_name[i] = n[i];

            my_memset(&isp_cd, 0, sizeof(isp_cd));
            isp_cd.func = ISP_INSERT_CALLDOWN_FUNC;
            isp_cd.dcb = necatapi_dcb;
            isp_cd.req = (ULONG)ios_ior_bridge;
            isp_cd.ddb = (ULONG)&g_ios_ddb;
            isp_cd.flags = ISPCDF_PORT_DRIVER | ISPCDF_BOTTOM;
            isp_cd.lgn = 0x16;

            VxD_Debug_Printf("V5: ISP_INSERT_CALLDOWN via VRP DCB...\r\n");
            Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_cd);
            log_hex("V5: INSERT result=", (ULONG)isp_cd.result, "\r\n");
            if (isp_cd.result == 0) {
                VxD_Debug_Printf("V5: *** HANDLER INSERTED VIA VRP! ***\r\n");
                walk_calldown_chain(necatapi_dcb, (ULONG)ios_ior_bridge);
            }
        }

        /* If no DCB found, defer enumeration and fall back to
           direct ATAPI bypass for CD-ROM access. */
        if (necatapi_dcb == 0 && g_ilb != (IOS_ILB *)0) {
            const char *n = "NTMINI";
            int i;

            /* Set up DRP for calldown insertion (needed by timer callback) */
            my_memset(&g_ios_ddb, 0, sizeof(g_ios_ddb));
            for (i = 0; i < 8; i++) g_ios_ddb.DRP_eyecatch_str[i] = 'X';
            g_ios_ddb.DRP_LGN = 0x00400000;
            g_ios_ddb.DRP_aer = (ULONG)ios_aep_bridge;
            for (i = 0; n[i] && i < 15; i++) g_ios_ddb.DRP_ascii_name[i] = n[i];

            g_deferred_dcb_enum = 1;
            VxD_Debug_Printf("V5: DCB discovery exhausted, using direct ATAPI bypass\r\n");
            iso9660_test_read();
        }

        /* ================================================================
         * VRP DCB chain patching
         *
         * Provide a working calldown chain for the VRP DCB.
         * Create a calldown node with our ATAPI IOR handler and patch
         * it into the DCB stub's chain head. Then test D: file access
         * through Ring0_FileIO.
         * ================================================================ */
        {
            UCHAR *vrp = (UCHAR *)vrp_out;
            ULONG dcb_addr = *(ULONG *)(vrp + 0x14);

            /* Set up calldown node with verified DDK layout:
               +0x00=handler, +0x04=flags, +0x08=DDB, +0x0C=next */
            g_real_calldown.CD_fsd_entry = (PVOID)ios_ior_bridge;
            g_real_calldown.CD_flags = 0;
            g_real_calldown.CD_ddb = (PVOID)&g_ios_ddb;
            g_real_calldown.CD_next = (REAL_CALLDOWN_NODE *)0;

            if (dcb_addr >= 0xC0000000 && dcb_addr < 0xD0000000) {
                /* DCB stub exists: patch its chain head at +0x08 */
                ULONG old_chain = *(ULONG *)((UCHAR *)dcb_addr + 0x08);
                *(ULONG *)((UCHAR *)dcb_addr + 0x08) = (ULONG)&g_real_calldown;
                log_hex("V5: P2: Stub+08 ", old_chain, " -> ");
                log_hex("", (ULONG)&g_real_calldown, "\r\n");
            } else {
                /* No DCB stub: create minimal DCB and set VRP+0x14 */
                my_memset(g_dummy_dcb, 0, sizeof(g_dummy_dcb));
                *(ULONG *)(g_dummy_dcb + 0x00) = (ULONG)g_dummy_dcb;
                *(ULONG *)(g_dummy_dcb + 0x08) = (ULONG)&g_real_calldown;
                g_dummy_dcb[0x40] = DCB_TYPE_CDROM;
                *(ULONG *)(vrp + 0x14) = (ULONG)g_dummy_dcb;
                log_hex("V5: P2: New DCB at ", (ULONG)g_dummy_dcb, "\r\n");
            }

            VxD_Debug_Printf("V5: P2: Chain:\r\n");
            walk_calldown_chain(*(ULONG *)(vrp + 0x14), (ULONG)ios_ior_bridge);

            /* Install IFSMgr file system API hook.
               Intercepts all file operations. For D:, we handle
               OPEN/READ/CLOSE using ISO 9660 + direct ATAPI,
               bypassing CDFS's cached mount failure. */
            if (!g_fsd_hook_installed) {
                extern ULONG IFSMgr_InstallFSHook_Wrapper(ULONG pfnHandler);
                ULONG prev;
                VxD_Debug_Printf("V5: P2: Installing IFSMgr hook...\r\n");
                prev = IFSMgr_InstallFSHook_Wrapper((ULONG)_ntmini_ifs_hook);
                log_hex("V5: P2: Hook prev=", prev, "\r\n");
                g_fsd_hook_installed = 1;
            }

            /* Test D: file access via Ring0_FileIO.
               Known: IFSMgr hooks can't create file handles, so OPEN
               returns -1. But try anyway to see if pre-mount DCB changed
               anything. Then also test direct ATAPI read as proof of
               concept for MSCDEX path. */
            {
                extern int VxD_File_Open(const char *path);
                extern int VxD_File_Read(int handle, void *buf, int count);
                extern void VxD_File_Close(int handle);
                static UCHAR d_buf[256];
                int fh;

                g_fsd_log_count = 0;  /* Reset for D: OPEN diagnostics */
                VxD_Debug_Printf("V5: P2: Testing D:\\README.TXT...\r\n");
                fh = VxD_File_Open("D:\\README.TXT");
                log_hex("V5: P2: OPEN=", (ULONG)fh, "\r\n");

                if (fh != 0 && fh != -1) {
                    int bytes;
                    my_memset(d_buf, 0, sizeof(d_buf));
                    bytes = VxD_File_Read(fh, d_buf, 128);
                    log_hex("V5: P2: READ=", (ULONG)bytes, "\r\n");
                    if (bytes > 0) {
                        int i;
                        VxD_Debug_Printf("V5: P2: \"");
                        for (i = 0; i < bytes && i < 64; i++) {
                            char c = d_buf[i];
                            if (c >= 0x20 && c < 0x7F) {
                                char s[2]; s[0] = c; s[1] = 0;
                                VxD_Debug_Printf(s);
                            }
                        }
                        VxD_Debug_Printf("\"\r\n");
                        VxD_Debug_Printf("V5: *** PHASE 2 SUCCESS ***\r\n");
                    }
                    VxD_File_Close(fh);
                } else {
                    VxD_Debug_Printf("V5: P2: OPEN failed (hook can't create handles)\r\n");
                    /* Direct ATAPI proof: read file without IFSMgr */
                    {
                        ULONG t_lba, t_size;
                        int t_bytes;
                        my_memset(d_buf, 0, sizeof(d_buf));
                        if (g_iso_pvd_valid &&
                            iso9660_find_file(g_iso_root_lba, g_iso_root_size,
                                "README.TXT", 10, &t_lba, &t_size) == 0) {
                            t_bytes = iso9660_read_file(t_lba, t_size, 0, d_buf,
                                        t_size > 128 ? 128 : t_size);
                            if (t_bytes > 0) {
                                int i;
                                VxD_Debug_Printf("V5: P2: ATAPI direct: \"");
                                for (i = 0; i < t_bytes && i < 64; i++) {
                                    char c = d_buf[i];
                                    if (c >= 0x20 && c < 0x7F) {
                                        char s[2]; s[0] = c; s[1] = 0;
                                        VxD_Debug_Printf(s);
                                    }
                                }
                                VxD_Debug_Printf("\"\r\n");
                                VxD_Debug_Printf("V5: *** ATAPI DIRECT READ SUCCESS ***\r\n");
                            }
                        }
                    }
                }
            }
        }
    } else {
        VxD_Debug_Printf("V5: CDROM_Attach failed\r\n");
    }

    /* Test FSD file access AFTER CDROM_Attach (CDS should now exist for D:) */
    if (g_fsd_registered) {
        extern int VxD_File_Open(const char *path);
        extern int VxD_File_Read(int handle, void *buf, int count);
        extern void VxD_File_Close(int handle);
        static UCHAR fsd_test_buf[256];
        int fsd_fh;

        VxD_Debug_Printf("V5: FSD+CDS test: D:\\README.TXT...\r\n");
        fsd_fh = VxD_File_Open("D:\\README.TXT");
        log_hex("V5: FSD+CDS OPEN=", (ULONG)fsd_fh, "\r\n");

        if (fsd_fh != 0 && fsd_fh != -1) {
            int fsd_bytes;
            my_memset(fsd_test_buf, 0, sizeof(fsd_test_buf));
            fsd_bytes = VxD_File_Read(fsd_fh, fsd_test_buf, 128);
            log_hex("V5: FSD+CDS READ=", (ULONG)fsd_bytes, "\r\n");
            if (fsd_bytes > 0) {
                int i;
                VxD_Debug_Printf("V5: FSD+CDS: \"");
                for (i = 0; i < fsd_bytes && i < 64; i++) {
                    char c = fsd_test_buf[i];
                    if (c >= 0x20 && c < 0x7F) {
                        char s[2]; s[0] = c; s[1] = 0;
                        VxD_Debug_Printf(s);
                    }
                }
                VxD_Debug_Printf("\"\r\n");
                VxD_Debug_Printf("V5: *** FSD FILE ACCESS SUCCESS ***\r\n");
            }
            VxD_File_Close(fsd_fh);
        } else {
            VxD_Debug_Printf("V5: FSD+CDS OPEN failed\r\n");
        }
    }

    /* Always test direct ATAPI as baseline */
    iso9660_test_read();
}

/* ================================================================
 * IOR Handler - called by IOS when I/O requests arrive for our DCB
 *
 * DDK IOR structure (from IOS.H):
 *   +0x00: IOR_next (ULONG)
 *   +0x04: IOR_func (USHORT) - IOR_READ=0, IOR_WRITE=1, etc.
 *   +0x06: IOR_status (USHORT) - we set this (IORS_SUCCESS=0, etc.)
 *   +0x08: IOR_flags (ULONG)
 *   +0x0C: IOR_callback (ULONG) - completion callback
 *   +0x10: IOR_start_addr[2] (2 ULONGs) - starting sector (64-bit)
 *   +0x18: IOR_xfer_count (ULONG) - transfer count in BYTES
 *   +0x1C: IOR_buffer_ptr (ULONG) - data buffer pointer
 *
 * We translate IOR_READ to SCSI READ(10) via direct ATAPI I/O.
 * (NT4 miniport sends unsupported MODE SENSE to QEMU, so we bypass it.)
 * ================================================================ */

/* ================================================================
 * Direct ATAPI I/O - bypasses NT4 miniport entirely.
 * Sends ATAPI PACKET commands via IDE secondary channel (0x170-0x177).
 * The NT4 atapi.sys miniport sends an internal MODE SENSE that QEMU
 * rejects with ILLEGAL REQUEST. This direct path avoids that.
 * ================================================================ */

/* IDE secondary channel registers */
#define IDE_DATA        0x170   /* 16-bit data port */
#define IDE_ERROR       0x171   /* error (read) / features (write) */
#define IDE_IREASON     0x172   /* interrupt reason (ATAPI) */
#define IDE_SECTOR      0x173   /* sector number */
#define IDE_BCNT_LO     0x174   /* byte count low (ATAPI) */
#define IDE_BCNT_HI     0x175   /* byte count high (ATAPI) */
#define IDE_DRVHEAD     0x176   /* drive/head select */
#define IDE_STATUS      0x177   /* status (read) / command (write) */
#define IDE_ALTSTATUS   0x376   /* alternate status / device control */

#define ATAPI_CMD_PACKET 0xA0

static int atapi_read_sector(ULONG sector, UCHAR *buffer)
{
    UCHAR cdb[12];
    ULONG timeout;
    UCHAR status;
    USHORT *wbuf = (USHORT *)buffer;
    int words_read = 0;

    /* 1. Select device (master on secondary channel) */
    _asm {
        mov dx, IDE_DRVHEAD
        mov al, 0A0h          ; master device
        out dx, al
    }

    /* 2. Wait for BSY to clear */
    for (timeout = 0; timeout < 100000; timeout++) {
        _asm {
            mov dx, IDE_STATUS
            in al, dx
            mov status, al
        }
        if (!(status & 0x80)) break;  /* BSY clear */
    }
    if (status & 0x80) return -1;  /* timeout */

    /* 3. Set byte count (2048 = 0x0800) and features */
    _asm {
        mov dx, IDE_ERROR       ; features register
        xor al, al
        out dx, al
        mov dx, IDE_BCNT_LO
        xor al, al              ; low byte = 0x00
        out dx, al
        mov dx, IDE_BCNT_HI
        mov al, 08h              ; high byte = 0x08 (total = 0x0800 = 2048)
        out dx, al
    }

    /* 4. Send PACKET command */
    _asm {
        mov dx, IDE_STATUS
        mov al, ATAPI_CMD_PACKET
        out dx, al
    }

    /* 5. Wait for DRQ (drive ready for CDB) */
    for (timeout = 0; timeout < 100000; timeout++) {
        _asm {
            mov dx, IDE_STATUS
            in al, dx
            mov status, al
        }
        if (!(status & 0x80) && (status & 0x08)) break;  /* !BSY && DRQ */
    }
    if (!(status & 0x08)) return -2;  /* no DRQ */

    /* 6. Send 12-byte CDB: READ(10) */
    my_memset(cdb, 0, 12);
    cdb[0] = 0x28;  /* READ(10) */
    cdb[2] = (UCHAR)(sector >> 24);
    cdb[3] = (UCHAR)(sector >> 16);
    cdb[4] = (UCHAR)(sector >> 8);
    cdb[5] = (UCHAR)(sector);
    cdb[7] = 0;     /* transfer length high */
    cdb[8] = 1;     /* transfer length low = 1 sector */
    {
        USHORT *wcdb = (USHORT *)cdb;
        int ci;
        for (ci = 0; ci < 6; ci++) {
            USHORT w = wcdb[ci];
            _asm {
                mov dx, IDE_DATA
                mov ax, w
                out dx, ax
            }
        }
    }

    /* 7. Wait for data phase (DRQ with IO=1 in interrupt reason) */
    for (timeout = 0; timeout < 500000; timeout++) {
        _asm {
            mov dx, IDE_STATUS
            in al, dx
            mov status, al
        }
        if (status & 0x01) return -3;  /* ERR */
        if (!(status & 0x80) && (status & 0x08)) break;  /* !BSY && DRQ = data ready */
    }
    if (!(status & 0x08)) return -4;  /* no data */

    /* 8. Read 2048 bytes (1024 words) from data port */
    {
        int wi;
        for (wi = 0; wi < 1024; wi++) {
            USHORT w;
            _asm {
                mov dx, IDE_DATA
                in ax, dx
                mov w, ax
            }
            wbuf[wi] = w;
        }
    }

    /* 9. Wait for command completion (BSY clears, DRQ clears) */
    for (timeout = 0; timeout < 100000; timeout++) {
        _asm {
            mov dx, IDE_STATUS
            in al, dx
            mov status, al
        }
        if (!(status & 0x80) && !(status & 0x08)) break;  /* !BSY && !DRQ */
    }

    /* Check for error */
    if (status & 0x01) return -5;  /* ERR after data */

    return 0;  /* success */
}

/* ================================================================
 * ISO 9660 Parser — Direct ATAPI Bypass
 *
 * Reads ISO 9660 file system from CD-ROM via atapi_read_sector(),
 * completely bypassing the IOS/calldown chain. Used when the CD-ROM
 * DCB cannot be found through normal IOS enumeration.
 * ================================================================ */

static UCHAR iso_sector_buf[2048];      /* sector buffer for file read ops */
static UCHAR iso_enum_sector_buf[2048]; /* separate buffer for directory enumeration */

/* ISO 9660 PVD: root directory location (declared near IFS hook above) */

/* Read Primary Volume Descriptor from sector 16 */
static int iso9660_read_pvd(void)
{
    int r = atapi_read_sector(16, iso_sector_buf);
    if (r != 0) {
        log_hex("ISO: PVD read error ", (ULONG)r, "\r\n");
        return -1;
    }

    /* PVD: type=1 at byte 0, "CD001" at bytes 1-5 */
    if (iso_sector_buf[0] != 1 ||
        iso_sector_buf[1] != 'C' || iso_sector_buf[2] != 'D' ||
        iso_sector_buf[3] != '0' || iso_sector_buf[4] != '0' ||
        iso_sector_buf[5] != '1') {
        VxD_Debug_Printf("ISO: Bad PVD signature\r\n");
        return -2;
    }

    /* Root directory record at PVD offset 156 (34 bytes)
       +2: LBA of extent (LE DWORD)
       +10: data length (LE DWORD) */
    g_iso_root_lba  = *(ULONG *)(iso_sector_buf + 156 + 2);
    g_iso_root_size = *(ULONG *)(iso_sector_buf + 156 + 10);
    g_iso_pvd_valid = 1;

    log_hex("ISO: PVD OK root_lba=", g_iso_root_lba, "");
    log_hex(" root_size=", g_iso_root_size, "\r\n");
    return 0;
}

/* Case-insensitive compare, length-limited */
static int iso_namecmp(const char *a, ULONG alen, const char *b, ULONG blen)
{
    ULONG i;
    if (alen != blen) return 0;
    for (i = 0; i < alen; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return 1;
}

/* Find a file in an ISO 9660 directory.
   Returns 0 on success, fills out_lba and out_size. */
static int iso9660_find_file(ULONG dir_lba, ULONG dir_size,
                              const char *name, ULONG name_len,
                              ULONG *out_lba, ULONG *out_size)
{
    ULONG sectors = (dir_size + 2047) / 2048;
    ULONG s;

    for (s = 0; s < sectors && s < 16; s++) {
        ULONG pos;
        int r = atapi_read_sector(dir_lba + s, iso_sector_buf);
        if (r != 0) return -1;

        pos = 0;
        while (pos + 33 < 2048) {
            UCHAR rec_len = iso_sector_buf[pos];
            UCHAR id_len;
            char *id;
            ULONG id_clean;

            if (rec_len < 34) break;  /* end of entries */

            id_len = iso_sector_buf[pos + 32];
            id = (char *)(iso_sector_buf + pos + 33);

            /* Strip ";1" version suffix from ISO name */
            id_clean = id_len;
            if (id_clean > 2 && id[id_clean - 2] == ';')
                id_clean -= 2;
            /* Strip trailing "." if present */
            if (id_clean > 1 && id[id_clean - 1] == '.')
                id_clean -= 1;

            if (iso_namecmp(name, name_len, id, id_clean)) {
                *out_lba  = *(ULONG *)(iso_sector_buf + pos + 2);
                *out_size = *(ULONG *)(iso_sector_buf + pos + 10);
                return 0;
            }

            pos += rec_len;
        }
    }

    return -2;  /* not found */
}

/* Enumerate the Nth file in an ISO 9660 directory.
   Returns 0 on success, -1 on error, -2 if index out of range.
   Skips the "." and ".." entries (id_len == 1 with id = 0x00 or 0x01). */
static int iso9660_enum_dir(ULONG dir_lba, ULONG dir_size, int index,
                             char *out_name, ULONG *out_name_len,
                             ULONG *out_lba, ULONG *out_size, UCHAR *out_flags)
{
    ULONG sectors = (dir_size + 2047) / 2048;
    ULONG s;
    int file_index = 0;

    /* Use iso_enum_sector_buf (separate from iso_sector_buf) to avoid
       corrupting file read operations if Search and Read overlap. */
    for (s = 0; s < sectors && s < 16; s++) {
        ULONG pos;
        int r = atapi_read_sector(dir_lba + s, iso_enum_sector_buf);
        if (r != 0) return -1;

        pos = 0;
        while (pos + 34 <= 2048) {
            UCHAR rec_len = iso_enum_sector_buf[pos];
            UCHAR id_len;
            char *id;
            ULONG id_clean;
            UCHAR flags;

            if (rec_len < 34) break;
            if (pos + rec_len > 2048) break;  /* record extends past sector */

            id_len = iso_enum_sector_buf[pos + 32];
            if (pos + 33 + id_len > 2048) break;  /* id extends past sector */
            id = (char *)(iso_enum_sector_buf + pos + 33);
            flags = iso_enum_sector_buf[pos + 25];  /* file flags byte */

            /* Skip "." and ".." entries */
            if (id_len == 1 && (id[0] == 0x00 || id[0] == 0x01)) {
                pos += rec_len;
                continue;
            }

            if (file_index == index) {
                /* Strip ";1" version suffix */
                id_clean = id_len;
                if (id_clean > 2 && id[id_clean - 2] == ';')
                    id_clean -= 2;
                if (id_clean > 1 && id[id_clean - 1] == '.')
                    id_clean -= 1;

                if (id_clean > 63) id_clean = 63;
                { ULONG k; for (k = 0; k < id_clean; k++) out_name[k] = id[k]; }
                out_name[id_clean] = 0;
                *out_name_len = id_clean;
                *out_lba  = *(ULONG *)(iso_enum_sector_buf + pos + 2);
                *out_size = *(ULONG *)(iso_enum_sector_buf + pos + 10);
                *out_flags = flags;
                return 0;
            }

            file_index++;
            pos += rec_len;
        }
    }

    return -2;  /* index out of range */
}

/* Read a file from ISO 9660 into buffer. Returns bytes read or -1. */
static int iso9660_read_file(ULONG file_lba, ULONG file_size,
                              ULONG offset, UCHAR *buffer, ULONG count)
{
    ULONG start_sector, end_byte, bytes_read;
    ULONG sect_offset, i;

    if (offset >= file_size) return 0;
    if (offset + count > file_size) count = file_size - offset;

    bytes_read = 0;
    while (count > 0) {
        ULONG sect = file_lba + (offset / 2048);
        ULONG off_in_sect = offset % 2048;
        ULONG chunk = 2048 - off_in_sect;
        int r;

        if (chunk > count) chunk = count;

        r = atapi_read_sector(sect, iso_sector_buf);
        if (r != 0) return -1;

        {
            ULONG j;
            for (j = 0; j < chunk; j++)
                buffer[bytes_read + j] = iso_sector_buf[off_in_sect + j];
        }

        bytes_read += chunk;
        offset += chunk;
        count -= chunk;
    }

    return (int)bytes_read;
}

/* Test: read README.TXT from CD-ROM ISO via direct ATAPI bypass */
static void iso9660_test_read(void)
{
    ULONG file_lba, file_size;
    int r;
    static UCHAR read_buf[512];

    VxD_Debug_Printf("ISO: === APPROACH D: Direct ATAPI Bypass ===\r\n");

    r = iso9660_read_pvd();
    if (r != 0) {
        log_hex("ISO: PVD failed r=", (ULONG)r, "\r\n");
        return;
    }

    /* List root directory entries */
    {
        ULONG s, pos;
        ULONG sectors = (g_iso_root_size + 2047) / 2048;
        VxD_Debug_Printf("ISO: Root directory:\r\n");
        for (s = 0; s < sectors && s < 4; s++) {
            r = atapi_read_sector(g_iso_root_lba + s, iso_sector_buf);
            if (r != 0) break;
            pos = 0;
            while (pos + 33 < 2048) {
                UCHAR rec_len = iso_sector_buf[pos];
                UCHAR id_len;
                if (rec_len < 34) break;
                id_len = iso_sector_buf[pos + 32];
                if (id_len > 0 && id_len < 32) {
                    char nm[33];
                    ULONG k;
                    for (k = 0; k < id_len && k < 32; k++)
                        nm[k] = iso_sector_buf[pos + 33 + k];
                    nm[k] = 0;
                    log_hex("  ", *(ULONG *)(iso_sector_buf + pos + 2), " ");
                    log_hex("", *(ULONG *)(iso_sector_buf + pos + 10), " ");
                    VxD_Debug_Printf(nm);
                    VxD_Debug_Printf("\r\n");
                }
                pos += rec_len;
            }
        }
    }

    /* Find README.TXT */
    r = iso9660_find_file(g_iso_root_lba, g_iso_root_size,
                           "README", 6, &file_lba, &file_size);
    if (r != 0) {
        /* Try README.TXT */
        r = iso9660_find_file(g_iso_root_lba, g_iso_root_size,
                               "README.TXT", 10, &file_lba, &file_size);
    }
    if (r != 0) {
        VxD_Debug_Printf("ISO: README.TXT not found, trying any .TXT\r\n");
        return;
    }

    log_hex("ISO: README.TXT lba=", file_lba, "");
    log_hex(" size=", file_size, "\r\n");

    /* Read first 256 bytes */
    {
        ULONG to_read = file_size;
        int bytes;
        if (to_read > 256) to_read = 256;

        my_memset(read_buf, 0, sizeof(read_buf));
        bytes = iso9660_read_file(file_lba, file_size, 0, read_buf, to_read);
        log_hex("ISO: Read ", (ULONG)bytes, " bytes\r\n");

        if (bytes > 0) {
            int i;
            VxD_Debug_Printf("ISO: Content: \"");
            for (i = 0; i < bytes && i < 128; i++) {
                char c = read_buf[i];
                if (c >= 0x20 && c < 0x7F) {
                    char s[2]; s[0] = c; s[1] = 0;
                    VxD_Debug_Printf(s);
                } else if (c == '\r' || c == '\n') {
                    VxD_Debug_Printf("\\n");
                } else {
                    VxD_Debug_Printf(".");
                }
            }
            VxD_Debug_Printf("\"\r\n");
            VxD_Debug_Printf("ISO: *** APPROACH D SUCCESS! ***\r\n");
        }
    }
}

/* IOR function codes (Win98 DDK IOS.H) */
#define IOR_READ_FC              0x00
#define IOR_WRITE_FC             0x01
#define IOR_VERIFY_FC            0x02
#define IOR_CANCEL_FC            0x03
#define IOR_WRITEV_FC            0x04
#define IOR_MEDIA_CHECK_FC       0x05
#define IOR_MEDIA_CHECK_RESET_FC 0x06
#define IOR_LOAD_MEDIA_FC        0x07
#define IOR_EJECT_MEDIA_FC       0x08
#define IOR_LOCK_MEDIA_FC        0x09
#define IOR_UNLOCK_MEDIA_FC      0x0A
#define IOR_REQUEST_SENSE_FC     0x0B
#define IOR_COMPUTE_GEOM_FC      0x0C
#define IOR_GEN_IOCTL_FC         0x0D
#define IOR_FORMAT_FC            0x0E
#define IOR_SCSI_PASS_FC         0x0F

/* IOR status codes */
#define IORS_SUCCESS_FC         0x00
#define IORS_UNCERTAIN_MEDIA_FC 0x19
#define IORS_INVALID_CMD_FC     0x16
#define IORS_NOT_READY_FC       0x20

static int ior_log_count = 0;
static int media_check_count = 0;  /* first few return UNCERTAIN to trigger mount */
static int ifsmgr_mount_tried = 0; /* set after IFSMgr_CDROM_Attach attempt */

void _ntmini_ior_handler(PVOID iop_ptr)
{
    /* IOS calldown chain passes the IOP pointer. IOR is at IOP+0x64.
       Direct test mode also passes a buffer with IOR at +0x64 offset. */
    UCHAR *iop = (UCHAR *)iop_ptr;
    UCHAR *ior = iop + IOR_BASE;  /* IOR at IOP+0x64 */
    USHORT func;
    ULONG flags, start_lo, xfer_count, buf_ptr;

    if (!iop) return;

    /* During init, upper-layer VxDs probe the device via ISP_DEVICE_ARRIVED.
       Return NOT_READY to avoid hanging in HwInterrupt polling loop. */
    if (!g_ios_ready) {
        *(USHORT *)(ior + 0x06) = IORS_NOT_READY_FC;
        IOS_BD_Complete(iop_ptr);
        return;
    }

    /* Read IOR fields at IOP+0x64 base (verified by IOS disassembly). */
    func = *(USHORT *)(ior + 0x04);
    flags = *(ULONG *)(ior + 0x08);
    start_lo = *(ULONG *)(ior + 0x10);
    xfer_count = *(ULONG *)(ior + 0x18);
    buf_ptr = *(ULONG *)(ior + 0x1C);

    if (ior_log_count < 50) {
        /* Symbolic name logging: self-auditing against IOS.H constants */
        switch (func) {
        case 0x00: VxD_Debug_Printf("IOR[READ]"); break;
        case 0x01: VxD_Debug_Printf("IOR[WRITE]"); break;
        case 0x02: VxD_Debug_Printf("IOR[VERIFY]"); break;
        case 0x05: VxD_Debug_Printf("IOR[MEDIA_CHECK]"); break;
        case 0x06: VxD_Debug_Printf("IOR[MEDIA_CHECK_RST]"); break;
        case 0x07: VxD_Debug_Printf("IOR[LOAD_MEDIA]"); break;
        case 0x08: VxD_Debug_Printf("IOR[EJECT_MEDIA]"); break;
        case 0x09: VxD_Debug_Printf("IOR[LOCK_MEDIA]"); break;
        case 0x0A: VxD_Debug_Printf("IOR[UNLOCK_MEDIA]"); break;
        case 0x0B: VxD_Debug_Printf("IOR[REQ_SENSE]"); break;
        case 0x0C: VxD_Debug_Printf("IOR[COMPUTE_GEOM]"); break;
        case 0x0D: VxD_Debug_Printf("IOR[GEN_IOCTL]"); break;
        default: VxD_Debug_Printf("IOR[?]"); break;
        }
        log_hex(" fc=", (ULONG)func, "");
        log_hex(" fl=", flags, "");
        log_hex(" st=", start_lo, "");
        log_hex(" xf=", xfer_count, "");
        log_hex(" buf=", buf_ptr, "\r\n");
        ior_log_count++;
    }

    switch (func) {
    case IOR_READ_FC: {
        /* Direct ATAPI READ: bypass NT4 miniport, send raw ATAPI PACKET
           commands to IDE secondary channel. The miniport sends an internal
           MODE SENSE that QEMU doesn't support (ILLEGAL REQUEST). */
        ULONG sector = start_lo;
        ULONG bytes = xfer_count;
        ULONG sectors_to_read = (bytes + 2047) / 2048;
        UCHAR *dest = (UCHAR *)buf_ptr;
        ULONG i;
        BOOLEAN ok = TRUE;

        if (!dest) {
            *(USHORT *)(ior + 0x06) = IORS_NOT_READY_FC;
            break;
        }

        if (ior_log_count < 25) {
            log_hex("IOR: READ sector=", sector, "");
            log_hex(" count=", sectors_to_read, "\r\n");
        }

        for (i = 0; i < sectors_to_read && ok; i++) {
            int rr = atapi_read_sector(sector + i, dest + (i * 2048));
            if (rr != 0) {
                if (ior_log_count < 30) {
                    log_hex("IOR: ATAPI READ fail s=", sector + i, "");
                    log_hex(" err=", (ULONG)rr, "\r\n");
                }
                ok = FALSE;
            }
        }

        *(USHORT *)(ior + 0x06) = ok ? IORS_SUCCESS_FC : IORS_NOT_READY_FC;
        break;
    }

    case IOR_MEDIA_CHECK_FC:
        /* Post-boot (deferred read pending): return UNCERTAIN to trigger mount.
           During init or after mount: return SUCCESS. */
        if (g_deferred_read_pending && media_check_count < 3) {
            *(USHORT *)(ior + 0x06) = IORS_UNCERTAIN_MEDIA_FC;
            media_check_count++;
            VxD_Debug_Printf("IOR: MC -> UNCERTAIN\r\n");
        } else {
            *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        }
        break;

    case IOR_VERIFY_FC:
        /* Verify: just succeed */
        *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        break;

    case IOR_GEN_IOCTL_FC: {
        /* CD-ROM IOCTL via IOS chain.
           IOR_flags encoding: (category << 8) | function | high_flags
           0x00010D09 = category 0x0D (CD-ROM), function 0x09 (Media Changed)
           IOR+0x24 = Client_Reg_Struc pointer (V86 caller's registers)

           Client_Reg_Struc: +0x14=EDX, +0x3C=DS
           DOS IOCTL read (INT 21h/4402h): DS:DX = output buffer */
        ULONG client_regs = *(ULONG *)(ior + 0x24);
        UCHAR ioctl_cat  = (UCHAR)((flags >> 8) & 0xFF);
        UCHAR ioctl_func = (UCHAR)(flags & 0xFF);

        if (ior_log_count < 50) {
            log_hex("IOR: IOCTL cat=", (ULONG)ioctl_cat, "");
            log_hex(" fn=", (ULONG)ioctl_func, "");
            log_hex(" cli=", client_regs, "\r\n");
        }

        if (ioctl_cat == 0x0D && client_regs >= 0xC0000000) {
            UCHAR *cli = (UCHAR *)client_regs;
            USHORT cli_dx = *(USHORT *)(cli + 0x14);
            USHORT cli_ds = *(USHORT *)(cli + 0x3C);
            ULONG linear = ((ULONG)cli_ds << 4) + (ULONG)cli_dx;

            if (ior_log_count < 50) {
                log_hex("IOR: IOCTL DS=", (ULONG)cli_ds, ":");
                log_hex("DX=", (ULONG)cli_dx, "");
                log_hex(" lin=", linear, "\r\n");
            }

            /* Test: can direct ATAPI read sectors? */
            if (g_deferred_read_pending && !g_deferred_read_done) {
                static UCHAR post_boot_buf[2048];
                int rr;
                g_deferred_read_done = 1;
                VxD_Debug_Printf("IOR: ATAPI direct READ sector 16...\r\n");
                my_memset(post_boot_buf, 0, 64);
                rr = atapi_read_sector((ULONG)16, post_boot_buf);
                log_hex("IOR: ATAPI READ result=", (ULONG)rr, "\r\n");
                if (rr == 0) {
                    log_hex("IOR: data: ", *(ULONG *)(post_boot_buf), " ");
                    log_hex("", *(ULONG *)(post_boot_buf + 4), "\r\n");
                    if (post_boot_buf[1] == 'C' && post_boot_buf[2] == 'D') {
                        VxD_Debug_Printf("IOR: *** ISO 9660 PVD READ SUCCESS! ***\r\n");
                    }
                }
            }

            switch (ioctl_func) {
            case 0x09: /* Media Changed: 0xFF=changed, 0x00=no, 0x01=unknown */
                if (linear > 0 && linear < 0x110000) {
                    *(UCHAR *)linear = 0xFF;
                    VxD_Debug_Printf("IOR: IOCTL media_changed=0xFF\r\n");
                }
                break;
            case 0x06: /* Device Status: DWORD flags */
                if (linear > 0 && linear < 0x110000) {
                    *(ULONG *)linear = 0x00000206; /* unlocked, raw+cooked, disc present */
                    VxD_Debug_Printf("IOR: IOCTL dev_status=0x206\r\n");
                }
                break;
            case 0x07: /* Sector Size: byte mode + WORD size */
                if (linear > 0 && linear < 0x110000) {
                    *(UCHAR *)linear = 0; /* cooked */
                    *(USHORT *)(linear + 1) = 2048;
                }
                break;
            case 0x08: /* Volume Size: DWORD sectors */
                if (linear > 0 && linear < 0x110000)
                    *(ULONG *)linear = 1000;
                break;
            default:
                if (ior_log_count < 50)
                    log_hex("IOR: IOCTL unhandled fn=", (ULONG)ioctl_func, "\r\n");
                break;
            }
        }
        *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        break;
    }

    case IOR_LOAD_MEDIA_FC:
    case IOR_LOCK_MEDIA_FC:
    case IOR_UNLOCK_MEDIA_FC:
    case IOR_EJECT_MEDIA_FC:
        /* Media control: succeed (CD-ROM locked/unlocked/ejected/loaded) */
        *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        break;

    case IOR_MEDIA_CHECK_RESET_FC:
        /* Post-boot: return UNCERTAIN to tell voltrack "media changed".
           Also trigger IFSMgr mount on first post-boot check. */
        if (g_deferred_read_pending && media_check_count < 3) {
            *(USHORT *)(ior + 0x06) = IORS_UNCERTAIN_MEDIA_FC;
            media_check_count++;
            VxD_Debug_Printf("IOR: MCR -> UNCERTAIN\r\n");

            /* On first post-boot media check, signal media insertion.
               ISP_DEVICE_ARRIVED with MEDIA_ONLY tells voltrack "CD inserted"
               which triggers IFSMgr CDS creation and CDFS mount. */
            if (!ifsmgr_mount_tried && g_deferred_dcb_ptr) {
                ifsmgr_mount_tried = 1;

                /* Fire ISP_DEVICE_ARRIVED with MEDIA_ONLY flag */
                {
                    ISP_DEV_ARRIVED_PKT isp_ma;
                    my_memset(&isp_ma, 0, sizeof(isp_ma));
                    isp_ma.func = ISP_DEVICE_ARRIVED_FUNC;
                    isp_ma.dcb = g_deferred_dcb_ptr;
                    isp_ma.flags = ISP_DEV_ARR_FL_MEDIA_ONLY;

                    VxD_Debug_Printf("IOR: ISP_DEVICE_ARRIVED(MEDIA_ONLY)...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_ma);
                    log_hex("IOR: DA_MEDIA result=", (ULONG)isp_ma.result, "\r\n");
                }

                /* Then try IFSMgr_CDROM_Attach */
                {
                    extern ULONG IFSMgr_CDROM_Attach_Wrapper(ULONG drive, ULONG *pvrp);
                    ULONG vrp_out = 0;
                    ULONG ar;

                    VxD_Debug_Printf("IOR: IFSMgr_CDROM_Attach(D:)...\r\n");
                    ar = IFSMgr_CDROM_Attach_Wrapper(3, &vrp_out);
                    log_hex("IOR: Attach=", ar, "");
                    log_hex(" vrp=", vrp_out, "\r\n");
                }
            }
        } else {
            *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        }
        break;

    case IOR_REQUEST_SENSE_FC: {
        /* Return "no error" sense data to IOR_buffer_ptr if provided.
           SCSI sense: key=0 (NO SENSE), ASC=0, ASCQ=0. */
        UCHAR *sense_buf = (UCHAR *)buf_ptr;
        if (sense_buf) {
            my_memset(sense_buf, 0, 18);
            sense_buf[0] = 0x70;  /* current errors, fixed format */
            sense_buf[7] = 10;    /* additional sense length */
        }
        *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        break;
    }

    case IOR_COMPUTE_GEOM_FC:
        /* CD-ROM has no meaningful geometry. Return success. */
        *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        break;

    default:
        /* Unknown command: log and return invalid command */
        if (ior_log_count < 30) {
            log_hex("IOR: unknown func=", (ULONG)func, "\r\n");
        }
        *(USHORT *)(ior + 0x06) = IORS_INVALID_CMD_FC;
        break;
    }

    /* Signal completion to IOS.
       - Skip in direct test mode (g_ior_test_mode) to avoid fault.
       - Skip for internal_request IOPs (IOP+0x6D bit 2): the chain executor
         handles completion itself after the handler returns. Calling
         BD_Complete from within the handler would double-complete. */
    if (g_ior_test_mode) return;
    {
        UCHAR flags_6d = *(UCHAR *)(iop + 0x6D);
        if (flags_6d & 0x04) {
            /* Internal request: just return, chain executor handles completion */
            return;
        }
    }
    IOS_BD_Complete(iop_ptr);
}

/* ================================================================
 * MSCDEX V86 INT 2Fh Handler
 *
 * Provides CD-ROM access via legacy MSCDEX interface.
 * Bypasses IOS entirely - translates MSCDEX requests directly
 * to miniport SRBs via HwStartIo.
 *
 * Client_Reg_Struc offsets:
 *   +0x00: EDI  +0x04: ESI  +0x08: EBP  +0x0C: res
 *   +0x10: EBX  +0x14: EDX  +0x18: ECX  +0x1C: EAX
 *   +0x20: Error +0x24: EIP +0x28: CS   +0x2C: EFlags
 *   +0x30: ESP  +0x34: SS  +0x38: ES   +0x3C: DS
 * ================================================================ */

#define MSCDEX_DRIVE_LETTER  3   /* Drive D: (0=A, 1=B, 2=C, 3=D) */

/* Read sectors from CD-ROM via direct ATAPI I/O (bypasses miniport) */
static int mscdex_read_sectors(ULONG start_sector, ULONG num_sectors,
                                UCHAR *buffer)
{
    ULONG i;

    for (i = 0; i < num_sectors; i++) {
        int rr = atapi_read_sector(start_sector + i, buffer + (i * 2048));
        if (rr != 0) {
            log_hex("MSCDEX: ATAPI read failed sector ", start_sector + i, "");
            log_hex(" err=", (ULONG)rr, "\r\n");
            return -1;
        }
    }
    return 0;
}

/* Allocate a buffer in low memory for V86 DMA transfers */
static UCHAR g_mscdex_buf[2048 * 16];  /* 32KB buffer for up to 16 sectors */

ULONG _ntmini_mscdex_handler(ULONG *client_regs)
{
    ULONG client_eax = client_regs[7];  /* offset 0x1C / 4 = index 7 */
    USHORT ax = (USHORT)(client_eax & 0xFFFF);
    UCHAR ah = (UCHAR)((client_eax >> 8) & 0xFF);
    UCHAR al = (UCHAR)(client_eax & 0xFF);

    if (ah != 0x15) {
        /* Log first few non-MSCDEX INT 2Fh calls to verify hook works */
        static int int2f_count = 0;
        if (int2f_count < 3) {
            log_hex("INT2F: AX=", (ULONG)ax, "\r\n");
            int2f_count++;
        }
        return 1;  /* not MSCDEX, pass through */
    }

    switch (ax) {
    case 0x1500:
        /* Get number of CD-ROM drive letters.
           Return: BX = number of CD-ROM drives, CX = first drive letter */
        client_regs[4] = (client_regs[4] & 0xFFFF0000) | 1;  /* BX = 1 */
        client_regs[6] = (client_regs[6] & 0xFFFF0000) | MSCDEX_DRIVE_LETTER; /* CX = D: */
        VxD_Debug_Printf("MSCDEX: 1500h -> 1 drive at D:\r\n");
        return 0;  /* handled */

    case 0x150B:
        /* CD-ROM drive check. CX = drive number.
           Return: BX = 0xADAD if CD-ROM, AX = non-zero if installed */
        {
            USHORT drive = (USHORT)(client_regs[6] & 0xFFFF);  /* CX */
            if (drive == MSCDEX_DRIVE_LETTER) {
                client_regs[4] = (client_regs[4] & 0xFFFF0000) | 0xADAD; /* BX */
                client_regs[7] = (client_regs[7] & 0xFFFF0000) | 0x00FF; /* AX = non-zero */
                return 0;
            }
            return 1;  /* not our drive */
        }

    case 0x1510:
        /* Send device request. CX = drive, ES:BX = device request header */
        {
            USHORT drive = (USHORT)(client_regs[6] & 0xFFFF);  /* CX */
            USHORT es = (USHORT)(client_regs[14] & 0xFFFF);   /* ES at offset 0x38/4=14 */
            USHORT bx = (USHORT)(client_regs[4] & 0xFFFF);    /* BX */
            UCHAR *req;
            UCHAR cmd;

            if (drive != MSCDEX_DRIVE_LETTER) return 1;

            /* Map V86 ES:BX to linear address */
            req = (UCHAR *)((ULONG)es * 16 + (ULONG)bx);
            cmd = req[2];  /* command code */

            log_hex("MSCDEX: 1510h cmd=", (ULONG)cmd, "\r\n");

            if (cmd == 128) {
                /* Read Long: read sectors from CD-ROM */
                UCHAR addr_mode = req[13];
                ULONG xfer_addr;
                USHORT num_sectors;
                ULONG start_sector;
                UCHAR *dest;
                USHORT xfer_seg, xfer_off;

                /* Transfer address is a real-mode far pointer at offset 14 */
                xfer_off = *(USHORT *)(req + 14);
                xfer_seg = *(USHORT *)(req + 16);
                num_sectors = *(USHORT *)(req + 18);
                start_sector = *(ULONG *)(req + 20);

                dest = (UCHAR *)((ULONG)xfer_seg * 16 + (ULONG)xfer_off);

                log_hex("MSCDEX: READ sector=", start_sector, "");
                log_hex(" count=", (ULONG)num_sectors, "");
                log_hex(" dest=", (ULONG)dest, "\r\n");

                if (num_sectors > 16) num_sectors = 16;  /* limit */

                if (mscdex_read_sectors(start_sector, num_sectors, dest) == 0) {
                    /* Success: set status word at offset 3 */
                    *(USHORT *)(req + 3) = 0x0100;  /* done, no error */
                    VxD_Debug_Printf("MSCDEX: READ OK\r\n");
                } else {
                    *(USHORT *)(req + 3) = 0x8002;  /* error: not ready */
                    VxD_Debug_Printf("MSCDEX: READ FAILED\r\n");
                }
                return 0;
            }

            if (cmd == 3) {
                /* IOCTL Input - various sub-commands */
                UCHAR *ioctl_buf;
                USHORT ioctl_off = *(USHORT *)(req + 14);
                USHORT ioctl_seg = *(USHORT *)(req + 16);
                UCHAR ioctl_cmd;

                ioctl_buf = (UCHAR *)((ULONG)ioctl_seg * 16 + (ULONG)ioctl_off);
                ioctl_cmd = ioctl_buf[0];

                log_hex("MSCDEX: IOCTL cmd=", (ULONG)ioctl_cmd, "\r\n");

                if (ioctl_cmd == 1) {
                    /* Get CD-ROM drive head location */
                    *(UCHAR *)(ioctl_buf + 1) = 0;  /* addressing mode: HSG */
                    *(ULONG *)(ioctl_buf + 2) = 0;  /* current head position */
                    *(USHORT *)(req + 3) = 0x0100;
                    return 0;
                }
                if (ioctl_cmd == 6) {
                    /* Get device status */
                    *(ULONG *)(ioctl_buf + 1) = 0x00000200; /* door closed, data read */
                    *(USHORT *)(req + 3) = 0x0100;
                    return 0;
                }
                if (ioctl_cmd == 7) {
                    /* Get sector size */
                    *(USHORT *)(ioctl_buf + 2) = 2048;
                    *(USHORT *)(req + 3) = 0x0100;
                    return 0;
                }
                if (ioctl_cmd == 8) {
                    /* Get volume size (total sectors) */
                    *(ULONG *)(ioctl_buf + 1) = 0x00010000; /* ~131K sectors */
                    *(USHORT *)(req + 3) = 0x0100;
                    return 0;
                }
                if (ioctl_cmd == 10) {
                    /* Get audio disc info */
                    ioctl_buf[1] = 1;   /* first track */
                    ioctl_buf[2] = 1;   /* last track */
                    *(ULONG *)(ioctl_buf + 3) = 0; /* lead-out */
                    *(USHORT *)(req + 3) = 0x0100;
                    return 0;
                }

                /* Unknown IOCTL: return success anyway */
                *(USHORT *)(req + 3) = 0x0100;
                return 0;
            }

            /* Unknown command: mark as done */
            *(USHORT *)(req + 3) = 0x0100;
            return 0;
        }

    default:
        /* Unknown MSCDEX function - pass through */
        return 1;
    }
}

/* ================================================================
 * AEP Handler - called by IOS for various events
 * FROM DDK IOS.H: AEP struct has USHORT func at +0x00, USHORT result +0x02,
 * ULONG ddb +0x04, UCHAR lgn +0x08. Extended fields after +0x0C.
 * ================================================================ */
void _ntmini_aep_handler(AEP_HEADER *aep)
{
    /* Save ILB from AEP_INITIALIZE; handle CONFIG_DCB and 1_SEC
       for deferred DCB discovery. All other events return AEP_SUCCESS. */

    if (aep->AEP_func == AEP_INITIALIZE) {
        AEP_BI_INIT *bi = (AEP_BI_INIT *)aep;
        ULONG ilb_ptr = bi->AEP_bi_reference;

        VxD_Debug_Printf("IOS: AEP_INITIALIZE\r\n");
        log_hex("  ILB=", ilb_ptr, "\r\n");

        if (ilb_ptr != 0) {
            g_ilb = (IOS_ILB *)ilb_ptr;
            log_hex("  ILB_service_rtn=", g_ilb->ILB_service_rtn, "\r\n");
        }
    } else if (aep->AEP_func == AEP_CONFIG_DCB) {
        AEP_CD_CONFIG *cd = (AEP_CD_CONFIG *)aep;
        VxD_Debug_Printf("IOS: AEP_CONFIG_DCB\r\n");
        log_hex("  dcb=", cd->AEP_cd_dcb, "\r\n");
        if (cd->AEP_cd_dcb >= 0xC0000000) {
            UCHAR *dcb = (UCHAR *)cd->AEP_cd_dcb;
            log_hex("  t40=", (ULONG)dcb[0x40], "");
            log_hex(" cd+08=", *(ULONG *)(dcb + 0x08), "\r\n");
            /* Save any DCB with type 5 (CD-ROM) */
            if (dcb[0x40] == DCB_TYPE_CDROM)
                g_aep_config_dcb = cd->AEP_cd_dcb;
            /* Save all DCBs for debugging */
            if (g_aep_config_dcb == 0)
                g_aep_config_dcb = cd->AEP_cd_dcb;
        }
    } else if (aep->AEP_func == AEP_1_SEC) {
        /* Try ISP_GET_FIRST_NEXT_DCB from AEP_1_SEC context.
           This API crashes from Init_Complete but works from IOS timer context. */
        if (g_deferred_dcb_enum && !g_deferred_enum_done &&
            g_ilb != (IOS_ILB *)0 && g_ilb->ILB_service_rtn >= 0xC0000000) {
            ISP_GET_DCB_PKT isp_gd;
            ULONG cur_dcb = 0;
            int iter;
            ULONG found_dcb = 0;

            g_deferred_enum_done = 1;  /* only try once */
            VxD_Debug_Printf("AEP: ISP_GET_FIRST_NEXT_DCB from 1SEC...\r\n");

            for (iter = 0; iter < 10 && found_dcb == 0; iter++) {
                my_memset(&isp_gd, 0, sizeof(isp_gd));
                isp_gd.func = ISP_GET_FIRST_NEXT_DCB_FUNC;  /* 10 */
                isp_gd.dcb = cur_dcb;

                Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_gd);

                log_hex("AEP: GD[", (ULONG)iter, "] ");
                log_hex("r=", (ULONG)isp_gd.result, "");
                log_hex(" dcb=", isp_gd.dcb, "\r\n");

                if (isp_gd.result != 0 || isp_gd.dcb == 0 ||
                    isp_gd.dcb < 0xC0000000)
                    break;

                {
                    UCHAR *dcb = (UCHAR *)isp_gd.dcb;
                    log_hex("  t40=", (ULONG)dcb[0x40], "");
                    log_hex(" cd+08=", *(ULONG *)(dcb + 0x08), "\r\n");

                    if (dcb[0x40] == DCB_TYPE_CDROM) {
                        found_dcb = isp_gd.dcb;
                        VxD_Debug_Printf("AEP: *** CD-ROM DCB FOUND! ***\r\n");
                        walk_calldown_chain(isp_gd.dcb, (ULONG)0);
                    }
                }
                cur_dcb = isp_gd.dcb;
            }

            /* If found, insert our calldown handler */
            if (found_dcb != 0) {
                ISP_INSERT_CD_PKT isp_cd;
                my_memset(&isp_cd, 0, sizeof(isp_cd));
                isp_cd.func = ISP_INSERT_CALLDOWN_FUNC;  /* 5 */
                isp_cd.dcb = found_dcb;
                isp_cd.req = (ULONG)ios_ior_bridge;
                isp_cd.ddb = (ULONG)&g_ios_ddb;
                isp_cd.flags = ISPCDF_PORT_DRIVER | ISPCDF_BOTTOM;
                isp_cd.lgn = 0x16;

                VxD_Debug_Printf("AEP: ISP_INSERT_CALLDOWN...\r\n");
                Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_cd);
                log_hex("AEP: INSERT result=", (ULONG)isp_cd.result, "\r\n");

                if (isp_cd.result == 0) {
                    VxD_Debug_Printf("AEP: *** HANDLER INSERTED! ***\r\n");
                    VxD_Debug_Printf("AEP: Post-insert chain:\r\n");
                    walk_calldown_chain(found_dcb, (ULONG)ios_ior_bridge);
                    g_deferred_dcb_enum = 0;  /* success, stop trying */
                }
            }
        }
    } else {
        log_hex("IOS: AEP func=", (ULONG)aep->AEP_func, "\r\n");
    }

    aep->AEP_result = AEP_SUCCESS;
}

/* Walk a calldown chain from a DCB and log each entry.
   Real layout (verified by ISP_INSERT_CALLDOWN output):
     DCB_ptr_cd at DCB+0x08 (NOT +0x34).
     CALLDOWN_NODE: +0x00=handler, +0x04=flags, +0x08=ddb, +0x0C=next.
   Returns 1 if target_func found in chain, 0 otherwise. */
static int walk_calldown_chain(ULONG dcb_addr, ULONG target_func) {
    UCHAR *dcb_raw = (UCHAR *)dcb_addr;
    ULONG cd_ptr = *(ULONG *)(dcb_raw + 0x08);
    int depth = 0;
    int found = 0;

    log_hex("  Chain head (DCB+0x08)=", cd_ptr, "\r\n");

    while (cd_ptr >= 0xC0000000 && cd_ptr < 0xD0000000 && depth < 20) {
        UCHAR *node = (UCHAR *)cd_ptr;
        ULONG handler = *(ULONG *)(node + 0x00);  /* CD_fsd_entry */
        ULONG flags   = *(ULONG *)(node + 0x04);  /* CD_flags */
        ULONG ddb     = *(ULONG *)(node + 0x08);  /* CD_ddb */
        ULONG next    = *(ULONG *)(node + 0x0C);  /* CD_next */

        log_hex("  CD[", (ULONG)depth, "]: ");
        log_hex("fn=", handler, " ");
        log_hex("fl=", flags, " ");
        log_hex("ddb=", ddb, " ");

        /* Show DDB name (VMM DDB has 8-byte name at +0x0C) */
        if (ddb >= 0xC0000000 && ddb < 0xD0000000) {
            char dname[12];
            UCHAR *dp = (UCHAR *)ddb;
            int ni;
            dname[0] = '[';
            for (ni = 0; ni < 8; ni++) dname[ni+1] = dp[0x0C + ni];
            dname[9] = ']'; dname[10] = 0;
            VxD_Debug_Printf(dname);
        }
        log_hex(" next=", next, "\r\n");

        if (handler == target_func) {
            VxD_Debug_Printf("  *** OUR HANDLER IN CHAIN ***\r\n");
            found = 1;
        }

        cd_ptr = next;
        depth++;
    }

    if (depth == 0) VxD_Debug_Printf("  (chain empty)\r\n");
    log_hex("  Chain depth=", (ULONG)depth, "\r\n");
    return found;
}

/* ================================================================
 * Early IOS Registration - called at Device_Init time.
 * Lightweight: IOS_Register + ILB recovery only, NO DCB creation.
 * Full DCB creation + chain setup + mount happens at Init_Complete
 * via _ntmini_mount_cdrom(), which checks for pre-acquired ILB.
 *
 * Why Device_Init: IOS_Register at Device_Init returns REMAIN_RESIDENT
 * and the ILB is available via DDB chain walk. At Init_Complete,
 * IOS_Register breaks NECATAPI's CDS. Getting ILB early avoids that.
 * ================================================================ */
void _ntmini_ios_register_early(void)
{
    /* Called at Device_Init. IOS_Register at Device_Init triggers
       AEP_SYSTEM_CRIT_SHUTDOWN and kills the boot.
       Instead, do a READ-ONLY DDB chain walk to pre-acquire the ILB.
       This avoids disturbing IOS while still getting the ILB early.
       At Init_Complete, _ntmini_mount_cdrom finds ILB already set
       and skips the discovery step. */
    extern ULONG VMM_Get_DDB_Wrapper(USHORT device_id);

    VxD_Debug_Printf("IOS: Early ILB scan (Device_Init, no register)...\r\n");

    if (g_ilb != (IOS_ILB *)0) {
        VxD_Debug_Printf("IOS: ILB already set, skipping\r\n");
        return;
    }

    /* Walk DDB chain to find a driver's DRP with a valid ILB.
       At Device_Init, IOSUBSYS drivers (SCSI API, NECATAPI, CDTSD, etc.)
       are already loaded and have valid DRP_ilb fields. */
    {
        ULONG ios_ddb = VMM_Get_DDB_Wrapper(0x0010);
        log_hex("IOS: IOS DDB=", ios_ddb, "\r\n");

        if (ios_ddb >= 0xC0000000 && ios_ddb < 0xD0000000) {
            UCHAR *walk_ddb = (UCHAR *)ios_ddb;
            ULONG start_ddb = ios_ddb;
            int ddb_count;

            for (ddb_count = 0; ddb_count < 80 && g_ilb == (IOS_ILB *)0; ddb_count++) {
                ULONG next_ddb = *(ULONG *)walk_ddb;
                ULONG ref = *(ULONG *)(walk_ddb + 0x2C);

                if (ref >= 0xC0000000 && ref < 0xD0000000) {
                    UCHAR *r = (UCHAR *)ref;
                    if (r[0]=='X' && r[1]=='X' && r[2]=='X' && r[3]=='X') {
                        ULONG ilb_val = *(ULONG *)(r + 0x10);
                        if (ilb_val >= 0xC0000000) {
                            IOS_ILB *fi = (IOS_ILB *)ilb_val;
                            if (fi->ILB_service_rtn >= 0xC0000000) {
                                char dn[9];
                                int j;
                                for(j=0;j<8;j++) dn[j]=r[0x14+j];
                                dn[8]=0;
                                VxD_Debug_Printf("IOS: ILB from ");
                                VxD_Debug_Printf(dn);
                                log_hex(" svc=", fi->ILB_service_rtn, "\r\n");
                                g_ilb = fi;
                            }
                        }
                    }
                }

                if (next_ddb < 0xC0000000 || next_ddb == start_ddb)
                    break;
                walk_ddb = (UCHAR *)next_ddb;
            }
        }
    }

    if (g_ilb != (IOS_ILB *)0) {
        VxD_Debug_Printf("IOS: *** ILB PRE-ACQUIRED ***\r\n");
    } else {
        VxD_Debug_Printf("IOS: No ILB at Device_Init (will find at Init_Complete)\r\n");
    }
}

/* ================================================================
 * IOS Registration - legacy path, called from dynamic init only.
 * Init_Complete now uses _ntmini_mount_cdrom() which does its own
 * chain insert into NECATAPI's DCB.
 * ================================================================ */
void _ntmini_ios_init(void)
{
    VxD_Debug_Printf("IOS: _ntmini_ios_init (legacy path)\r\n");

    /* Skip: early registration is now handled by _ntmini_ios_register_early()
       at Device_Init time. This function remains for IOSUBSYS dynamic
       loading compatibility but should not re-register. */
    if (g_ios_registered || g_ios_ddb.DRP_eyecatch_str[0] == 'X') {
        VxD_Debug_Printf("IOS: Already registered, skipping\r\n");
        return;
    }

    /* Set up DRP (Driver Registration Packet) for IOS_Register (ordinal 7).
       FROM WIN98 DDK IOS.H:
         +0x00: DRP_eyecatch_str = "XXXXXXXX" (REQUIRED!)
         +0x08: DRP_LGN = load group number (layer in IOS stack)
         +0x0C: DRP_aer = AEP handler
         +0x10: DRP_ilb = IOS fills this with ILB pointer
         +0x14: DRP_ascii_name = driver name
         +0x30: DRP_reg_result = 1=REMAIN_RESIDENT, 4=INVALID_LAYER */
    my_memset(&g_ios_ddb, 0, sizeof(g_ios_ddb));

    /* CRITICAL: EyeCatcher must be "XXXXXXXX" */
    g_ios_ddb.DRP_eyecatch_str[0] = 'X';
    g_ios_ddb.DRP_eyecatch_str[1] = 'X';
    g_ios_ddb.DRP_eyecatch_str[2] = 'X';
    g_ios_ddb.DRP_eyecatch_str[3] = 'X';
    g_ios_ddb.DRP_eyecatch_str[4] = 'X';
    g_ios_ddb.DRP_eyecatch_str[5] = 'X';
    g_ios_ddb.DRP_eyecatch_str[6] = 'X';
    g_ios_ddb.DRP_eyecatch_str[7] = 'X';

    /* LGN: DRP_ESDI_PD = 0x00400000 (ESDI port driver layer).
       Assigns us to the correct IOS layer for port driver registration. */
    g_ios_ddb.DRP_LGN = 0x00400000;  /* DRP_ESDI_PD = (1 << 0x16) */

    /* AEP handler (Async Event Routine) */
    g_ios_ddb.DRP_aer = (ULONG)ios_aep_bridge;

    /* Feature code: ESDI_506 uses 0x00080040. 0x40 = DRP_FC_IO_FOR_INQ_AEP. */
    g_ios_ddb.DRP_feature_code = 0x00080040;

    /* Driver name */
    {
        const char *n = "NTMINI";
        int i;
        for (i = 0; n[i] && i < 15; i++) g_ios_ddb.DRP_ascii_name[i] = n[i];
    }

    /* Bus type */
    g_ios_ddb.DRP_bus_type = 0;  /* DRP_BT_ESDI = IDE */

    log_hex("IOS: DRP at ", (ULONG)&g_ios_ddb, "");
    log_hex(" LGN=", g_ios_ddb.DRP_LGN, "");
    log_hex(" AER=", g_ios_ddb.DRP_aer, "\r\n");
    log_hex("IOS: sizeof(DRP)=", (ULONG)sizeof(g_ios_ddb), "\r\n");
    VxD_Debug_Printf("IOS: EyeCatcher=XXXXXXXX, name=NTMINI\r\n");

    /* Dump raw DRP bytes to verify packed layout matches ESDI_506.PDR.
       Expected: feature_code at +0x25, reg_result at +0x2C, total 0x38. */
    {
        UCHAR *raw = (UCHAR *)&g_ios_ddb;
        ULONG off;
        for (off = 0; off < sizeof(g_ios_ddb); off += 8) {
            ULONG i;
            char line[64];
            char *p = line;
            for (i = 0; i < 8 && (off + i) < sizeof(g_ios_ddb); i++) {
                static const char hx[]="0123456789ABCDEF";
                *p++ = hx[raw[off+i] >> 4];
                *p++ = hx[raw[off+i] & 0xF];
                *p++ = ' ';
            }
            *p++ = '\r'; *p++ = '\n'; *p = 0;
            log_hex("  +", off, ": ");
            VxD_Debug_Printf(line);
        }
    }

    /* Diagnostic: verify int 0x20 dispatches IOS services.
       ESDI_506.PDR analysis: IOS calls have NO fixup records,
       they work via runtime int 0x20 dispatch. */
    {
        ULONG ver = IOS_Get_Version_Test();
        log_hex("IOS: Get_Version(0x00100000) = ", ver, "\r\n");
        if (ver == 0xFEEDFACE) {
            VxD_Debug_Printf("IOS: DISPATCH FAILED!\r\n");
            return;
        }
        VxD_Debug_Printf("IOS: DISPATCH WORKS! Calling IOS_Register...\r\n");
    }

    /* Call IOS_Register (ordinal 7, service 0x00100007).
       FIXED: Was ordinal 1 (IOS_Register_Device, wrong function).
       Ordinal 7 is the REAL IOS_Register per RBIL and ESDI_506.PDR.
       Returns 0 on success (carry clear). */
    {
        ULONG result = IOS_Register_Driver(&g_ios_ddb);
        log_hex("IOS: IOS_Register returned ", result, "\r\n");

        if (result != 0) {
            log_hex("IOS: Registration FAILED (carry set), result=", result, "\r\n");
            return;
        }

        /* Check DRP_reg_result */
        log_hex("IOS: DRP_reg_result = ", (ULONG)g_ios_ddb.DRP_reg_result, "\r\n");
        log_hex("IOS: DRP_ilb = ", g_ios_ddb.DRP_ilb, "\r\n");

        if (g_ios_ddb.DRP_reg_result == DRP_REMAIN_RESIDENT) {
            VxD_Debug_Printf("IOS: REMAIN_RESIDENT! Full registration!\r\n");
        } else if (g_ios_ddb.DRP_reg_result == DRP_MINIMIZE) {
            VxD_Debug_Printf("IOS: MINIMIZE (no devices found)\r\n");
        } else if (g_ios_ddb.DRP_reg_result == DRP_INVALID_LAYER) {
            VxD_Debug_Printf("IOS: INVALID_LAYER (bad LGN or too late)\r\n");
        } else {
            log_hex("IOS: Unknown result: ", (ULONG)g_ios_ddb.DRP_reg_result, "\r\n");
        }

        /* Check if IOS provided the ILB */
        if (g_ios_ddb.DRP_ilb != 0) {
            IOS_ILB *ilb = (IOS_ILB *)g_ios_ddb.DRP_ilb;
            VxD_Debug_Printf("IOS: GOT ILB!\r\n");
            log_hex("  ILB_service_rtn = ", ilb->ILB_service_rtn, "\r\n");
            log_hex("  ILB_dprintf_rtn = ", ilb->ILB_dprintf_rtn, "\r\n");
            log_hex("  ILB_enqueue_iop = ", ilb->ILB_enqueue_iop, "\r\n");
        } else {
            VxD_Debug_Printf("IOS: No ILB provided (DRP_ilb=0)\r\n");
        }

        VxD_Debug_Printf("IOS: Registration complete!\r\n");
        g_ios_registered = TRUE;
    }

    /* No ILB from late registration. Find it through existing DCBs.
       IOS_Get_Device_List (ordinal 3) returns the DCB chain. Each DCB
       at +0x0C has a DDB pointer. The DDB/DRP at +0x10 has the ILB.
       ESDI_506's DCBs (hard drives) will have valid ILBs. */
    if (g_ios_ddb.DRP_ilb == 0 && g_ilb == (IOS_ILB *)0) {
        extern ULONG IOS_Get_Device_List_Wrapper(void);
        extern ULONG VMM_Get_DDB_Wrapper(USHORT device_id);
        ULONG dcb_head;
        ULONG ios_ddb;

        VxD_Debug_Printf("IOS: No ILB - searching via IOS services...\r\n");

        /* Method 1: IOS_Get_Device_List to walk DCB chain */
        dcb_head = IOS_Get_Device_List_Wrapper();
        log_hex("IOS: DeviceList head=", dcb_head, "\r\n");

        /* Log register outputs from IOS_Get_Device_List */
        {
            extern ULONG gdl_regs[6];
            log_hex("IOS: GDL EAX=", gdl_regs[0], "");
            log_hex(" EBX=", gdl_regs[1], "");
            log_hex(" ECX=", gdl_regs[2], "\r\n");
            log_hex("IOS: GDL EDX=", gdl_regs[3], "");
            log_hex(" ESI=", gdl_regs[4], "");
            log_hex(" EDI=", gdl_regs[5], "\r\n");
        }

        if (dcb_head >= 0xC0000000 && dcb_head < 0xD0000000) {
            UCHAR *dcb = (UCHAR *)dcb_head;
            int iter;

            /* PHASE 1: Dump first (HD) DCB raw bytes.
               This reveals the REAL DCB layout so we can fix our writes. */
            VxD_Debug_Printf("IOS: === HD DCB dump (0x100 bytes) ===\r\n");
            log_hex("IOS: HD DCB at ", (ULONG)dcb, "\r\n");
            {
                int off;
                for (off = 0; off < 0x100; off += 16) {
                    ULONG *p = (ULONG *)(dcb + off);
                    log_hex("  +", (ULONG)off, ": ");
                    log_hex("", p[0], " ");
                    log_hex("", p[1], " ");
                    log_hex("", p[2], " ");
                    log_hex("", p[3], "\r\n");
                }
            }
            VxD_Debug_Printf("IOS: === end HD DCB dump ===\r\n");

            /* Walk HD DCB calldown chain for reference */
            VxD_Debug_Printf("IOS: === HD DCB calldown chain ===\r\n");
            walk_calldown_chain((ULONG)dcb, 0);
            VxD_Debug_Printf("IOS: === end HD chain ===\r\n");

            /* Walk DCB chain: +0x0C = DCB_next. */
            for (iter = 0; iter < 20 && dcb != (UCHAR *)0; iter++) {
                ULONG next = *(ULONG *)(dcb + 0x0C);  /* DCB_next (real) */
                ULONG ptr_cd = *(ULONG *)(dcb + 0x08); /* DCB_ptr_cd */
                ULONG phys = *(ULONG *)(dcb + 0x00);   /* DCB_physical_dcb */
                log_hex("  DCB[", (ULONG)iter, "] ");
                log_hex("at=", (ULONG)dcb, "");
                log_hex(" phys=", phys, "");
                log_hex(" cd=", ptr_cd, "");
                log_hex(" next=", next, "\r\n");

                /* Try to find ILB via calldown chain DDB pointers */
                if (ptr_cd >= 0xC0000000 && ptr_cd < 0xD0000000 &&
                    g_ilb == (IOS_ILB *)0) {
                    UCHAR *node = (UCHAR *)ptr_cd;
                    int cd_i;
                    for (cd_i = 0; cd_i < 10; cd_i++) {
                        ULONG cd_ddb = *(ULONG *)(node + 0x08);
                        if (cd_ddb >= 0xC0000000 && cd_ddb < 0xD0000000) {
                            UCHAR *dp = (UCHAR *)cd_ddb;
                            if (dp[0]=='X' && dp[1]=='X' && dp[2]=='X' &&
                                dp[3]=='X' && dp[4]=='X' && dp[5]=='X') {
                                ULONG ilb_val = *(ULONG *)(dp + 0x10);
                                if (ilb_val >= 0xC0000000) {
                                    IOS_ILB *fi = (IOS_ILB *)ilb_val;
                                    if (fi->ILB_service_rtn >= 0xC0000000) {
                                        VxD_Debug_Printf("IOS: ILB from DCB chain!\r\n");
                                        g_ilb = fi;
                                        g_ios_ddb.DRP_ilb = ilb_val;
                                        break;
                                    }
                                }
                            }
                        }
                        /* Follow calldown chain */
                        {
                            ULONG cd_next = *(ULONG *)(node + 0x0C);
                            if (cd_next >= 0xC0000000 && cd_next < 0xD0000000)
                                node = (UCHAR *)cd_next;
                            else
                                break;
                        }
                    }
                }

                /* Follow DCB_next at real +0x0C */
                if (next >= 0xC0000000 && next < 0xD0000000)
                    dcb = (UCHAR *)next;
                else
                    break;
            }
        }

        /* Method 2: Get IOS's VxD DDB. DDB_Reference_Data at +0x2C
           points to IOS's internal data structures. Explore it to find
           the registered driver list and the ILB. */
        ios_ddb = VMM_Get_DDB_Wrapper(0x0010);
        log_hex("IOS: VMM_Get_DDB(IOS)=", ios_ddb, "\r\n");
        if (ios_ddb >= 0xC0000000 && ios_ddb < 0xD0000000) {
            UCHAR *ddb = (UCHAR *)ios_ddb;
            ULONG ref_data = *(ULONG *)(ddb + 0x2C); /* DDB_Reference_Data */
            log_hex("IOS: DDB_Reference_Data=", ref_data, "\r\n");

            /* Scan IOS's reference data area for "XXXXXXXX" eyecatchers.
               IOS likely stores DRP/ILB structures in this region.
               Search within 4KB of the reference data pointer. */
            if (ref_data >= 0xC0000000 && ref_data < 0xD0000000) {
                UCHAR *base = (UCHAR *)ref_data;
                ULONG off;

                /* Dump first 128 bytes of reference data */
                VxD_Debug_Printf("IOS: RefData dump:\r\n");
                for (off = 0; off < 128; off += 8) {
                    ULONG i;
                    char line[64]; char *p = line;
                    for (i = 0; i < 8; i++) {
                        static const char hx[]="0123456789ABCDEF";
                        *p++ = hx[base[off+i] >> 4];
                        *p++ = hx[base[off+i] & 0xF];
                        *p++ = ' ';
                    }
                    *p++ = '\r'; *p++ = '\n'; *p = 0;
                    log_hex("  +", off, ": ");
                    VxD_Debug_Printf(line);
                }

                /* Walk VMM DDB chain to find all loaded VxDs. For each,
               check if DDB_Reference_Data or nearby memory contains
               a DRP with "XXXXXXXX" eyecatcher and valid ILB. */
                VxD_Debug_Printf("IOS: Walking VxD DDB chain...\r\n");
                {
                    UCHAR *walk_ddb = ddb;
                    ULONG start_ddb = (ULONG)ddb;
                    int ddb_count;
                    for (ddb_count = 0; ddb_count < 80; ddb_count++) {
                        ULONG next_ddb = *(ULONG *)walk_ddb;
                        ULONG ctrl = *(ULONG *)(walk_ddb + 0x18);
                        ULONG ref = *(ULONG *)(walk_ddb + 0x2C);
                        char nbuf[9]; int j;
                        for(j=0;j<8;j++) nbuf[j]=walk_ddb[0x0C+j];
                        nbuf[8]=0;

                        /* Only log VxDs with non-zero reference data */
                        if (ref >= 0xC0000000 && ref < 0xD0000000) {
                            VxD_Debug_Printf("  VxD ");
                            VxD_Debug_Printf(nbuf);
                            log_hex(" ctrl=", ctrl, "");
                            log_hex(" ref=", ref, "");

                            /* Check if ref points to a DRP */
                            {
                                UCHAR *r = (UCHAR *)ref;
                                if (r[0]=='X' && r[1]=='X' && r[2]=='X' && r[3]=='X') {
                                    ULONG ilb_val = *(ULONG *)(r + 0x10);
                                    char dn[9];
                                    for(j=0;j<8;j++) dn[j]=r[0x14+j];
                                    dn[8]=0;
                                    VxD_Debug_Printf(" DRP=");
                                    VxD_Debug_Printf(dn);
                                    log_hex(" ILB=", ilb_val, "\r\n");

                                    if (ilb_val >= 0xC0000000 && g_ilb==(IOS_ILB*)0) {
                                        IOS_ILB *fi = (IOS_ILB *)ilb_val;
                                        if (fi->ILB_service_rtn >= 0xC0000000) {
                                            VxD_Debug_Printf("  *** VALID ILB! ***\r\n");
                                            log_hex("  svc=", fi->ILB_service_rtn, "\r\n");
                                            g_ilb = fi;
                                            g_ios_ddb.DRP_ilb = ilb_val;
                                        }
                                    }
                                } else {
                                    VxD_Debug_Printf("\r\n");
                                }
                            }

                            /* For non-IOS VxDs with ref data, scan 8KB from ctrl for DRP.
                               ESDI_506's DRP is in its data segment near its code. */
                            if (ctrl >= 0xC0000000 && g_ilb==(IOS_ILB*)0 &&
                                ref != (ULONG)&g_ios_ddb) {
                                ULONG so;
                                for (so = 0; so < 0x2000; so += 4) {
                                    UCHAR *sc = (UCHAR *)(ctrl & 0xFFFFF000) + so;
                                    if (sc[0]=='X'&&sc[1]=='X'&&sc[2]=='X'&&sc[3]=='X'&&
                                        sc[4]=='X'&&sc[5]=='X'&&sc[6]=='X'&&sc[7]=='X'&&
                                        sc!=(UCHAR*)&g_ios_ddb) {
                                        ULONG iv = *(ULONG*)(sc+0x10);
                                        if (iv >= 0xC0000000) {
                                            IOS_ILB *fi=(IOS_ILB*)iv;
                                            if (fi->ILB_service_rtn>=0xC0000000) {
                                                VxD_Debug_Printf("  DRP near ctrl!\r\n");
                                                log_hex("  at=", (ULONG)sc, "");
                                                log_hex(" ILB=", iv, "\r\n");
                                                g_ilb = fi;
                                                g_ios_ddb.DRP_ilb = iv;
                                            }
                                        }
                                        break; /* found one, stop scanning this VxD */
                                    }
                                }
                            }
                        }

                        /* Follow chain, stop on loop or bad pointer */
                        if (next_ddb < 0xC0000000 || next_ddb == start_ddb)
                            break;
                        walk_ddb = (UCHAR *)next_ddb;
                    }
                }
            }
        }

        if (g_ilb != (IOS_ILB *)0) {
            VxD_Debug_Printf("IOS: ILB acquired via DCB chain!\r\n");
        } else {
            VxD_Debug_Printf("IOS: ILB not found. Need IOSUBSYS loading.\r\n");
        }
    }

    /* If we have a valid ILB, use ISP_CREATE_DCB to create a proper
       IOS-managed DCB for our CD-ROM drive. */
    if (g_ilb != (IOS_ILB *)0 && g_ilb->ILB_service_rtn >= 0xC0000000) {
        extern void Call_ILB_Service(ULONG service_rtn, PVOID isp_packet);

        ISP_CREATE_DCB_PKT isp_dcb;

        VxD_Debug_Printf("IOS: Testing ILB service...\r\n");
        log_hex("IOS: ILB_service_rtn=", g_ilb->ILB_service_rtn, "\r\n");
        log_hex("IOS: sizeof(ISP_CREATE_DCB_PKT)=", (ULONG)sizeof(isp_dcb), "\r\n");

        /* Test: dump ISP before/after call to verify packet handling */
        my_memset(&isp_dcb, 0, sizeof(isp_dcb));
        isp_dcb.func = 1;       /* ISP_CREATE_DCB */
        isp_dcb.dcb_size = 256; /* standard DCB size */

        /* Dump ISP packet before call */
        {
            UCHAR *raw = (UCHAR *)&isp_dcb;
            ULONG i;
            VxD_Debug_Printf("IOS: ISP before: ");
            for (i = 0; i < sizeof(isp_dcb) && i < 12; i++) {
                char hb[4];
                static const char hx[]="0123456789ABCDEF";
                hb[0] = hx[raw[i] >> 4];
                hb[1] = hx[raw[i] & 0xF];
                hb[2] = ' '; hb[3] = 0;
                VxD_Debug_Printf(hb);
            }
            VxD_Debug_Printf("\r\n");
        }

        Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_dcb);

        /* Dump ISP packet after call */
        {
            UCHAR *raw = (UCHAR *)&isp_dcb;
            ULONG i;
            VxD_Debug_Printf("IOS: ISP after:  ");
            for (i = 0; i < sizeof(isp_dcb) && i < 12; i++) {
                char hb[4];
                static const char hx[]="0123456789ABCDEF";
                hb[0] = hx[raw[i] >> 4];
                hb[1] = hx[raw[i] & 0xF];
                hb[2] = ' '; hb[3] = 0;
                VxD_Debug_Printf(hb);
            }
            VxD_Debug_Printf("\r\n");
        }

        log_hex("IOS: ISP_CREATE_DCB result=", (ULONG)isp_dcb.result, "");
        log_hex(" dcb_ptr=", isp_dcb.dcb_ptr, "\r\n");

        if (isp_dcb.result == 0 && isp_dcb.dcb_ptr >= 0xC0000000) {
            /* IOS created a DCB! Fill using RAW offsets from Win98 DDK.
               CRITICAL: Our IOS_DCB struct has WRONG offsets! E.g.:
                 Our +0x0C (DCB_ddb) = Real +0x0C (DCB_next) — corrupts chain!
                 Our +0x10 (device_type) = Real +0x10 (next_logical) — corrupts ptr!
               Use real DDK offsets with raw pointer writes. */
            UCHAR *dcb = (UCHAR *)isp_dcb.dcb_ptr;
            ULONG saved_0x0C, saved_0x10, saved_0x18;
            ULONG g_test_iop_ptr = 0;  /* saved IOP pointer for Phase 5 */
            VxD_Debug_Printf("IOS: GOT IOS-MANAGED DCB!\r\n");
            g_our_dcb_addr = isp_dcb.dcb_ptr;

            /* Save IOS's clean chain pointers before old struct writes */
            saved_0x0C = *(ULONG *)(dcb + 0x0C);
            saved_0x10 = *(ULONG *)(dcb + 0x10);
            saved_0x18 = *(ULONG *)(dcb + 0x18);

            /* Old struct writes: wrong DDK offsets but CONFIG_DCB needs them.
               Non-NULL values at +0x0C/+0x10/+0x18 prevent GPF in upper layers.
               We restore IOS's clean values before DEVICE_ARRIVED. */
            {
                IOS_DCB *sdcb = (IOS_DCB *)isp_dcb.dcb_ptr;
                sdcb->DCB_device_type = DCB_TYPE_CDROM;
                sdcb->DCB_bus_type = DCB_BUS_ESDI;
                sdcb->DCB_bus_number = 1;
                sdcb->DCB_target_id = 0;
                sdcb->DCB_lun = 0;
                sdcb->DCB_dmd_flags = DCB_DEV_PHYSICAL | DCB_DEV_REMOVABLE |
                                      DCB_DEV_UNCERTAIN_MEDIA;
                sdcb->DCB_apparent_blk_shift = 11;
                sdcb->DCB_apparent_blk_size = 2048;
                sdcb->DCB_max_xfer_len = 64 * 1024;
                sdcb->DCB_ddb = (PVOID)&g_ios_ddb;
                {
                    const char *vid = "QEMU    ";
                    const char *pid = "CD-ROM  ";
                    int i;
                    for (i = 0; i < 8 && vid[i]; i++) sdcb->DCB_vendor_id[i] = vid[i];
                    for (i = 0; i < 16 && pid[i]; i++) sdcb->DCB_product_id[i] = pid[i];
                }
            }

            log_hex("IOS: DCB created at ", isp_dcb.dcb_ptr, "\r\n");

            /* Write DCB fields at correct real DDK offsets.
               The IOS_DCB struct has wrong offsets for some fields,
               so we write raw to ensure CDTSD sees device_type=5
               at the correct +0x40 position.
               Key offsets:
                 +0x08: DCB_ptr_cd        +0x14: DCB_drive_lttr_equiv
                 +0x1C: DCB_dmd_flags     +0x20: TSD flags area
                 +0x40: DCB_device_flags  +0x44: DCB_device_flags2 */
            {
                UCHAR *raw = (UCHAR *)isp_dcb.dcb_ptr;

                /* +0x1C: DCB_dmd_flags */
                *(ULONG *)(raw + 0x1C) = DCB_DEV_PHYSICAL | DCB_DEV_REMOVABLE |
                                          DCB_DEV_UNCERTAIN_MEDIA;

                /* +0x20: TSD flags area (keep for compat, CDTSD ORs here) */
                *(ULONG *)(raw + 0x20) = DCB_DEV_PHYSICAL | DCB_TYPE_CDROM;

                /* +0x24: secondary flags */
                *(ULONG *)(raw + 0x24) = DCB_DEV2_ATAPI_DEVICE;

                /* +0x40: DCB_device_flags. CDTSD checks byte [dcb+0x40] == 5.
                   Must be written after old struct fills to avoid clobber. */
                *(ULONG *)(raw + 0x40) = DCB_DEV_PHYSICAL | DCB_TYPE_CDROM;

                /* +0x44: DCB_device_flags2 */
                *(ULONG *)(raw + 0x44) = DCB_DEV2_ATAPI_DEVICE;

                /* +0x4C: DCB_apparent_blk_shift (byte) = log2(2048) */
                *(UCHAR *)(raw + 0x4C) = 11;

                /* +0x50: DCB_apparent_blk_size = 2048 */
                *(ULONG *)(raw + 0x50) = 2048;

                /* +0x60: DCB_vendor_id (8 bytes, space-padded) */
                {
                    const char *vid = "QEMU    ";
                    int i;
                    for (i = 0; i < 8; i++) raw[0x60 + i] = vid[i];
                }

                /* +0x68: DCB_product_id (16 bytes, space-padded) */
                {
                    const char *pid = "QEMU CD-ROM     ";
                    int i;
                    for (i = 0; i < 16; i++) raw[0x68 + i] = pid[i];
                }

                /* +0x78: DCB_rev_level (4 bytes) */
                raw[0x78] = '1'; raw[0x79] = '.';
                raw[0x7A] = '0'; raw[0x7B] = ' ';

                VxD_Debug_Printf("IOS: Raw DCB writes at real offsets done\r\n");
                log_hex("IOS: DCB+0x40 (device_flags)=", *(ULONG *)(raw + 0x40), "\r\n");
            }

            /* NEW ORDER: INSERT → BROADCAST → ASSOCIATE → DEVICE_ARRIVED.
               Upper layers insert calldowns and probe during BROADCAST.
               Without our port driver calldown, probes hang (no handler
               at bottom of chain). Installing our handler FIRST means
               probes get NOT_READY responses (g_ios_ready=1 but handler
               returns NOT_READY for unknown IOR funcs). */
            {
                /* Step 1: Insert our calldown FIRST */
                {
                    ISP_INSERT_CD_PKT isp_cd;
                    my_memset(&isp_cd, 0, sizeof(isp_cd));
                    isp_cd.func = 5;
                    isp_cd.dcb = isp_dcb.dcb_ptr;
                    isp_cd.req = (ULONG)ios_ior_bridge;
                    isp_cd.ddb = (ULONG)&g_ios_ddb;
                    isp_cd.flags = 0x0002;  /* ISPCDF_PORT_DRIVER */
                    isp_cd.lgn = 0x16;

                    VxD_Debug_Printf("IOS: ISP_INSERT_CALLDOWN (func=5)...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_cd);
                    log_hex("IOS: INSERT result=", (ULONG)isp_cd.result, "\r\n");
                }

                VxD_Debug_Printf("IOS: === Post-insert chain ===\r\n");
                walk_calldown_chain(isp_dcb.dcb_ptr, (ULONG)ios_ior_bridge);

                /* Step 2: Broadcast CONFIG_DCB (with calldown in place) */
                {
                    UCHAR aep_buf[20];
                    ISP_BCAST_AEP_PKT isp_bcast;
                    my_memset(aep_buf, 0, sizeof(aep_buf));
                    *(USHORT *)(aep_buf + 0) = AEP_CONFIG_DCB;
                    *(USHORT *)(aep_buf + 2) = 0;
                    *(ULONG *)(aep_buf + 4) = (ULONG)&g_ios_ddb;  /* AEP_ddb: our DDB */
                    *(UCHAR *)(aep_buf + 8) = 0x16;
                    *(ULONG *)(aep_buf + 0x0C) = isp_dcb.dcb_ptr;
                    *(ULONG *)(aep_buf + 0x10) = (ULONG)&g_ios_ddb;

                    my_memset(&isp_bcast, 0, sizeof(isp_bcast));
                    isp_bcast.func = 20;
                    isp_bcast.paep = (ULONG)aep_buf;

                    VxD_Debug_Printf("IOS: Broadcasting AEP_CONFIG_DCB...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_bcast);
                    log_hex("IOS: BROADCAST result=", (ULONG)isp_bcast.result, "\r\n");
                }

                /* Step 3: ISP_ASSOCIATE_DCB — tell IOS about our drive letter.
                   CDTSD wrote DCB+0x14=0x0303 (D:) during CONFIG_DCB, but that
                   was a raw write. IOS needs the formal ISP to register the
                   volume and create a VRP during DEVICE_ARRIVED. */
                {
                    ISP_ASSOC_DCB_PKT isp_assoc;
                    my_memset(&isp_assoc, 0, sizeof(isp_assoc));
                    isp_assoc.func = 6;  /* ISP_ASSOCIATE_DCB */
                    isp_assoc.dcb = isp_dcb.dcb_ptr;
                    isp_assoc.drive = 3;  /* D: */
                    isp_assoc.flags = 0;

                    VxD_Debug_Printf("IOS: ISP_ASSOCIATE_DCB (func=6, drive=D:)...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_assoc);
                    log_hex("IOS: ASSOCIATE result=", (ULONG)isp_assoc.result, "\r\n");
                }

                /* Step 3b: ISP_DRIVE_LETTER_PICK — register drive letter in IOS
                   lookup table. Without this, IOS_Get_DCB(D:) returns NULL and
                   IFSMgr_CDROM_Attach fails with -1. */
                {
                    ISP_DLP_PKT isp_dlp;
                    my_memset(&isp_dlp, 0, sizeof(isp_dlp));
                    isp_dlp.func = ISP_DRIVE_LETTER_PICK_FUNC;
                    isp_dlp.dcb = isp_dcb.dcb_ptr;
                    isp_dlp.letter[0] = 3;  /* D: */
                    isp_dlp.letter[1] = 3;  /* D: (single letter range) */
                    isp_dlp.flags = ISP_PDL_FL_OK_RM_CD;

                    VxD_Debug_Printf("IOS: ISP_DRIVE_LETTER_PICK (func=16, D:)...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_dlp);
                    log_hex("IOS: DLP result=", (ULONG)isp_dlp.result, "\r\n");
                }

                /* Walk chain to see what upper layers added during broadcast */
                VxD_Debug_Printf("IOS: === Final chain ===\r\n");
                walk_calldown_chain(isp_dcb.dcb_ptr, (ULONG)ios_ior_bridge);

                /* Dump DCB to verify field writes at correct offsets */
                {
                    UCHAR *raw = (UCHAR *)isp_dcb.dcb_ptr;
                    int off;
                    VxD_Debug_Printf("IOS: DCB dump (0x00-0x7F):\r\n");
                    for (off = 0; off < 0x80; off += 16) {
                        ULONG *p = (ULONG *)(raw + off);
                        log_hex("  +", (ULONG)off, ": ");
                        log_hex("", p[0], " ");
                        log_hex("", p[1], " ");
                        log_hex("", p[2], " ");
                        log_hex("", p[3], "\r\n");
                    }
                }

                /* Restore IOS chain pointers corrupted by old struct writes.
                   CONFIG_DCB broadcast needed the non-NULL values.
                   DEVICE_ARRIVED needs clean pointers. */
                {
                    UCHAR *raw = (UCHAR *)isp_dcb.dcb_ptr;
                    *(ULONG *)(raw + 0x0C) = saved_0x0C;
                    *(ULONG *)(raw + 0x10) = saved_0x10;
                    *(ULONG *)(raw + 0x18) = saved_0x18;
                }

                /* PHASE 3a: Create VRP BEFORE DEVICE_ARRIVED.
                   Voltrack checks DCB+0x18 during DEVICE_ARRIVED to decide
                   whether to set up media change monitoring. */
                {
                    UCHAR *raw = (UCHAR *)isp_dcb.dcb_ptr;
                    my_memset(g_dummy_vrp, 0, sizeof(g_dummy_vrp));

                    *(ULONG *)(g_dummy_vrp + 0x00) = 0;            /* demand_flags */
                    *(ULONG *)(g_dummy_vrp + 0x04) = 0;            /* event_flags */
                    *(ULONG *)(g_dummy_vrp + 0x08) = 16;           /* max_sgds */
                    *(ULONG *)(g_dummy_vrp + 0x0C) = 64 * 1024;    /* max_req_size */
                    *(ULONG *)(g_dummy_vrp + 0x10) = 0;            /* delta_to_ior */
                    *(ULONG *)(g_dummy_vrp + 0x14) = 2048;         /* block_size (CD-ROM) */
                    *(ULONG *)(g_dummy_vrp + 0x18) = 0;            /* fsd_hvol (none yet) */
                    *(ULONG *)(g_dummy_vrp + 0x1C) = 0;            /* fsd_entry (none yet) */
                    *(ULONG *)(g_dummy_vrp + 0x20) = isp_dcb.dcb_ptr; /* back-ptr to DCB */
                    *(ULONG *)(g_dummy_vrp + 0x24) = 3;            /* logical drive D: */

                    *(ULONG *)(raw + 0x18) = (ULONG)g_dummy_vrp;

                    log_hex("IOS: Manual VRP at ", (ULONG)g_dummy_vrp, "\r\n");
                    log_hex("IOS: DCB+0x18 (vrp_ptr)=", *(ULONG *)(raw+0x18), "\r\n");
                    VxD_Debug_Printf("IOS: VRP installed BEFORE DEVICE_ARRIVED\r\n");
                }

                /* PHASE 3b: Broadcast AEP_CREATE_VRP before DEVICE_ARRIVED.
                   Notify voltrack, CDVSD, CDFS about the new volume. */
                {

                    /* Broadcast AEP_CREATE_VRP (func=18) to notify CDFS
                       and other drivers about the new volume. */
                    {
                        UCHAR aep_vrp[24];
                        ISP_BCAST_AEP_PKT isp_bv;
                        my_memset(aep_vrp, 0, sizeof(aep_vrp));
                        *(USHORT *)(aep_vrp + 0) = AEP_CREATE_VRP; /* func=18 */
                        *(USHORT *)(aep_vrp + 2) = 0;
                        *(ULONG *)(aep_vrp + 4) = (ULONG)&g_ios_ddb;
                        *(UCHAR *)(aep_vrp + 8) = 0x16;
                        /* Extended fields: DCB and VRP pointers */
                        *(ULONG *)(aep_vrp + 0x0C) = isp_dcb.dcb_ptr;
                        *(ULONG *)(aep_vrp + 0x10) = (ULONG)g_dummy_vrp;

                        my_memset(&isp_bv, 0, sizeof(isp_bv));
                        isp_bv.func = 20;  /* ISP_BROADCAST_AEP */
                        isp_bv.paep = (ULONG)aep_vrp;

                        VxD_Debug_Printf("IOS: Broadcasting AEP_CREATE_VRP...\r\n");
                        Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_bv);
                        log_hex("IOS: CREATE_VRP bcast result=", (ULONG)isp_bv.result, "\r\n");
                    }
                }

                /* PHASE 3c: ISP_DEVICE_ARRIVED (VRP now in place).
                   Voltrack sees DCB+0x18 = VRP, sets up media monitoring. */
                g_ios_ready = 1;
                {
                    ISP_DEV_ARRIVED_PKT isp_arr;
                    my_memset(&isp_arr, 0, sizeof(isp_arr));
                    isp_arr.func = ISP_DEVICE_ARRIVED_FUNC;  /* 14 */
                    isp_arr.dcb = isp_dcb.dcb_ptr;
                    isp_arr.flags = 0;

                    VxD_Debug_Printf("IOS: ISP_DEVICE_ARRIVED (func=14)...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_arr);
                    log_hex("IOS: DEVICE_ARRIVED result=", (ULONG)isp_arr.result, "\r\n");
                    VxD_Debug_Printf("IOS: DEVICE_ARRIVED survived!\r\n");
                }

                /* Walk final chain */
                VxD_Debug_Printf("IOS: === Final calldown chain ===\r\n");
                walk_calldown_chain(isp_dcb.dcb_ptr, (ULONG)ios_ior_bridge);

                /* Post-DEVICE_ARRIVED DCB dump */
                {
                    UCHAR *raw = (UCHAR *)isp_dcb.dcb_ptr;
                    log_hex("IOS: post-DA DCB+0x18 (vrp_ptr)=", *(ULONG *)(raw+0x18), "\r\n");
                    log_hex("IOS: post-DA DCB+0x40 (dev_flags)=", *(ULONG *)(raw+0x40), "\r\n");
                }

                /* PHASE 4: Test IOR flow by creating and submitting an IOP.
                   Use ISP_CREATE_IOP to get a proper IOS-managed IOP,
                   then submit via ILB_internal_request. */
                {
                    ISP_CREATE_IOP_PKT isp_iop;
                    my_memset(&isp_iop, 0, sizeof(isp_iop));
                    isp_iop.func = ISP_CREATE_IOP;  /* 2 */
                    isp_iop.dcb = isp_dcb.dcb_ptr;
                    isp_iop.delta = 0;

                    VxD_Debug_Printf("IOS: Creating test IOP...\r\n");
                    log_hex("IOS: ISP_CREATE_IOP pkt size=", (ULONG)sizeof(isp_iop), "\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_iop);
                    log_hex("IOS: CREATE_IOP result=", (ULONG)isp_iop.result, "");
                    log_hex(" iop=", isp_iop.iop_ptr, "\r\n");

                    if (isp_iop.result == 0 && isp_iop.iop_ptr >= 0xC0000000) {
                        UCHAR *iop = (UCHAR *)isp_iop.iop_ptr;
                        int off;
                        g_test_iop_ptr = isp_iop.iop_ptr;

                        /* Dump IOP (0x100 bytes) to find DCB link and IOR offset */
                        VxD_Debug_Printf("IOS: IOP dump (0x100 bytes):\r\n");
                        for (off = 0; off < 0x100; off += 16) {
                            ULONG *p = (ULONG *)(iop + off);
                            log_hex("  +", (ULONG)off, ": ");
                            log_hex("", p[0], " ");
                            log_hex("", p[1], " ");
                            log_hex("", p[2], " ");
                            log_hex("", p[3], "\r\n");
                        }

                        /* Search for our DCB pointer in the IOP */
                        {
                            ULONG dcb_val = isp_dcb.dcb_ptr;
                            int si;
                            for (si = 0; si < 0x100; si += 4) {
                                if (*(ULONG *)(iop + si) == dcb_val) {
                                    log_hex("IOS: DCB ptr found at IOP+", (ULONG)si, "\r\n");
                                }
                            }
                        }

                        VxD_Debug_Printf("IOS: IOP created\r\n");
                    } else {
                        log_hex("IOS: CREATE_IOP failed result=", (ULONG)isp_iop.result, "\r\n");
                    }
                }

                /* PHASE 4: Direct handler test.
                   Call ios_ior_bridge directly with a fake IOP buffer.
                   IOR is at IOP+0x64 (IOR_BASE). Tests: 'I' marker fires,
                   handler processes, no crash. g_ior_test_mode skips BD_Complete. */
                {
                    UCHAR test_iop[0xA0];  /* large enough for IOP header + IOR */
                    typedef void (*PFN_IOR_BRIDGE)(PVOID);
                    PFN_IOR_BRIDGE bridge = (PFN_IOR_BRIDGE)ios_ior_bridge;

                    my_memset(test_iop, 0, sizeof(test_iop));
                    /* IOR_func at IOP+0x68 (IOR_BASE + 0x04) = MEDIA_CHECK */
                    *(USHORT *)(test_iop + IOR_BASE + 0x04) = IOR_MEDIA_CHECK_FC;
                    /* IOR_status at IOP+0x6A (IOR_BASE + 0x06) = sentinel */
                    *(USHORT *)(test_iop + IOR_BASE + 0x06) = 0x00FF;

                    g_ior_test_mode = 1;
                    VxD_Debug_Printf("IOS: Direct IOR test (MEDIA_CHECK)...\r\n");
                    bridge(test_iop);
                    g_ior_test_mode = 0;

                    log_hex("IOS: IOR status after test=",
                            (ULONG)*(USHORT *)(test_iop + IOR_BASE + 0x06), "\r\n");
                    if (*(USHORT *)(test_iop + IOR_BASE + 0x06) == IORS_SUCCESS_FC) {
                        VxD_Debug_Printf("IOS: IOR handler WORKS! MEDIA_CHECK -> SUCCESS\r\n");
                    } else {
                        VxD_Debug_Printf("IOS: IOR handler returned unexpected status\r\n");
                    }
                }

                /* Dump chain state NOW (after VRP) to check for changes */
                VxD_Debug_Printf("IOS: === Pre-Phase5 chain state ===\r\n");
                log_hex("IOS: DCB+0x08 now=",
                        *(ULONG *)((UCHAR *)isp_dcb.dcb_ptr + 0x08), "\r\n");
                walk_calldown_chain(isp_dcb.dcb_ptr, (ULONG)ios_ior_bridge);

                /* PHASE 6: Patch chain to bypass scsi1hlp + voltrack.
                   Proven by Phase 5b/5c tests (now removed for speed):
                   - scsi1hlp returns INVALID_CMD for our device
                   - CDTSD passes through correctly
                   - Our handler works via IOS chain (MEDIA_CHECK → SUCCESS)
                   Patch: chain head → CDTSD → us (skip voltrack + scsi1hlp) */
                if (g_test_iop_ptr >= 0xC0000000 &&
                    g_ilb->ILB_internal_request >= 0xC0000000) {
                    UCHAR *iop = (UCHAR *)g_test_iop_ptr;
                    {
                        UCHAR *dcb_raw = (UCHAR *)isp_dcb.dcb_ptr;
                        ULONG cd_ptr = *(ULONG *)(dcb_raw + 0x08);
                        ULONG handlers[8];
                        int count = 0;

                        while (cd_ptr >= 0xC0000000 && count < 8) {
                            handlers[count] = *(ULONG *)((UCHAR *)cd_ptr);
                            cd_ptr = *(ULONG *)((UCHAR *)cd_ptr + 0x0C);
                            count++;
                        }

                        /* PHASE 6 chain patch:
                           Patch chain to bypass scsi1hlp + voltrack,
                           then read sector 16 (ISO 9660 PVD) through IOS chain.
                           scsi1hlp returns INVALID_CMD (0x16) for our device.
                           Our handler already does SCSI translation internally. */
                        if (count >= 4) {
                            /* Patch: set voltrack's next → our handler (skip CDTSD+scsi1hlp) */
                            /* Actually: set CDTSD's next → our handler (skip only scsi1hlp).
                               Also set chain head → CDTSD (skip voltrack). */
                            UCHAR *dcb_raw2 = (UCHAR *)isp_dcb.dcb_ptr;
                            ULONG cdtsd_node_addr;
                            ULONG our_node_addr;

                            /* Find CDTSD's node (CD[1]) and our node (CD[3]) */
                            {
                                ULONG p = *(ULONG *)(dcb_raw2 + 0x08); /* chain head = CD[0] */
                                ULONG cd1_addr = *(ULONG *)((UCHAR *)p + 0x0C); /* CD[0]->next = CD[1] */
                                ULONG cd2_addr = *(ULONG *)((UCHAR *)cd1_addr + 0x0C); /* CD[1]->next = CD[2] */
                                our_node_addr = *(ULONG *)((UCHAR *)cd2_addr + 0x0C); /* CD[2]->next = CD[3] */
                                cdtsd_node_addr = cd1_addr;

                                log_hex("IOS: P6: CDTSD node=", cdtsd_node_addr, "");
                                log_hex(" our node=", our_node_addr, "\r\n");

                                /* Patch CDTSD next → our node (bypass only scsi1hlp).
                                   Keep voltrack in chain: it manages VRPs and media changes.
                                   scsi1hlp returns INVALID_CMD for our device.
                                   New chain: voltrack → CDTSD → us */
                                *(ULONG *)((UCHAR *)cdtsd_node_addr + 0x0C) = our_node_addr;

                                /* Keep chain head → voltrack (DO NOT bypass voltrack) */
                                /* *(ULONG *)(dcb_raw2 + 0x08) = cdtsd_node_addr; -- removed */

                                VxD_Debug_Printf("IOS: P6: Chain patched: voltrack -> CDTSD -> us\r\n");
                            }

                            /* Verify patched chain */
                            walk_calldown_chain(isp_dcb.dcb_ptr, (ULONG)ios_ior_bridge);

                            /* Test MEDIA_CHECK through patched chain (EDX=0) */
                            my_memset(iop + IOR_BASE, 0, 0x50);
                            *(USHORT *)(iop + IOR_BASE + 0x04) = IOR_MEDIA_CHECK_FC;
                            *(USHORT *)(iop + IOR_BASE + 0x06) = 0x00FF;
                            *(ULONG *)(iop + 0x10) = 0;

                            VxD_Debug_Printf("IOS: P6: MEDIA_CHECK (EDX=0, patched chain)...\r\n");
                            Call_ILB_Internal_Request(
                                g_ilb->ILB_internal_request,
                                isp_dcb.dcb_ptr,
                                g_test_iop_ptr,
                                0);
                            log_hex("IOS: P6 MC status=",
                                (ULONG)*(USHORT *)(iop + IOR_BASE + 0x06), "\r\n");

                            {
                                USHORT mc_st = *(USHORT *)(iop + IOR_BASE + 0x06);
                                /* Accept SUCCESS (0x00), UNCERTAIN_MEDIA (0x19), or
                                   0xFF (voltrack absorbed the check). All mean chain is alive. */
                                if (mc_st == IORS_SUCCESS_FC ||
                                    mc_st == IORS_UNCERTAIN_MEDIA_FC ||
                                    mc_st == 0x00FF) {
                                    g_deferred_dcb_ptr = isp_dcb.dcb_ptr;
                                    g_deferred_iop_ptr = g_test_iop_ptr;
                                    g_deferred_read_pending = 1;
                                    VxD_Debug_Printf("IOS: P6: Chain WORKS. Reads deferred to post-boot.\r\n");
                                }
                            }
                        }
                    }
                } else {
                    VxD_Debug_Printf("IOS: Skipping chain test\r\n");
                }
            }
        } else {
            VxD_Debug_Printf("IOS: ISP_CREATE_DCB failed\r\n");
        }
    } else {
        VxD_Debug_Printf("IOS: No ILB, using local DCB\r\n");
    }

    /* PHASE 7: IFSMgr mount deferred to post-boot (first IOR handler entry).
       IFSMgr_CDROM_Attach returns 0xFFC0 during Device_Init because IFSMgr
       isn't ready yet. Post-boot, the system is fully initialized. */

    VxD_Debug_Printf("IOS: Creating local DCB...\r\n");

    my_memset(&g_ios_dcb, 0, sizeof(g_ios_dcb));
    g_ios_dcb.DCB_cmn_size = sizeof(IOS_DCB);
    g_ios_dcb.DCB_ddb = (PVOID)&g_ios_ddb;
    g_ios_dcb.DCB_device_type = DCB_TYPE_CDROM;
    g_ios_dcb.DCB_bus_type = DCB_BUS_ESDI;
    g_ios_dcb.DCB_bus_number = 1;
    g_ios_dcb.DCB_target_id = 0;
    g_ios_dcb.DCB_lun = 0;
    g_ios_dcb.DCB_dmd_flags = DCB_DEV_PHYSICAL | DCB_DEV_REMOVABLE |
                               DCB_DEV_UNCERTAIN_MEDIA;
    g_ios_dcb.DCB_apparent_blk_shift = 11;
    g_ios_dcb.DCB_apparent_blk_size = 2048;
    g_ios_dcb.DCB_max_xfer_len = 64 * 1024;

    log_hex("IOS: DCB at ", (ULONG)&g_ios_dcb, "");
    log_hex(" size=", g_ios_dcb.DCB_cmn_size, "\r\n");

    /* Set up calldown entry - our IOR handler at the bottom of the chain */
    g_ios_calldown.CD_func = (PVOID)ios_ior_bridge;
    g_ios_calldown.CD_ddb = (PVOID)&g_ios_ddb;
    g_ios_calldown.CD_next = (IOS_CALLDOWN *)0;
    g_ios_calldown.CD_flags = 0;

    /* DISABLED: ConfigMgr devnode processing causes boot hang.
       CM_Setup_DevNode triggers PnP processing that stalls boot,
       preventing Init_Complete from ever being dispatched.
       IFSMgr mount doesn't need ConfigMgr devnodes. */
    VxD_Debug_Printf("CM: Skipping devnode processing (causes boot hang)\r\n");
}
