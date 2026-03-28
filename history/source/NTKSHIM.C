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
#include "PORTIO.H"

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

/* Heap flags */
#define HEAPF_ZEROINIT  0x0001

/* ================================================================
 * INTERNAL STATE
 * ================================================================ */

/* Current (notional) IRQL. Real synchronisation is cli/sti. */
static KIRQL g_CurrentIrql = PASSIVE_LEVEL;

/* DPC queue: simple fixed-size array */
static PKDPC g_DpcQueue[NTK_MAX_PENDING_DPCS];
static ULONG g_DpcCount = 0;

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
 * EVENTS
 *
 * Simple flag-based implementation. On single-CPU Win9x we cannot
 * truly block, so KeWaitForSingleObject busy-waits with
 * Time_Slice_Sleep to yield the CPU.
 * ================================================================ */

VOID NTAPI KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State)
{
    if (!Event) return;

    Event->Type = Type;
    Event->SignalState = State ? 1 : 0;
}

LONG NTAPI KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait)
{
    LONG previousState;

    (void)Increment;
    (void)Wait;

    if (!Event) return 0;

    previousState = Event->SignalState;
    Event->SignalState = 1;

    return previousState;
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

    if (!Object) {
        return STATUS_UNSUCCESSFUL;
    }

    event = (PKEVENT)Object;

    /* If already signaled, consume and return immediately */
    if (event->SignalState) {
        if (event->Type == SynchronizationEvent) {
            event->SignalState = 0;  /* auto-reset */
        }
        return STATUS_SUCCESS;
    }

    /* NULL timeout = wait forever (dangerous in VxD, but honor it) */
    if (!Timeout) {
        timeout_ms = 0xFFFFFFFF; /* ~49 days, effectively forever */
    } else if (Timeout->QuadPart == 0) {
        /* Zero timeout = poll only */
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
        if (timeout_ms == 0) return STATUS_TIMEOUT;
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
            return STATUS_TIMEOUT;
        }
    }

    /* Consume the signal for synchronization events */
    if (event->Type == SynchronizationEvent) {
        event->SignalState = 0;
    }

    return STATUS_SUCCESS;
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
        VxD_HeapFree(Ptr, 0);
    }
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

    /* No-op: fake handles need no cleanup */
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
    return PORT_IN_BYTE((unsigned long)Port);
}

USHORT NTAPI READ_PORT_USHORT(PUSHORT Port)
{
    return PORT_IN_WORD((unsigned long)Port);
}

ULONG NTAPI READ_PORT_ULONG(PULONG Port)
{
    return PORT_IN_DWORD((unsigned long)Port);
}

VOID NTAPI WRITE_PORT_UCHAR(PUCHAR Port, UCHAR Value)
{
    PORT_OUT_BYTE((unsigned long)Port, Value);
}

VOID NTAPI WRITE_PORT_USHORT(PUSHORT Port, USHORT Value)
{
    PORT_OUT_WORD((unsigned long)Port, Value);
}

VOID NTAPI WRITE_PORT_ULONG(PULONG Port, ULONG Value)
{
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
    return g_CurrentIrql;
}
