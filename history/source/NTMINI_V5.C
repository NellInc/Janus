/* NTMINI_V5.C - Real ScsiPort shim with embedded ATAPI.SYS
 *
 * Loads NT4 atapi.sys, provides real ScsiPort functions that do
 * actual IDE port I/O. Calls DriverEntry → ScsiPortInitialize →
 * HwFindAdapter to detect IDE hardware in QEMU.
 *
 * Build: wcc386 -bt=windows -3s -s -zl -d0 -i=. NTMINI_V5.C
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

/* SCSI_ADAPTER_CONTROL_TYPE values (NT5 HwScsiAdapterControl) */
#define ScsiQuerySupportedControlTypes  0
#define ScsiStopAdapter                 1
#define ScsiRestartAdapter              2
#define ScsiSetBootConfig               3
#define ScsiSetRunningConfig            4

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
    ULONG   (__stdcall *HwAdapterControl)(PVOID DeviceExtension, ULONG ControlType, PVOID Parameters);
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

    /* Detect NT5 extended HW_INITIALIZATION_DATA (HwScsiAdapterControl support).
       NT4: HwInitializationDataSize == 0x40 (no HwAdapterControl field)
       NT5: HwInitializationDataSize >= 0x44 (HwAdapterControl at offset 0x40) */
    log_hex("SP: HwInitializationDataSize=", HwInitData->HwInitializationDataSize, "\r\n");
    if (HwInitData->HwInitializationDataSize >= 0x44) {
        g_state.HwAdapterControl = (ULONG(__stdcall *)(PVOID, ULONG, PVOID))
                                    HwInitData->HwAdapterControl;
        log_hex("SP: NT5 miniport detected, HwAdapterControl=", (ULONG)g_state.HwAdapterControl, "\r\n");
    } else {
        g_state.HwAdapterControl = NULL;
        VxD_Debug_Printf("SP: NT4 miniport detected\r\n");
    }

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

    /* NT5: notify miniport that adapter is running */
    if (g_state.HwAdapterControl != NULL) {
        ULONG acResult;
        VxD_Debug_Printf("SP: Calling HwAdapterControl(ScsiSetRunningConfig)...\r\n");
        acResult = g_state.HwAdapterControl(g_state.DeviceExtension,
                                            ScsiSetRunningConfig, NULL);
        log_hex("SP: HwAdapterControl(ScsiSetRunningConfig) returned ", acResult, "\r\n");
    }

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

    /* Verify embedded data */
    memlog("MZ_check\r\n");
    if (atapi_embedded_data[0] != 'M' || atapi_embedded_data[1] != 'Z') {
        memlog("FAIL:no_MZ\r\n");
        VxD_Debug_Printf("V5: No MZ in embedded data!\r\n");
        return 1;  /* Stay loaded for memlog */
    }

    /* PE load */
    memlog("PE_load\r\n");
    VxD_Debug_Printf("V5: Loading PE...\r\n");
    entry_point = (void *)0;
    image_base = (void *)0;
    rc = pe_load_image(atapi_embedded_data, ATAPI_EMBEDDED_SIZE,
                       scsiport_funcs, &entry_point, &image_base);

    if (rc != 0) {
        memlog("FAIL:PE_load\r\n");
        VxD_Debug_Printf("V5: PE load FAILED\r\n");
        return 1;  /* Stay loaded for memlog */
    }

    memlog_hex("entry=", (ULONG)entry_point);
    memlog("\r\n");
    log_hex("V5: entry=", (ULONG)entry_point, "\r\n");

    /* Call DriverEntry → ScsiPortInitialize → HwFindAdapter */
    memlog("DriverEntry\r\n");
    VxD_Debug_Printf("V5: Calling DriverEntry...\r\n");
    DriverEntry = (PFN_DRIVER_ENTRY)entry_point;
    status = DriverEntry((void *)0, (void *)0);
    memlog_hex("DE_ret=", status);
    memlog("\r\n");
    log_hex("V5: DriverEntry returned ", status, "\r\n");

    if (status == STATUS_SUCCESS) {
        VxD_Debug_Printf("V5: SUCCESS - IDE miniport initialized!\r\n");

        /* Populate IOSBRIDGE-compatible globals for the IOS bridge layer.
           These are defined in the ASM file and referenced by IOSBRIDGE.C. */
        g_HwStartIo = (PVOID)g_state.HwStartIo;
        g_HwResetBus = (PVOID)g_state.HwResetBus;
        g_HwInterrupt = (PVOID)g_state.HwInterrupt;
        g_DeviceExtension = g_state.DeviceExtension;
        g_SrbExtensionSize = 0;  /* Not tracked yet */
        g_SrbCompleted = FALSE;
        g_ReadyForNext = FALSE;
        VxD_Debug_Printf("V5: IOSBRIDGE globals populated\r\n");

        /* IOS registration and CONFIGMG devnode creation during Device_Init.
           This is the CONFIG MANAGER processing window - devnodes created here
           get processed immediately by CONFIGMG. */
        {
            extern ULONG IOS_Get_Version_Test(void);
            extern void _ntmini_ios_init(void);
            extern ULONG CM_Create_DevNode_Wrapper(ULONG *pdn, char *id, ULONG parent, ULONG flags);
            extern ULONG CM_Locate_DevNode_Wrapper(ULONG *pdn, char *id, ULONG flags);
            extern ULONG CM_Register_Device_Driver_Wrapper(ULONG dn, PVOID handler, ULONG ref, ULONG flags);
            extern ULONG CM_Setup_DevNode_Wrapper(ULONG dn, ULONG flags);

            ULONG ver = IOS_Get_Version_Test();
            log_hex("V5: IOS_Get_Version during DevInit = ", ver, "\r\n");
            if (ver != 0xFEEDFACE) {
                ULONG devnode = 0, parent = 0, cr;
                char root_id[] = "Root\\SCSIAdapter\\0000";

                VxD_Debug_Printf("V5: IOS available!\r\n");
                _ntmini_ios_init();

                /* Create devnode during Device_Init processing window.
                   First find root devnode as parent. */
                cr = CM_Locate_DevNode_Wrapper(&parent, (char *)0, 0);
                log_hex("V5: Root devnode=", parent, "\r\n");

                /* Try to create our devnode */
                cr = CM_Create_DevNode_Wrapper(&devnode, root_id, parent, 0);
                log_hex("V5: Create_DevNode cr=", cr, "");
                log_hex(" devnode=", devnode, "\r\n");

                if (cr == 0 && devnode != 0) {
                    /* Created! Now register as driver and setup */
                    cr = CM_Register_Device_Driver_Wrapper(devnode, (PVOID)0, 0, 0);
                    log_hex("V5: Register_Device_Driver cr=", cr, "\r\n");
                    cr = CM_Setup_DevNode_Wrapper(devnode, 0);
                    log_hex("V5: Setup_DevNode cr=", cr, "\r\n");
                } else if (cr == 0x10) {
                    /* CR_ALREADY_SUCH_DEVNODE - already exists from REGEDIT */
                    VxD_Debug_Printf("V5: DevNode already exists (from registry)\r\n");
                    cr = CM_Locate_DevNode_Wrapper(&devnode, root_id, 0);
                    if (cr == 0) {
                        cr = CM_Register_Device_Driver_Wrapper(devnode, (PVOID)0, 0, 0);
                        log_hex("V5: Register on existing cr=", cr, "\r\n");
                        cr = CM_Setup_DevNode_Wrapper(devnode, 0);
                        log_hex("V5: Setup on existing cr=", cr, "\r\n");
                    }
                }
            } else {
                VxD_Debug_Printf("V5: IOS not ready, defer to Init_Complete\r\n");
            }
        }
    } else {
        VxD_Debug_Printf("V5: DriverEntry failed\r\n");
    }

    g_ntmini_initialized = TRUE;
    memlog("DONE\r\n");
    VxD_Debug_Printf("V5: DONE\r\n");
    return 1;  /* Always stay loaded so memlog can be read */
}

void _ntmini_cleanup(void) {
    VxD_Debug_Printf("V5: unloaded\r\n");
}

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
#pragma pack(pop)

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

/* IOP (I/O Packet) - simplified, key fields only */
typedef struct _IOP {
    USHORT IOP_func;         /* 0x00 function code */
    USHORT IOP_result;       /* 0x02 result */
    ULONG  IOP_flags;        /* 0x04 flags */
    PVOID  IOP_callback;     /* 0x08 callback function */
    ULONG  IOP_callback_ref; /* 0x0C callback reference data */
    PVOID  IOP_original_dcb; /* 0x10 original DCB */
    UCHAR  IOP_reserved[12]; /* 0x14 */
    ULONG  IOP_ior_flags;    /* 0x20 IOR flags */
    ULONG  IOP_ior_start_addr[2]; /* 0x24 start sector (low, high) */
    PVOID  IOP_ior_buffer;   /* 0x2C transfer buffer */
    ULONG  IOP_ior_xfer_count; /* 0x30 bytes to transfer */
    ULONG  IOP_ior_status;   /* 0x34 status */
    /* More fields follow */
} IOP;

/* IOS DDB class and flags (from W9XDDK.H) */
#define DDB_CLASS_PORT       0x0001
#define DDB_MERIT_PORT       0x10000000

static IOS_DDB g_ios_ddb;
static IOS_DCB g_ios_dcb;
static IOS_CALLDOWN g_ios_calldown;
static BOOLEAN g_ios_registered = FALSE;

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
 * We translate IOR_READ to SCSI READ(10) SRBs via the miniport.
 * ================================================================ */

/* IOR function codes */
#define IOR_READ_FC          0x00
#define IOR_WRITE_FC         0x01
#define IOR_VERIFY_FC        0x02
#define IOR_MEDIA_CHECK_FC   0x05
#define IOR_SCSI_PASS_FC     0x0F

/* IOR status codes */
#define IORS_SUCCESS_FC      0x00
#define IORS_INVALID_CMD_FC  0x16
#define IORS_NOT_READY_FC    0x20

static int ior_log_count = 0;

void _ntmini_ior_handler(PVOID ior_ptr)
{
    UCHAR *ior = (UCHAR *)ior_ptr;
    USHORT func;
    ULONG flags, start_lo, xfer_count, buf_ptr;

    if (!ior) return;

    /* Read IOR fields. The IOR might be passed directly or via IOP wrapper.
       Try IOR layout first (func at +0x04). If func looks invalid, try
       alternative offsets. */
    func = *(USHORT *)(ior + 0x04);
    flags = *(ULONG *)(ior + 0x08);
    start_lo = *(ULONG *)(ior + 0x10);
    xfer_count = *(ULONG *)(ior + 0x18);
    buf_ptr = *(ULONG *)(ior + 0x1C);

    if (ior_log_count < 20) {
        log_hex("IOR: func=", (ULONG)func, "");
        log_hex(" start=", start_lo, "");
        log_hex(" xfer=", xfer_count, "");
        log_hex(" buf=", buf_ptr, "\r\n");
        ior_log_count++;
    }

    switch (func) {
    case IOR_READ_FC: {
        /* Translate to SCSI READ(10) via miniport */
        ULONG sector = start_lo;
        ULONG bytes = xfer_count;
        ULONG sectors_to_read = (bytes + 2047) / 2048;
        UCHAR *dest = (UCHAR *)buf_ptr;
        ULONG i;
        BOOLEAN ok = TRUE;

        if (!g_state.HwStartIo || !g_state.DeviceExtension || !dest) {
            *(USHORT *)(ior + 0x06) = IORS_NOT_READY_FC;
            break;
        }

        if (ior_log_count < 25) {
            log_hex("IOR: READ sector=", sector, "");
            log_hex(" count=", sectors_to_read, "\r\n");
        }

        for (i = 0; i < sectors_to_read && ok; i++) {
            SCSI_REQUEST_BLOCK srb;
            ULONG s = sector + i;

            my_memset(&srb, 0, sizeof(srb));
            srb.Length = sizeof(srb);
            srb.Function = 0x00;    /* SRB_FUNCTION_EXECUTE_SCSI */
            srb.PathId = 0;
            srb.TargetId = 0;
            srb.Lun = 0;
            srb.CdbLength = 10;
            srb.DataTransferLength = 2048;
            srb.DataBuffer = dest + (i * 2048);
            srb.SrbFlags = 0x08;    /* SRB_FLAGS_DATA_IN */

            /* READ(10) CDB */
            srb.Cdb[0] = 0x28;     /* READ(10) */
            srb.Cdb[2] = (UCHAR)(s >> 24);
            srb.Cdb[3] = (UCHAR)(s >> 16);
            srb.Cdb[4] = (UCHAR)(s >> 8);
            srb.Cdb[5] = (UCHAR)(s);
            srb.Cdb[7] = 0;
            srb.Cdb[8] = 1;        /* 1 sector */

            /* Clear CurrentSrb in device extension */
            *(ULONG *)g_state.DeviceExtension = 0;

            g_SrbCompleted = FALSE;
            {
                typedef BOOLEAN (__stdcall *PFN_HWSTARTIO)(PVOID, PVOID);
                PFN_HWSTARTIO fn = (PFN_HWSTARTIO)g_state.HwStartIo;
                fn(g_state.DeviceExtension, &srb);
            }

            /* Poll for completion */
            {
                ULONG timeout;
                typedef BOOLEAN (__stdcall *PFN_HWINT)(PVOID);
                PFN_HWINT hwint = (PFN_HWINT)g_state.HwInterrupt;
                for (timeout = 0; timeout < 500000 && !g_SrbCompleted; timeout++) {
                    if (hwint) hwint(g_state.DeviceExtension);
                }
            }

            if ((srb.SrbStatus & 0x3F) != 0x01) {  /* not SRB_STATUS_SUCCESS */
                if (ior_log_count < 30) {
                    log_hex("IOR: READ fail s=", s, "");
                    log_hex(" status=", (ULONG)srb.SrbStatus, "\r\n");
                }
                ok = FALSE;
            }
        }

        *(USHORT *)(ior + 0x06) = ok ? IORS_SUCCESS_FC : IORS_NOT_READY_FC;
        break;
    }

    case IOR_MEDIA_CHECK_FC:
        /* Media is present (CD-ROM inserted) */
        *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        break;

    case IOR_VERIFY_FC:
        /* Verify: just succeed */
        *(USHORT *)(ior + 0x06) = IORS_SUCCESS_FC;
        break;

    default:
        /* Unknown command: return invalid command but don't crash */
        if (ior_log_count < 30) {
            log_hex("IOR: unknown func=", (ULONG)func, "\r\n");
        }
        *(USHORT *)(ior + 0x06) = IORS_INVALID_CMD_FC;
        break;
    }

    /* Signal completion to IOS */
    IOS_BD_Complete(ior_ptr);
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

/* Read sectors from CD-ROM via miniport HwStartIo */
static int mscdex_read_sectors(ULONG start_sector, ULONG num_sectors,
                                UCHAR *buffer)
{
    SCSI_REQUEST_BLOCK srb;
    ULONG i;

    for (i = 0; i < num_sectors; i++) {
        ULONG sector = start_sector + i;
        my_memset(&srb, 0, sizeof(srb));
        srb.Length = sizeof(srb);
        srb.Function = 0x00;    /* SRB_FUNCTION_EXECUTE_SCSI */
        srb.PathId = 0;
        srb.TargetId = 0;
        srb.Lun = 0;
        srb.CdbLength = 10;
        srb.DataTransferLength = 2048;
        srb.DataBuffer = buffer + (i * 2048);
        srb.SrbStatus = 0;

        /* READ(10) CDB */
        srb.Cdb[0] = 0x28;     /* READ(10) */
        srb.Cdb[2] = (UCHAR)(sector >> 24);
        srb.Cdb[3] = (UCHAR)(sector >> 16);
        srb.Cdb[4] = (UCHAR)(sector >> 8);
        srb.Cdb[5] = (UCHAR)(sector);
        srb.Cdb[7] = 0;        /* transfer length high */
        srb.Cdb[8] = 1;        /* transfer length low = 1 sector */

        /* Clear CurrentSrb in device extension */
        if (g_state.DeviceExtension) {
            *(ULONG *)g_state.DeviceExtension = 0;
        }

        /* Call HwStartIo */
        if (!g_state.HwStartIo) return -1;

        g_SrbCompleted = FALSE;
        {
            typedef BOOLEAN (__stdcall *PFN_HWSTARTIO)(PVOID, PVOID);
            PFN_HWSTARTIO fn = (PFN_HWSTARTIO)g_state.HwStartIo;
            fn(g_state.DeviceExtension, &srb);
        }

        /* Poll for completion */
        {
            ULONG timeout;
            typedef BOOLEAN (__stdcall *PFN_HWINT)(PVOID);
            PFN_HWINT hwint = (PFN_HWINT)g_state.HwInterrupt;
            for (timeout = 0; timeout < 500000 && !g_SrbCompleted; timeout++) {
                if (hwint) hwint(g_state.DeviceExtension);
            }
        }

        if ((srb.SrbStatus & 0x3F) != 0x01) {  /* not SRB_STATUS_SUCCESS */
            log_hex("MSCDEX: read failed sector ", sector, "");
            log_hex(" status=", (ULONG)srb.SrbStatus, "\r\n");
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
    USHORT func = aep->AEP_func;

    switch (func) {
    case AEP_INITIALIZE: {
        AEP_BI_INIT *bi = (AEP_BI_INIT *)aep;
        ULONG ilb_ptr = bi->AEP_bi_reference;

        VxD_Debug_Printf("IOS: AEP_INITIALIZE!\r\n");
        log_hex("  sizeof(AEP_HDR)=", (ULONG)sizeof(AEP_HEADER), "");
        log_hex("  sizeof(AEP_BI)=", (ULONG)sizeof(AEP_BI_INIT), "\r\n");

        /* Dump raw AEP bytes to verify packed layout.
           Expected: ILB ptr at +0x09 (packed after 9-byte header). */
        {
            UCHAR *raw = (UCHAR *)aep;
            ULONG off;
            for (off = 0; off < 32; off += 8) {
                ULONG i;
                char line[64];
                char *p = line;
                for (i = 0; i < 8; i++) {
                    static const char hx[]="0123456789ABCDEF";
                    *p++ = hx[raw[off+i] >> 4];
                    *p++ = hx[raw[off+i] & 0xF];
                    *p++ = ' ';
                }
                *p++ = '\r'; *p++ = '\n'; *p = 0;
                log_hex("  AEP+", off, ": ");
                VxD_Debug_Printf(line);
            }
        }

        log_hex("  ILB ptr = ", ilb_ptr, "\r\n");
        log_hex("  max_target = ", (ULONG)bi->AEP_bi_max_target, "");
        log_hex("  max_lun = ", (ULONG)bi->AEP_bi_max_lun, "\r\n");
        log_hex("  dcb = ", bi->AEP_bi_dcb, "");
        log_hex("  hdevnode = ", bi->AEP_bi_hdevnode, "\r\n");

        if (ilb_ptr != 0) {
            g_ilb = (IOS_ILB *)ilb_ptr;
            VxD_Debug_Printf("  GOT ILB!\r\n");
            log_hex("    ILB_service_rtn = ", g_ilb->ILB_service_rtn, "\r\n");
            log_hex("    ILB_dprintf_rtn = ", g_ilb->ILB_dprintf_rtn, "\r\n");
            log_hex("    ILB_enqueue_iop = ", g_ilb->ILB_enqueue_iop, "\r\n");
        } else {
            VxD_Debug_Printf("  No ILB (ptr=0)\r\n");
        }

        aep->AEP_result = AEP_SUCCESS;
        break;
    }

    case AEP_CONFIG_DCB: {
        AEP_CD_CONFIG *cd = (AEP_CD_CONFIG *)aep;
        VxD_Debug_Printf("IOS: AEP_CONFIG_DCB!\r\n");
        log_hex("  dcb = ", cd->AEP_cd_dcb, "\r\n");
        /* TODO: Insert our calldown handler into this DCB's chain */
        aep->AEP_result = AEP_SUCCESS;
        break;
    }

    case AEP_BOOT_COMPLETE:
        VxD_Debug_Printf("IOS: AEP_BOOT_COMPLETE\r\n");
        aep->AEP_result = AEP_SUCCESS;
        break;

    case AEP_UNINITIALIZE:
        VxD_Debug_Printf("IOS: AEP_UNINITIALIZE\r\n");
        aep->AEP_result = AEP_SUCCESS;
        break;

    case 14: /* AEP_SYSTEM_SHUTDOWN */
        aep->AEP_result = AEP_SUCCESS;
        break;

    default:
        /* Don't log every unknown AEP (func=0x8C floods the log) */
        aep->AEP_result = AEP_SUCCESS;
        break;
    }
}

/* ================================================================
 * IOS Registration - called during Init_Complete
 * ================================================================ */
void _ntmini_ios_init(void)
{
    VxD_Debug_Printf("IOS: Registering with IOS (ordinal 7)...\r\n");

    /* Only register if miniport initialization succeeded */
    if (!g_state.HwStartIo) {
        VxD_Debug_Printf("IOS: No miniport, skipping IOS registration\r\n");
        return;
    }

    /* Check if already registered using eyecatcher */
    if (g_ios_ddb.DRP_eyecatch_str[0] == 'X') {
        VxD_Debug_Printf("IOS: Already registered (eyecatcher valid), skipping\r\n");
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

    /* LGN: ESDI_506.PDR hex dump shows DRP_LGN = 0x00400000 = DRP_ESDI_PD.
       Previous attempts with 0, 0x16 (bit number not mask!), 0x80000 all failed
       because the struct was also mis-packed (padding shifted all fields after +0x24).
       Now with packed struct + correct LGN, IOS should assign us to the ESDI port
       driver layer and provide an ILB. */
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

        if (dcb_head >= 0xC0000000 && dcb_head < 0xD0000000) {
            UCHAR *dcb = (UCHAR *)dcb_head;
            int iter;
            for (iter = 0; iter < 20 && dcb != (UCHAR *)0; iter++) {
                ULONG ddb_val = *(ULONG *)(dcb + 0x0C); /* DCB_ddb */
                ULONG next_dcb = *(ULONG *)(dcb + 0x04); /* DCB_next */
                log_hex("  DCB ", (ULONG)dcb, "");
                log_hex(" DDB=", ddb_val, "");

                /* Check if DDB has DRP eyecatcher */
                if (ddb_val >= 0xC0000000 && ddb_val < 0xD0000000) {
                    UCHAR *ddb_ptr = (UCHAR *)ddb_val;
                    if (ddb_ptr[0] == 'X' && ddb_ptr[1] == 'X' &&
                        ddb_ptr[2] == 'X' && ddb_ptr[3] == 'X') {
                        ULONG ilb_val = *(ULONG *)(ddb_ptr + 0x10);
                        VxD_Debug_Printf(" name=");
                        {   /* Print first 8 chars of name safely */
                            char nbuf[9];
                            int j;
                            for (j = 0; j < 8; j++) nbuf[j] = ddb_ptr[0x14+j];
                            nbuf[8] = 0;
                            VxD_Debug_Printf(nbuf);
                        }
                        log_hex(" ILB=", ilb_val, "\r\n");

                        if (ilb_val >= 0xC0000000 && ilb_val < 0xD0000000) {
                            IOS_ILB *found_ilb = (IOS_ILB *)ilb_val;
                            if (found_ilb->ILB_service_rtn >= 0xC0000000) {
                                VxD_Debug_Printf("IOS: FOUND VALID ILB!\r\n");
                                log_hex("  ILB_service_rtn=", found_ilb->ILB_service_rtn, "\r\n");
                                g_ilb = found_ilb;
                                g_ios_ddb.DRP_ilb = ilb_val;
                                break;
                            }
                        }
                    } else {
                        VxD_Debug_Printf(" (no eyecatcher)\r\n");
                    }
                } else {
                    VxD_Debug_Printf(" (bad ptr)\r\n");
                }

                /* Follow DCB_next chain */
                if (next_dcb >= 0xC0000000 && next_dcb < 0xD0000000)
                    dcb = (UCHAR *)next_dcb;
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
            /* IOS created a DCB for us! Fill it with CD-ROM info. */
            IOS_DCB *dcb = (IOS_DCB *)isp_dcb.dcb_ptr;
            VxD_Debug_Printf("IOS: GOT IOS-MANAGED DCB!\r\n");

            dcb->DCB_device_type = DCB_TYPE_CDROM;
            dcb->DCB_bus_type = DCB_BUS_ESDI;
            dcb->DCB_bus_number = 1;
            dcb->DCB_target_id = 0;
            dcb->DCB_lun = 0;
            dcb->DCB_dmd_flags = DCB_DEV_PHYSICAL | DCB_DEV_REMOVABLE |
                                  DCB_DEV_UNCERTAIN_MEDIA;
            dcb->DCB_apparent_blk_shift = 11;  /* log2(2048) */
            dcb->DCB_apparent_blk_size = 2048;
            dcb->DCB_max_xfer_len = 64 * 1024;
            dcb->DCB_ddb = (PVOID)&g_ios_ddb;

            /* Set vendor/product strings */
            {
                const char *vid = "QEMU    ";
                const char *pid = "CD-ROM  ";
                int i;
                for (i = 0; i < 8 && vid[i]; i++) dcb->DCB_vendor_id[i] = vid[i];
                for (i = 0; i < 16 && pid[i]; i++) dcb->DCB_product_id[i] = pid[i];
            }

            log_hex("IOS: DCB created at ", isp_dcb.dcb_ptr, "\r\n");

            /* Now insert our calldown handler via ISP_INSERT_CALLDOWN */
            {
                g_ios_calldown.CD_func = (PVOID)ios_ior_bridge;
                g_ios_calldown.CD_ddb = (PVOID)&g_ios_ddb;
                g_ios_calldown.CD_next = (IOS_CALLDOWN *)0;
                g_ios_calldown.CD_flags = 0;

                /* ISP_INSERT_CALLDOWN via ILB */
                {
                    ISP_INSERT_CD_PKT isp_cd;
                    my_memset(&isp_cd, 0, sizeof(isp_cd));
                    isp_cd.func = 5;  /* ISP_INSERT_CALLDOWN */
                    isp_cd.dcb = isp_dcb.dcb_ptr;
                    isp_cd.req = (ULONG)ios_ior_bridge;
                    isp_cd.ddb = (ULONG)&g_ios_ddb;
                    isp_cd.flags = 0;
                    isp_cd.lgn = 0x16; /* DRP_ESDI_PD_BIT */

                    VxD_Debug_Printf("IOS: Inserting calldown...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_cd);
                    log_hex("IOS: ISP_INSERT_CALLDOWN result=", (ULONG)isp_cd.result, "\r\n");
                }

                /* Broadcast AEP_CONFIG_DCB so higher-layer drivers (CDTSD,
                   CDVSD, CDFS, voltrack) insert their calldowns into our
                   DCB's chain. Without this, the file system layer can't
                   route I/O to our DCB. */
                {
                    AEP_HEADER aep_cfg;
                    ISP_BCAST_AEP_PKT isp_bcast;

                    /* Build AEP_CONFIG_DCB packet */
                    my_memset(&aep_cfg, 0, sizeof(aep_cfg));
                    aep_cfg.AEP_func = AEP_CONFIG_DCB;
                    aep_cfg.AEP_result = 0;

                    /* AEP_CONFIG_DCB extended: DCB pointer at +0x0C */
                    {
                        ULONG *ext = (ULONG *)((UCHAR *)&aep_cfg + sizeof(AEP_HEADER));
                        /* We need room for the extended field, use a larger buffer */
                    }

                    /* Actually, AEP_dcb_config has hdr + dcb ptr. Use a raw buffer. */
                    {
                        UCHAR aep_buf[16];
                        my_memset(aep_buf, 0, sizeof(aep_buf));
                        *(USHORT *)(aep_buf + 0) = AEP_CONFIG_DCB;
                        *(USHORT *)(aep_buf + 2) = 0;     /* AEP_result */
                        *(ULONG *)(aep_buf + 4) = 0;      /* AEP_ddb */
                        *(UCHAR *)(aep_buf + 8) = 0x16;   /* AEP_lgn */
                        /* +0x09: padding zeros */
                        *(ULONG *)(aep_buf + 0x0C) = isp_dcb.dcb_ptr; /* DCB */

                        my_memset(&isp_bcast, 0, sizeof(isp_bcast));
                        isp_bcast.func = 20;  /* ISP_BROADCAST_AEP */
                        isp_bcast.paep = (ULONG)aep_buf;

                        VxD_Debug_Printf("IOS: Broadcasting AEP_CONFIG_DCB...\r\n");
                        Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_bcast);
                        log_hex("IOS: BROADCAST result=", (ULONG)isp_bcast.result, "\r\n");
                    }
                }

                /* Associate DCB with drive letter D: (index 3) */
                {
                    ISP_ASSOC_DCB_PKT isp_assoc;
                    my_memset(&isp_assoc, 0, sizeof(isp_assoc));
                    isp_assoc.func = 6;  /* ISP_ASSOCIATE_DCB */
                    isp_assoc.dcb = isp_dcb.dcb_ptr;
                    isp_assoc.drive = 3; /* D: */
                    isp_assoc.flags = 0;

                    VxD_Debug_Printf("IOS: Associating DCB with drive D:...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_assoc);
                    log_hex("IOS: ISP_ASSOCIATE_DCB result=", (ULONG)isp_assoc.result, "\r\n");
                }

                /* ISP_DEVICE_ARRIVED: Tell IOS a new device has appeared.
                   This triggers full device enumeration, causing CDTSD,
                   CDVSD, voltrack, and CDFS to discover our DCB and insert
                   their calldown layers. Without this, the upper layers
                   never learn about our device and IOR callbacks never fire. */
                {
                    ISP_DEV_ARRIVED_PKT isp_arr;
                    my_memset(&isp_arr, 0, sizeof(isp_arr));
                    isp_arr.func = ISP_DEVICE_ARRIVED_FUNC;
                    isp_arr.dcb = isp_dcb.dcb_ptr;
                    isp_arr.flags = 0;

                    VxD_Debug_Printf("IOS: Sending ISP_DEVICE_ARRIVED...\r\n");
                    Call_ILB_Service(g_ilb->ILB_service_rtn, &isp_arr);
                    log_hex("IOS: ISP_DEVICE_ARRIVED result=", (ULONG)isp_arr.result, "\r\n");
                }

                /* Dump the DCB calldown chain to see who inserted layers.
                   If CDTSD/CDVSD/voltrack responded to DEVICE_ARRIVED,
                   there should be multiple entries beyond our own. */
                {
                    IOS_DCB *dcb = (IOS_DCB *)isp_dcb.dcb_ptr;
                    IOS_CALLDOWN *cd = (IOS_CALLDOWN *)dcb->DCB_cd;
                    int depth = 0;

                    VxD_Debug_Printf("IOS: === DCB Calldown Chain ===\r\n");
                    log_hex("IOS: DCB_cd head=", (ULONG)dcb->DCB_cd, "\r\n");
                    log_hex("IOS: DCB_device_type=", (ULONG)dcb->DCB_device_type, "");
                    log_hex(" DCB_dmd_flags=", dcb->DCB_dmd_flags, "\r\n");

                    while (cd && depth < 15) {
                        log_hex("IOS: CD[", (ULONG)depth, "]: ");
                        log_hex("func=", (ULONG)cd->CD_func, " ");
                        log_hex("ddb=", (ULONG)cd->CD_ddb, " ");
                        log_hex("flags=", cd->CD_flags, " ");
                        log_hex("next=", (ULONG)cd->CD_next, "\r\n");
                        cd = cd->CD_next;
                        depth++;
                    }
                    if (depth == 0) {
                        VxD_Debug_Printf("IOS: WARNING: Calldown chain is EMPTY!\r\n");
                    }
                    log_hex("IOS: Calldown depth=", (ULONG)depth, "\r\n");
                    VxD_Debug_Printf("IOS: === End Calldown Chain ===\r\n");
                }
            }
        } else {
            VxD_Debug_Printf("IOS: ISP_CREATE_DCB failed\r\n");
        }
    } else {
        VxD_Debug_Printf("IOS: No ILB, using local DCB\r\n");
    }

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

    /* Use CONFIGMG to trigger DevLoader processing for our device node.
       Registry entries (Enum\Root\SCSIAdapter\0000 with DevLoader=*IOS,
       PortDriver=NTMINI.PDR) were imported via AUTOEXEC.BAT + REGEDIT.
       CM_Setup_DevNode should make Config Manager process the DevLoader. */
    {
        extern ULONG CM_Locate_DevNode_Wrapper(ULONG *pdevnode, char *id, ULONG flags);
        extern ULONG CM_Setup_DevNode_Wrapper(ULONG devnode, ULONG flags);
        extern ULONG CM_Reenumerate_DevNode_Wrapper(ULONG devnode, ULONG flags);

        ULONG devnode = 0;
        ULONG cr;
        char dev_id[] = "Root\\SCSIAdapter\\0000";

        VxD_Debug_Printf("CM: Locating devnode Root\\SCSIAdapter\\0000...\r\n");
        cr = CM_Locate_DevNode_Wrapper(&devnode, dev_id, 0);
        log_hex("CM: CM_Locate_DevNode returned ", cr, "");
        log_hex(" devnode=", devnode, "\r\n");

        if (cr == 0 && devnode != 0) {
            /* Try calling CONFIGMG ordinal 0x0033002A with devnode,0
               which should be CM_Setup_DevNode in the standard DDK.
               Also try the IOS_Call_Service_N trick to probe ordinals. */
            extern ULONG IOS_Call_Service_N(ULONG ordinal, PVOID param);
            ULONG ord;

            /* Probe CONFIGMG ordinals 0x24-0x2C with (devnode, 0) on stack.
               We use the self-modifying IOS_Call_Service_N but change
               the device ID from 0x0010 to 0x0033. */
            VxD_Debug_Printf("CM: Probing ordinals with devnode...\r\n");
            /* Actually, we can't use IOS_Call_Service_N for CONFIGMG (wrong device).
               Let me just call a few key ordinals directly. */

            /* The key question: what CONFIGMG ordinal triggers DevLoader?
               Win98 DDK CONFIGMG ordinals (from various sources):
                 0x0A = CM_Get_DevNode_Status
                 0x19 = CM_Set_DevNode_Problem
                 0x1D = CM_Query_Remove_SubTree
                 0x21 = CM_Set_DevNode_PowerState
                 0x25 = CM_CallBack_Device_Loader
                 0x2A = CM_Setup_DevNode
                 0x2B = CM_Reenumerate_DevNode
               The ordinal numbering varies by Win98 version. */

            /* Just dump the devnode address and try ordinal 0x0A for status */
            log_hex("CM: DevNode at ", devnode, "\r\n");

            {
                extern ULONG CM_Get_DevNode_Status_Wrapper(ULONG *ps, ULONG *pp, ULONG dn, ULONG f);
                extern ULONG CM_Register_Device_Driver_Wrapper(ULONG dn, PVOID handler, ULONG ref, ULONG f);
                extern ULONG CM_Setup_DevNode_Wrapper(ULONG dn, ULONG f);
                ULONG status = 0, problem = 0;

                cr = CM_Get_DevNode_Status_Wrapper(&status, &problem, devnode, 0);
                log_hex("CM: Status=", status, "");
                log_hex(" Problem=", problem, "\r\n");

                /* Register ourselves as the device driver for this devnode.
                   This should change status from NOT_CONFIGURED to DRIVER_LOADED.
                   Handler=NULL, refdata=0 (we handle I/O via IOS, not CM callbacks). */
                VxD_Debug_Printf("CM: CM_Register_Device_Driver...\r\n");
                cr = CM_Register_Device_Driver_Wrapper(devnode, (PVOID)0, 0, 0);
                log_hex("CM: Register_Device_Driver returned ", cr, "\r\n");

                /* Check status after registration */
                cr = CM_Get_DevNode_Status_Wrapper(&status, &problem, devnode, 0);
                log_hex("CM: Post-reg Status=", status, "");
                log_hex(" Problem=", problem, "\r\n");

                /* Try various CM_Setup_DevNode flags */
                VxD_Debug_Printf("CM: Setup(PROP_CHANGE=3)...\r\n");
                cr = CM_Setup_DevNode_Wrapper(devnode, 3);
                log_hex("CM: returned ", cr, "\r\n");

                cr = CM_Get_DevNode_Status_Wrapper(&status, &problem, devnode, 0);
                log_hex("CM: Status=", status, " Problem=");
                log_hex("", problem, "\r\n");

                /* Try CM_Setup_DevNode(DOWNLOAD=1) */
                VxD_Debug_Printf("CM: Setup(DOWNLOAD=1)...\r\n");
                cr = CM_Setup_DevNode_Wrapper(devnode, 1);
                log_hex("CM: returned ", cr, "\r\n");

                cr = CM_Get_DevNode_Status_Wrapper(&status, &problem, devnode, 0);
                log_hex("CM: Status=", status, " Problem=");
                log_hex("", problem, "\r\n");
            }
        } else {
            VxD_Debug_Printf("CM: DevNode NOT found - registry entries missing?\r\n");
        }
    }
}
