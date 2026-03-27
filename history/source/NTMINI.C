/*
 * NTMINI.C - NT ScsiPort Miniport Shim for Windows 9x
 *
 * Loads an unmodified Windows NT4 SCSI miniport driver (.sys) on
 * Windows 98 by providing a compatibility layer between the NT
 * ScsiPort API and the Win9x IOS storage architecture.
 *
 * No known prior implementation exists in the public record. If it works, any NT4 SCSI
 * miniport driver can run on Win9x without modification.
 *
 * Architecture:
 *
 *   Win9x IOS (I/O Supervisor)
 *        |
 *   [NTMINI.PDR] <-- this file (IOS Port Driver)
 *        |
 *   [ScsiPort Shim] -- 22 ScsiPortXxx function stubs
 *        |
 *   [PE Loader] ----- loads NT4 .sys into ring 0 memory
 *        |
 *   [NT4 atapi.sys] - unmodified miniport binary
 *        |
 *   IDE/ATAPI Hardware
 *
 * BUILD: Requires Windows 98 DDK or Open Watcom C.
 *        See BUILD.TXT for instructions.
 *
 * AUTHOR: Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License.
 */

/* ================================================================
 * NOTE: This is a SKELETON with detailed pseudocode. Sections
 * marked [IMPL] have real logic. Sections marked [STUB] need
 * Win98 DDK APIs that can't be verified without the DDK headers.
 * The intent is to be compilable with minimal changes once the
 * DDK is available.
 * ================================================================ */

#include <stddef.h>
#include "PORTIO.H"

/* === Type definitions (NT-compatible, for miniport interface) === */

typedef unsigned char   UCHAR;
typedef unsigned short  USHORT;
typedef unsigned long   ULONG;
typedef signed long     LONG;
typedef int             BOOLEAN;
typedef void            VOID;
typedef void*           PVOID;
typedef UCHAR*          PUCHAR;
typedef USHORT*         PUSHORT;
typedef ULONG*          PULONG;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

#define STATUS_SUCCESS          0x00000000
#define STATUS_UNSUCCESSFUL     0xC0000001

/* Physical address (NT uses LARGE_INTEGER, but miniports only
   use the low 32 bits on x86) */
typedef union {
    struct {
        ULONG LowPart;
        LONG  HighPart;
    };
    long long QuadPart;
} SCSI_PHYSICAL_ADDRESS;

/* === NT ScsiPort structures (must match NT4 DDK exactly) === */

/* SRB Function codes */
#define SRB_FUNCTION_EXECUTE_SCSI   0x00
#define SRB_FUNCTION_ABORT_COMMAND  0x10
#define SRB_FUNCTION_IO_CONTROL     0x02
#define SRB_FUNCTION_RESET_BUS      0x12
#define SRB_FUNCTION_RESET_DEVICE   0x13
#define SRB_FUNCTION_FLUSH          0x08
#define SRB_FUNCTION_SHUTDOWN       0x07

/* SRB Status codes */
#define SRB_STATUS_PENDING          0x00
#define SRB_STATUS_SUCCESS          0x01
#define SRB_STATUS_ERROR            0x04
#define SRB_STATUS_INVALID_REQUEST  0x06
#define SRB_STATUS_NO_DEVICE        0x08
#define SRB_STATUS_TIMEOUT          0x09
#define SRB_STATUS_BUS_RESET        0x0E

/* ScsiPortNotification types */
#define RequestComplete     0
#define NextRequest         1
#define NextLuRequest       2
#define ResetDetected       3
#define CallDisableInterrupts  4
#define CallEnableInterrupts   5
#define RequestTimerCall    6
#define BusChangeDetected   7

/* ACCESS_RANGE: describes an I/O or memory resource */
typedef struct _ACCESS_RANGE {
    SCSI_PHYSICAL_ADDRESS RangeStart;
    ULONG                 RangeLength;
    BOOLEAN               RangeInMemory;
} ACCESS_RANGE, *PACCESS_RANGE;

/* PORT_CONFIGURATION_INFORMATION */
typedef struct _PORT_CONFIGURATION_INFORMATION {
    ULONG   Length;
    ULONG   SystemIoBusNumber;
    ULONG   AdapterInterfaceType; /* Isa=1, Eisa=2, PCI=5 */
    ULONG   BusInterruptLevel;
    ULONG   BusInterruptVector;
    ULONG   InterruptMode;       /* LevelSensitive=0, Latched=1 */
    ULONG   MaximumTransferLength;
    ULONG   NumberOfPhysicalBreaks;
    ULONG   DmaChannel;
    ULONG   DmaPort;
    ULONG   DmaWidth;            /* Width8Bits=0, etc. */
    ULONG   DmaSpeed;
    ULONG   AlignmentMask;
    ULONG   NumberOfAccessRanges;
    ACCESS_RANGE (*AccessRanges)[];
    PVOID   Reserved;
    UCHAR   NumberOfBuses;
    UCHAR   InitiatorBusId[8];
    BOOLEAN ScatterGather;
    BOOLEAN Master;
    BOOLEAN CachesData;
    BOOLEAN AdapterScansDown;
    BOOLEAN AtdiskPrimaryClaimed;
    BOOLEAN AtdiskSecondaryClaimed;
    BOOLEAN Dma32BitAddresses;
    BOOLEAN DemandMode;
    BOOLEAN MapBuffers;
    BOOLEAN NeedPhysicalAddresses;
    BOOLEAN TaggedQueuing;
    BOOLEAN AutoRequestSense;
    BOOLEAN MultipleRequestPerLu;
    BOOLEAN ReceiveEvent;
    BOOLEAN RealModeInitialized;
    BOOLEAN BufferAccessScsiPortControlled;
    UCHAR   MaximumNumberOfTargets;
    UCHAR   ReservedUchars[2];
    ULONG   SlotNumber;
    ULONG   BusInterruptLevel2;
    ULONG   BusInterruptVector2;
    ULONG   InterruptMode2;
    ULONG   DmaChannel2;
    ULONG   DmaPort2;
    ULONG   DmaWidth2;
    ULONG   DmaSpeed2;
    ULONG   DeviceExtensionSize;
    ULONG   SpecificLuExtensionSize;
    ULONG   SrbExtensionSize;
} PORT_CONFIGURATION_INFORMATION, *PPORT_CONFIGURATION_INFORMATION;

/* SCSI_REQUEST_BLOCK (simplified for IDE miniport use) */
typedef struct _SCSI_REQUEST_BLOCK {
    USHORT  Length;
    UCHAR   Function;
    UCHAR   SrbStatus;
    UCHAR   ScsiStatus;
    UCHAR   PathId;
    UCHAR   TargetId;
    UCHAR   Lun;
    UCHAR   QueueTag;
    UCHAR   QueueAction;
    UCHAR   CdbLength;
    UCHAR   SenseInfoBufferLength;
    ULONG   SrbFlags;
    ULONG   DataTransferLength;
    ULONG   TimeOutValue;
    PVOID   DataBuffer;
    PVOID   SenseInfoBuffer;
    struct _SCSI_REQUEST_BLOCK *NextSrb;
    PVOID   OriginalRequest;
    PVOID   SrbExtension;
    ULONG   InternalStatus;
    UCHAR   Cdb[16];
} SCSI_REQUEST_BLOCK, *PSCSI_REQUEST_BLOCK;

/* HW_INITIALIZATION_DATA: miniport fills this in DriverEntry */
typedef struct _HW_INITIALIZATION_DATA {
    ULONG   HwInitializationDataSize;
    ULONG   AdapterInterfaceType;
    BOOLEAN (*HwInitialize)(PVOID);
    BOOLEAN (*HwStartIo)(PVOID, PSCSI_REQUEST_BLOCK);
    BOOLEAN (*HwInterrupt)(PVOID);
    PVOID   HwFindAdapter;  /* complex signature, cast as needed */
    BOOLEAN (*HwResetBus)(PVOID, ULONG);
    PVOID   HwDmaStarted;
    PVOID   HwAdapterState;
    ULONG   DeviceExtensionSize;
    ULONG   SpecificLuExtensionSize;
    ULONG   SrbExtensionSize;
    ULONG   NumberOfAccessRanges;
    PVOID   Reserved;
    BOOLEAN MapBuffers;
    BOOLEAN NeedPhysicalAddresses;
    BOOLEAN TaggedQueuing;
    BOOLEAN AutoRequestSense;
    BOOLEAN MultipleRequestPerLu;
    BOOLEAN ReceiveEvent;
    USHORT  VendorIdLength;
    PVOID   VendorId;
    USHORT  DeviceIdLength;
    PVOID   DeviceId;
    PVOID   HwAdapterControl;
} HW_INITIALIZATION_DATA, *PHW_INITIALIZATION_DATA;


/* ================================================================
 * PART 1: PE LOADER
 * Loads an NT4 .sys file into ring 0 memory and resolves imports
 * against our ScsiPort function table.
 * ================================================================ */

/* PE header structures (minimal, just what we need) */
#pragma pack(push, 1)
typedef struct {
    USHORT Machine;
    USHORT NumberOfSections;
    ULONG  TimeDateStamp;
    ULONG  PointerToSymbolTable;
    ULONG  NumberOfSymbols;
    USHORT SizeOfOptionalHeader;
    USHORT Characteristics;
} PE_FILE_HEADER;

typedef struct {
    char   Name[8];
    ULONG  VirtualSize;
    ULONG  VirtualAddress;
    ULONG  SizeOfRawData;
    ULONG  PointerToRawData;
    ULONG  PointerToRelocations;
    ULONG  PointerToLinenumbers;
    USHORT NumberOfRelocations;
    USHORT NumberOfLinenumbers;
    ULONG  Characteristics;
} PE_SECTION_HEADER;
#pragma pack(pop)

/* ScsiPort function dispatch table */
typedef struct {
    const char *name;
    PVOID       func;
} SCSIPORT_FUNC;

/* Forward declarations of our ScsiPort implementations */
PVOID  NTMINI_ScsiPortGetDeviceBase(PVOID, ULONG, SCSI_PHYSICAL_ADDRESS, ULONG, BOOLEAN);
void   NTMINI_ScsiPortFreeDeviceBase(PVOID, PVOID);
UCHAR  NTMINI_ScsiPortReadPortUchar(PUCHAR);
USHORT NTMINI_ScsiPortReadPortUshort(PUSHORT);
ULONG  NTMINI_ScsiPortReadPortUlong(PULONG);
void   NTMINI_ScsiPortReadPortBufferUshort(PUSHORT, PUSHORT, ULONG);
void   NTMINI_ScsiPortReadPortBufferUlong(PULONG, PULONG, ULONG);
void   NTMINI_ScsiPortWritePortUchar(PUCHAR, UCHAR);
void   NTMINI_ScsiPortWritePortBufferUshort(PUSHORT, PUSHORT, ULONG);
void   NTMINI_ScsiPortWritePortBufferUlong(PULONG, PULONG, ULONG);
void   NTMINI_ScsiPortWritePortUlong(PULONG, ULONG);
void   NTMINI_ScsiPortStallExecution(ULONG);
void   NTMINI_ScsiPortMoveMemory(PVOID, PVOID, ULONG);
SCSI_PHYSICAL_ADDRESS NTMINI_ScsiPortGetPhysicalAddress(PVOID, PSCSI_REQUEST_BLOCK, PVOID, PULONG);
PVOID  NTMINI_ScsiPortGetUncachedExtension(PVOID, PPORT_CONFIGURATION_INFORMATION, ULONG);
void   NTMINI_ScsiPortNotification(ULONG, PVOID, ...);
void   NTMINI_ScsiPortCompleteRequest(PVOID, UCHAR, UCHAR, UCHAR, UCHAR);
void   NTMINI_ScsiPortLogError(PVOID, PSCSI_REQUEST_BLOCK, UCHAR, UCHAR, UCHAR, ULONG, ULONG);
ULONG  NTMINI_ScsiPortInitialize(PVOID, PVOID, PHW_INITIALIZATION_DATA, PVOID);
SCSI_PHYSICAL_ADDRESS NTMINI_ScsiPortConvertUlongToPhysicalAddress(ULONG);
ULONG  NTMINI_ScsiPortGetBusData(PVOID, ULONG, ULONG, ULONG, PVOID, ULONG);
ULONG  NTMINI_ScsiPortSetBusDataByOffset(PVOID, ULONG, ULONG, ULONG, PVOID, ULONG, ULONG);

/* The function table that the PE loader resolves imports against */
static SCSIPORT_FUNC scsiport_functions[] = {
    { "ScsiPortGetDeviceBase",               (PVOID)NTMINI_ScsiPortGetDeviceBase },
    { "ScsiPortFreeDeviceBase",              (PVOID)NTMINI_ScsiPortFreeDeviceBase },
    { "ScsiPortReadPortUchar",               (PVOID)NTMINI_ScsiPortReadPortUchar },
    { "ScsiPortReadPortUshort",              (PVOID)NTMINI_ScsiPortReadPortUshort },
    { "ScsiPortReadPortUlong",               (PVOID)NTMINI_ScsiPortReadPortUlong },
    { "ScsiPortReadPortBufferUshort",        (PVOID)NTMINI_ScsiPortReadPortBufferUshort },
    { "ScsiPortReadPortBufferUlong",         (PVOID)NTMINI_ScsiPortReadPortBufferUlong },
    { "ScsiPortWritePortUchar",              (PVOID)NTMINI_ScsiPortWritePortUchar },
    { "ScsiPortWritePortBufferUshort",       (PVOID)NTMINI_ScsiPortWritePortBufferUshort },
    { "ScsiPortWritePortBufferUlong",        (PVOID)NTMINI_ScsiPortWritePortBufferUlong },
    { "ScsiPortWritePortUlong",              (PVOID)NTMINI_ScsiPortWritePortUlong },
    { "ScsiPortStallExecution",              (PVOID)NTMINI_ScsiPortStallExecution },
    { "ScsiPortMoveMemory",                  (PVOID)NTMINI_ScsiPortMoveMemory },
    { "ScsiPortGetPhysicalAddress",          (PVOID)NTMINI_ScsiPortGetPhysicalAddress },
    { "ScsiPortGetUncachedExtension",        (PVOID)NTMINI_ScsiPortGetUncachedExtension },
    { "ScsiPortNotification",                (PVOID)NTMINI_ScsiPortNotification },
    { "ScsiPortCompleteRequest",             (PVOID)NTMINI_ScsiPortCompleteRequest },
    { "ScsiPortLogError",                    (PVOID)NTMINI_ScsiPortLogError },
    { "ScsiPortInitialize",                  (PVOID)NTMINI_ScsiPortInitialize },
    { "ScsiPortConvertUlongToPhysicalAddress",(PVOID)NTMINI_ScsiPortConvertUlongToPhysicalAddress },
    { "ScsiPortGetBusData",                  (PVOID)NTMINI_ScsiPortGetBusData },
    { "ScsiPortSetBusDataByOffset",          (PVOID)NTMINI_ScsiPortSetBusDataByOffset },
    { NULL, NULL }
};


/* ================================================================
 * GLOBAL STATE
 * ================================================================ */

static struct {
    /* Miniport callbacks (filled by ScsiPortInitialize) */
    BOOLEAN (*HwInitialize)(PVOID);
    BOOLEAN (*HwStartIo)(PVOID, PSCSI_REQUEST_BLOCK);
    BOOLEAN (*HwInterrupt)(PVOID);
    ULONG   (*HwFindAdapter)(PVOID, PVOID, PVOID, PVOID,
                             PPORT_CONFIGURATION_INFORMATION, PUCHAR);
    BOOLEAN (*HwResetBus)(PVOID, ULONG);

    /* Device extension (miniport's private data) */
    PVOID   DeviceExtension;
    ULONG   DeviceExtensionSize;

    /* I/O resources */
    ULONG   IoBase;         /* e.g. 0x170 for secondary IDE */
    ULONG   IoControl;      /* e.g. 0x376 */
    ULONG   IrqLevel;       /* e.g. 15 */

    /* Current SRB being processed */
    PSCSI_REQUEST_BLOCK CurrentSrb;
    BOOLEAN             SrbPending;

    /* PE image */
    PVOID   ImageBase;
    ULONG   ImageSize;
    ULONG   EntryPoint;     /* DriverEntry RVA */

} g_state;


/* ================================================================
 * PART 2: ScsiPort FUNCTION IMPLEMENTATIONS
 * These are what NT4's atapi.sys calls. We translate to Win9x
 * equivalents or implement directly.
 * ================================================================ */

/* --- Port I/O (trivial: just IN/OUT instructions) --- */
/* [IMPL] These are the most critical functions. They're how the
   miniport talks to the IDE hardware. On x86, port I/O is the
   same in ring 0 regardless of OS. */

UCHAR NTMINI_ScsiPortReadPortUchar(PUCHAR Port)
{
    return PORT_IN_BYTE((USHORT)(ULONG)Port);
}

USHORT NTMINI_ScsiPortReadPortUshort(PUSHORT Port)
{
    return PORT_IN_WORD((USHORT)(ULONG)Port);
}

ULONG NTMINI_ScsiPortReadPortUlong(PULONG Port)
{
    return PORT_IN_DWORD((USHORT)(ULONG)Port);
}

void NTMINI_ScsiPortWritePortUchar(PUCHAR Port, UCHAR Value)
{
    PORT_OUT_BYTE((USHORT)(ULONG)Port, Value);
}

void NTMINI_ScsiPortWritePortUlong(PULONG Port, ULONG Value)
{
    PORT_OUT_DWORD((USHORT)(ULONG)Port, Value);
}

void NTMINI_ScsiPortReadPortBufferUshort(PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    PORT_READ_BUFFER_USHORT((USHORT)(ULONG)Port, Buffer, Count);
}

void NTMINI_ScsiPortReadPortBufferUlong(PULONG Port, PULONG Buffer, ULONG Count)
{
    PORT_READ_BUFFER_ULONG((USHORT)(ULONG)Port, Buffer, Count);
}

void NTMINI_ScsiPortWritePortBufferUshort(PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    PORT_WRITE_BUFFER_USHORT((USHORT)(ULONG)Port, Buffer, Count);
}

void NTMINI_ScsiPortWritePortBufferUlong(PULONG Port, PULONG Buffer, ULONG Count)
{
    PORT_WRITE_BUFFER_ULONG((USHORT)(ULONG)Port, Buffer, Count);
}

/* --- Timing --- */

void NTMINI_ScsiPortStallExecution(ULONG Microseconds)
{
    /* [IMPL] Calibrated delay using port 0x80 reads (~1us each on ISA).
       For a VxD, could also use VMM's Call_Global_Event or similar. */
    ULONG i;
    for (i = 0; i < Microseconds; i++) {
        PORT_STALL_ONE();
    }
}

/* --- Memory --- */

void NTMINI_ScsiPortMoveMemory(PVOID Dest, PVOID Src, ULONG Length)
{
    /* [IMPL] Simple memcpy */
    PUCHAR d = (PUCHAR)Dest;
    PUCHAR s = (PUCHAR)Src;
    ULONG i;
    for (i = 0; i < Length; i++)
        d[i] = s[i];
}

/* --- Device base (I/O space mapping) --- */

PVOID NTMINI_ScsiPortGetDeviceBase(
    PVOID HwDeviceExtension,
    ULONG BusType,
    SCSI_PHYSICAL_ADDRESS IoAddress,
    ULONG NumberOfBytes,
    BOOLEAN InIoSpace)
{
    /* [IMPL] For ISA I/O space, the "mapped" address is just the
       port number. No translation needed on x86. */
    if (InIoSpace) {
        return (PVOID)IoAddress.LowPart;
    }
    /* [STUB] For memory-mapped I/O, would need VxD MapPhysToLinear.
       Not needed for IDE/ATAPI (uses port I/O). */
    return NULL;
}

void NTMINI_ScsiPortFreeDeviceBase(PVOID HwDeviceExtension, PVOID MappedAddress)
{
    /* [IMPL] Nothing to free for port I/O */
}

/* --- Physical addresses --- */

SCSI_PHYSICAL_ADDRESS NTMINI_ScsiPortGetPhysicalAddress(
    PVOID HwDeviceExtension,
    PSCSI_REQUEST_BLOCK Srb,
    PVOID VirtualAddress,
    PULONG Length)
{
    /* [STUB] For DMA. In PIO mode (no DMA), this isn't called.
       For a real implementation: VxD CopyPageTable or
       _LinPageLock + _CopyPageTable to get physical address. */
    SCSI_PHYSICAL_ADDRESS pa;
    pa.LowPart = (ULONG)VirtualAddress; /* identity map (wrong but placeholder) */
    pa.HighPart = 0;
    if (Length) *Length = 4096;
    return pa;
}

SCSI_PHYSICAL_ADDRESS NTMINI_ScsiPortConvertUlongToPhysicalAddress(ULONG UlongAddress)
{
    /* [IMPL] Simple type conversion */
    SCSI_PHYSICAL_ADDRESS pa;
    pa.LowPart = UlongAddress;
    pa.HighPart = 0;
    return pa;
}

PVOID NTMINI_ScsiPortGetUncachedExtension(
    PVOID HwDeviceExtension,
    PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    ULONG NumberOfBytes)
{
    /* [STUB] Allocates physically contiguous, non-cached memory for DMA.
       VxD equivalent: _PageAllocate with PageFixed | PageContig.
       Not needed for PIO mode. */
    return NULL;
}

/* --- Completion and notification --- */

void NTMINI_ScsiPortNotification(ULONG NotificationType, PVOID HwDeviceExtension, ...)
{
    /* [IMPL] This is how the miniport signals completion.
       For RequestComplete: mark the current SRB as done.
       For NextRequest: tell us we can send the next SRB.
       Other types can be no-ops for now. */
    switch (NotificationType) {
    case RequestComplete:
        /* The SRB passed as the third variadic arg is complete.
           Signal the IOS bridge to complete the I/O request. */
        g_state.SrbPending = FALSE;
        /* [TODO] Signal IOS completion callback */
        break;

    case NextRequest:
        /* Miniport is ready for another SRB.
           [TODO] Dequeue next IOR from IOS and submit */
        break;

    case NextLuRequest:
        /* Same as NextRequest but per-LU. Treat as NextRequest. */
        break;

    case ResetDetected:
        /* Bus was reset. [TODO] Notify IOS. */
        break;

    case RequestTimerCall:
        /* Miniport wants a timer callback.
           [TODO] Use VMM Set_Global_Time_Out */
        break;

    default:
        break;
    }
}

void NTMINI_ScsiPortCompleteRequest(
    PVOID HwDeviceExtension,
    UCHAR PathId, UCHAR TargetId, UCHAR Lun,
    UCHAR SrbStatus)
{
    /* [STUB] Complete all outstanding SRBs with given status.
       Used during reset. */
    g_state.SrbPending = FALSE;
}

/* --- Error logging --- */

void NTMINI_ScsiPortLogError(
    PVOID HwDeviceExtension,
    PSCSI_REQUEST_BLOCK Srb,
    UCHAR PathId, UCHAR TargetId, UCHAR Lun,
    ULONG ErrorCode, ULONG UniqueId)
{
    /* [STUB] Log error. Could write to debug output.
       For now, silently ignore. */
}

/* --- PCI configuration space --- */

ULONG NTMINI_ScsiPortGetBusData(
    PVOID HwDeviceExtension,
    ULONG BusDataType,  /* PCIConfiguration = 4 */
    ULONG SystemIoBusNumber,
    ULONG SlotNumber,
    PVOID Buffer,
    ULONG Length)
{
    /* [STUB] Read PCI configuration space.
       Win9x method: INT 1Ah, AX=B10Ah (PCI read config byte/word/dword)
       or direct I/O via 0xCF8/0xCFC.
       For IDE controllers, the miniport reads PCI config to determine
       I/O base addresses and capabilities. */

    /* Direct PCI config access via port 0xCF8/0xCFC */
    ULONG bus = SystemIoBusNumber;
    ULONG dev = (SlotNumber >> 0) & 0x1F;
    ULONG func = (SlotNumber >> 5) & 0x07;
    ULONG i;
    PUCHAR buf = (PUCHAR)Buffer;

    for (i = 0; i < Length; i++) {
        ULONG addr = 0x80000000 | (bus << 16) | (dev << 11) |
                     (func << 8) | (i & 0xFC);
        ULONG val = PORT_PCI_CONFIG_READ(addr);
        buf[i] = (UCHAR)(val >> ((i & 3) * 8));
    }
    return Length;
}

ULONG NTMINI_ScsiPortSetBusDataByOffset(
    PVOID HwDeviceExtension,
    ULONG BusDataType,
    ULONG SystemIoBusNumber,
    ULONG SlotNumber,
    PVOID Buffer,
    ULONG Offset,
    ULONG Length)
{
    /* [STUB] Write PCI config space. Similar to GetBusData but writes. */
    return Length;
}


/* ================================================================
 * PART 3: ScsiPortInitialize (the central glue)
 * Called from the miniport's DriverEntry. Saves callbacks,
 * allocates device extension, calls HwFindAdapter, then
 * HwInitialize.
 * ================================================================ */

ULONG NTMINI_ScsiPortInitialize(
    PVOID Argument1,         /* DriverObject (unused on Win9x) */
    PVOID Argument2,         /* RegistryPath (unused on Win9x) */
    PHW_INITIALIZATION_DATA HwInitData,
    PVOID HwContext)
{
    PORT_CONFIGURATION_INFORMATION configInfo;
    ACCESS_RANGE accessRanges[8];
    UCHAR again;
    ULONG status;

    /* Save miniport callbacks */
    g_state.HwInitialize = HwInitData->HwInitialize;
    g_state.HwStartIo    = HwInitData->HwStartIo;
    g_state.HwInterrupt  = HwInitData->HwInterrupt;
    g_state.HwFindAdapter = (ULONG(*)(PVOID,PVOID,PVOID,PVOID,
                            PPORT_CONFIGURATION_INFORMATION,PUCHAR))
                            HwInitData->HwFindAdapter;
    g_state.HwResetBus   = HwInitData->HwResetBus;
    g_state.DeviceExtensionSize = HwInitData->DeviceExtensionSize;

    /* Allocate device extension */
    /* [STUB] Use VxD HeapAllocate or _PageAllocate */
    /* g_state.DeviceExtension = HeapAllocate(g_state.DeviceExtensionSize); */

    /* Zero initialize */
    NTMINI_ScsiPortMoveMemory(g_state.DeviceExtension, NULL,
                              g_state.DeviceExtensionSize);

    /* Set up PORT_CONFIGURATION_INFORMATION for secondary IDE */
    NTMINI_ScsiPortMoveMemory(&configInfo, NULL, sizeof(configInfo));
    configInfo.Length = sizeof(PORT_CONFIGURATION_INFORMATION);
    configInfo.SystemIoBusNumber = 0;
    configInfo.AdapterInterfaceType = 1; /* Isa */
    configInfo.BusInterruptLevel = 15;   /* Secondary IDE IRQ */
    configInfo.BusInterruptVector = 15;
    configInfo.InterruptMode = 1;        /* Latched */
    configInfo.MaximumTransferLength = 0x10000;
    configInfo.NumberOfPhysicalBreaks = 0;
    configInfo.NumberOfAccessRanges = 2;
    configInfo.AccessRanges = &accessRanges;
    configInfo.NumberOfBuses = 1;
    configInfo.MaximumNumberOfTargets = 2; /* Master + Slave */
    configInfo.AtdiskPrimaryClaimed = TRUE;
    configInfo.AtdiskSecondaryClaimed = FALSE;
    configInfo.MapBuffers = TRUE;

    /* Set up access ranges for secondary IDE channel */
    accessRanges[0].RangeStart.LowPart = 0x170;
    accessRanges[0].RangeStart.HighPart = 0;
    accessRanges[0].RangeLength = 8;
    accessRanges[0].RangeInMemory = FALSE;

    accessRanges[1].RangeStart.LowPart = 0x376;
    accessRanges[1].RangeStart.HighPart = 0;
    accessRanges[1].RangeLength = 1;
    accessRanges[1].RangeInMemory = FALSE;

    /* Call miniport's HwFindAdapter */
    again = FALSE;
    status = g_state.HwFindAdapter(
        g_state.DeviceExtension,
        HwContext,
        NULL,        /* BusInformation */
        NULL,        /* ArgumentString */
        &configInfo,
        &again);

    if (status != 0 /* SP_RETURN_FOUND */) {
        return STATUS_UNSUCCESSFUL;
    }

    /* Call HwInitialize */
    if (!g_state.HwInitialize(g_state.DeviceExtension)) {
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_SUCCESS;
}


/* ================================================================
 * PART 4: IOS BRIDGE (SKELETON)
 * Translates between Win9x IOS I/O Requests (IORs) and NT4
 * SCSI Request Blocks (SRBs).
 *
 * This is the hardest part and requires Win98 DDK headers
 * (ios.h, ior.h, etc.) to compile.
 * ================================================================ */

/*
 * [STUB] IOS Port Driver Registration
 *
 * A Win9x .PDR registers with IOS using AEP (Async Event Packet)
 * callbacks. The key entry points:
 *
 * AEP_INITIALIZE   - IOS initializes the port driver
 * AEP_BOOT_COMPLETE - System boot is done
 * AEP_CONFIG_DCB   - Configure a Device Control Block
 * AEP_UNCONFIG_DCB - Remove a DCB
 * AEP_IOR          - Process an I/O Request
 *
 * The IOR handler is where we translate IORs to SRBs:
 *
 * void ios_request_handler(IOR *ior) {
 *     SCSI_REQUEST_BLOCK srb;
 *     memset(&srb, 0, sizeof(srb));
 *
 *     switch (ior->IOR_func) {
 *     case IOR_READ:
 *     case IOR_WRITE:
 *         srb.Function = SRB_FUNCTION_EXECUTE_SCSI;
 *         srb.PathId = 0;
 *         srb.TargetId = ior->IOR_target;
 *         srb.Lun = 0;
 *         srb.DataBuffer = ior->IOR_buffer_ptr;
 *         srb.DataTransferLength = ior->IOR_xfer_count;
 *         // Build CDB for READ(10) or WRITE(10)
 *         srb.CdbLength = 10;
 *         srb.Cdb[0] = (ior->IOR_func == IOR_READ) ? 0x28 : 0x2A;
 *         // Set LBA in CDB bytes 2-5
 *         // Set transfer length in CDB bytes 7-8
 *         break;
 *
 *     case IOR_SCSI_PASS_THROUGH:
 *         // Pass SCSI/ATAPI command directly
 *         break;
 *     }
 *
 *     g_state.CurrentSrb = &srb;
 *     g_state.SrbPending = TRUE;
 *     g_state.HwStartIo(g_state.DeviceExtension, &srb);
 *
 *     // Wait for ScsiPortNotification(RequestComplete)
 *     // Then signal IOS completion
 * }
 */


/* ================================================================
 * PART 5: INTERRUPT HANDLING (SKELETON)
 *
 * Win9x uses VPICD (Virtual Programmable Interrupt Controller
 * Device) for interrupt management. We need to:
 * 1. Virtualize IRQ 15 (secondary IDE) via VPICD_Virtualize_IRQ
 * 2. In our ISR, call the miniport's HwInterrupt callback
 * 3. EOI the interrupt via VPICD_Phys_EOI
 *
 * void __declspec(naked) irq_handler(void) {
 *     // Save registers
 *     // Call g_state.HwInterrupt(g_state.DeviceExtension)
 *     // If it returns TRUE: interrupt was ours, EOI
 *     // If FALSE: chain to previous handler
 *     // Restore registers, iret
 * }
 * ================================================================ */


/* ================================================================
 * PART 6: PE LOADER
 * ================================================================ */

/*
 * [STUB] pe_load(filename) pseudocode:
 *
 * 1. Read .sys file into temporary buffer
 * 2. Parse DOS header (MZ), find PE offset
 * 3. Parse PE header: get ImageBase, SizeOfImage, EntryPoint
 * 4. Allocate SizeOfImage bytes of ring 0 memory
 *    (VxD: _PageAllocate with PageFixed)
 * 5. Map sections: for each section header:
 *    - Copy SizeOfRawData bytes from file to VirtualAddress
 *    - Zero-fill remainder (VirtualSize - SizeOfRawData)
 * 6. Process relocations (if ImageBase != preferred):
 *    - Walk .reloc section, apply fixups
 * 7. Resolve imports:
 *    - Walk import directory
 *    - For each DLL (should only be SCSIPORT.SYS):
 *      - For each imported function:
 *        - Look up name in scsiport_functions[] table
 *        - Write our function pointer into the IAT
 * 8. Call EntryPoint (= DriverEntry):
 *    - DriverEntry(NULL, NULL) -- both args unused by miniport,
 *      it immediately calls ScsiPortInitialize which we intercept
 * 9. If DriverEntry returns STATUS_SUCCESS, we're loaded.
 */


/* ================================================================
 * PART 7: VxD ENTRY POINT (SKELETON)
 *
 * The VxD control procedure handles system messages:
 * - Sys_Dynamic_Device_Init: Load PE, register with IOS
 * - Sys_Dynamic_Device_Exit: Cleanup
 * ================================================================ */

/*
 * For a .PDR (Port Driver), the entry point is different from
 * a regular VxD. A PDR exports a single entry point that IOS
 * calls with AEP packets. The DDB (Device Descriptor Block)
 * identifies it as a port driver.
 *
 * [TODO] Write the actual VxD/PDR boilerplate in assembly.
 * The C code above handles the logic; the assembly wrapper
 * handles the Win9x calling conventions and VxD infrastructure.
 */
