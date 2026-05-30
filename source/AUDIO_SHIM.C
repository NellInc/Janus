/*
 * AUDIO_SHIM.C - Port Class (PORTCLS.SYS) Driver Shim for Win98 VxD PE Loader
 *
 * Provides a PORTCLS.SYS import resolution layer enabling XP WDM audio
 * miniport .sys files to run on Win98 via the VxD PE loader (PELOAD.C).
 *
 * Architecture:
 *   - Registers port driver shim: "PORTCLS.SYS"
 *   - Provides PortCls API functions (PcInitializeAdapterDriver, etc.)
 *   - Implements COM vtable stubs for IPortWaveCyclic, IDmaChannel,
 *     IServiceGroup, IResourceList
 *   - Stores miniport's AddDevice callback from PcInitializeAdapterDriver
 *
 * COM Interface Pattern:
 *   PortCls uses COM-style interfaces. The miniport receives interface
 *   pointers (e.g. IPortWaveCyclic*) and calls methods through vtables.
 *   We provide static vtable structs with correct-arity __stdcall stubs.
 *   Every method includes the hidden PVOID This as first parameter.
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VxD wrapper)
 *
 * CRITICAL: Parameter counts must be EXACT for all __stdcall functions.
 * Wrong param count = stack corruption = silent data corruption.
 * Each function annotated with param count per task spec.
 *
 * Phase 2 TODO (when integrating a real miniport driver):
 *   1. IDmaChannel vtable slot ordering is INCOMPLETE. Real DDK has ~15
 *      slots (TransferCount, SetBufferSize, Start, Stop, ReadCounter,
 *      WaitForTC, etc.) between FreeBuffer and SystemAddress. A real
 *      miniport calling SystemAddress will read the wrong slot. Must
 *      verify slot ordering against WDK portcls.h / ReactOS headers.
 *   2. Same risk applies to IPortWaveCyclic, IServiceGroup, IResourceList
 *      if the real DDK has more methods than listed here.
 *   3. Verify PcInitializeAdapterDriver param count (DDK may be 3 not 4).
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
typedef ULONG               NTSTATUS;

/* 64-bit type for PHYSICAL_ADDRESS returns (EDX:EAX on __stdcall) */
typedef unsigned long long  ULONGLONG;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

#define STATUS_SUCCESS          0x00000000UL
#define STATUS_UNSUCCESSFUL     0xC0000001UL
#define STATUS_NOT_IMPLEMENTED  0xC0000002UL
#define PAGESIZE  4096
#define PAGEFIXED 0x00000001

/* COM HRESULTs */
#define S_OK            0x00000000UL
#define E_NOTIMPL       0x80004001UL
#define E_NOINTERFACE   0x80004002UL

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
extern PVOID VxD_MapPhysToLinear(ULONG physAddr, ULONG nBytes);
extern void  VxD_Debug_Printf(const char *fmt, ...);

/* ================================================================
 * PE loader externals
 * ================================================================ */

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

static void pc_memset(void *dst, int val, ULONG n) {
    UCHAR *d = (UCHAR *)dst;
    while (n--) *d++ = (UCHAR)val;
}

static void pc_log_hex(const char *prefix, ULONG v, const char *suffix) {
    static const char hx[] = "0123456789ABCDEF";
    char h[12]; int i;
    h[0] = '0'; h[1] = 'x';
    for (i = 7; i >= 0; i--) h[2 + (7 - i)] = hx[(v >> (i * 4)) & 0xF];
    h[10] = 0;
    VxD_Debug_Printf(prefix); VxD_Debug_Printf(h); VxD_Debug_Printf(suffix);
}

/* ================================================================
 * Adapter State (stores miniport's callbacks)
 * ================================================================ */

static struct {
    PVOID AddDeviceCallback;    /* From PcInitializeAdapterDriver */
    PVOID StartDeviceCallback;  /* From PcAddAdapterDevice */
    PVOID DriverObject;
    PVOID PhysicalDeviceObject;
} g_audio_adapter;

/* Fake DRIVER_EXTENSION: just need AddDevice at offset +0x04.
 * Real layout: +0x00=DriverObject, +0x04=AddDevice, +0x08=Count, +0x0C=ServiceKeyName */
static ULONG g_fake_driver_ext[4];  /* 16 bytes */

/* Fake DRIVER_OBJECT: 0x168 bytes on XP i386.
 * DriverExtension is at offset +0x28. */
static ULONG g_fake_driver_object[0x60]; /* 0x180 bytes, zero-filled */

static ULONG __stdcall safe_irp_dispatch(PVOID DevObj, PVOID Irp)
{
    return 0xC00000BBUL; /* STATUS_NOT_SUPPORTED */
}

PVOID audio_get_fake_driver_object(void)
{
    ULONG i;
    PULONG drvObj = g_fake_driver_object;

    /* Zero the entire DRIVER_OBJECT */
    for (i = 0; i < 0x60; i++) drvObj[i] = 0;

    /* DRIVER_OBJECT layout (XP i386):
       +0x00 Type(2) Size(2) +0x04 DeviceObject +0x08 Flags
       +0x0C DriverStart +0x10 DriverSize +0x14 DriverSection
       +0x18 DriverExtension +0x1C DriverName(8) +0x24 HardwareDatabase
       +0x28 FastIoDispatch +0x2C DriverInit +0x30 DriverStartIo
       +0x34 DriverUnload +0x38 MajorFunction[28] */
    drvObj[0] = 4 | (0x168 << 16); /* Type=4 (IO_TYPE_DRIVER), Size=0x168 */
    drvObj[6] = (ULONG)&g_fake_driver_ext[0]; /* DriverExtension at +0x18 */
    g_fake_driver_ext[0] = (ULONG)drvObj;     /* DriverExtension->DriverObject */

    /* MajorFunction[0..27] at +0x38 (ULONG index 14..41) */
    for (i = 14; i < 14 + 28; i++)
        drvObj[i] = (ULONG)safe_irp_dispatch;

    /* DriverInit at +0x2C (index 11) */
    drvObj[11] = (ULONG)safe_irp_dispatch;

    return (PVOID)drvObj;
}

/* ================================================================
 * COM Vtable: IPortWaveCyclic
 *
 * Method order per DDK (IUnknown + IPort + IPortWaveCyclic):
 *   0: QueryInterface(This, riid, ppv) — 3 params
 *   1: AddRef(This) — 1 param
 *   2: Release(This) — 1 param
 *   3: Init(This, DeviceObject, Irp, ResourceList, UnknownMiniport, UnknownAdapter) — 6 params
 *   4: GetDeviceProperty(This, Property, BufferLength, Buffer, ResultLength) — 5 params
 *   5: NewMasterDmaChannel(This, OutDma, OuterUnknown, ResourceList, MaxWidth, DemandMode) — 6 params
 *   6: NewSlaveDmaChannel(This, OutDma, OuterUnknown, ResourceList, DmaIndex, MaxWidth, DemandMode, Speed) — 8 params
 *   7: Notify(This, ServiceGroup) — 2 params
 *   8: NewRegistryKey(This, OutKey, OuterUnknown, Type, DesiredAccess) — 5 params (per task spec; verify vs DDK)
 * ================================================================ */

static ULONG __stdcall pc_port_QueryInterface(PVOID This, PVOID riid, PVOID *ppv)
{
    VxD_Debug_Printf("PC: IPortWaveCyclic::QueryInterface\r\n");
    if (ppv) *ppv = This;  /* Return self for any interface query */
    return S_OK;
}

static ULONG __stdcall pc_port_AddRef(PVOID This)
{
    return 1;
}

static ULONG __stdcall pc_port_Release(PVOID This)
{
    return 1;
}

static ULONG __stdcall pc_port_Init(
    PVOID This, PVOID DeviceObject, PVOID Irp,
    PVOID ResourceList, PVOID UnknownMiniport, PVOID UnknownAdapter)
{
    VxD_Debug_Printf("PC: IPortWaveCyclic::Init\r\n");
    return S_OK;
}

static ULONG __stdcall pc_port_GetDeviceProperty(
    PVOID This, ULONG Property, ULONG BufferLength,
    PVOID Buffer, PULONG ResultLength)
{
    VxD_Debug_Printf("PC: IPortWaveCyclic::GetDeviceProperty\r\n");
    if (ResultLength) *ResultLength = 0;
    return E_NOTIMPL;
}

static ULONG __stdcall pc_port_NewMasterDmaChannel(
    PVOID This, PVOID *OutDma, PVOID OuterUnknown,
    PVOID ResourceList, ULONG MaxWidth, BOOLEAN DemandMode)
{
    VxD_Debug_Printf("PC: IPortWaveCyclic::NewMasterDmaChannel\r\n");
    if (OutDma) *OutDma = NULL;
    return E_NOTIMPL;
}

static ULONG __stdcall pc_port_NewSlaveDmaChannel(
    PVOID This, PVOID *OutDma, PVOID OuterUnknown, PVOID ResourceList,
    ULONG DmaIndex, ULONG MaxWidth, BOOLEAN DemandMode, ULONG Speed)
{
    VxD_Debug_Printf("PC: IPortWaveCyclic::NewSlaveDmaChannel\r\n");
    if (OutDma) *OutDma = NULL;
    return E_NOTIMPL;
}

static ULONG __stdcall pc_port_Notify(PVOID This, PVOID ServiceGroup)
{
    VxD_Debug_Printf("PC: IPortWaveCyclic::Notify\r\n");
    return S_OK;
}

static ULONG __stdcall pc_port_NewRegistryKey(
    PVOID This, PVOID *OutKey, PVOID OuterUnknown, ULONG Type, ULONG DesiredAccess)
{
    VxD_Debug_Printf("PC: IPortWaveCyclic::NewRegistryKey\r\n");
    if (OutKey) *OutKey = NULL;
    return E_NOTIMPL;
}

/* Static vtable */
typedef struct {
    PVOID QueryInterface;
    PVOID AddRef;
    PVOID Release;
    PVOID Init;
    PVOID GetDeviceProperty;
    PVOID NewMasterDmaChannel;
    PVOID NewSlaveDmaChannel;
    PVOID Notify;
    PVOID NewRegistryKey;
} IPortWaveCyclicVtbl;

static IPortWaveCyclicVtbl g_port_vtbl = {
    (PVOID)pc_port_QueryInterface,
    (PVOID)pc_port_AddRef,
    (PVOID)pc_port_Release,
    (PVOID)pc_port_Init,
    (PVOID)pc_port_GetDeviceProperty,
    (PVOID)pc_port_NewMasterDmaChannel,
    (PVOID)pc_port_NewSlaveDmaChannel,
    (PVOID)pc_port_Notify,
    (PVOID)pc_port_NewRegistryKey
};

/* Object instance (lpVtbl + refCount pattern) */
static struct { IPortWaveCyclicVtbl *lpVtbl; ULONG refCount; } g_port_obj = { &g_port_vtbl, 1 };

/* ================================================================
 * COM Vtable: IDmaChannel
 *
 *   0: QueryInterface(This, riid, ppv) — 3
 *   1: AddRef(This) — 1
 *   2: Release(This) — 1
 *   3: AllocateBuffer(This, BufferSize, PhysicalMemoryConstraint) — 3
 *   4: FreeBuffer(This) — 1
 *   5: SystemAddress(This) — 1
 *   6: PhysicalAddress(This) — 1, returns PHYSICAL_ADDRESS (64-bit in EDX:EAX)
 *   7: BufferSize(This) — 1
 * ================================================================ */

static UCHAR g_dma_buffer[4096];

static ULONG __stdcall pc_dma_QueryInterface(PVOID This, PVOID riid, PVOID *ppv)
{
    if (ppv) *ppv = This;
    return S_OK;
}
static ULONG __stdcall pc_dma_AddRef(PVOID This) { return 1; }
static ULONG __stdcall pc_dma_Release(PVOID This) { return 1; }

static ULONG __stdcall pc_dma_AllocateBuffer(PVOID This, ULONG BufferSize, PVOID Constraint)
{
    pc_log_hex("PC: IDmaChannel::AllocateBuffer size=", BufferSize, "\r\n");
    return S_OK;
}
static void __stdcall pc_dma_FreeBuffer(PVOID This)
{
    VxD_Debug_Printf("PC: IDmaChannel::FreeBuffer\r\n");
}
static PVOID __stdcall pc_dma_SystemAddress(PVOID This)
{
    return (PVOID)g_dma_buffer;
}
static ULONGLONG __stdcall pc_dma_PhysicalAddress(PVOID This)
{
    /* Returns PHYSICAL_ADDRESS (64-bit). In ring 0 flat model, linear == physical
     * for our statically allocated buffer. Compiler returns in EDX:EAX. */
    return (ULONGLONG)(ULONG)g_dma_buffer;
}
static ULONG __stdcall pc_dma_BufferSize(PVOID This)
{
    return sizeof(g_dma_buffer);
}

typedef struct {
    PVOID QueryInterface; PVOID AddRef; PVOID Release;
    PVOID AllocateBuffer; PVOID FreeBuffer;
    PVOID SystemAddress; PVOID PhysicalAddress; PVOID BufferSize;
} IDmaChannelVtbl;

static IDmaChannelVtbl g_dma_vtbl = {
    (PVOID)pc_dma_QueryInterface, (PVOID)pc_dma_AddRef, (PVOID)pc_dma_Release,
    (PVOID)pc_dma_AllocateBuffer, (PVOID)pc_dma_FreeBuffer,
    (PVOID)pc_dma_SystemAddress, (PVOID)pc_dma_PhysicalAddress, (PVOID)pc_dma_BufferSize
};

static struct { IDmaChannelVtbl *lpVtbl; ULONG refCount; } g_dma_obj = { &g_dma_vtbl, 1 };

/* ================================================================
 * COM Vtable: IServiceGroup
 *
 *   0: QueryInterface(This, riid, ppv) — 3
 *   1: AddRef(This) — 1
 *   2: Release(This) — 1
 *   3: AddMember(This, ServiceSink) — 2
 *   4: RequestService(This) — 1
 * ================================================================ */

static ULONG __stdcall pc_sg_QueryInterface(PVOID This, PVOID riid, PVOID *ppv)
{
    if (ppv) *ppv = This;
    return S_OK;
}
static ULONG __stdcall pc_sg_AddRef(PVOID This) { return 1; }
static ULONG __stdcall pc_sg_Release(PVOID This) { return 1; }

static ULONG __stdcall pc_sg_AddMember(PVOID This, PVOID ServiceSink)
{
    VxD_Debug_Printf("PC: IServiceGroup::AddMember\r\n");
    return S_OK;
}
static void __stdcall pc_sg_RequestService(PVOID This)
{
    VxD_Debug_Printf("PC: IServiceGroup::RequestService\r\n");
}

typedef struct {
    PVOID QueryInterface; PVOID AddRef; PVOID Release;
    PVOID AddMember; PVOID RequestService;
} IServiceGroupVtbl;

static IServiceGroupVtbl g_sg_vtbl = {
    (PVOID)pc_sg_QueryInterface, (PVOID)pc_sg_AddRef, (PVOID)pc_sg_Release,
    (PVOID)pc_sg_AddMember, (PVOID)pc_sg_RequestService
};

static struct { IServiceGroupVtbl *lpVtbl; ULONG refCount; } g_sg_obj = { &g_sg_vtbl, 1 };

/* ================================================================
 * COM Vtable: IResourceList
 *
 *   0: QueryInterface(This, riid, ppv) — 3
 *   1: AddRef(This) — 1
 *   2: Release(This) — 1
 *   3: NumberOfEntries(This) — 1
 *   4: NumberOfEntriesOfType(This, Type) — 2
 *   5: FindTranslatedEntry(This, Type, Index) — 3
 *   6: FindUntranslatedEntry(This, Type, Index) — 3
 *   7: AddEntry(This, Translated, Untranslated) — 3
 *   8: AddEntryFromParent(This, Parent, Type, Index) — 4
 * ================================================================ */

static ULONG __stdcall pc_rl_QueryInterface(PVOID This, PVOID riid, PVOID *ppv)
{
    if (ppv) *ppv = This;
    return S_OK;
}
static ULONG __stdcall pc_rl_AddRef(PVOID This) { return 1; }
static ULONG __stdcall pc_rl_Release(PVOID This) { return 1; }

static ULONG __stdcall pc_rl_NumberOfEntries(PVOID This)
{
    return 0;  /* No hardware resources in scaffold */
}
static ULONG __stdcall pc_rl_NumberOfEntriesOfType(PVOID This, ULONG Type)
{
    return 0;
}
static PVOID __stdcall pc_rl_FindTranslatedEntry(PVOID This, ULONG Type, ULONG Index)
{
    return NULL;
}
static PVOID __stdcall pc_rl_FindUntranslatedEntry(PVOID This, ULONG Type, ULONG Index)
{
    return NULL;
}
static ULONG __stdcall pc_rl_AddEntry(PVOID This, PVOID Translated, PVOID Untranslated)
{
    return S_OK;
}
static ULONG __stdcall pc_rl_AddEntryFromParent(PVOID This, PVOID Parent, ULONG Type, ULONG Index)
{
    return S_OK;
}

typedef struct {
    PVOID QueryInterface; PVOID AddRef; PVOID Release;
    PVOID NumberOfEntries; PVOID NumberOfEntriesOfType;
    PVOID FindTranslatedEntry; PVOID FindUntranslatedEntry;
    PVOID AddEntry; PVOID AddEntryFromParent;
} IResourceListVtbl;

static IResourceListVtbl g_rl_vtbl = {
    (PVOID)pc_rl_QueryInterface, (PVOID)pc_rl_AddRef, (PVOID)pc_rl_Release,
    (PVOID)pc_rl_NumberOfEntries, (PVOID)pc_rl_NumberOfEntriesOfType,
    (PVOID)pc_rl_FindTranslatedEntry, (PVOID)pc_rl_FindUntranslatedEntry,
    (PVOID)pc_rl_AddEntry, (PVOID)pc_rl_AddEntryFromParent
};

static struct { IResourceListVtbl *lpVtbl; ULONG refCount; } g_rl_obj = { &g_rl_vtbl, 1 };

/* ================================================================
 * PortCls API Implementations
 * ================================================================ */

/*
 * PcInitializeAdapterDriver — 3 params (WDK verified)
 * NTSTATUS PcInitializeAdapterDriver(PDRIVER_OBJECT, PUNICODE_STRING, PDRIVER_ADD_DEVICE)
 * Stores AddDevice callback and sets up driver dispatch table.
 */
static NTSTATUS __stdcall pc_InitializeAdapterDriver(
    PVOID DriverObject, PVOID RegistryPath,
    PVOID AddDeviceCallback)
{
    VxD_Debug_Printf("PC: PcInitializeAdapterDriver\r\n");
    g_audio_adapter.DriverObject = DriverObject;
    g_audio_adapter.AddDeviceCallback = AddDeviceCallback;
    pc_log_hex("  DriverObject=", (ULONG)DriverObject, "");
    pc_log_hex("  AddDevice=", (ULONG)AddDeviceCallback, "\r\n");
    /* In real PortCls, this stores AddDevice into
     * DriverObject->DriverExtension->AddDevice and sets up IRP dispatch.
     * We store it for potential later invocation. */
    if (DriverObject) {
        PULONG drvObj = (PULONG)DriverObject;
        PULONG drvExt = (PULONG)drvObj[10]; /* DriverExtension at +0x28 */
        if (drvExt) {
            drvExt[1] = (ULONG)AddDeviceCallback; /* AddDevice at DriverExtension+0x04 */
            VxD_Debug_Printf("PC: Stored AddDevice into DriverExtension\r\n");
        }
    }
    return STATUS_SUCCESS;
}

/*
 * PcAddAdapterDevice — 5 params (per task spec; verify vs DDK)
 */
static NTSTATUS __stdcall pc_AddAdapterDevice(
    PVOID DriverObject, PVOID PhysicalDeviceObject,
    PVOID StartDevice, ULONG MaxObjects, ULONG Reserved)
{
    VxD_Debug_Printf("PC: PcAddAdapterDevice\r\n");
    g_audio_adapter.StartDeviceCallback = StartDevice;
    g_audio_adapter.PhysicalDeviceObject = PhysicalDeviceObject;
    pc_log_hex("  StartDevice=", (ULONG)StartDevice, "\r\n");
    return STATUS_SUCCESS;
}

/*
 * PcRegisterAdapterPowerManagement — 3 params (per task spec; verify vs DDK)
 */
static NTSTATUS __stdcall pc_RegisterAdapterPowerManagement(
    PVOID Unknown, PVOID DeviceObject, PVOID Reserved)
{
    VxD_Debug_Printf("PC: PcRegisterAdapterPowerManagement (stub)\r\n");
    return STATUS_SUCCESS;
}

/*
 * PcNewPort — 3 params (per task spec; verify vs DDK)
 * Returns a fake IPortWaveCyclic with stubbed vtable.
 */
static NTSTATUS __stdcall pc_NewPort(PVOID *OutPort, PVOID ClassId, PVOID Reserved)
{
    VxD_Debug_Printf("PC: PcNewPort\r\n");
    if (!OutPort) return STATUS_UNSUCCESSFUL;
    *OutPort = (PVOID)&g_port_obj;
    pc_log_hex("  -> IPort=", (ULONG)&g_port_obj, "\r\n");
    return STATUS_SUCCESS;
}

/*
 * PcNewMiniport — 3 params (per task spec; verify vs DDK)
 * The miniport creates its own miniport object; we just log.
 */
static NTSTATUS __stdcall pc_NewMiniport(PVOID *OutMiniport, PVOID ClassId, PVOID Reserved)
{
    VxD_Debug_Printf("PC: PcNewMiniport\r\n");
    if (OutMiniport) *OutMiniport = NULL;
    return E_NOTIMPL;  /* Miniport should create its own */
}

/*
 * PcRegisterSubdevice — 4 params (per task spec; verify vs DDK)
 * Logs and returns success.
 */
static NTSTATUS __stdcall pc_RegisterSubdevice(
    PVOID DeviceObject, PVOID Name, PVOID Unknown, PVOID Reserved)
{
    VxD_Debug_Printf("PC: PcRegisterSubdevice\r\n");
    return STATUS_SUCCESS;
}

/*
 * PcRegisterPhysicalConnection — 5 params (per task spec; verify vs DDK)
 */
static NTSTATUS __stdcall pc_RegisterPhysicalConnection(
    PVOID DeviceObject, PVOID FromUnknown, ULONG FromPin,
    PVOID ToUnknown, ULONG ToPin)
{
    VxD_Debug_Printf("PC: PcRegisterPhysicalConnection\r\n");
    return STATUS_SUCCESS;
}

/*
 * PcNewResourceList — 4 params (per task spec; verify vs DDK)
 * Returns our static IResourceList object.
 */
static NTSTATUS __stdcall pc_NewResourceList(
    PVOID *OutResourceList, PVOID OuterUnknown,
    ULONG PoolType, PVOID TranslatedResources)
{
    VxD_Debug_Printf("PC: PcNewResourceList\r\n");
    if (!OutResourceList) return STATUS_UNSUCCESSFUL;
    *OutResourceList = (PVOID)&g_rl_obj;
    return STATUS_SUCCESS;
}

/*
 * PcNewResourceSublist — 4 params (per task spec; verify vs DDK)
 */
static NTSTATUS __stdcall pc_NewResourceSublist(
    PVOID *OutResourceList, PVOID OuterUnknown,
    ULONG PoolType, PVOID ParentList)
{
    VxD_Debug_Printf("PC: PcNewResourceSublist\r\n");
    if (!OutResourceList) return STATUS_UNSUCCESSFUL;
    *OutResourceList = (PVOID)&g_rl_obj;
    return STATUS_SUCCESS;
}

/*
 * PcNewInterruptSync — 4 params (per task spec; verify vs DDK)
 */
static NTSTATUS __stdcall pc_NewInterruptSync(
    PVOID *OutInterruptSync, PVOID OuterUnknown,
    PVOID ResourceList, ULONG ResourceIndex)
{
    VxD_Debug_Printf("PC: PcNewInterruptSync (stub)\r\n");
    if (OutInterruptSync) *OutInterruptSync = NULL;
    return E_NOTIMPL;
}

/*
 * PcNewDmaChannel — 4 params (per task spec; verify vs DDK)
 * Returns our static IDmaChannel object.
 */
static NTSTATUS __stdcall pc_NewDmaChannel(
    PVOID *OutDmaChannel, PVOID OuterUnknown,
    ULONG PoolType, PVOID DeviceDescription)
{
    VxD_Debug_Printf("PC: PcNewDmaChannel\r\n");
    if (!OutDmaChannel) return STATUS_UNSUCCESSFUL;
    *OutDmaChannel = (PVOID)&g_dma_obj;
    return STATUS_SUCCESS;
}

/*
 * PcNewServiceGroup — 2 params (per task spec; verify vs DDK)
 * Returns our static IServiceGroup object.
 */
static NTSTATUS __stdcall pc_NewServiceGroup(PVOID *OutServiceGroup, PVOID OuterUnknown)
{
    VxD_Debug_Printf("PC: PcNewServiceGroup\r\n");
    if (!OutServiceGroup) return STATUS_UNSUCCESSFUL;
    *OutServiceGroup = (PVOID)&g_sg_obj;
    return STATUS_SUCCESS;
}

/*
 * PcCompletePendingPropertyRequest — 2 params (per task spec; verify vs DDK)
 */
static NTSTATUS __stdcall pc_CompletePendingPropertyRequest(
    PVOID PropertyRequest, NTSTATUS NtStatus)
{
    pc_log_hex("PC: PcCompletePendingPropertyRequest status=", NtStatus, "\r\n");
    return STATUS_SUCCESS;
}

/*
 * PcGetTimeInterval — 1 param (per task spec; verify vs DDK)
 * Returns a monotonic tick count (fake: just returns 0).
 */
static ULONG __stdcall pc_GetTimeInterval(ULONG Since)
{
    return 0;
}

/*
 * PcNewRegistryKey — 5 params (per task spec; verify vs DDK)
 */
static NTSTATUS __stdcall pc_NewRegistryKey(
    PVOID *OutRegistryKey, PVOID OuterUnknown,
    ULONG Type, ULONG DesiredAccess, PVOID DeviceObject)
{
    VxD_Debug_Printf("PC: PcNewRegistryKey (stub)\r\n");
    if (OutRegistryKey) *OutRegistryKey = NULL;
    return E_NOTIMPL;
}

/* ================================================================
 * Additional PortCls stubs (less commonly used)
 * ================================================================ */

/* PcRegisterIoTimeout — 3 params */
static NTSTATUS __stdcall pc_RegisterIoTimeout(
    PVOID DeviceObject, PVOID Callback, PVOID Context)
{
    VxD_Debug_Printf("PC: PcRegisterIoTimeout (stub)\r\n");
    return STATUS_SUCCESS;
}

/* PcUnregisterIoTimeout — 3 params */
static NTSTATUS __stdcall pc_UnregisterIoTimeout(
    PVOID DeviceObject, PVOID Callback, PVOID Context)
{
    VxD_Debug_Printf("PC: PcUnregisterIoTimeout (stub)\r\n");
    return STATUS_SUCCESS;
}

/* PcDispatchIrp — 3-param version (superseded; WDK signature is 2 params).
 * Left as dead code; portcls_funcs[] now points at pc_DispatchIrp_2p below. */
static NTSTATUS __stdcall pc_DispatchIrp(
    PVOID DeviceObject, PVOID Irp, PVOID Reserved)
{
    VxD_Debug_Printf("PC: PcDispatchIrp (stub)\r\n");
    return STATUS_SUCCESS;
}

/*
 * PcForwardIrpSynchronous — 2 params __stdcall
 * Forwards an IRP synchronously to the next driver in the stack.
 */
static NTSTATUS __stdcall pc_ForwardIrpSynchronous(PVOID DeviceObject, PVOID Irp)
{
    VxD_Debug_Printf("PORTCLS: PcForwardIrpSynchronous (stub)\r\n");
    return STATUS_SUCCESS;
}

/*
 * PcDispatchIrp — 2 params __stdcall (correct WDK signature)
 * PortCls IRP dispatch function. Replaces the 3-param stub above in the
 * import table; the old 3-param body is left in place as dead code.
 */
static NTSTATUS __stdcall pc_DispatchIrp_2p(PVOID DeviceObject, PVOID Irp)
{
    VxD_Debug_Printf("PORTCLS: PcDispatchIrp (stub)\r\n");
    return STATUS_SUCCESS;
}

/* PcRequestNewPowerState — 2 params */
static NTSTATUS __stdcall pc_RequestNewPowerState(PVOID DeviceObject, ULONG NewState)
{
    VxD_Debug_Printf("PC: PcRequestNewPowerState (stub)\r\n");
    return STATUS_SUCCESS;
}

/* PcGetDeviceProperty — 5 params */
static NTSTATUS __stdcall pc_GetDeviceProperty(
    PVOID FunctionalDeviceObject, ULONG Property,
    ULONG BufferLength, PVOID Buffer, PULONG ResultLength)
{
    VxD_Debug_Printf("PC: PcGetDeviceProperty (stub)\r\n");
    if (ResultLength) *ResultLength = 0;
    return STATUS_UNSUCCESSFUL;
}

/* ================================================================
 * PortCls Import Function Table
 * ================================================================ */

static const IMPORT_FUNC_ENTRY portcls_funcs[] = {
    { "PcInitializeAdapterDriver",          (PVOID)pc_InitializeAdapterDriver },
    { "PcAddAdapterDevice",                 (PVOID)pc_AddAdapterDevice },
    { "PcRegisterAdapterPowerManagement",   (PVOID)pc_RegisterAdapterPowerManagement },
    { "PcNewPort",                          (PVOID)pc_NewPort },
    { "PcNewMiniport",                      (PVOID)pc_NewMiniport },
    { "PcRegisterSubdevice",                (PVOID)pc_RegisterSubdevice },
    { "PcRegisterPhysicalConnection",       (PVOID)pc_RegisterPhysicalConnection },
    { "PcNewResourceList",                  (PVOID)pc_NewResourceList },
    { "PcNewResourceSublist",               (PVOID)pc_NewResourceSublist },
    { "PcNewInterruptSync",                 (PVOID)pc_NewInterruptSync },
    { "PcNewDmaChannel",                    (PVOID)pc_NewDmaChannel },
    { "PcNewServiceGroup",                  (PVOID)pc_NewServiceGroup },
    { "PcCompletePendingPropertyRequest",   (PVOID)pc_CompletePendingPropertyRequest },
    { "PcGetTimeInterval",                  (PVOID)pc_GetTimeInterval },
    { "PcNewRegistryKey",                   (PVOID)pc_NewRegistryKey },
    { "PcRegisterIoTimeout",                (PVOID)pc_RegisterIoTimeout },
    { "PcUnregisterIoTimeout",              (PVOID)pc_UnregisterIoTimeout },
    { "PcDispatchIrp",                      (PVOID)pc_DispatchIrp_2p },
    { "PcForwardIrpSynchronous",            (PVOID)pc_ForwardIrpSynchronous },
    { "PcRequestNewPowerState",             (PVOID)pc_RequestNewPowerState },
    { "PcGetDeviceProperty",                (PVOID)pc_GetDeviceProperty },
    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Registration
 * ================================================================ */

static PORT_DRIVER_SHIM portcls_shim = {
    "PORTCLS.SYS",
    portcls_funcs,
    0,
    NULL, NULL, NULL
};

void audio_shim_register(void)
{
    VxD_Debug_Printf("PC: Registering PORTCLS.SYS shim\r\n");
    register_port_driver(&portcls_shim);
}

/* ================================================================
 * Test Entrypoint
 *
 * Exercises the PortCls shim without a real miniport driver:
 *   1. Register PORTCLS.SYS shim
 *   2. Test PcInitializeAdapterDriver (stores callback)
 *   3. Test PcNewPort (returns IPortWaveCyclic with vtable)
 *   4. Test vtable methods (QueryInterface, Init, Notify)
 *   5. Test PcNewDmaChannel, PcNewServiceGroup, PcNewResourceList
 *   6. Test PcRegisterSubdevice
 * ================================================================ */

/* Fake AddDevice callback for testing */
static NTSTATUS __stdcall test_AddDevice(PVOID DriverObject, PVOID PhysDevObj)
{
    VxD_Debug_Printf("PC: TEST AddDevice callback invoked!\r\n");
    return STATUS_SUCCESS;
}

int audio_test(void)
{
    NTSTATUS status;
    PVOID port_out = NULL;
    PVOID dma_out = NULL;
    PVOID sg_out = NULL;
    PVOID rl_out = NULL;
    ULONG qi_result;

    /* Vtable method pointer types for testing */
    typedef ULONG (__stdcall *PFN_QI)(PVOID, PVOID, PVOID*);
    typedef ULONG (__stdcall *PFN_INIT6)(PVOID, PVOID, PVOID, PVOID, PVOID, PVOID);
    typedef ULONG (__stdcall *PFN_NOTIFY)(PVOID, PVOID);

    IPortWaveCyclicVtbl *vtbl;

    VxD_Debug_Printf("PC: === audio_test BEGIN ===\r\n");

    /* Step 1: Register shim */
    audio_shim_register();

    /* Step 2: PcInitializeAdapterDriver */
    status = pc_InitializeAdapterDriver(
        (PVOID)0xDEAD0001, (PVOID)0xDEAD0002,
        (PVOID)test_AddDevice, NULL);
    if (status != STATUS_SUCCESS) {
        VxD_Debug_Printf("PC: FAIL PcInitializeAdapterDriver\r\n");
        return -1;
    }
    if (g_audio_adapter.AddDeviceCallback != (PVOID)test_AddDevice) {
        VxD_Debug_Printf("PC: FAIL AddDevice not stored\r\n");
        return -2;
    }
    VxD_Debug_Printf("PC: PASS PcInitializeAdapterDriver\r\n");

    /* Step 3: PcNewPort */
    status = pc_NewPort(&port_out, NULL, NULL);
    if (status != STATUS_SUCCESS || !port_out) {
        VxD_Debug_Printf("PC: FAIL PcNewPort\r\n");
        return -3;
    }
    VxD_Debug_Printf("PC: PASS PcNewPort\r\n");

    /* Step 4: Vtable method calls */
    vtbl = ((struct { IPortWaveCyclicVtbl *lpVtbl; } *)port_out)->lpVtbl;

    /* QueryInterface */
    {
        PVOID qiout = NULL;
        PFN_QI fnQI = (PFN_QI)vtbl->QueryInterface;
        qi_result = fnQI(port_out, NULL, &qiout);
        if (qi_result != S_OK || qiout != port_out) {
            VxD_Debug_Printf("PC: FAIL QueryInterface\r\n");
            return -4;
        }
    }
    VxD_Debug_Printf("PC: PASS QueryInterface\r\n");

    /* Init */
    {
        PFN_INIT6 fnInit = (PFN_INIT6)vtbl->Init;
        qi_result = fnInit(port_out, NULL, NULL, NULL, NULL, NULL);
        if (qi_result != S_OK) {
            VxD_Debug_Printf("PC: FAIL Init\r\n");
            return -5;
        }
    }
    VxD_Debug_Printf("PC: PASS Init\r\n");

    /* Notify */
    {
        PFN_NOTIFY fnNotify = (PFN_NOTIFY)vtbl->Notify;
        qi_result = fnNotify(port_out, NULL);
        if (qi_result != S_OK) {
            VxD_Debug_Printf("PC: FAIL Notify\r\n");
            return -6;
        }
    }
    VxD_Debug_Printf("PC: PASS Notify\r\n");

    /* Step 5: PcNewDmaChannel */
    status = pc_NewDmaChannel(&dma_out, NULL, 0, NULL);
    if (status != STATUS_SUCCESS || !dma_out) {
        VxD_Debug_Printf("PC: FAIL PcNewDmaChannel\r\n");
        return -7;
    }
    VxD_Debug_Printf("PC: PASS PcNewDmaChannel\r\n");

    /* PcNewServiceGroup */
    status = pc_NewServiceGroup(&sg_out, NULL);
    if (status != STATUS_SUCCESS || !sg_out) {
        VxD_Debug_Printf("PC: FAIL PcNewServiceGroup\r\n");
        return -8;
    }
    VxD_Debug_Printf("PC: PASS PcNewServiceGroup\r\n");

    /* PcNewResourceList */
    status = pc_NewResourceList(&rl_out, NULL, 0, NULL);
    if (status != STATUS_SUCCESS || !rl_out) {
        VxD_Debug_Printf("PC: FAIL PcNewResourceList\r\n");
        return -9;
    }
    VxD_Debug_Printf("PC: PASS PcNewResourceList\r\n");

    /* Step 6: PcRegisterSubdevice */
    status = pc_RegisterSubdevice(NULL, NULL, NULL, NULL);
    if (status != STATUS_SUCCESS) {
        VxD_Debug_Printf("PC: FAIL PcRegisterSubdevice\r\n");
        return -10;
    }
    VxD_Debug_Printf("PC: PASS PcRegisterSubdevice\r\n");

    VxD_Debug_Printf("PC: === audio_test PASS ===\r\n");
    return 0;
}
