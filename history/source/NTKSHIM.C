/*
 * NTKSHIM.C - NT Kernel / HAL Shim Implementation for Win9x VxD
 *
 * Implements ntoskrnl.exe and HAL.dll functions on top of Win98 VMM
 * services, so that NT5 kernel-mode drivers can run inside a VxD.
 *
 * Target environment: ring 0, single CPU, Win98 VMM. Many NT
 * primitives that exist to handle SMP and preemption collapse to
 * trivial operations here.
 *
 * Build: wcc386 -bt=windows -3s -s -zl -d0 -i=. NTKSHIM.C
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#include "NTKSHIM.H"
#include "IRPMGR.H"
#include "PORTIO.H"

#ifndef IRP_SYNCHRONOUS_API
#define IRP_SYNCHRONOUS_API     0x00000004
#endif
#ifndef IRP_BUFFERED_IO
#define IRP_BUFFERED_IO         0x00000010
#endif
#ifndef IRP_READ_OPERATION
#define IRP_READ_OPERATION      0x00000100
#endif
#ifndef IRP_WRITE_OPERATION
#define IRP_WRITE_OPERATION     0x00000080
#endif

/* ================================================================
 * VxD WRAPPER EXTERNALS
 *
 * Provided by VXDWRAP.ASM (or equivalent assembly layer).
 * ================================================================ */

extern PVOID __cdecl VxD_HeapAllocate(ULONG size, ULONG flags);
extern void  __cdecl VxD_HeapFree(PVOID ptr, ULONG flags);
extern void  __cdecl VxD_Debug_Printf(const char *fmt, ...);
extern ULONG __cdecl VxD_SetTimer(ULONG milliseconds, PVOID callback,
                                   PVOID refdata);
extern void  __cdecl VxD_CancelTimer(ULONG handle);
extern ULONG __cdecl VxD_GetSystemTime(void);   /* VMM Get_System_Time, ms */
extern void  __cdecl VxD_TimesliceSleep(void);   /* VMM Time_Slice_Sleep */
extern void  __cdecl dbg_mark(char c);
extern int   g_nt5_trace_ports;

static VOID ntk_mark_hex8(UCHAR value)
{
    static const CHAR hex[] = "0123456789ABCDEF";
    dbg_mark(hex[(value >> 4) & 0x0F]);
    dbg_mark(hex[value & 0x0F]);
}

static VOID ntk_mark_hex32(ULONG value)
{
    ntk_mark_hex8((UCHAR)((value >> 24) & 0xFF));
    ntk_mark_hex8((UCHAR)((value >> 16) & 0xFF));
    ntk_mark_hex8((UCHAR)((value >> 8) & 0xFF));
    ntk_mark_hex8((UCHAR)(value & 0xFF));
}

static ULONG g_ntk_port_trace_count = 0;

static VOID ntk_trace_port(char op, ULONG port, ULONG value)
{
    if (g_nt5_trace_ports != 2 || g_ntk_port_trace_count >= 220) {
        return;
    }

    dbg_mark('~');
    dbg_mark(op);
    ntk_mark_hex32(port);
    ntk_mark_hex8((UCHAR)(value & 0xFF));
    g_ntk_port_trace_count++;
}

static VOID ntk_DebugPrintWstr(PCWSTR s)
{
    CHAR buf[64];
    ULONG i;

    if (!s) {
        VxD_Debug_Printf("(null)");
        return;
    }

    i = 0;
    while (s[i] && i < sizeof(buf) - 1) {
        WCHAR c;

        c = s[i];
        buf[i] = (c >= 0x20 && c < 0x7F) ? (CHAR)c : '?';
        i++;
    }
    buf[i] = 0;
    VxD_Debug_Printf(buf);
}

/* VPICD services for interrupt management */
extern ULONG __cdecl VxD_VPICD_Virtualize_IRQ(PVOID desc);
extern void  __cdecl VxD_VPICD_ForceDefaultBehavior(ULONG handle);
extern void  __cdecl VxD_VPICD_PhysicalEOI(ULONG handle);

/* VxD_MapPhysToLinear for memory-mapped I/O */
extern PVOID __cdecl VxD_MapPhysToLinear(ULONG physAddr, ULONG size,
                                          ULONG flags);

/* Heap flags */
#define HEAPF_ZEROINIT  0x0001

/* ================================================================
 * INTERNAL STATE
 * ================================================================ */

/* Current (notional) IRQL. Real synchronisation is cli/sti. */
static KIRQL g_CurrentIrql = PASSIVE_LEVEL;
static volatile int g_ntk_force_device_queue_start = 1;

/* DPC queue: simple fixed-size array */
static PKDPC g_DpcQueue[NTK_MAX_PENDING_DPCS];
static ULONG g_DpcCount = 0;

/* ================================================================
 * EXPORTED VARIABLES
 * ================================================================ */

PVOID ntk_MmHighestUserAddress = (PVOID)0x7FFEFFFF;
ULONG ntk_NlsMbCodePageTag = 0;    /* single-byte codepage */
ULONG ntk_InitSafeBootMode = 0;    /* not safe mode */
ULONG ntk_KeTickCount = 0;         /* updated periodically */

/* Static configuration information for IoGetConfigurationInformation */
static CONFIGURATION_INFORMATION g_ConfigInfo;
static BOOLEAN g_ConfigInfoInit = FALSE;

/* Driver object extension storage (single driver scenario) */
static PVOID g_DriverObjExt = NULL;
static PVOID g_DriverObjExtId = NULL;
static ULONG g_DriverObjExtSize = 0;

static DRIVER_OBJECT    g_ntk_legacy_pdo_driver;
static DRIVER_EXTENSION g_ntk_legacy_pdo_driver_ext;
static BOOLEAN          g_ntk_legacy_pdo_driver_init = FALSE;

static NTSTATUS NTAPI ntk_legacy_pdo_dispatch(PDEVICE_OBJECT DeviceObject,
                                                PIRP Irp)
{
    static const char hex[] = "0123456789ABCDEF";
    PUCHAR irp_sp;
    UCHAR major;
    UCHAR minor;
    PUCHAR caps;
    NTSTATUS status;

    (void)DeviceObject;

    dbg_mark('V');
    if (!Irp) {
        return 0xC000000DUL;
    }

    status = 0xC00000BBUL;
    irp_sp = (PUCHAR)Irp->Tail.Overlay.CurrentStackLocation;
    major = 0xFF;
    minor = 0xFF;
    if (irp_sp) {
        major = irp_sp[0];
        minor = irp_sp[1];
        dbg_mark('p');
        dbg_mark(hex[(major >> 4) & 0x0F]);
        dbg_mark(hex[major & 0x0F]);
        dbg_mark(hex[(minor >> 4) & 0x0F]);
        dbg_mark(hex[minor & 0x0F]);
    }

    if (major == IRP_MJ_POWER) {
        status = STATUS_SUCCESS;
    } else if (major == IRP_MJ_PNP) {
        if (minor == 0x00) {
            status = STATUS_SUCCESS;
        } else if (minor == 0x09) {
            caps = *(PUCHAR *)(irp_sp + 0x04);
            if (caps) {
                caps[0] = 0x40;
                caps[1] = 0x00;
                caps[2] = 0x01;
                caps[3] = 0x00;
                *(ULONG *)(caps + 0x08) = 0xFFFFFFFFUL;
                *(ULONG *)(caps + 0x0C) = 0xFFFFFFFFUL;
                status = STATUS_SUCCESS;
            } else {
                status = 0xC000000DUL;
            }
        } else if (minor == 0x13) {
            status = 0xC00000BBUL;
        }
    }

    if (status == STATUS_SUCCESS) {
        Irp->IoStatus.Information = 0;
    }
    Irp->IoStatus.Status = status;
    if (Irp->UserIosb) {
        Irp->UserIosb->Status = status;
        Irp->UserIosb->Information = Irp->IoStatus.Information;
    }
    if (Irp->UserEvent) {
        KeSetEvent(Irp->UserEvent, 0, FALSE);
    }
    return status;
}

static void ntk_init_legacy_pdo_driver(void)
{
    ULONG i;

    if (g_ntk_legacy_pdo_driver_init) {
        return;
    }

    RtlZeroMemory(&g_ntk_legacy_pdo_driver, sizeof(g_ntk_legacy_pdo_driver));
    RtlZeroMemory(&g_ntk_legacy_pdo_driver_ext,
                  sizeof(g_ntk_legacy_pdo_driver_ext));
    g_ntk_legacy_pdo_driver.Type = 4;
    g_ntk_legacy_pdo_driver.Size = sizeof(DRIVER_OBJECT);
    g_ntk_legacy_pdo_driver.DriverExtension = &g_ntk_legacy_pdo_driver_ext;
    g_ntk_legacy_pdo_driver_ext.DriverObject = &g_ntk_legacy_pdo_driver;
    for (i = 0; i < IRP_MJ_MAXIMUM; i++) {
        g_ntk_legacy_pdo_driver.MajorFunction[i] = ntk_legacy_pdo_dispatch;
    }
    g_ntk_legacy_pdo_driver_init = TRUE;
}

PIRP NTAPI ntk_IoAllocateIrp(CHAR StackSize, BOOLEAN ChargeQuota)
{
    dbg_mark('n');
    return IrpMgr_IoAllocateIrp(StackSize, ChargeQuota);
}

VOID NTAPI ntk_IoFreeIrp(PIRP Irp)
{
    dbg_mark('x');
    IrpMgr_IoFreeIrp(Irp);
}

NTSTATUS NTAPI ntk_IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    return IrpMgr_IoCallDriver(DeviceObject, Irp);
}

VOID NTAPI ntk_IoCompleteRequest(PIRP Irp, CHAR PriorityBoost)
{
    IrpMgr_IoCompleteRequest(Irp, PriorityBoost);
}

NTSTATUS NTAPI ntk_IoCreateDevice(PDRIVER_OBJECT DriverObject,
                                   ULONG ExtensionSize,
                                   PUNICODE_STRING DeviceName,
                                   ULONG DeviceType,
                                   ULONG Characteristics,
                                   BOOLEAN Exclusive,
                                   PDEVICE_OBJECT *DeviceObject)
{
    return IrpMgr_IoCreateDevice(DriverObject,
                                 ExtensionSize,
                                 DeviceName,
                                 DeviceType,
                                 Characteristics,
                                 Exclusive,
                                 DeviceObject);
}

VOID NTAPI ntk_IoDeleteDevice(PDEVICE_OBJECT DeviceObject)
{
    IrpMgr_IoDeleteDevice(DeviceObject);
}

PDEVICE_OBJECT NTAPI ntk_IoAttachDeviceToDeviceStack(
    PDEVICE_OBJECT SourceDevice, PDEVICE_OBJECT TargetDevice)
{
    return IrpMgr_IoAttachDeviceToDeviceStack(SourceDevice, TargetDevice);
}

VOID NTAPI ntk_IoDetachDevice(PDEVICE_OBJECT TargetDevice)
{
    IrpMgr_IoDetachDevice(TargetDevice);
}

PDEVICE_OBJECT NTAPI ntk_IoGetAttachedDeviceReference(
    PDEVICE_OBJECT DeviceObject)
{
    return IrpMgr_IoGetAttachedDeviceReference(DeviceObject);
}


/* ================================================================
 * SPINLOCKS
 *
 * Win98 is single-CPU and non-preemptive at ring 0, so spinlocks
 * reduce to interrupt enable/disable. We save/restore EFLAGS to
 * handle nested acquire correctly.
 * ================================================================ */

VOID NTAPI KeInitializeSpinLock(PKSPIN_LOCK SpinLock)
{
    if (SpinLock) {
        *SpinLock = 0;
    }
}

VOID NTAPI KeAcquireSpinLock(PKSPIN_LOCK SpinLock, PKIRQL OldIrql)
{
    ULONG flags;

    PORT_SAVE_FLAGS_CLI(flags);

    if (OldIrql) {
        *OldIrql = g_CurrentIrql;
    }
    g_CurrentIrql = DISPATCH_LEVEL;

    if (SpinLock) {
        *SpinLock = flags;
    }
}

VOID NTAPI KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql)
{
    ULONG flags = 0;

    if (SpinLock) {
        flags = *SpinLock;
    }

    g_CurrentIrql = NewIrql;
    PORT_RESTORE_FLAGS(flags);
}

/*
 * At DPC level we are already above DISPATCH_LEVEL conceptually,
 * so just cli without saving previous IRQL.
 */
VOID NTAPI KeAcquireSpinLockAtDpcLevel(PKSPIN_LOCK SpinLock)
{
    PORT_CLI();
    if (SpinLock) {
        *SpinLock = 1; /* mark held */
    }
}

VOID NTAPI KeReleaseSpinLockFromDpcLevel(PKSPIN_LOCK SpinLock)
{
    if (SpinLock) {
        *SpinLock = 0;
    }
    PORT_STI();
}


/* ================================================================
 * DPC (Deferred Procedure Calls)
 *
 * On NT, DPCs run at DISPATCH_LEVEL after an ISR completes. On
 * our single-CPU Win9x VxD the safest approach is to queue them
 * and drain the queue after interrupt handling, or run inline
 * when queued outside interrupt context.
 * ================================================================ */

VOID NTAPI KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine,
                           PVOID DeferredContext)
{
    if (!Dpc) return;

    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
    Dpc->SystemArgument1 = NULL;
    Dpc->SystemArgument2 = NULL;
    Dpc->Queued = FALSE;
}

BOOLEAN NTAPI KeInsertQueueDpc(PKDPC Dpc, PVOID Arg1, PVOID Arg2)
{
    if (!Dpc || !Dpc->DeferredRoutine) {
        return FALSE;
    }

    if (Dpc->Queued) {
        return FALSE;  /* already queued */
    }

    Dpc->SystemArgument1 = Arg1;
    Dpc->SystemArgument2 = Arg2;

    /*
     * If we are at PASSIVE or APC level, run the DPC inline.
     * This is safe on single-CPU Win9x where ring 0 code is
     * non-preemptive.
     */
    if (g_CurrentIrql < DISPATCH_LEVEL) {
        KIRQL oldIrql = g_CurrentIrql;
        g_CurrentIrql = DISPATCH_LEVEL;

        Dpc->DeferredRoutine(Dpc, Dpc->DeferredContext,
                             Dpc->SystemArgument1,
                             Dpc->SystemArgument2);

        g_CurrentIrql = oldIrql;
        return TRUE;
    }

    /* At DISPATCH or higher (ISR context): queue for later drain */
    if (g_DpcCount < NTK_MAX_PENDING_DPCS) {
        g_DpcQueue[g_DpcCount++] = Dpc;
        Dpc->Queued = TRUE;
        return TRUE;
    }

    VxD_Debug_Printf("NTK: DPC queue full, dropping DPC 0x%08lX\n",
                     (ULONG)Dpc);
    return FALSE;
}

/*
 * ntk_DrainDpcQueue - Call after ISR processing to run queued DPCs.
 * Called from the interrupt wrapper in VXDWRAP.ASM.
 */
VOID __cdecl ntk_DrainDpcQueue(void)
{
    ULONG i;
    KIRQL oldIrql;

    if (g_DpcCount == 0) {
        return;
    }

    oldIrql = g_CurrentIrql;
    g_CurrentIrql = DISPATCH_LEVEL;

    for (i = 0; i < g_DpcCount; i++) {
        PKDPC dpc = g_DpcQueue[i];
        if (dpc && dpc->DeferredRoutine) {
            dpc->Queued = FALSE;
            dpc->DeferredRoutine(dpc, dpc->DeferredContext,
                                 dpc->SystemArgument1,
                                 dpc->SystemArgument2);
        }
        g_DpcQueue[i] = NULL;
    }

    g_DpcCount = 0;
    g_CurrentIrql = oldIrql;
}

KIRQL __cdecl ntk_EnterManualIsrIrql(void)
{
    KIRQL oldIrql;

    oldIrql = g_CurrentIrql;
    g_CurrentIrql = DIRQL;
    return oldIrql;
}

VOID __cdecl ntk_LeaveManualIsrIrql(KIRQL OldIrql)
{
    g_CurrentIrql = OldIrql;
}


/* ================================================================
 * TIMERS
 *
 * Map NT kernel timers onto VMM Set_Global_Time_Out / Cancel_Time_Out
 * via the VxD wrapper layer.
 * ================================================================ */

/*
 * Timer callback trampoline. VMM calls us with the refdata pointer
 * which is our KTIMER. We fire the associated DPC if present.
 */
static void __cdecl ntk_TimerCallback(PVOID refdata)
{
    PKTIMER timer = (PKTIMER)refdata;

    if (!timer) return;

    timer->Active = FALSE;
    timer->Signaled = TRUE;
    timer->ShimTimerHandle = 0;

    if (timer->Dpc && timer->Dpc->DeferredRoutine) {
        KeInsertQueueDpc(timer->Dpc, NULL, NULL);
    }
}

VOID NTAPI KeInitializeTimer(PKTIMER Timer)
{
    if (!Timer) return;

    Timer->Signaled = FALSE;
    Timer->Dpc = NULL;
    Timer->ShimTimerHandle = 0;
    Timer->Active = FALSE;
}

BOOLEAN NTAPI KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc)
{
    BOOLEAN wasActive;
    ULONG ms;

    if (!Timer) return FALSE;

    wasActive = Timer->Active;

    /* Cancel any existing timer */
    if (Timer->Active && Timer->ShimTimerHandle) {
        VxD_CancelTimer(Timer->ShimTimerHandle);
        Timer->ShimTimerHandle = 0;
        Timer->Active = FALSE;
    }

    Timer->Dpc = Dpc;
    Timer->Signaled = FALSE;

    /*
     * DueTime interpretation:
     *   Negative = relative (in 100ns units)
     *   Positive = absolute (we convert to relative)
     *
     * Convert 100ns units to milliseconds. For small values
     * that round to zero, use a minimum of 1 ms.
     */
    if (DueTime.QuadPart < 0) {
        /* Relative: negate and convert 100ns -> ms */
        long long rel = -DueTime.QuadPart;
        ms = (ULONG)(rel / 10000);
    } else {
        /*
         * Absolute: compute relative from current system time.
         * VMM Get_System_Time returns milliseconds since boot.
         * NT absolute time is 100ns since 1601. We approximate
         * by treating the low bits as ms offset from now.
         */
        ULONG now_ms = VxD_GetSystemTime();
        ULONG target_ms = (ULONG)(DueTime.QuadPart / 10000);
        if (target_ms > now_ms) {
            ms = target_ms - now_ms;
        } else {
            ms = 0;
        }
    }

    if (ms == 0) ms = 1;  /* VMM needs at least 1 ms */

    Timer->ShimTimerHandle = VxD_SetTimer(ms,
                                          (PVOID)ntk_TimerCallback,
                                          (PVOID)Timer);
    if (Timer->ShimTimerHandle) {
        Timer->Active = TRUE;
    } else {
        VxD_Debug_Printf("NTK: VxD_SetTimer failed for timer 0x%08lX\n",
                         (ULONG)Timer);
    }

    return wasActive;
}

BOOLEAN NTAPI KeCancelTimer(PKTIMER Timer)
{
    BOOLEAN wasActive;

    if (!Timer) return FALSE;

    wasActive = Timer->Active;

    if (Timer->Active && Timer->ShimTimerHandle) {
        VxD_CancelTimer(Timer->ShimTimerHandle);
        Timer->ShimTimerHandle = 0;
        Timer->Active = FALSE;
    }

    return wasActive;
}

VOID NTAPI KeQuerySystemTime(PLARGE_INTEGER CurrentTime)
{
    ULONG ms;

    if (!CurrentTime) return;

    /*
     * VMM Get_System_Time returns milliseconds since boot.
     * NT KeQuerySystemTime returns 100ns units since Jan 1, 1601.
     * We return ms * 10000 as a relative-from-boot approximation.
     * Drivers that need wall-clock time will get wrong answers,
     * but timeout and interval calculations will be correct.
     */
    ms = VxD_GetSystemTime();
    CurrentTime->QuadPart = (long long)ms * 10000LL;
}


/* ================================================================
 * I/O TIMER
 *
 * IoInitializeTimer stores the routine on the device object.
 * IoStartTimer begins a 1-second periodic callback.
 * In our VxD, we implement with a VMM timer that re-arms itself.
 * ================================================================ */

/*
 * Internal struct to hold I/O timer state. We store on the device
 * object's Timer field (which is PVOID in the structure).
 */
typedef struct _NTK_IO_TIMER {
    PIO_TIMER_ROUTINE   Routine;
    PVOID               Context;
    PDEVICE_OBJECT      DeviceObject;
    ULONG               VmmTimerHandle;
    BOOLEAN             Active;
} NTK_IO_TIMER, *PNTK_IO_TIMER;

/* Static storage for I/O timer (single device scenario) */
static PNTK_IO_TIMER g_IoTimer = NULL;

static void __cdecl ntk_IoTimerCallback(PVOID refdata)
{
    PNTK_IO_TIMER iot = (PNTK_IO_TIMER)refdata;
    if (!iot || !iot->Active) return;

    /* Call the driver's timer routine */
    if (iot->Routine) {
        iot->Routine(iot->DeviceObject, iot->Context);
    }

    /* Re-arm for another second */
    if (iot->Active) {
        iot->VmmTimerHandle = VxD_SetTimer(1000,
            (PVOID)ntk_IoTimerCallback, (PVOID)iot);
    }
}

VOID NTAPI ntk_IoInitializeTimer(PDEVICE_OBJECT DeviceObject,
                                  PIO_TIMER_ROUTINE TimerRoutine,
                                  PVOID Context)
{
    PNTK_IO_TIMER iot;

    if (!DeviceObject || !TimerRoutine) return;

    iot = (PNTK_IO_TIMER)VxD_HeapAllocate(sizeof(NTK_IO_TIMER),
                                            HEAPF_ZEROINIT);
    if (!iot) {
        VxD_Debug_Printf("NTK: IoInitializeTimer alloc failed\n");
        return;
    }

    iot->Routine = TimerRoutine;
    iot->Context = Context;
    iot->DeviceObject = DeviceObject;
    iot->VmmTimerHandle = 0;
    iot->Active = FALSE;

    /*
     * Store on DeviceObject. We don't have the full DEVICE_OBJECT
     * struct here (it lives in IRPMGR.H), but the Timer field is
     * at a known offset. We use a PVOID cast.
     */
    /* DeviceObject->Timer = (PVOID)iot; */
    /* Since we can't dereference the opaque type, store in a simple
     * mapping. For single-device scenarios this is sufficient. */
    VxD_Debug_Printf("NTK: IoInitializeTimer set up for DevObj 0x%08lX\n",
                     (ULONG)DeviceObject);

    /* Store the timer pointer at a well-known location: we reuse
     * this via IoStartTimer. For now, store as a static. For
     * multi-device support this would need a lookup table. */
    g_IoTimer = iot;
}

VOID NTAPI ntk_IoStartTimer(PDEVICE_OBJECT DeviceObject)
{
    (void)DeviceObject;

    if (!g_IoTimer) {
        VxD_Debug_Printf("NTK: IoStartTimer called without IoInitializeTimer\n");
        return;
    }

    if (g_IoTimer->Active) return;  /* already running */

    g_IoTimer->Active = TRUE;
    g_IoTimer->VmmTimerHandle = VxD_SetTimer(1000,
        (PVOID)ntk_IoTimerCallback, (PVOID)g_IoTimer);

    VxD_Debug_Printf("NTK: IoStartTimer armed 1s periodic timer\n");
}


/* ================================================================
 * EVENTS
 *
 * Simple flag-based implementation. On single-CPU Win9x we cannot
 * truly block, so KeWaitForSingleObject busy-waits with
 * Time_Slice_Sleep to yield the CPU.
 * ================================================================ */

VOID NTAPI KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State)
{
    dbg_mark('N');
    if (!Event) return;

    Event->Type = Type;
    Event->SignalState = State ? 1 : 0;
    dbg_mark('n');
}

VOID __cdecl ntk_KeInitializeEvent_cdecl(PKEVENT Event, ULONG Type, ULONG State)
{
    KeInitializeEvent(Event, (EVENT_TYPE)Type, (BOOLEAN)State);
}

LONG NTAPI KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait)
{
    LONG previousState;

    (void)Increment;
    (void)Wait;

    dbg_mark('T');
    if (!Event) return 0;

    previousState = Event->SignalState;
    Event->SignalState = 1;

    dbg_mark('t');
    return previousState;
}

LONG __cdecl ntk_KeSetEvent_cdecl(PKEVENT Event, LONG Increment, ULONG Wait)
{
    return KeSetEvent(Event, Increment, (BOOLEAN)Wait);
}

VOID NTAPI KeResetEvent(PKEVENT Event)
{
    if (!Event) return;

    Event->SignalState = 0;
}

NTSTATUS NTAPI KeWaitForSingleObject(PVOID Object, ULONG WaitReason,
                                     ULONG WaitMode, BOOLEAN Alertable,
                                     PLARGE_INTEGER Timeout)
{
    PKEVENT event;
    ULONG timeout_ms;
    ULONG start_ms;
    ULONG elapsed;

    (void)WaitReason;
    (void)WaitMode;
    (void)Alertable;

    dbg_mark('W');
    if (!Object) {
        dbg_mark('w');
        return STATUS_UNSUCCESSFUL;
    }

    event = (PKEVENT)Object;

    /* If already signaled, consume and return immediately */
    if (event->SignalState) {
        if (event->Type == SynchronizationEvent) {
            event->SignalState = 0;  /* auto-reset */
        }
        dbg_mark('w');
        return STATUS_SUCCESS;
    }

    /* NULL timeout = wait forever (dangerous in VxD, but honor it) */
    if (!Timeout) {
        timeout_ms = 0xFFFFFFFF; /* ~49 days, effectively forever */
    } else if (Timeout->QuadPart == 0) {
        /* Zero timeout = poll only */
        dbg_mark('w');
        return STATUS_TIMEOUT;
    } else if (Timeout->QuadPart < 0) {
        /* Relative, 100ns units */
        timeout_ms = (ULONG)((-Timeout->QuadPart) / 10000);
        if (timeout_ms == 0) timeout_ms = 1;
    } else {
        /* Absolute: approximate */
        ULONG now = VxD_GetSystemTime();
        ULONG target = (ULONG)(Timeout->QuadPart / 10000);
        timeout_ms = (target > now) ? (target - now) : 0;
        if (timeout_ms == 0) {
            dbg_mark('w');
            return STATUS_TIMEOUT;
        }
    }

    /*
     * Busy-wait loop. We yield the CPU with Time_Slice_Sleep on
     * each iteration. This is crude but workable for the short
     * waits that NT drivers typically perform at ring 0.
     */
    start_ms = VxD_GetSystemTime();

    while (!event->SignalState) {
        VxD_TimesliceSleep();

        elapsed = VxD_GetSystemTime() - start_ms;
        if (elapsed >= timeout_ms) {
            dbg_mark('w');
            return STATUS_TIMEOUT;
        }
    }

    /* Consume the signal for synchronization events */
    if (event->Type == SynchronizationEvent) {
        event->SignalState = 0;
    }

    dbg_mark('w');
    return STATUS_SUCCESS;
}

NTSTATUS __cdecl ntk_KeWaitForSingleObject_cdecl(PVOID Object, ULONG WaitReason,
                                                 ULONG WaitMode, ULONG Alertable,
                                                 PLARGE_INTEGER Timeout)
{
    return KeWaitForSingleObject(Object, WaitReason, WaitMode,
                                 (BOOLEAN)Alertable, Timeout);
}


/* ================================================================
 * POOL ALLOCATION
 *
 * Map NT pool allocators onto the VxD heap. Win9x VxD heap memory
 * is always non-paged and locked, so PagedPool and NonPagedPool
 * behave identically.
 * ================================================================ */

PVOID NTAPI ExAllocatePoolWithTag(ULONG PoolType, SIZE_T Size, ULONG Tag)
{
    PVOID ptr;

    (void)PoolType;
    (void)Tag;

    dbg_mark('E');
    if (Size == 0) return NULL;

    ptr = VxD_HeapAllocate(Size, HEAPF_ZEROINIT);
    if (!ptr) {
        VxD_Debug_Printf("NTK: ExAllocatePoolWithTag failed, size=%lu tag=0x%08lX\n",
                         (ULONG)Size, Tag);
    }
    return ptr;
}

VOID NTAPI ExFreePoolWithTag(PVOID Ptr, ULONG Tag)
{
    (void)Tag;

    if (Ptr) {
        dbg_mark('k');
        VxD_HeapFree(Ptr, 0);
    }
}

VOID __cdecl ntk_ExFreePool_cdecl(PVOID Ptr)
{
    dbg_mark('j');
    ExFreePoolWithTag(Ptr, 0);
    dbg_mark('J');
}

VOID NTAPI ExFreePool(PVOID Ptr)
{
    ntk_ExFreePool_cdecl(Ptr);
}

PVOID NTAPI ExAllocatePool(ULONG PoolType, SIZE_T Size)
{
    return ExAllocatePoolWithTag(PoolType, Size, 0);
}


/* ================================================================
 * REGISTRY STUBS
 *
 * NT drivers often probe the registry during initialisation. We
 * provide stubs that let the driver proceed without real registry
 * access. Reads return "not found", writes silently succeed.
 * ================================================================ */

NTSTATUS NTAPI ZwOpenKey(PVOID KeyHandle, ULONG DesiredAccess,
                         PVOID ObjectAttributes)
{
    PVOID *pHandle;

    (void)DesiredAccess;
    (void)ObjectAttributes;

    VxD_Debug_Printf("NTK: ZwOpenKey stub called\n");

    if (KeyHandle) {
        pHandle = (PVOID *)KeyHandle;
        *pHandle = (PVOID)0xDEAD0001UL;  /* fake handle */
    }
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ZwQueryValueKey(PVOID KeyHandle, PVOID ValueName,
                               ULONG KeyValueInformationClass,
                               PVOID KeyValueInformation,
                               ULONG Length, PULONG ResultLength)
{
    (void)KeyHandle;
    (void)ValueName;
    (void)KeyValueInformationClass;
    (void)KeyValueInformation;
    (void)Length;

    VxD_Debug_Printf("NTK: ZwQueryValueKey stub, returning NAME_NOT_FOUND\n");

    if (ResultLength) {
        *ResultLength = 0;
    }
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

NTSTATUS NTAPI ZwSetValueKey(PVOID KeyHandle, PVOID ValueName,
                             ULONG TitleIndex, ULONG Type,
                             PVOID Data, ULONG DataSize)
{
    (void)KeyHandle;
    (void)ValueName;
    (void)TitleIndex;
    (void)Type;
    (void)Data;
    (void)DataSize;

    VxD_Debug_Printf("NTK: ZwSetValueKey stub, silently succeeding\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ZwClose(PVOID Handle)
{
    (void)Handle;

    dbg_mark('Z');
    /* No-op: fake handles need no cleanup */
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_ZwCreateKey(PVOID KeyHandle, ULONG DesiredAccess,
                                PVOID ObjectAttributes, ULONG TitleIndex,
                                PVOID Class, ULONG CreateOptions,
                                PULONG Disposition)
{
    PVOID *pHandle;

    (void)DesiredAccess;
    (void)ObjectAttributes;
    (void)TitleIndex;
    (void)Class;
    (void)CreateOptions;

    VxD_Debug_Printf("NTK: ZwCreateKey stub called\n");

    if (KeyHandle) {
        pHandle = (PVOID *)KeyHandle;
        *pHandle = (PVOID)0xDEAD0002UL;
    }
    if (Disposition) {
        *Disposition = 1;  /* REG_CREATED_NEW_KEY */
    }
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_ZwCreateDirectoryObject(PVOID DirHandle, ULONG Access,
                                            PVOID ObjectAttributes)
{
    PVOID *pHandle;

    (void)Access;
    (void)ObjectAttributes;

    VxD_Debug_Printf("NTK: ZwCreateDirectoryObject stub called\n");

    if (DirHandle) {
        pHandle = (PVOID *)DirHandle;
        *pHandle = (PVOID)0xDEAD0003UL;
    }
    return STATUS_SUCCESS;
}


/* ================================================================
 * HAL I/O PORT ACCESS
 *
 * Direct x86 IN/OUT instructions. On Win9x VxD at ring 0 we have
 * full I/O privilege, so these are trivial wrappers.
 * ================================================================ */

UCHAR NTAPI READ_PORT_UCHAR(PUCHAR Port)
{
    UCHAR value;

    value = PORT_IN_BYTE((unsigned long)Port);
    ntk_trace_port('i', (ULONG)Port, value);
    return value;
}

USHORT NTAPI READ_PORT_USHORT(PUSHORT Port)
{
    USHORT value;

    value = PORT_IN_WORD((unsigned long)Port);
    ntk_trace_port('I', (ULONG)Port, value);
    return value;
}

ULONG NTAPI READ_PORT_ULONG(PULONG Port)
{
    ULONG value;

    value = PORT_IN_DWORD((unsigned long)Port);
    ntk_trace_port('J', (ULONG)Port, value);
    return value;
}

VOID NTAPI WRITE_PORT_UCHAR(PUCHAR Port, UCHAR Value)
{
    ntk_trace_port('o', (ULONG)Port, Value);
    PORT_OUT_BYTE((unsigned long)Port, Value);
}

VOID NTAPI WRITE_PORT_USHORT(PUSHORT Port, USHORT Value)
{
    ntk_trace_port('O', (ULONG)Port, Value);
    PORT_OUT_WORD((unsigned long)Port, Value);
}

VOID NTAPI WRITE_PORT_ULONG(PULONG Port, ULONG Value)
{
    ntk_trace_port('P', (ULONG)Port, Value);
    PORT_OUT_DWORD((unsigned long)Port, Value);
}


/* ================================================================
 * HAL BUS ACCESS
 *
 * HalTranslateBusAddress: identity mapping for ISA and PCI on x86.
 * HalGetBusData / HalSetBusData: PCI config space via 0xCF8/0xCFC.
 * ================================================================ */

BOOLEAN NTAPI HalTranslateBusAddress(ULONG InterfaceType, ULONG BusNumber,
                                     PHYSICAL_ADDRESS BusAddress,
                                     PULONG AddressSpace,
                                     PPHYSICAL_ADDRESS TranslatedAddress)
{
    (void)InterfaceType;
    (void)BusNumber;

    dbg_mark('L');

    /*
     * On x86 with ISA/PCI, bus addresses are the same as physical
     * addresses. I/O ports stay in I/O space, memory stays in
     * memory space.
     */
    if (TranslatedAddress) {
        TranslatedAddress->QuadPart = BusAddress.QuadPart;
    }

    /* AddressSpace: 1 = I/O port, 0 = memory. Pass through unchanged. */
    /* (Caller sets it before calling; we just confirm.) */

    return TRUE;
}

/*
 * PCI configuration space access via mechanism 1 (ports 0xCF8/0xCFC).
 * BusDataType 4 = PCIConfiguration.
 * SlotNumber encodes device and function: bits 0-4 = device, 5-7 = function.
 */
ULONG NTAPI HalGetBusData(ULONG BusDataType, ULONG BusNumber,
                           ULONG SlotNumber, PVOID Buffer, ULONG Length)
{
    ULONG devNum, funcNum, regOff, cfgAddr;
    ULONG i;
    PUCHAR buf = (PUCHAR)Buffer;

    if (BusDataType != 4 || !Buffer || Length == 0) {
        return 0;
    }

    devNum  = SlotNumber & 0x1F;
    funcNum = (SlotNumber >> 5) & 0x07;

    for (i = 0; i < Length; i += 4) {
        ULONG val;
        ULONG j;

        regOff  = i & 0xFC;
        cfgAddr = 0x80000000UL
                | (BusNumber << 16)
                | (devNum << 11)
                | (funcNum << 8)
                | regOff;

        PORT_OUT_DWORD(0xCF8, cfgAddr);
        val = PORT_IN_DWORD(0xCFC);

        for (j = 0; j < 4 && (i + j) < Length; j++) {
            buf[i + j] = (UCHAR)(val >> (j * 8));
        }
    }

    return Length;
}

ULONG NTAPI HalSetBusData(ULONG BusDataType, ULONG BusNumber,
                           ULONG SlotNumber, PVOID Buffer, ULONG Length)
{
    ULONG devNum, funcNum, regOff, cfgAddr;
    ULONG i;
    PUCHAR buf = (PUCHAR)Buffer;

    if (BusDataType != 4 || !Buffer || Length == 0) {
        return 0;
    }

    devNum  = SlotNumber & 0x1F;
    funcNum = (SlotNumber >> 5) & 0x07;

    for (i = 0; i < Length; i += 4) {
        ULONG val = 0;
        ULONG j;

        /* Build dword from buffer bytes */
        for (j = 0; j < 4 && (i + j) < Length; j++) {
            val |= ((ULONG)buf[i + j]) << (j * 8);
        }

        /*
         * For partial dword writes, read-modify-write to preserve
         * the bytes we are not changing.
         */
        if ((i + 4) > Length) {
            ULONG existing;
            ULONG mask = 0;

            regOff  = i & 0xFC;
            cfgAddr = 0x80000000UL
                    | (BusNumber << 16)
                    | (devNum << 11)
                    | (funcNum << 8)
                    | regOff;

            PORT_OUT_DWORD(0xCF8, cfgAddr);
            existing = PORT_IN_DWORD(0xCFC);

            for (j = 0; j < 4; j++) {
                if ((i + j) < Length) {
                    mask |= (0xFFUL << (j * 8));
                }
            }

            val = (existing & ~mask) | (val & mask);
        }

        regOff  = i & 0xFC;
        cfgAddr = 0x80000000UL
                | (BusNumber << 16)
                | (devNum << 11)
                | (funcNum << 8)
                | regOff;

        PORT_OUT_DWORD(0xCF8, cfgAddr);
        PORT_OUT_DWORD(0xCFC, val);
    }

    return Length;
}


/* ================================================================
 * RTL MEMORY UTILITIES
 *
 * Tiny inline implementations. No libc dependency.
 * ================================================================ */

VOID __cdecl RtlZeroMemory(PVOID Destination, SIZE_T Length)
{
    PUCHAR d = (PUCHAR)Destination;
    SIZE_T i;

    if (!d) return;

    for (i = 0; i < Length; i++) {
        d[i] = 0;
    }
}

VOID __cdecl RtlCopyMemory(PVOID Destination, PVOID Source, SIZE_T Length)
{
    PUCHAR d = (PUCHAR)Destination;
    PUCHAR s = (PUCHAR)Source;
    SIZE_T i;

    if (!d || !s) return;

    for (i = 0; i < Length; i++) {
        d[i] = s[i];
    }
}

VOID __cdecl RtlMoveMemory(PVOID Destination, PVOID Source, SIZE_T Length)
{
    PUCHAR d = (PUCHAR)Destination;
    PUCHAR s = (PUCHAR)Source;
    SIZE_T i;

    if (!d || !s || Length == 0) return;

    if (d < s || d >= (s + Length)) {
        /* No overlap or destination before source: forward copy */
        for (i = 0; i < Length; i++) {
            d[i] = s[i];
        }
    } else {
        /* Overlapping, destination after source: backward copy */
        for (i = Length; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
}

SIZE_T __cdecl RtlCompareMemory(PVOID Source1, PVOID Source2, SIZE_T Length)
{
    PUCHAR s1 = (PUCHAR)Source1;
    PUCHAR s2 = (PUCHAR)Source2;
    SIZE_T i;

    if (!s1 || !s2) return 0;

    for (i = 0; i < Length; i++) {
        if (s1[i] != s2[i]) {
            return i;  /* number of bytes that matched */
        }
    }

    return Length;  /* all bytes matched */
}


/* ================================================================
 * I/O WORK ITEMS
 *
 * In the VxD environment there are no deferred worker threads.
 * IoQueueWorkItem calls the routine directly (synchronous).
 * ================================================================ */

PIO_WORKITEM NTAPI ntk_IoAllocateWorkItem(PDEVICE_OBJECT DeviceObject)
{
    PIO_WORKITEM wi;

    if (!DeviceObject) return NULL;

    wi = (PIO_WORKITEM)VxD_HeapAllocate(sizeof(IO_WORKITEM), HEAPF_ZEROINIT);
    if (!wi) {
        VxD_Debug_Printf("NTK: IoAllocateWorkItem alloc failed\n");
        return NULL;
    }

    wi->DeviceObject = DeviceObject;
    wi->Routine = NULL;
    wi->Context = NULL;
    wi->QueueType = 0;

    return wi;
}

VOID NTAPI ntk_IoQueueWorkItem(PIO_WORKITEM WorkItem,
                                PIO_WORKITEM_ROUTINE Routine,
                                ULONG QueueType, PVOID Context)
{
    (void)QueueType;

    if (!WorkItem || !Routine) return;

    /*
     * Ring-0 VxD: no deferred worker threads. Call directly.
     * This is safe because we are non-preemptive at ring 0.
     */
    Routine(WorkItem->DeviceObject, Context);
}

VOID NTAPI ntk_IoFreeWorkItem(PIO_WORKITEM WorkItem)
{
    if (WorkItem) {
        VxD_HeapFree(WorkItem, 0);
    }
}


/* ================================================================
 * StartIo SERIALIZATION
 *
 * IoStartPacket and IoStartNextPacket implement the classic
 * StartIo serialization model. The device object has a CurrentIrp
 * field and a device queue. When a new IRP arrives:
 *   - If the device is idle, set CurrentIrp and call StartIo
 *   - If the device is busy, queue the IRP
 * When processing completes, IoStartNextPacket dequeues the next.
 *
 * We cannot dereference DEVICE_OBJECT (opaque here), so we use
 * a simplified approach: store CurrentIrp state via a static.
 * For the real build this would use the DEVICE_OBJECT fields.
 * ================================================================ */

/*
 * ntk_IoStartPacket: Set CurrentIrp and call driver's StartIo.
 *
 * We take Irp as PVOID since the IRP struct is opaque from this
 * translation unit. The caller (IRPMGR or thunk layer) will have
 * the full type information.
 */
VOID NTAPI ntk_IoStartPacket(PDEVICE_OBJECT DeviceObject, PVOID Irp,
                               PULONG Key, PVOID CancelFunction)
{
    PDRIVER_STARTIO start_io;

    (void)Key;
    (void)CancelFunction;

    if (!DeviceObject || !Irp) return;

    dbg_mark('P');
    start_io = NULL;
    if (DeviceObject->DriverObject) {
        start_io = DeviceObject->DriverObject->DriverStartIo;
    }
    if (start_io) {
        DeviceObject->CurrentIrp = (PIRP)Irp;
        start_io(DeviceObject, (PIRP)Irp);
        dbg_mark('o');
    }
}

VOID NTAPI ntk_IoStartNextPacket(PDEVICE_OBJECT DeviceObject,
                                   BOOLEAN Cancelable)
{
    (void)Cancelable;

    if (!DeviceObject) return;

    dbg_mark('N');
    DeviceObject->CurrentIrp = NULL;
    DeviceObject->DeviceQueue.Busy = FALSE;
}


/* ================================================================
 * DEVICE QUEUE
 *
 * KDEVICE_QUEUE is a sorted linked list used by the StartIo
 * model to queue IRPs when the device is busy.
 * ================================================================ */

VOID NTAPI ntk_KeInitializeDeviceQueue(PKDEVICE_QUEUE DeviceQueue)
{
    if (!DeviceQueue) return;

    DeviceQueue->Type = 4;
    DeviceQueue->Size = sizeof(KDEVICE_QUEUE);
    DeviceQueue->DeviceListHead.Flink = &DeviceQueue->DeviceListHead;
    DeviceQueue->DeviceListHead.Blink = &DeviceQueue->DeviceListHead;
    DeviceQueue->Lock = 0;
    DeviceQueue->Busy = FALSE;
}

/*
 * Insert sorted by SortKey. Returns TRUE if entry was queued
 * (device was busy). Returns FALSE if device was idle (caller
 * should process the entry directly).
 */
BOOLEAN NTAPI ntk_KeInsertByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue,
                                            PKDEVICE_QUEUE_ENTRY Entry,
                                            ULONG SortKey)
{
    PLIST_ENTRY listHead;
    PLIST_ENTRY current;

    if (!DeviceQueue || !Entry) return FALSE;

    Entry->SortKey = SortKey;
    Entry->Inserted = FALSE;

    dbg_mark('K');
    dbg_mark(DeviceQueue->Busy ? 'B' : 'b');

    listHead = &DeviceQueue->DeviceListHead;
    if (g_ntk_force_device_queue_start ||
        !DeviceQueue->Busy ||
        (listHead->Flink == listHead && listHead->Blink == listHead)) {
        /* Device is idle: mark busy but don't queue */
        DeviceQueue->Busy = TRUE;
        return FALSE;
    }

    /* Device is busy: insert sorted */
    current = listHead->Flink;

    while (current != listHead) {
        PKDEVICE_QUEUE_ENTRY existing;
        existing = (PKDEVICE_QUEUE_ENTRY)((PUCHAR)current -
            (ULONG_PTR)&((PKDEVICE_QUEUE_ENTRY)0)->DeviceListEntry);

        if (SortKey < existing->SortKey) {
            break;  /* insert before this entry */
        }
        current = current->Flink;
    }

    /* Insert before 'current' */
    Entry->DeviceListEntry.Flink = current;
    Entry->DeviceListEntry.Blink = current->Blink;
    current->Blink->Flink = &Entry->DeviceListEntry;
    current->Blink = &Entry->DeviceListEntry;
    Entry->Inserted = TRUE;

    return TRUE;
}

PKDEVICE_QUEUE_ENTRY NTAPI ntk_KeRemoveByKeyDeviceQueue(
    PKDEVICE_QUEUE DeviceQueue, ULONG SortKey)
{
    PLIST_ENTRY listHead;
    PLIST_ENTRY current;
    PKDEVICE_QUEUE_ENTRY bestEntry;

    if (!DeviceQueue) return NULL;

    listHead = &DeviceQueue->DeviceListHead;

    if (listHead->Flink == listHead) {
        /* Queue is empty */
        DeviceQueue->Busy = FALSE;
        return NULL;
    }

    /*
     * Find the first entry with SortKey >= requested key.
     * If none found, wrap around and take the first entry.
     */
    bestEntry = NULL;
    current = listHead->Flink;

    while (current != listHead) {
        PKDEVICE_QUEUE_ENTRY entry;
        entry = (PKDEVICE_QUEUE_ENTRY)((PUCHAR)current -
            (ULONG_PTR)&((PKDEVICE_QUEUE_ENTRY)0)->DeviceListEntry);

        if (entry->SortKey >= SortKey) {
            bestEntry = entry;
            break;
        }
        current = current->Flink;
    }

    if (!bestEntry) {
        /* Wrap: take first entry */
        current = listHead->Flink;
        bestEntry = (PKDEVICE_QUEUE_ENTRY)((PUCHAR)current -
            (ULONG_PTR)&((PKDEVICE_QUEUE_ENTRY)0)->DeviceListEntry);
    }

    /* Remove from list */
    bestEntry->DeviceListEntry.Blink->Flink = bestEntry->DeviceListEntry.Flink;
    bestEntry->DeviceListEntry.Flink->Blink = bestEntry->DeviceListEntry.Blink;
    bestEntry->Inserted = FALSE;

    return bestEntry;
}

PKDEVICE_QUEUE_ENTRY NTAPI ntk_KeRemoveDeviceQueue(
    PKDEVICE_QUEUE DeviceQueue)
{
    PLIST_ENTRY listHead;
    PLIST_ENTRY first;
    PKDEVICE_QUEUE_ENTRY entry;

    if (!DeviceQueue) return NULL;

    listHead = &DeviceQueue->DeviceListHead;

    if (listHead->Flink == listHead) {
        /* Queue is empty */
        DeviceQueue->Busy = FALSE;
        return NULL;
    }

    first = listHead->Flink;
    entry = (PKDEVICE_QUEUE_ENTRY)((PUCHAR)first -
        (ULONG_PTR)&((PKDEVICE_QUEUE_ENTRY)0)->DeviceListEntry);

    /* Remove from list */
    first->Blink->Flink = first->Flink;
    first->Flink->Blink = first->Blink;
    entry->Inserted = FALSE;

    return entry;
}


/* ================================================================
 * MEMORY MANAGER
 *
 * MmMapIoSpace: map physical address to linear using VxD service.
 * Most other Mm functions are no-ops or trivial in VxD context
 * where everything is ring 0, non-paged, and identity-mapped.
 * ================================================================ */

PVOID NTAPI ntk_MmMapIoSpace(PHYSICAL_ADDRESS PhysAddr, SIZE_T Length,
                              ULONG CacheType)
{
    PVOID mapped;

    (void)CacheType;

    dbg_mark('m');
    if (Length == 0) return NULL;

    /*
     * Use VxD_MapPhysToLinear to get a linear address for MMIO.
     * The physical address is in PhysAddr.LowPart (we don't
     * support >4GB physical on Win9x).
     */
    mapped = VxD_MapPhysToLinear(PhysAddr.u.LowPart, Length, 0);
    if (!mapped) {
        VxD_Debug_Printf("NTK: MmMapIoSpace failed phys=0x%08lX len=%lu\n",
                         PhysAddr.u.LowPart, (ULONG)Length);
    }
    return mapped;
}

VOID NTAPI ntk_MmUnmapIoSpace(PVOID BaseAddr, SIZE_T Length)
{
    (void)BaseAddr;
    (void)Length;

    /* No-op in VxD context: mapped addresses stay valid */
}

PVOID NTAPI ntk_MmMapLockedPagesSpecifyCache(PMDL Mdl, ULONG AccessMode,
                                               ULONG CacheType,
                                               PVOID BaseAddr,
                                               ULONG BugCheckOnFail,
                                               ULONG Priority)
{
    (void)AccessMode;
    (void)CacheType;
    (void)BaseAddr;
    (void)BugCheckOnFail;
    (void)Priority;

    if (!Mdl) return NULL;

    /*
     * In VxD context, all memory is kernel-mode accessible.
     * Return the MDL's virtual address directly.
     */
    if (Mdl->MappedSystemVa) {
        return Mdl->MappedSystemVa;
    }

    /* Compute from StartVa + ByteOffset */
    return (PVOID)((PUCHAR)Mdl->StartVa + Mdl->ByteOffset);
}

VOID NTAPI ntk_MmBuildMdlForNonPagedPool(PMDL Mdl)
{
    if (!Mdl) return;

    /*
     * Fill MDL fields from the virtual address.
     * In VxD context (flat 32-bit, ring 0), the page array is
     * trivially the physical pages backing the linear address.
     * Since we don't do DMA scatter-gather in this shim, we just
     * mark the MDL as mapped.
     */
    Mdl->MappedSystemVa = (PVOID)((PUCHAR)Mdl->StartVa + Mdl->ByteOffset);
    Mdl->MdlFlags |= MDL_SOURCE_IS_NONPAGED_POOL | MDL_PAGES_LOCKED;
}

PVOID NTAPI ntk_MmLockPagableDataSection(PVOID Address)
{
    /*
     * Everything is ring 0 in VxD, already locked.
     * Return the address itself as the "section handle".
     */
    return Address;
}

VOID NTAPI ntk_MmUnlockPagableImageSection(PVOID SectionHandle)
{
    (void)SectionHandle;
    /* No-op */
}

VOID NTAPI ntk_MmUnlockPages(PMDL Mdl)
{
    (void)Mdl;
    /* No-op: VxD memory is always locked */
}

PMDL NTAPI ntk_IoAllocateMdl(PVOID VirtualAddress, ULONG Length,
                               BOOLEAN SecondaryBuffer,
                               BOOLEAN ChargeQuota, PVOID Irp)
{
    PMDL mdl;

    (void)SecondaryBuffer;
    (void)ChargeQuota;
    (void)Irp;

    mdl = (PMDL)VxD_HeapAllocate(sizeof(MDL), HEAPF_ZEROINIT);
    if (!mdl) {
        VxD_Debug_Printf("NTK: IoAllocateMdl alloc failed\n");
        return NULL;
    }

    mdl->Next = NULL;
    mdl->Size = sizeof(MDL);
    mdl->MdlFlags = 0;
    mdl->MappedSystemVa = VirtualAddress;
    mdl->StartVa = (PVOID)((ULONG)VirtualAddress & ~0xFFF);
    mdl->ByteCount = Length;
    mdl->ByteOffset = (ULONG)VirtualAddress & 0xFFF;

    return mdl;
}

VOID NTAPI ntk_IoFreeMdl(PMDL Mdl)
{
    if (Mdl) {
        VxD_HeapFree(Mdl, 0);
    }
}


/* ================================================================
 * OBJECT MANAGER
 *
 * Simplified stubs. In our shim, object reference counting is
 * largely a no-op because we control object lifetimes explicitly.
 * Handles ARE the object pointers.
 * ================================================================ */

NTSTATUS NTAPI ntk_ObReferenceObjectByPointer(PVOID Object, ULONG Access,
                                               PVOID ObjectType,
                                               ULONG AccessMode)
{
    (void)Access;
    (void)ObjectType;
    (void)AccessMode;

    if (!Object) return STATUS_UNSUCCESSFUL;

    /* No-op: no refcount tracking in shim */
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_ObReferenceObjectByHandle(PVOID Handle, ULONG Access,
                                              PVOID ObjectType,
                                              ULONG AccessMode,
                                              PVOID *Object,
                                              PVOID HandleInfo)
{
    (void)Access;
    (void)ObjectType;
    (void)AccessMode;
    (void)HandleInfo;

    if (!Object) return STATUS_UNSUCCESSFUL;

    /*
     * In our shim, handles ARE object pointers.
     * Return the handle value as the object.
     */
    *Object = Handle;
    return STATUS_SUCCESS;
}

VOID FASTCALL ntk_ObfDereferenceObject(PVOID Object)
{
    dbg_mark('O');
    (void)Object;
    /* No-op: no refcount tracking in shim */
}


/* ================================================================
 * INTERRUPT
 *
 * IoConnectInterrupt: hook a hardware IRQ via VPICD.
 * IoDisconnectInterrupt: unhook via VPICD_Force_Default_Behavior.
 * KeSynchronizeExecution: acquire spinlock, call routine, release.
 * ================================================================ */

/*
 * ISR trampoline: VPICD calls us; we call the NT driver's ISR.
 */
static PKINTERRUPT g_ConnectedInterrupts[16]; /* max 16 IRQ lines */
typedef struct _NTK_VPICD_IRQ_DESCRIPTOR {
    USHORT  VID_IRQ_Number;
    USHORT  VID_Options;
    PVOID   VID_Hw_Int_Proc;
    PVOID   VID_Virt_Int_Proc;
    PVOID   VID_EOI_Proc;
    PVOID   VID_Mask_Change_Proc;
    PVOID   VID_IRET_Proc;
    ULONG   VID_IRET_Time_Out;
    PVOID   VID_Hw_Int_Ref;
} NTK_VPICD_IRQ_DESCRIPTOR;

static void __cdecl ntk_IsrTrampoline(ULONG irq)
{
    PKINTERRUPT intObj;

    if (irq >= 16) return;

    intObj = g_ConnectedInterrupts[irq];
    if (intObj && intObj->ServiceRoutine) {
        KIRQL oldIrql = g_CurrentIrql;
        g_CurrentIrql = DIRQL;

        intObj->ServiceRoutine(intObj, intObj->ServiceContext);

        g_CurrentIrql = oldIrql;

        /* Send EOI and drain DPCs */
        VxD_VPICD_PhysicalEOI(intObj->ShimIrqHandle);
        ntk_DrainDpcQueue();
    }
}

UCHAR __cdecl ntk_InvokeManualInterrupt(ULONG irq)
{
    if (irq >= 16 || !g_ConnectedInterrupts[irq]) {
        return 0;
    }
    ntk_IsrTrampoline(irq);
    return 1;
}

NTSTATUS NTAPI ntk_IoConnectInterrupt(
    PKINTERRUPT *InterruptObject,
    PKSERVICE_ROUTINE ServiceRoutine,
    PVOID ServiceContext,
    PKSPIN_LOCK SpinLock,
    ULONG Vector,
    KIRQL Irql,
    KIRQL SyncIrql,
    ULONG InterruptMode,
    BOOLEAN ShareVector,
    ULONG ProcessorMask,
    BOOLEAN FloatingSave)
{
    PKINTERRUPT intObj;
    ULONG irq;
    ULONG handle;

    (void)ProcessorMask;
    (void)FloatingSave;

    dbg_mark('C');
    dbg_mark('I');
    ntk_mark_hex32((ULONG)InterruptObject);
    ntk_mark_hex32((ULONG)ServiceRoutine);
    ntk_mark_hex32((ULONG)ServiceContext);
    ntk_mark_hex32(Vector);

    if (!InterruptObject || !ServiceRoutine) {
        dbg_mark('C');
        dbg_mark('0');
        return STATUS_UNSUCCESSFUL;
    }

    /*
     * On x86 the IRQ number is typically Vector - base.
     * For ISA, vectors 0x30-0x3F map to IRQ 0-15 on NT.
     * For PCI, the mapping varies. We extract the low nibble.
     */
    irq = Vector & 0x0F;
    dbg_mark('Q');
    ntk_mark_hex8((UCHAR)irq);

    intObj = (PKINTERRUPT)VxD_HeapAllocate(sizeof(KINTERRUPT),
                                            HEAPF_ZEROINIT);
    if (!intObj) {
        VxD_Debug_Printf("NTK: IoConnectInterrupt alloc failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    intObj->ServiceRoutine = ServiceRoutine;
    intObj->ServiceContext = ServiceContext;
    intObj->Vector = Vector;
    intObj->Irql = Irql;
    intObj->SynchronizeIrql = SyncIrql;
    intObj->InterruptMode = (KINTERRUPT_MODE)InterruptMode;
    intObj->ShareVector = ShareVector;
    intObj->ShimConnected = FALSE;

    if (SpinLock) {
        intObj->SpinLock = SpinLock;
    } else {
        intObj->OwnSpinLock = 0;
        intObj->SpinLock = &intObj->OwnSpinLock;
    }

    /* Store in IRQ table for the trampoline */
    if (irq < 16) {
        g_ConnectedInterrupts[irq] = intObj;
    }

    handle = 0;
#if 0
    {
        NTK_VPICD_IRQ_DESCRIPTOR irqDesc;
        RtlZeroMemory(&irqDesc, sizeof(irqDesc));
        irqDesc.VID_IRQ_Number = (USHORT)irq;
        irqDesc.VID_Options = ShareVector ? 0x0002 : 0;
        irqDesc.VID_Hw_Int_Proc = (PVOID)ntk_IsrTrampoline;
        irqDesc.VID_Hw_Int_Ref = (PVOID)irq;
        handle = VxD_VPICD_Virtualize_IRQ(&irqDesc);
    }
#endif
    if (handle) {
        intObj->ShimIrqHandle = handle;
        intObj->ShimConnected = TRUE;
        VxD_Debug_Printf("NTK: IoConnectInterrupt IRQ %lu -> handle 0x%08lX\n",
                         irq, handle);
    } else {
        VxD_Debug_Printf("NTK: IoConnectInterrupt VPICD failed for IRQ %lu\n",
                         irq);
        /* Continue anyway: some configurations may still work */
    }

    *InterruptObject = intObj;
    dbg_mark('C');
    dbg_mark('1');
    ntk_mark_hex32((ULONG)intObj);
    ntk_mark_hex32((ULONG)*InterruptObject);
    return STATUS_SUCCESS;
}

VOID NTAPI ntk_IoDisconnectInterrupt(PKINTERRUPT InterruptObject)
{
    ULONG irq;

    if (!InterruptObject) return;

    if (InterruptObject->ShimConnected && InterruptObject->ShimIrqHandle) {
        VxD_VPICD_ForceDefaultBehavior(InterruptObject->ShimIrqHandle);
        InterruptObject->ShimConnected = FALSE;
    }

    /* Remove from IRQ table */
    irq = InterruptObject->Vector & 0x0F;
    if (irq < 16 && g_ConnectedInterrupts[irq] == InterruptObject) {
        g_ConnectedInterrupts[irq] = NULL;
    }

    VxD_HeapFree(InterruptObject, 0);
}

/*
 * ntk_ConnectDeferredVpicd - Hook VPICD after ServiceRoutine is set
 *
 * Called from NT5LOADER after pnp_start_device + IoConnectInterrupt.
 * The #if 0 block in ntk_IoConnectInterrupt is disabled because stray
 * IRQs crash before the ISR bridge is ready. This function hooks VPICD
 * only after confirming the interrupt object has a valid ServiceRoutine.
 */
int CDECL ntk_ConnectDeferredVpicd(ULONG irq)
{
    PKINTERRUPT intObj;
    NTK_VPICD_IRQ_DESCRIPTOR irqDesc;
    ULONG handle;

    if (irq >= 16) {
        VxD_Debug_Printf("NTK: DeferredVPICD: invalid IRQ %lu\n", irq);
        return -1;
    }

    intObj = g_ConnectedInterrupts[irq];
    if (!intObj) {
        VxD_Debug_Printf("NTK: DeferredVPICD: no interrupt object for IRQ %lu\n", irq);
        return -2;
    }

    if (!intObj->ServiceRoutine) {
        VxD_Debug_Printf("NTK: DeferredVPICD: ServiceRoutine NULL for IRQ %lu\n", irq);
        return -3;
    }

    if (intObj->ShimConnected) {
        VxD_Debug_Printf("NTK: DeferredVPICD: IRQ %lu already hooked\n", irq);
        return 0;
    }

    RtlZeroMemory(&irqDesc, sizeof(irqDesc));
    irqDesc.VID_IRQ_Number = (USHORT)irq;
    irqDesc.VID_Options = intObj->ShareVector ? 0x0002 : 0;
    irqDesc.VID_Hw_Int_Proc = (PVOID)ntk_IsrTrampoline;
    irqDesc.VID_Hw_Int_Ref = (PVOID)irq;

    handle = VxD_VPICD_Virtualize_IRQ(&irqDesc);
    if (!handle) {
        VxD_Debug_Printf("NTK: DeferredVPICD: VPICD_Virtualize_IRQ failed for IRQ %lu\n", irq);
        return -4;
    }

    intObj->ShimIrqHandle = handle;
    intObj->ShimConnected = TRUE;

    VxD_Debug_Printf("NTK: DeferredVPICD: IRQ %lu hooked -> handle 0x%08lX\n",
                     irq, handle);
    dbg_mark('V');
    dbg_mark('H');
    return 0;
}

BOOLEAN NTAPI ntk_KeSynchronizeExecution(PKINTERRUPT Interrupt,
                                          PKSYNCHRONIZE_ROUTINE SyncRoutine,
                                          PVOID SyncContext)
{
    BOOLEAN result;
    ULONG flags;

    if (!SyncRoutine) return FALSE;
    if (!Interrupt) {
        dbg_mark('!');
    }

    /*
     * Acquire the interrupt's spinlock (cli + mark), call routine,
     * release. On single-CPU this ensures the ISR doesn't fire
     * while we hold the lock.
     */
    PORT_SAVE_FLAGS_CLI(flags);

    result = SyncRoutine(SyncContext);

    PORT_RESTORE_FLAGS(flags);

    return result;
}


/* ================================================================
 * UNICODE / ANSI STRING FUNCTIONS
 * ================================================================ */

/* Internal wide char helper: uppercase a single WCHAR */
static WCHAR ntk_WcharUpper(WCHAR c)
{
    if (c >= (WCHAR)'a' && c <= (WCHAR)'z') {
        return c - (WCHAR)('a' - 'A');
    }
    return c;
}

/* Internal: strlen for narrow strings */
static USHORT ntk_StrLen(const CHAR *s)
{
    USHORT len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    return len;
}

/* Internal: wcslen */
static USHORT ntk_WcsLen(PCWSTR s)
{
    USHORT len = 0;
    if (!s) return 0;
    while (s[len]) len++;
    return len;
}

VOID NTAPI ntk_RtlInitUnicodeString(PUNICODE_STRING Dest, PCWSTR Source)
{
    if (!Dest) return;

    if (Source) {
        USHORT chars = ntk_WcsLen(Source);
        Dest->Length = chars * 2;
        Dest->MaximumLength = (chars + 1) * 2;
        Dest->Buffer = (PWSTR)Source;
    } else {
        Dest->Length = 0;
        Dest->MaximumLength = 0;
        Dest->Buffer = NULL;
    }
}

VOID NTAPI ntk_RtlInitAnsiString(PANSI_STRING Dest, const CHAR *Source)
{
    if (!Dest) return;

    if (Source) {
        USHORT len = ntk_StrLen(Source);
        Dest->Length = len;
        Dest->MaximumLength = len + 1;
        Dest->Buffer = (PCHAR)Source;
    } else {
        Dest->Length = 0;
        Dest->MaximumLength = 0;
        Dest->Buffer = NULL;
    }
}

VOID NTAPI ntk_RtlCopyUnicodeString(PUNICODE_STRING Dest,
                                      PCUNICODE_STRING Source)
{
    USHORT copyLen;
    USHORT i;

    if (!Dest) return;

    if (!Source || !Source->Buffer || Source->Length == 0) {
        Dest->Length = 0;
        return;
    }

    copyLen = Source->Length;
    if (copyLen > Dest->MaximumLength) {
        copyLen = Dest->MaximumLength;
    }

    if (Dest->Buffer && Source->Buffer) {
        for (i = 0; i < copyLen; i++) {
            ((PUCHAR)Dest->Buffer)[i] = ((PUCHAR)Source->Buffer)[i];
        }
    }

    Dest->Length = copyLen;
}

VOID NTAPI ntk_RtlFreeUnicodeString(PUNICODE_STRING Str)
{
    if (!Str) return;

    if (Str->Buffer) {
        VxD_HeapFree(Str->Buffer, 0);
        Str->Buffer = NULL;
    }
    Str->Length = 0;
    Str->MaximumLength = 0;
}

LONG NTAPI ntk_RtlCompareUnicodeString(PCUNICODE_STRING Str1,
                                         PCUNICODE_STRING Str2,
                                         BOOLEAN CaseInsensitive)
{
    USHORT len1, len2, minLen;
    USHORT i;

    if (!Str1 || !Str2) return 0;

    len1 = Str1->Length / 2;  /* char count */
    len2 = Str2->Length / 2;
    minLen = (len1 < len2) ? len1 : len2;

    for (i = 0; i < minLen; i++) {
        WCHAR c1 = Str1->Buffer[i];
        WCHAR c2 = Str2->Buffer[i];

        if (CaseInsensitive) {
            c1 = ntk_WcharUpper(c1);
            c2 = ntk_WcharUpper(c2);
        }

        if (c1 < c2) return -1;
        if (c1 > c2) return 1;
    }

    if (len1 < len2) return -1;
    if (len1 > len2) return 1;
    return 0;
}

NTSTATUS NTAPI ntk_RtlAnsiStringToUnicodeString(PUNICODE_STRING Dest,
                                                   PANSI_STRING Source,
                                                   BOOLEAN AllocDest)
{
    USHORT i;
    USHORT uniLen;

    if (!Dest || !Source) return STATUS_UNSUCCESSFUL;

    uniLen = Source->Length * 2;

    if (AllocDest) {
        Dest->Buffer = (PWSTR)VxD_HeapAllocate(uniLen + 2, HEAPF_ZEROINIT);
        if (!Dest->Buffer) return STATUS_INSUFFICIENT_RESOURCES;
        Dest->MaximumLength = uniLen + 2;
    }

    if (uniLen > Dest->MaximumLength) {
        uniLen = Dest->MaximumLength;
    }

    /* Simple ASCII-to-wide: byte to word */
    for (i = 0; i < Source->Length && (i * 2) < uniLen; i++) {
        Dest->Buffer[i] = (WCHAR)(UCHAR)Source->Buffer[i];
    }

    Dest->Length = i * 2;

    /* Null-terminate if space */
    if (Dest->Length + 2 <= Dest->MaximumLength) {
        Dest->Buffer[i] = 0;
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_RtlAppendUnicodeStringToString(PUNICODE_STRING Dest,
                                                     PCUNICODE_STRING Source)
{
    USHORT copyLen;
    USHORT destChars;
    USHORT i;

    if (!Dest || !Source) return STATUS_UNSUCCESSFUL;
    if (!Source->Buffer || Source->Length == 0) return STATUS_SUCCESS;

    copyLen = Source->Length;
    if (Dest->Length + copyLen > Dest->MaximumLength) {
        return STATUS_BUFFER_OVERFLOW;
    }

    destChars = Dest->Length / 2;
    for (i = 0; i < copyLen / 2; i++) {
        Dest->Buffer[destChars + i] = Source->Buffer[i];
    }

    Dest->Length += copyLen;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_RtlIntegerToUnicodeString(ULONG Value, ULONG Base,
                                               PUNICODE_STRING Str)
{
    WCHAR tmpBuf[12]; /* enough for 32-bit in any base */
    USHORT pos = 0;
    USHORT i;
    ULONG v;

    if (!Str || !Str->Buffer) return STATUS_UNSUCCESSFUL;
    if (Base == 0) Base = 10;

    if (Value == 0) {
        tmpBuf[pos++] = (WCHAR)'0';
    } else {
        v = Value;
        while (v > 0 && pos < 11) {
            ULONG digit = v % Base;
            if (digit < 10) {
                tmpBuf[pos++] = (WCHAR)('0' + digit);
            } else {
                tmpBuf[pos++] = (WCHAR)('A' + digit - 10);
            }
            v = v / Base;
        }
    }

    if (pos * 2 > Str->MaximumLength) {
        return STATUS_BUFFER_OVERFLOW;
    }

    /* Reverse into destination */
    for (i = 0; i < pos; i++) {
        Str->Buffer[i] = tmpBuf[pos - 1 - i];
    }
    Str->Length = pos * 2;

    /* Null-terminate if space */
    if (Str->Length + 2 <= Str->MaximumLength) {
        Str->Buffer[pos] = 0;
    }

    return STATUS_SUCCESS;
}

BOOLEAN NTAPI ntk_RtlPrefixUnicodeString(PCUNICODE_STRING Prefix,
                                           PCUNICODE_STRING String,
                                           BOOLEAN CaseInsensitive)
{
    USHORT prefixChars, stringChars;
    USHORT i;

    if (!Prefix || !String) return FALSE;

    prefixChars = Prefix->Length / 2;
    stringChars = String->Length / 2;

    if (prefixChars > stringChars) return FALSE;

    for (i = 0; i < prefixChars; i++) {
        WCHAR c1 = Prefix->Buffer[i];
        WCHAR c2 = String->Buffer[i];

        if (CaseInsensitive) {
            c1 = ntk_WcharUpper(c1);
            c2 = ntk_WcharUpper(c2);
        }

        if (c1 != c2) return FALSE;
    }

    return TRUE;
}

ULONG NTAPI ntk_RtlxAnsiStringToUnicodeSize(PANSI_STRING AnsiStr)
{
    if (!AnsiStr) return 0;
    return (ULONG)(AnsiStr->Length * 2 + 2);
}

NTSTATUS NTAPI ntk_RtlWriteRegistryValue(ULONG RelativeTo, PCWSTR Path,
                                           PCWSTR ValueName, ULONG ValueType,
                                           PVOID ValueData, ULONG ValueLength)
{
    (void)RelativeTo;
    (void)Path;
    (void)ValueName;
    (void)ValueType;
    (void)ValueData;
    (void)ValueLength;

    VxD_Debug_Printf("NTK: RtlWriteRegistryValue stub\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_RtlQueryRegistryValues(ULONG RelativeTo, PCWSTR Path,
                                            PRTL_QUERY_REGISTRY_TABLE Table,
                                            PVOID Context, PVOID Environment)
{
    PRTL_QUERY_REGISTRY_TABLE entry;
    BOOLEAN has_default;

    (void)RelativeTo;
    (void)Path;
    (void)Environment;

    VxD_Debug_Printf("NTK: RtlQueryRegistryValues stub\n");
    if (!Table) {
        return STATUS_SUCCESS;
    }

    for (entry = Table; entry->Name || entry->Flags; entry++) {
        has_default = (entry->DefaultData && entry->DefaultLength > 0);

        VxD_Debug_Printf("NTK: RtlQueryRegistryValues name=");
        ntk_DebugPrintWstr(entry->Name);
        VxD_Debug_Printf("\n");

        if ((entry->Flags & RTL_QUERY_REGISTRY_DIRECT) &&
            entry->EntryContext &&
            has_default) {
            dbg_mark('d');
            RtlCopyMemory(entry->EntryContext,
                          entry->DefaultData,
                          entry->DefaultLength);
        } else if (entry->QueryRoutine && has_default) {
            dbg_mark('q');
            entry->QueryRoutine(entry->Name,
                                entry->DefaultType,
                                entry->DefaultData,
                                entry->DefaultLength,
                                Context,
                                entry->EntryContext);
        } else if ((entry->Flags & RTL_QUERY_REGISTRY_REQUIRED) &&
                   !has_default) {
            VxD_Debug_Printf("NTK: RtlQueryRegistryValues missing required name=");
            ntk_DebugPrintWstr(entry->Name);
            VxD_Debug_Printf("\n");
            return STATUS_OBJECT_NAME_NOT_FOUND;
        }
    }

    return STATUS_SUCCESS;
}


/* ================================================================
 * IRP BUILDERS
 *
 * These allocate IRPs and set up I/O stack locations for common
 * operations. Since IRP is opaque here, we allocate raw memory
 * and fill the fields at known offsets. In the full build, these
 * would use the proper IRP type from NTKRNL.H.
 * ================================================================ */

/*
 * Simplified IRP allocation for builder functions.
 * We allocate enough for a 1-stack-location IRP.
 * Stack size = sizeof(IRP) + sizeof(IO_STACK_LOCATION).
 * These are ~100-200 bytes; we allocate a generous 512.
 */
#define NTK_SIMPLE_IRP_SIZE 512

PVOID NTAPI ntk_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode, PDEVICE_OBJECT DeviceObject,
    PVOID InBuf, ULONG InLen, PVOID OutBuf, ULONG OutLen,
    BOOLEAN InternalDeviceControl, PKEVENT Event,
    PIO_STATUS_BLOCK IoStatusBlock)
{
    PIRP irp;
    PIO_STACK_LOCATION irp_sp;

    if (!DeviceObject) {
        return NULL;
    }

    irp = IrpMgr_IoAllocateIrp(DeviceObject->StackSize, FALSE);
    dbg_mark('i');
    if (!irp) {
        VxD_Debug_Printf("NTK: IoBuildDeviceIoControlRequest alloc failed\n");
        return NULL;
    }

    dbg_mark('1');
    irp->UserEvent = Event;
    irp->UserIosb = IoStatusBlock;
    irp->RequestorMode = KernelMode;
    irp->Flags = IRP_SYNCHRONOUS_API;
    irp->UserBuffer = OutBuf;
    irp->AssociatedIrp.SystemBuffer = InBuf;

    dbg_mark('2');
    irp_sp = IrpMgr_IoGetNextIrpStackLocation(irp);
    irp_sp->MajorFunction = InternalDeviceControl ?
        IRP_MJ_INTERNAL_DEVICE_CONTROL : IRP_MJ_DEVICE_CONTROL;
    irp_sp->Parameters.DeviceIoControl.IoControlCode = IoControlCode;
    irp_sp->Parameters.DeviceIoControl.InputBufferLength = InLen;
    irp_sp->Parameters.DeviceIoControl.OutputBufferLength = OutLen;
    irp_sp->Parameters.DeviceIoControl.Type3InputBuffer = InBuf;
    irp_sp->DeviceObject = DeviceObject;
    *(PVOID *)((PUCHAR)irp + 0x60) = (PVOID)((PUCHAR)irp_sp + 0x24);

    dbg_mark('J');
    VxD_Debug_Printf("NTK: IoBuildDeviceIoControlRequest IOCTL=0x%08lX\n",
                     IoControlCode);

    return irp;
}

PVOID NTAPI ntk_IoBuildSynchronousFsdRequest(
    ULONG MajorFunction, PDEVICE_OBJECT DeviceObject,
    PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
    PKEVENT Event, PIO_STATUS_BLOCK IoStatusBlock)
{
    PIRP irp;
    PIO_STACK_LOCATION irp_sp;

    if (!DeviceObject) {
        return NULL;
    }

    irp = IrpMgr_IoAllocateIrp(DeviceObject->StackSize, FALSE);
    dbg_mark('i');
    if (!irp) {
        VxD_Debug_Printf("NTK: IoBuildSynchronousFsdRequest alloc failed\n");
        return NULL;
    }

    dbg_mark('1');
    irp->UserEvent = Event;
    irp->UserIosb = IoStatusBlock;
    irp->RequestorMode = KernelMode;
    irp->Flags = IRP_SYNCHRONOUS_API;
    irp->UserBuffer = Buffer;

    dbg_mark('2');
    irp_sp = IrpMgr_IoGetNextIrpStackLocation(irp);
    irp_sp->MajorFunction = (UCHAR)MajorFunction;
    irp_sp->DeviceObject = DeviceObject;
    dbg_mark('3');
    if (MajorFunction == IRP_MJ_READ) {
        irp_sp->Parameters.Read.Length = Length;
        if (StartingOffset) {
            irp_sp->Parameters.Read.ByteOffset = *StartingOffset;
        }
        irp->Flags |= IRP_READ_OPERATION;
    } else if (MajorFunction == IRP_MJ_WRITE) {
        irp_sp->Parameters.Write.Length = Length;
        if (StartingOffset) {
            irp_sp->Parameters.Write.ByteOffset = *StartingOffset;
        }
        irp->Flags |= IRP_WRITE_OPERATION;
    }
    dbg_mark('4');
    if (Buffer && Length > 0 && (DeviceObject->Flags & DO_BUFFERED_IO)) {
        irp->AssociatedIrp.SystemBuffer = Buffer;
        irp->Flags |= IRP_BUFFERED_IO;
    }
    *(PVOID *)((PUCHAR)irp + 0x60) = (PVOID)((PUCHAR)irp_sp + 0x24);

    dbg_mark('F');
    dbg_mark((char)('0' + (MajorFunction & 0x0F)));
    VxD_Debug_Printf("NTK: IoBuildSynchronousFsdRequest MJ=0x%02X\n",
                     (UCHAR)MajorFunction);
    return irp;
}

PVOID NTAPI ntk_IoBuildAsynchronousFsdRequest(
    ULONG MajorFunction, PDEVICE_OBJECT DeviceObject,
    PVOID Buffer, ULONG Length, PLARGE_INTEGER StartingOffset,
    PIO_STATUS_BLOCK IoStatusBlock)
{
    PIRP irp;
    PIO_STACK_LOCATION irp_sp;

    if (!DeviceObject) {
        return NULL;
    }

    irp = IrpMgr_IoAllocateIrp(DeviceObject->StackSize, FALSE);
    dbg_mark('i');
    if (!irp) {
        VxD_Debug_Printf("NTK: IoBuildAsynchronousFsdRequest alloc failed\n");
        return NULL;
    }

    dbg_mark('1');
    irp->UserIosb = IoStatusBlock;
    irp->RequestorMode = KernelMode;
    irp->UserBuffer = Buffer;

    irp_sp = IrpMgr_IoGetNextIrpStackLocation(irp);
    irp_sp->MajorFunction = (UCHAR)MajorFunction;
    irp_sp->DeviceObject = DeviceObject;
    if (MajorFunction == IRP_MJ_READ) {
        irp_sp->Parameters.Read.Length = Length;
        if (StartingOffset) {
            irp_sp->Parameters.Read.ByteOffset = *StartingOffset;
        }
        irp->Flags |= IRP_READ_OPERATION;
    } else if (MajorFunction == IRP_MJ_WRITE) {
        irp_sp->Parameters.Write.Length = Length;
        if (StartingOffset) {
            irp_sp->Parameters.Write.ByteOffset = *StartingOffset;
        }
        irp->Flags |= IRP_WRITE_OPERATION;
    }
    *(PVOID *)((PUCHAR)irp + 0x60) = (PVOID)((PUCHAR)irp_sp + 0x24);

    dbg_mark('G');
    dbg_mark((char)('0' + (MajorFunction & 0x0F)));
    VxD_Debug_Printf("NTK: IoBuildAsynchronousFsdRequest MJ=0x%02X\n",
                     (UCHAR)MajorFunction);
    return irp;
}


/* ================================================================
 * DRIVER OBJECT EXTENSIONS
 *
 * IoAllocateDriverObjectExtension: allocate per-driver extension.
 * IoGetDriverObjectExtension: retrieve it.
 * Simplified: single driver, single extension.
 * ================================================================ */

NTSTATUS NTAPI ntk_IoAllocateDriverObjectExtension(
    PDRIVER_OBJECT DriverObject, PVOID ClientId,
    ULONG ExtensionSize, PVOID *Extension)
{
    (void)DriverObject;

    if (!Extension) return STATUS_UNSUCCESSFUL;

    if (g_DriverObjExt) {
        /* Already allocated */
        *Extension = NULL;
        return STATUS_UNSUCCESSFUL;
    }

    g_DriverObjExt = VxD_HeapAllocate(ExtensionSize, HEAPF_ZEROINIT);
    if (!g_DriverObjExt) {
        VxD_Debug_Printf("NTK: IoAllocateDriverObjectExtension alloc failed\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    g_DriverObjExtId = ClientId;
    g_DriverObjExtSize = ExtensionSize;
    *Extension = g_DriverObjExt;

    return STATUS_SUCCESS;
}

PVOID NTAPI ntk_IoGetDriverObjectExtension(PDRIVER_OBJECT DriverObject,
                                            PVOID ClientId)
{
    (void)DriverObject;

    if (g_DriverObjExt && g_DriverObjExtId == ClientId) {
        return g_DriverObjExt;
    }
    return NULL;
}


/* ================================================================
 * MISC I/O STUBS
 *
 * These functions are imported by atapi.sys but either don't need
 * real implementations in a VxD context, or are not on the
 * critical path. They return success / no-op.
 * ================================================================ */

NTSTATUS NTAPI ntk_IoCreateSymbolicLink(PVOID SymLinkName, PVOID DevName)
{
    (void)SymLinkName;
    (void)DevName;

    VxD_Debug_Printf("NTK: IoCreateSymbolicLink stub\n");
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_IoDeleteSymbolicLink(PVOID SymLinkName)
{
    (void)SymLinkName;

    VxD_Debug_Printf("NTK: IoDeleteSymbolicLink stub\n");
    return STATUS_SUCCESS;
}

PCONFIGURATION_INFORMATION NTAPI ntk_IoGetConfigurationInformation(void)
{
    dbg_mark('C');
    if (!g_ConfigInfoInit) {
        RtlZeroMemory(&g_ConfigInfo, sizeof(g_ConfigInfo));
        g_ConfigInfo.Version = 1;
        g_ConfigInfoInit = TRUE;
    }
    return &g_ConfigInfo;
}

VOID NTAPI ntk_IoInvalidateDeviceRelations(PDEVICE_OBJECT DeviceObject,
                                             ULONG Type)
{
    (void)DeviceObject;
    (void)Type;
    /* No-op: PnP re-enumeration is not supported in VxD shim */
}

VOID NTAPI ntk_IoInvalidateDeviceState(PDEVICE_OBJECT DeviceObject)
{
    (void)DeviceObject;
    /* No-op */
}

NTSTATUS NTAPI ntk_IoReportDetectedDevice(PDRIVER_OBJECT DriverObject,
                                            ULONG LegacyBusType,
                                            ULONG BusNumber,
                                            ULONG SlotNumber,
                                            PVOID ResourceList,
                                            PVOID ResourceRequirements,
                                            BOOLEAN ResourceAssigned,
                                            PDEVICE_OBJECT *DeviceObject)
{
    NTSTATUS status;

    dbg_mark('D');

    (void)DriverObject;
    (void)LegacyBusType;
    (void)BusNumber;
    (void)SlotNumber;
    (void)ResourceList;
    (void)ResourceRequirements;
    (void)ResourceAssigned;

    VxD_Debug_Printf("NTK: IoReportDetectedDevice stub\n");
    if (!DeviceObject) {
        return STATUS_INVALID_PARAMETER;
    }

    ntk_init_legacy_pdo_driver();
    status = IrpMgr_IoCreateDevice(&g_ntk_legacy_pdo_driver,
                                   0,
                                   NULL,
                                   FILE_DEVICE_CONTROLLER,
                                   0,
                                   FALSE,
                                   DeviceObject);
    if (NT_SUCCESS(status) && *DeviceObject) {
        (*DeviceObject)->Flags &= ~DO_DEVICE_INITIALIZING;
        dbg_mark('P');
    }
    return status;
}

NTSTATUS NTAPI ntk_IoReportResourceForDetection(PDRIVER_OBJECT DriverObject,
                                                  PVOID DriverList,
                                                  ULONG DriverListSize,
                                                  PDEVICE_OBJECT DeviceObject,
                                                  PVOID DeviceList,
                                                  ULONG DeviceListSize,
                                                  PVOID ConflictDetected)
{
    BOOLEAN *pConflict;

    dbg_mark('R');

    (void)DriverObject;
    (void)DriverList;
    (void)DriverListSize;
    (void)DeviceObject;
    (void)DeviceList;
    (void)DeviceListSize;

    VxD_Debug_Printf("NTK: IoReportResourceForDetection stub\n");

    if (ConflictDetected) {
        pConflict = (BOOLEAN *)ConflictDetected;
        *pConflict = FALSE;  /* no conflict */
    }
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_IoOpenDeviceRegistryKey(PDEVICE_OBJECT DeviceObject,
                                            ULONG DevInstKeyType,
                                            ULONG DesiredAccess,
                                            PVOID DevInstRegKey)
{
    PVOID *pHandle;

    (void)DeviceObject;
    (void)DevInstKeyType;
    (void)DesiredAccess;

    VxD_Debug_Printf("NTK: IoOpenDeviceRegistryKey stub\n");

    if (DevInstRegKey) {
        pHandle = (PVOID *)DevInstRegKey;
        *pHandle = (PVOID)0xDEAD0004UL;
    }
    return STATUS_SUCCESS;
}

PVOID NTAPI ntk_PoRegisterDeviceForIdleDetection(
    PDEVICE_OBJECT DeviceObject, ULONG ConservationTime,
    ULONG PerformanceTime, ULONG State)
{
    (void)DeviceObject;
    (void)ConservationTime;
    (void)PerformanceTime;
    (void)State;

    VxD_Debug_Printf("NTK: PoRegisterDeviceForIdleDetection stub\n");
    return NULL;
}

NTSTATUS NTAPI ntk_IoWMIRegistrationControl(PDEVICE_OBJECT DeviceObject,
                                              ULONG Action)
{
    (void)DeviceObject;
    (void)Action;

    VxD_Debug_Printf("NTK: IoWMIRegistrationControl stub\n");
    return STATUS_SUCCESS;
}


/* ================================================================
 * WMILIB.SYS
 * ================================================================ */

NTSTATUS NTAPI ntk_WmiCompleteRequest(PDEVICE_OBJECT DeviceObject,
                                       PVOID Irp, NTSTATUS Status,
                                       ULONG BufferUsed, CHAR PrioBoost)
{
    (void)DeviceObject;
    (void)Irp;
    (void)Status;
    (void)BufferUsed;
    (void)PrioBoost;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI ntk_WmiSystemControl(PWMILIB_CONTEXT WmiLibInfo,
                                     PDEVICE_OBJECT DeviceObject,
                                     PVOID Irp,
                                     PSYSCTL_IRP_DISPOSITION IrpDisposition)
{
    (void)WmiLibInfo;
    (void)DeviceObject;
    (void)Irp;

    /*
     * Return IrpNotWmi so the driver handles the IRP itself.
     * This matches behavior when WMILIB doesn't recognize the
     * WMI request.
     */
    if (IrpDisposition) {
        *IrpDisposition = IrpNotWmi;
    }
    return STATUS_SUCCESS;
}


/* ================================================================
 * C RUNTIME / COMPILER INTRINSICS
 *
 * Minimal implementations of CRT functions that NT drivers import.
 * No libc dependency. Watcom C compatible (no C99).
 * ================================================================ */

/*
 * ntk_sprintf: simplified sprintf handling %s, %d, %u, %x, %08x, %lx, %ld, %lu.
 * Returns number of characters written (excluding null).
 */
static PCHAR ntk_utoa(ULONG val, PCHAR buf, ULONG base, BOOLEAN uppercase)
{
    PCHAR p = buf;
    PCHAR digits;
    CHAR tmp[12];
    int i = 0;

    if (uppercase) {
        digits = "0123456789ABCDEF";
    } else {
        digits = "0123456789abcdef";
    }

    if (val == 0) {
        *p++ = '0';
        *p = '\0';
        return p;
    }

    while (val > 0 && i < 11) {
        tmp[i++] = digits[val % base];
        val = val / base;
    }

    /* Reverse */
    while (i > 0) {
        *p++ = tmp[--i];
    }
    *p = '\0';
    return p;
}

int __cdecl ntk_sprintf(PCHAR buf, const PCHAR fmt, ...)
{
    /* Simplified: only handle %s, %d, %u, %x, %lx, %ld, %lu, %08x, %% */
    PCHAR dst = buf;
    PCHAR src = fmt;
    PVOID *args;
    int argIdx = 0;

    if (!buf || !fmt) return 0;

    /*
     * Access varargs manually via stack pointer.
     * After fmt on the stack come the arguments.
     */
    args = (PVOID *)(&fmt + 1);

    while (*src) {
        if (*src != '%') {
            *dst++ = *src++;
            continue;
        }

        src++;  /* skip '%' */

        if (*src == '%') {
            *dst++ = '%';
            src++;
            continue;
        }

        /* Check for width/flags */
        {
            BOOLEAN padZero = FALSE;
            int width = 0;
            BOOLEAN isLong = FALSE;

            if (*src == '0') {
                padZero = TRUE;
                src++;
            }

            while (*src >= '0' && *src <= '9') {
                width = width * 10 + (*src - '0');
                src++;
            }

            if (*src == 'l') {
                isLong = TRUE;
                src++;
            }

            (void)isLong;  /* all args are 32-bit on this platform */

            switch (*src) {
            case 's': {
                PCHAR s = (PCHAR)args[argIdx++];
                if (!s) s = "(null)";
                while (*s) *dst++ = *s++;
                src++;
                break;
            }
            case 'd': {
                LONG val = (LONG)args[argIdx++];
                CHAR tmp[12];
                if (val < 0) {
                    *dst++ = '-';
                    val = -val;
                }
                ntk_utoa((ULONG)val, tmp, 10, FALSE);
                {
                    PCHAR t = tmp;
                    int len = 0;
                    while (t[len]) len++;
                    while (padZero && len < width) { *dst++ = '0'; width--; }
                    t = tmp;
                    while (*t) *dst++ = *t++;
                }
                src++;
                break;
            }
            case 'u': {
                ULONG val = (ULONG)args[argIdx++];
                CHAR tmp[12];
                ntk_utoa(val, tmp, 10, FALSE);
                {
                    PCHAR t = tmp;
                    int len = 0;
                    while (t[len]) len++;
                    while (padZero && len < width) { *dst++ = '0'; width--; }
                    t = tmp;
                    while (*t) *dst++ = *t++;
                }
                src++;
                break;
            }
            case 'x':
            case 'X': {
                ULONG val = (ULONG)args[argIdx++];
                CHAR tmp[12];
                BOOLEAN upper = (*src == 'X') ? TRUE : FALSE;
                int len;
                PCHAR t;

                ntk_utoa(val, tmp, 16, upper);
                len = 0;
                t = tmp;
                while (t[len]) len++;

                while (padZero && len < width) {
                    *dst++ = '0';
                    width--;
                }
                t = tmp;
                while (*t) *dst++ = *t++;
                src++;
                break;
            }
            case 'c': {
                CHAR c = (CHAR)(ULONG)args[argIdx++];
                *dst++ = c;
                src++;
                break;
            }
            default:
                /* Unknown format, just output literally */
                *dst++ = '%';
                *dst++ = *src++;
                break;
            }
        }
    }

    *dst = '\0';
    return (int)(dst - buf);
}

/*
 * ntk_swprintf: simplified wide sprintf. Handles %s (wide), %d, %x.
 */
int __cdecl ntk_swprintf(PWSTR buf, PCWSTR fmt, ...)
{
    /* Very minimal: just copy format string as-is for now.
     * Full implementation would parse format specifiers on wide chars. */
    PWSTR dst = buf;
    PCWSTR src = fmt;

    if (!buf || !fmt) return 0;

    while (*src) {
        *dst++ = *src++;
    }
    *dst = 0;
    return (int)(dst - buf);
}

PCHAR __cdecl ntk_strstr(const PCHAR haystack, const PCHAR needle)
{
    PCHAR h;
    PCHAR n;

    if (!haystack || !needle) return NULL;
    if (!*needle) return (PCHAR)haystack;

    h = (PCHAR)haystack;
    while (*h) {
        PCHAR h2 = h;
        n = (PCHAR)needle;

        while (*h2 && *n && *h2 == *n) {
            h2++;
            n++;
        }

        if (!*n) return h;  /* found */
        h++;
    }

    return NULL;
}

PCHAR __cdecl ntk_strupr(PCHAR str)
{
    PCHAR p = str;
    if (!str) return NULL;

    while (*p) {
        if (*p >= 'a' && *p <= 'z') {
            *p = *p - ('a' - 'A');
        }
        p++;
    }
    return str;
}

PVOID __cdecl ntk_memmove(PVOID dest, PVOID src, SIZE_T count)
{
    RtlMoveMemory(dest, src, count);
    return dest;
}

/*
 * ntk_except_handler3: Simplified SEH handler.
 * On Win9x VxD we don't have real structured exception handling.
 * Return EXCEPTION_CONTINUE_SEARCH (1) to let the system handle it.
 */
int __cdecl ntk_except_handler3(PVOID ExceptionRecord,
                                 PVOID EstablisherFrame,
                                 PVOID ContextRecord,
                                 PVOID DispatcherContext)
{
    (void)ExceptionRecord;
    (void)EstablisherFrame;
    (void)ContextRecord;
    (void)DispatcherContext;

    VxD_Debug_Printf("NTK: _except_handler3 called, continuing search\n");
    return 1;  /* EXCEPTION_CONTINUE_SEARCH */
}

/*
 * 64-bit math: _aulldiv (unsigned), _allmul, _alldiv (signed).
 * On 32-bit x86, the compiler generates calls to these for
 * 64-bit arithmetic.
 */
ULONGLONG __cdecl ntk_aulldiv(ULONGLONG a, ULONGLONG b)
{
    if (b == 0) {
        VxD_Debug_Printf("NTK: _aulldiv divide by zero!\n");
        return 0;
    }
    return a / b;
}

LONGLONG __cdecl ntk_allmul(LONGLONG a, LONGLONG b)
{
    return a * b;
}

LONGLONG __cdecl ntk_alldiv(LONGLONG a, LONGLONG b)
{
    if (b == 0) {
        VxD_Debug_Printf("NTK: _alldiv divide by zero!\n");
        return 0;
    }
    return a / b;
}


/* ================================================================
 * INTERLOCKED OPERATIONS
 *
 * On single-CPU Win9x, these are cli/sti + operation.
 * No SMP concerns.
 * ================================================================ */

LONG NTAPI ntk_InterlockedDecrement(PLONG Addend)
{
    LONG result;
    ULONG flags;

    PORT_SAVE_FLAGS_CLI(flags);
    (*Addend)--;
    result = *Addend;
    PORT_RESTORE_FLAGS(flags);

    return result;
}

LONG NTAPI ntk_InterlockedIncrement(PLONG Addend)
{
    LONG result;
    ULONG flags;

    PORT_SAVE_FLAGS_CLI(flags);
    (*Addend)++;
    result = *Addend;
    PORT_RESTORE_FLAGS(flags);

    return result;
}

LONG NTAPI ntk_InterlockedExchange(PLONG Target, LONG Value)
{
    LONG old;
    ULONG flags;

    PORT_SAVE_FLAGS_CLI(flags);
    old = *Target;
    *Target = Value;
    PORT_RESTORE_FLAGS(flags);

    return old;
}


/* ================================================================
 * MISCELLANEOUS
 * ================================================================ */

ULONG __cdecl DbgPrint(const char *Format, ...)
{
    /*
     * Redirect to VxD debug output. We cannot easily handle varargs
     * across calling conventions, so just print the format string.
     * A proper implementation would use vsprintf, but we avoid
     * pulling in libc. The format string alone is usually enough
     * for debugging.
     */
    VxD_Debug_Printf("NTK: %s", Format);
    return 0;
}

VOID NTAPI KeBugCheckEx(ULONG Code, ULONG_PTR P1, ULONG_PTR P2,
                        ULONG_PTR P3, ULONG_PTR P4)
{
    VxD_Debug_Printf("NTK: *** BUGCHECK 0x%08lX ***\n", Code);
    VxD_Debug_Printf("NTK:   P1=0x%08lX P2=0x%08lX P3=0x%08lX P4=0x%08lX\n",
                     (ULONG)P1, (ULONG)P2, (ULONG)P3, (ULONG)P4);
    VxD_Debug_Printf("NTK: System halted.\n");

    /* Halt the CPU. On a real system this is fatal. */
    PORT_CLI_HLT();

    /* Should never reach here, but satisfy the compiler */
    for (;;) {}
}

PVOID NTAPI IoGetCurrentProcess(void)
{
    /*
     * NT drivers occasionally call this. Return a non-NULL fake
     * pointer so NULL checks pass. The "process" concept does not
     * apply in VxD context.
     */
    return (PVOID)0xFFFF0000UL;
}

KIRQL NTAPI KeGetCurrentIrql(void)
{
    /*
     * In VxD context we are always at ring 0 with no preemption.
     * Return PASSIVE_LEVEL unless we have explicitly raised it
     * (via spinlock acquire or DPC execution).
     */
    if (g_nt5_trace_ports) {
        dbg_mark('I');
    }
    return g_CurrentIrql;
}


/* ================================================================
 * EXPORTED VARIABLE: KeQueryTimeIncrement
 *
 * On NT this returns the number of 100ns units per clock tick.
 * Default is 10ms = 100,000 units of 100ns.
 * ================================================================ */

ULONG ntk_KeQueryTimeIncrement = 100000; /* 10ms in 100ns units */


/* ================================================================
 * HAL: HalGetInterruptVector
 *
 * On x86 ISA/PCI, the interrupt vector is typically the IRQ
 * number plus a base offset. For legacy IDE (IRQ 14/15), the
 * vector mapping is identity on our shim. We return the
 * BusInterruptVector unchanged and set IRQL to DIRQL.
 * ================================================================ */

ULONG NTAPI ntk_HalGetInterruptVector(
    ULONG InterfaceType, ULONG BusNumber, ULONG BusInterruptLevel,
    ULONG BusInterruptVector, PKIRQL Irql, PULONG Affinity)
{
    (void)InterfaceType;
    (void)BusNumber;
    (void)BusInterruptLevel;

    if (Irql) {
        *Irql = DIRQL;
    }
    if (Affinity) {
        *Affinity = 1;  /* single CPU */
    }
    return BusInterruptVector;
}


/* ================================================================
 * HAL: Kf* (fast IRQL/spinlock) stubs
 *
 * On NT these are FASTCALL. In our single-CPU VxD they are cdecl
 * wrappers around cli/sti and the notional IRQL variable.
 * ================================================================ */

KIRQL FASTCALL ntk_KfAcquireSpinLock(PKSPIN_LOCK SpinLock)
{
    KIRQL oldIrql;

    if (g_nt5_trace_ports) {
        dbg_mark('A');
    }
    oldIrql = g_CurrentIrql;
    g_CurrentIrql = DISPATCH_LEVEL;

    (void)SpinLock;
    return oldIrql;
}

VOID FASTCALL ntk_KfReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL OldIrql)
{
    if (g_nt5_trace_ports) {
        dbg_mark('R');
    }
    (void)SpinLock;
    g_CurrentIrql = OldIrql;
}

KIRQL FASTCALL ntk_KfRaiseIrql(KIRQL NewIrql)
{
    KIRQL old = g_CurrentIrql;
    if (g_nt5_trace_ports) {
        dbg_mark('^');
    }
    if (NewIrql > g_CurrentIrql) {
        g_CurrentIrql = NewIrql;
    }
    return old;
}

VOID FASTCALL ntk_KfLowerIrql(KIRQL NewIrql)
{
    if (g_nt5_trace_ports) {
        dbg_mark('v');
    }
    g_CurrentIrql = NewIrql;
}


/* ================================================================
 * HAL: KeStallExecutionProcessor
 *
 * Busy-wait for the specified number of microseconds using port
 * 0x80 reads (~1us each on ISA bus). Same technique as the
 * PORT_STALL_ONE macro from PORTIO.H.
 * ================================================================ */

VOID NTAPI ntk_KeStallExecutionProcessor(ULONG Microseconds)
{
    ULONG i;
    for (i = 0; i < Microseconds; i++) {
        PORT_STALL_ONE();
    }
}


/* ================================================================
 * HAL: Buffer port I/O (USHORT)
 *
 * READ_PORT_BUFFER_USHORT / WRITE_PORT_BUFFER_USHORT use REP
 * INSW/OUTSW via the PORTIO.H macros for efficient PIO transfers.
 * ================================================================ */

VOID NTAPI ntk_READ_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer,
                                        ULONG Count)
{
    ntk_trace_port('B', (ULONG)Port, Count);
    PORT_READ_BUFFER_USHORT((unsigned short)(unsigned long)Port,
                            Buffer, Count);
}

VOID NTAPI ntk_WRITE_PORT_BUFFER_USHORT(PUSHORT Port, PUSHORT Buffer,
                                          ULONG Count)
{
    ntk_trace_port('W', (ULONG)Port, Count);
    PORT_WRITE_BUFFER_USHORT((unsigned short)(unsigned long)Port,
                             Buffer, Count);
}
