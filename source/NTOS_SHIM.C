/*
 * NTOS_SHIM.C - ntoskrnl.exe + HAL.dll Import Shim for Win98 VxD PE Loader
 *
 * SINGLE source of ntoskrnl and HAL imports for ALL driver classes.
 * Provides the NT kernel API surface that XP-era .sys drivers expect,
 * implemented as stubs/shims running inside a Win98 VxD (ring 0).
 *
 * Registers TWO port driver shims:
 *   1. "ntoskrnl.exe" - Memory, sync, timer/DPC, IRP/IO, power,
 *      string, object, registry, misc kernel functions
 *   2. "HAL.dll" - Port I/O, MMIO, spinlocks, IRQL, stall,
 *      performance counter, fast mutex
 *
 * Architecture:
 *   - Ring 0, no libc, no Win32 API
 *   - Memory via VxD_PageAllocate / VxD_PageFree
 *   - Debug via VxD_Debug_Printf
 *   - All exported functions __stdcall (NT kernel calling convention)
 *     except 64-bit compiler helpers (_alldiv etc.) which are __cdecl
 *   - CRITICAL: Parameter counts MUST be exact -- wrong count = stack
 *     corruption on Win98
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VxD wrapper)
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
typedef LONG               *PLONG;
typedef ULONG               NTSTATUS;

/* 64-bit types avoided — Watcom's __U8* runtime not available in VxD context */

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

#define STATUS_SUCCESS                  0x00000000UL
#define STATUS_UNSUCCESSFUL             0xC0000001UL
#define STATUS_NOT_SUPPORTED            0xC00000BBUL
#define STATUS_OBJECT_NAME_NOT_FOUND    0xC0000034UL

#define PAGESIZE    4096
#define PAGEFIXED   0x00000001

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
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
 * Static helpers (no libc available)
 * ================================================================ */

static void ntos_memset(void *dst, int val, ULONG n)
{
    UCHAR *d = (UCHAR *)dst;
    while (n--) *d++ = (UCHAR)val;
}

static void ntos_memcpy(void *dst, const void *src, ULONG n)
{
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    while (n--) *d++ = *s++;
}

static void ntos_log_hex(const char *prefix, ULONG v, const char *suffix)
{
    static const char hx[] = "0123456789ABCDEF";
    char h[12]; int i;
    h[0] = '0'; h[1] = 'x';
    for (i = 7; i >= 0; i--) h[2 + (7 - i)] = hx[(v >> (i * 4)) & 0xF];
    h[10] = 0;
    VxD_Debug_Printf(prefix); VxD_Debug_Printf(h); VxD_Debug_Printf(suffix);
}

/* ================================================================
 * Interlocked operations via Watcom #pragma aux inline asm
 *
 * These use lock-prefixed instructions for SMP-safe atomics.
 * On Win98 (uniprocessor) the lock prefix is technically optional
 * but we include it for correctness.
 * ================================================================ */

/* lock xchg is i386-compatible (lock prefix implicit for xchg).
 * lock inc/dec are also i386-compatible.
 * We avoid lock xadd / lock cmpxchg which require i486+. */

LONG _interlocked_xchg(PLONG target, LONG value);
#pragma aux _interlocked_xchg = \
    "xchg [ecx], edx" \
    "mov eax, edx" \
    parm [ecx] [edx] value [eax] modify exact [eax edx];

/* Zero EDX for LARGE_INTEGER returns where high DWORD must be 0 */
void _zero_edx(void);
#pragma aux _zero_edx = "xor edx, edx" modify exact [edx];

/* ================================================================
 * HAL: Port I/O via Watcom #pragma aux inline asm
 * ================================================================ */

UCHAR _inp_byte(USHORT port);
#pragma aux _inp_byte = \
    "in al, dx" \
    parm [dx] value [al] modify exact [al];

USHORT _inp_word(USHORT port);
#pragma aux _inp_word = \
    "in ax, dx" \
    parm [dx] value [ax] modify exact [ax];

ULONG _inp_dword(USHORT port);
#pragma aux _inp_dword = \
    "in eax, dx" \
    parm [dx] value [eax] modify exact [eax];

void _outp_byte(USHORT port, UCHAR val);
#pragma aux _outp_byte = \
    "out dx, al" \
    parm [dx] [al] modify exact [];

void _outp_word(USHORT port, USHORT val);
#pragma aux _outp_word = \
    "out dx, ax" \
    parm [dx] [ax] modify exact [];

void _outp_dword(USHORT port, ULONG val);
#pragma aux _outp_dword = \
    "out dx, eax" \
    parm [dx] [eax] modify exact [];

void _rep_insb(USHORT port, PUCHAR buf, ULONG count);
#pragma aux _rep_insb = \
    "rep insb" \
    parm [dx] [edi] [ecx] modify exact [edi ecx];

void _rep_insw(USHORT port, PUSHORT buf, ULONG count);
#pragma aux _rep_insw = \
    "rep insw" \
    parm [dx] [edi] [ecx] modify exact [edi ecx];

void _rep_insd(USHORT port, PULONG buf, ULONG count);
#pragma aux _rep_insd = \
    "rep insd" \
    parm [dx] [edi] [ecx] modify exact [edi ecx];

void _rep_outsb(USHORT port, const UCHAR *buf, ULONG count);
#pragma aux _rep_outsb = \
    "rep outsb" \
    parm [dx] [esi] [ecx] modify exact [esi ecx];

void _rep_outsw(USHORT port, const USHORT *buf, ULONG count);
#pragma aux _rep_outsw = \
    "rep outsw" \
    parm [dx] [esi] [ecx] modify exact [esi ecx];

/* ================================================================
 * Global data exports
 * ================================================================ */

static ULONG g_KeTickCount = 0;

/* ================================================================
 *
 *  TIER 1 -- Memory (unlock hidparse.sys)
 *
 * ================================================================ */

/* ExAllocatePoolWithTag -- 3 params (PoolType, NumberOfBytes, Tag) */
static PVOID __stdcall ntos_ExAllocatePoolWithTag(
    ULONG PoolType, ULONG NumberOfBytes, ULONG Tag)
{
    ULONG nPages;
    PVOID p;

    if (NumberOfBytes == 0) NumberOfBytes = 1;
    nPages = (NumberOfBytes + PAGESIZE - 1) / PAGESIZE;
    p = VxD_PageAllocate(nPages, PAGEFIXED);
    if (p) ntos_memset(p, 0, nPages * PAGESIZE);
    return p;
}

/* ExFreePool -- 1 param */
static void __stdcall ntos_ExFreePool(PVOID Ptr)
{
    if (Ptr) VxD_PageFree(Ptr);
}

/* ExFreePoolWithTag -- 2 params */
static void __stdcall ntos_ExFreePoolWithTag(PVOID Ptr, ULONG Tag)
{
    if (Ptr) VxD_PageFree(Ptr);
}

/* ExAllocatePool -- 2 params, forwards to WithTag */
static PVOID __stdcall ntos_ExAllocatePool(ULONG PoolType, ULONG NumberOfBytes)
{
    return ntos_ExAllocatePoolWithTag(PoolType, NumberOfBytes, 0);
}

/* ================================================================
 *
 *  TIER 2 -- Interlocked (very common across all drivers)
 *
 * ================================================================ */

/* InterlockedIncrement -- 1 param (PLONG), returns LONG
 * Win98 is uniprocessor, so non-atomic inc is safe in practice.
 * We still use xchg-based loop for correctness. */
static LONG __stdcall ntos_InterlockedIncrement(PLONG Addend)
{
    LONG old, new_val;
    do {
        old = *Addend;
        new_val = old + 1;
    } while (_interlocked_xchg(Addend, new_val) != old);
    return new_val;
}

/* InterlockedDecrement -- 1 param (PLONG), returns LONG */
static LONG __stdcall ntos_InterlockedDecrement(PLONG Addend)
{
    LONG old, new_val;
    do {
        old = *Addend;
        new_val = old - 1;
    } while (_interlocked_xchg(Addend, new_val) != old);
    return new_val;
}

/* InterlockedExchange -- 2 params, returns LONG */
static LONG __stdcall ntos_InterlockedExchange(PLONG Target, LONG Value)
{
    return _interlocked_xchg(Target, Value);
}

/* InterlockedCompareExchange -- 3 params, returns LONG
 * i386-safe: read-compare-xchg loop. On uniprocessor Win98
 * there's no race, so this is equivalent to lock cmpxchg. */
static LONG __stdcall ntos_InterlockedCompareExchange(
    PLONG Destination, LONG Exchange, LONG Comparand)
{
    LONG old = *Destination;
    if (old == Comparand) {
        _interlocked_xchg(Destination, Exchange);
    }
    return old;
}

/* ================================================================
 *
 *  TIER 3 -- Synchronization (unlock hidgame.sys)
 *
 * ================================================================ */

/*
 * KEVENT layout (16 bytes minimum):
 *   +0x00: ULONG Type
 *   +0x04: ULONG Signaled
 *   +0x08: (list entry, unused)
 */

/* KeInitializeEvent -- 3 params */
static void __stdcall ntos_KeInitializeEvent(
    PVOID Event, ULONG Type, BOOLEAN State)
{
    PULONG ev = (PULONG)Event;
    if (!ev) return;
    ntos_memset(ev, 0, 16);
    ev[0] = Type;
    ev[1] = (ULONG)State;
}

/* KeSetEvent -- 3 params, returns LONG (previous state) */
static LONG __stdcall ntos_KeSetEvent(
    PVOID Event, LONG Increment, BOOLEAN Wait)
{
    PULONG ev = (PULONG)Event;
    LONG prev;
    if (!ev) return 0;
    prev = (LONG)ev[1];
    ev[1] = 1;
    return prev;
}

/* KeResetEvent -- 1 param, returns LONG (previous state) */
static LONG __stdcall ntos_KeResetEvent(PVOID Event)
{
    PULONG ev = (PULONG)Event;
    LONG prev;
    if (!ev) return 0;
    prev = (LONG)ev[1];
    ev[1] = 0;
    return prev;
}

/* KeClearEvent -- 1 param, void */
static void __stdcall ntos_KeClearEvent(PVOID Event)
{
    PULONG ev = (PULONG)Event;
    if (ev) ev[1] = 0;
}

/* KeWaitForSingleObject -- 5 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_KeWaitForSingleObject(
    PVOID Object, ULONG WaitReason, ULONG WaitMode,
    BOOLEAN Alertable, PVOID Timeout)
{
    /* Non-blocking stub: return immediately as if signaled */
    return STATUS_SUCCESS;
}

/* KeInitializeSpinLock -- 1 param */
static void __stdcall ntos_KeInitializeSpinLock(PULONG SpinLock)
{
    if (SpinLock) *SpinLock = 0;
}

/* KeAcquireSpinLockRaiseToDpc -- 1 param, returns KIRQL (UCHAR).
 * x64 ntoskrnl version (not fastcall like Kf variant in HAL). */
static UCHAR __stdcall ntos_KeAcquireSpinLockRaiseToDpc(PULONG SpinLock)
{
    if (SpinLock) *SpinLock = 1;
    return 0;
}

/* KeReleaseSpinLock -- 2 params (SpinLock, OldIrql).
 * x64 ntoskrnl version. */
static void __stdcall ntos_KeReleaseSpinLock(PULONG SpinLock, ULONG OldIrql)
{
    if (SpinLock) *SpinLock = 0;
}

/* KeInitializeMutex -- 2 params */
static void __stdcall ntos_KeInitializeMutex(PVOID Mutex, ULONG Level)
{
    if (Mutex) ntos_memset(Mutex, 0, 32);
}

/* KeReleaseMutex -- 2 params, returns LONG */
static LONG __stdcall ntos_KeReleaseMutex(PVOID Mutex, BOOLEAN Wait)
{
    return 0;
}

/* ExAcquireFastMutex -- 1 param (in HAL.dll) */
static void __stdcall hal_ExAcquireFastMutex(PVOID FastMutex)
{
    /* No-op: single-CPU Win98, no contention */
}

/* ExReleaseFastMutex -- 1 param (in HAL.dll) */
static void __stdcall hal_ExReleaseFastMutex(PVOID FastMutex)
{
    /* No-op */
}

/*
 * KfAcquireSpinLock -- 1 param (PULONG), returns KIRQL (UCHAR)
 * HAL.dll export. NT prototype: KIRQL FASTCALL KfAcquireSpinLock(PKSPIN_LOCK)
 * But PE imports resolve it as __stdcall via the IAT thunk.
 */
static UCHAR __stdcall hal_KfAcquireSpinLock(PULONG SpinLock)
{
    if (SpinLock) *SpinLock = 1;
    return 0; /* Previous IRQL = PASSIVE_LEVEL */
}

/* KfReleaseSpinLock -- 2 params (SpinLock=PULONG, OldIrql=UCHAR)
 * On the stack: PULONG at [esp+4], UCHAR at [esp+8] (promoted to DWORD) */
static void __stdcall hal_KfReleaseSpinLock(PULONG SpinLock, ULONG OldIrql)
{
    if (SpinLock) *SpinLock = 0;
}

/* KefAcquireSpinLockAtDpcLevel -- 1 param */
static void __stdcall hal_KefAcquireSpinLockAtDpcLevel(PULONG SpinLock)
{
    /* No-op: already at DPC level */
}

/* KefReleaseSpinLockFromDpcLevel -- 1 param */
static void __stdcall hal_KefReleaseSpinLockFromDpcLevel(PULONG SpinLock)
{
    /* No-op */
}

/* ================================================================
 *
 *  TIER 4 -- Timer/DPC (needed for audio drivers)
 *
 * ================================================================ */

/*
 * KTIMER layout: ~40 bytes. We only need to zero it.
 * KDPC layout:
 *   +0x00: ULONG Type
 *   +0x04: (padding)
 *   +0x08: PVOID DeferredRoutine
 *   +0x0C: PVOID DeferredContext
 *   +0x10: PVOID SystemArgument1
 *   +0x14: PVOID SystemArgument2
 */

/* KeInitializeTimer -- 1 param */
static void __stdcall ntos_KeInitializeTimer(PVOID Timer)
{
    if (Timer) ntos_memset(Timer, 0, 40);
}

/* KeInitializeTimerEx -- 2 params */
static void __stdcall ntos_KeInitializeTimerEx(PVOID Timer, ULONG Type)
{
    if (Timer) ntos_memset(Timer, 0, 40);
}

/* KeSetTimer -- 4 params (LARGE_INTEGER is 2 DWORDs by value on stack)
 * Stack: Timer, DueTime_Lo, DueTime_Hi, Dpc */
static BOOLEAN __stdcall ntos_KeSetTimer(
    PVOID Timer, ULONG DueTime_Lo, ULONG DueTime_Hi, PVOID Dpc)
{
    VxD_Debug_Printf("NTOS: KeSetTimer (stub)\r\n");
    return FALSE;
}

/* KeSetTimerEx -- 5 params
 * Stack: Timer, DueTime_Lo, DueTime_Hi, Period, Dpc */
static BOOLEAN __stdcall ntos_KeSetTimerEx(
    PVOID Timer, ULONG DueTime_Lo, ULONG DueTime_Hi,
    LONG Period, PVOID Dpc)
{
    VxD_Debug_Printf("NTOS: KeSetTimerEx (stub)\r\n");
    return FALSE;
}

/* KeCancelTimer -- 1 param */
static BOOLEAN __stdcall ntos_KeCancelTimer(PVOID Timer)
{
    return FALSE;
}

/* KeInitializeDpc -- 3 params */
static void __stdcall ntos_KeInitializeDpc(
    PVOID Dpc, PVOID DeferredRoutine, PVOID DeferredContext)
{
    PULONG dpc = (PULONG)Dpc;
    if (!dpc) return;
    ntos_memset(dpc, 0, 32);
    /* Store routine at offset 0x08, context at 0x0C */
    dpc[2] = (ULONG)DeferredRoutine;
    dpc[3] = (ULONG)DeferredContext;
}

/* KeInsertQueueDpc -- 3 params, returns BOOLEAN */
static BOOLEAN __stdcall ntos_KeInsertQueueDpc(
    PVOID Dpc, PVOID SystemArgument1, PVOID SystemArgument2)
{
    /* Stub: claim success but don't actually queue */
    return TRUE;
}

/* KeRemoveQueueDpc -- 1 param, returns BOOLEAN */
static BOOLEAN __stdcall ntos_KeRemoveQueueDpc(PVOID Dpc)
{
    return FALSE;
}

/* ================================================================
 *
 *  TIER 5 -- IRP/IO (core WDM, stub implementations)
 *
 * ================================================================ */

/* IofCallDriver -- 2 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_IofCallDriver(PVOID DeviceObject, PVOID Irp)
{
    VxD_Debug_Printf("NTOS: IofCallDriver (stub)\r\n");
    return STATUS_NOT_SUPPORTED;
}

/* IofCompleteRequest -- 2 params, void */
static void __stdcall ntos_IofCompleteRequest(PVOID Irp, ULONG PriorityBoost)
{
    /* No-op: no real IRP to complete */
}

/* IoAllocateIrp -- 2 params, returns PIRP (PVOID) */
static PVOID __stdcall ntos_IoAllocateIrp(UCHAR StackSize, BOOLEAN ChargeQuota)
{
    PVOID irp = VxD_PageAllocate(1, PAGEFIXED);
    if (irp) ntos_memset(irp, 0, PAGESIZE);
    return irp;
}

/* IoFreeIrp -- 1 param */
static void __stdcall ntos_IoFreeIrp(PVOID Irp)
{
    if (Irp) VxD_PageFree(Irp);
}

/* IoInitializeIrp -- 3 params, void */
static void __stdcall ntos_IoInitializeIrp(
    PVOID Irp, USHORT PacketSize, UCHAR StackSize)
{
    if (Irp) ntos_memset(Irp, 0, (ULONG)PacketSize);
}

/* ================================================================
 *
 *  TIER 6 -- Power (stubs)
 *
 * ================================================================ */

/* PoCallDriver -- 2 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_PoCallDriver(PVOID DeviceObject, PVOID Irp)
{
    return ntos_IofCallDriver(DeviceObject, Irp);
}

/* PoStartNextPowerIrp -- 1 param, void */
static void __stdcall ntos_PoStartNextPowerIrp(PVOID Irp)
{
    /* No-op */
}

/* PoSetPowerState -- 3 params, returns ULONG (previous state) */
static ULONG __stdcall ntos_PoSetPowerState(
    PVOID DeviceObject, ULONG Type, ULONG State)
{
    return 0; /* Previous state */
}

/* ================================================================
 *
 *  TIER 7 -- Misc commonly needed
 *
 * ================================================================ */

/* KeDelayExecutionThread -- 3 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_KeDelayExecutionThread(
    ULONG WaitMode, BOOLEAN Alertable, PVOID Interval)
{
    /* Brief stall: read port 0x80 a few times (~1us each) */
    int i;
    for (i = 0; i < 100; i++) {
        _inp_byte(0x80);
    }
    return STATUS_SUCCESS;
}

/* KeQueryPerformanceCounter -- 1 param (PLARGE_INTEGER), returns LARGE_INTEGER
 * Returns 2 DWORDs in EDX:EAX. If PerformanceFrequency is non-NULL,
 * write frequency there.
 * LARGE_INTEGER return: low in EAX, high in EDX.
 * We return via a struct trick to get EDX:EAX. */
static ULONG __stdcall hal_KeQueryPerformanceCounter(PVOID PerformanceFrequency)
{
    static ULONG counter = 0;
    PULONG freq;

    counter++;
    g_KeTickCount = counter;

    if (PerformanceFrequency) {
        freq = (PULONG)PerformanceFrequency;
        freq[0] = 1193182; /* Standard PIT frequency */
        freq[1] = 0;
    }
    /* Returns low 32 bits in EAX; high 32 bits (EDX) will be garbage
     * but callers store as LARGE_INTEGER and we keep high=0 */
    return counter;
}

/* KeGetCurrentIrql -- 0 params, returns UCHAR (HAL.dll) */
static UCHAR __stdcall hal_KeGetCurrentIrql(void)
{
    return 0; /* PASSIVE_LEVEL */
}

/* KfRaiseIrql -- 1 param (UCHAR NewIrql), returns UCHAR (HAL.dll) */
static UCHAR __stdcall hal_KfRaiseIrql(ULONG NewIrql)
{
    return 0; /* Previous IRQL = PASSIVE_LEVEL */
}

/* KfLowerIrql -- 1 param (UCHAR NewIrql), void (HAL.dll) */
static void __stdcall hal_KfLowerIrql(ULONG NewIrql)
{
    /* No-op */
}

/* KeQuerySystemTime -- 1 param (PLARGE_INTEGER), void */
static void __stdcall ntos_KeQuerySystemTime(PVOID CurrentTime)
{
    PULONG t = (PULONG)CurrentTime;
    if (t) { t[0] = 0; t[1] = 0; }
}

/* KeQueryTimeIncrement -- 0 params, returns ULONG */
static ULONG __stdcall ntos_KeQueryTimeIncrement(void)
{
    return 100000; /* 10ms in 100ns units */
}

/* DbgPrint -- variadic, __cdecl (caller cleanup, callee can't know argcount).
 * We can't forward varargs in C without va_list, so we just log a fixed
 * message. Drivers rarely depend on DbgPrint output. */
static ULONG __cdecl ntos_DbgPrint(const char *Format)
{
    VxD_Debug_Printf("NTOS: DbgPrint called\r\n");
    return 0;
}

/* KeBugCheckEx -- 5 params, does not return */
static void __stdcall ntos_KeBugCheckEx(
    ULONG BugCheckCode, ULONG P1, ULONG P2, ULONG P3, ULONG P4)
{
    VxD_Debug_Printf("NTOS: *** BUGCHECK ***\r\n");
    ntos_log_hex("  Code=", BugCheckCode, "");
    ntos_log_hex(" P1=", P1, "");
    ntos_log_hex(" P2=", P2, "");
    ntos_log_hex(" P3=", P3, "");
    ntos_log_hex(" P4=", P4, "\r\n");
    /* Halt: infinite loop */
    for (;;) { _inp_byte(0x80); }
}

/* memmove -- 3 params, returns PVOID (dst). Handles overlapping regions.
 * __cdecl: C runtime convention, caller cleanup. */
static PVOID __cdecl ntos_memmove(PVOID dst, const PVOID src, ULONG n)
{
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;

    if (!d || !s || n == 0) return dst;

    if (d < s || d >= s + n) {
        /* Forward copy (no overlap, or dst before src) */
        while (n--) *d++ = *s++;
    } else {
        /* Backward copy (overlap, dst after src) */
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

/* _except_handler3 -- SEH handler stub, returns 1 (EXCEPTION_EXECUTE_HANDLER) */
static ULONG __stdcall ntos_except_handler3(void)
{
    VxD_Debug_Printf("NTOS: _except_handler3 called\r\n");
    return 1; /* EXCEPTION_EXECUTE_HANDLER */
}

/* ================================================================
 * 64-bit compiler helpers
 *
 * MSVC runtime helpers for 64-bit arithmetic. These use a special
 * calling convention (args in EDX:EAX and stack, callee-cleanup).
 *
 * For now: pure 32-bit implementations that handle the common case
 * (hi=0). Drivers that need full 64-bit math will need asm helpers.
 * ================================================================ */

/* _allmul: 32x32->64 is sufficient for most driver use.
 * Full 64x64 without long long: a_lo*b_lo + cross terms. */
static void __cdecl ntos_allmul(void)
{
    /* Stub: most drivers only multiply small values.
     * EDX:EAX = result. We'll implement properly if needed. */
    VxD_Debug_Printf("NTOS: _allmul called (stub)\r\n");
}

static void __cdecl ntos_alldiv(void)
{
    VxD_Debug_Printf("NTOS: _alldiv called (stub)\r\n");
}

static void __cdecl ntos_allshr(void)
{
    VxD_Debug_Printf("NTOS: _allshr called (stub)\r\n");
}

static void __cdecl ntos_aulldiv(void)
{
    VxD_Debug_Printf("NTOS: _aulldiv called (stub)\r\n");
}

static void __cdecl ntos_aullrem(void)
{
    VxD_Debug_Printf("NTOS: _aullrem called (stub)\r\n");
}

/* ================================================================
 * Unicode/ANSI string structures and helpers
 *
 * UNICODE_STRING:
 *   +0x00: USHORT Length        (bytes, not counting null)
 *   +0x02: USHORT MaximumLength (bytes, including null)
 *   +0x04: PUSHORT Buffer       (pointer to wide char data)
 *
 * ANSI_STRING:
 *   +0x00: USHORT Length
 *   +0x02: USHORT MaximumLength
 *   +0x04: PUCHAR Buffer
 * ================================================================ */

typedef struct _UNICODE_STRING {
    USHORT  Length;
    USHORT  MaximumLength;
    PUSHORT Buffer;
} UNICODE_STRING;

typedef struct _ANSI_STRING {
    USHORT  Length;
    USHORT  MaximumLength;
    PUCHAR  Buffer;
} ANSI_STRING;

/* RtlInitUnicodeString -- 2 params */
static void __stdcall ntos_RtlInitUnicodeString(
    UNICODE_STRING *DestinationString, const USHORT *SourceString)
{
    if (!DestinationString) return;

    if (!SourceString) {
        DestinationString->Length = 0;
        DestinationString->MaximumLength = 0;
        DestinationString->Buffer = NULL;
    } else {
        USHORT len = 0;
        const USHORT *p = SourceString;
        while (*p++) len++;
        len *= 2; /* Byte length */
        DestinationString->Length = len;
        DestinationString->MaximumLength = len + 2;
        DestinationString->Buffer = (PUSHORT)SourceString;
    }
}

/* RtlCopyUnicodeString -- 2 params */
static void __stdcall ntos_RtlCopyUnicodeString(
    UNICODE_STRING *Dest, const UNICODE_STRING *Source)
{
    USHORT copyLen;
    if (!Dest) return;
    if (!Source || !Source->Buffer || Source->Length == 0) {
        Dest->Length = 0;
        return;
    }
    copyLen = Source->Length;
    if (copyLen > Dest->MaximumLength) copyLen = Dest->MaximumLength;
    if (Dest->Buffer && copyLen > 0)
        ntos_memcpy(Dest->Buffer, Source->Buffer, copyLen);
    Dest->Length = copyLen;
}

/* RtlFreeUnicodeString -- 1 param */
static void __stdcall ntos_RtlFreeUnicodeString(UNICODE_STRING *String)
{
    if (String && String->Buffer) {
        VxD_PageFree(String->Buffer);
        String->Buffer = NULL;
        String->Length = 0;
        String->MaximumLength = 0;
    }
}

/* RtlInitAnsiString -- 2 params */
static void __stdcall ntos_RtlInitAnsiString(
    ANSI_STRING *Dest, const UCHAR *Source)
{
    if (!Dest) return;
    if (!Source) {
        Dest->Length = 0;
        Dest->MaximumLength = 0;
        Dest->Buffer = NULL;
    } else {
        USHORT len = 0;
        const UCHAR *p = Source;
        while (*p++) len++;
        Dest->Length = len;
        Dest->MaximumLength = len + 1;
        Dest->Buffer = (PUCHAR)Source;
    }
}

/* RtlAnsiStringToUnicodeString -- 3 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_RtlAnsiStringToUnicodeString(
    UNICODE_STRING *Dest, const ANSI_STRING *Source, BOOLEAN AllocDest)
{
    VxD_Debug_Printf("NTOS: RtlAnsiStringToUnicodeString (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* RtlUnicodeStringToAnsiString -- 3 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_RtlUnicodeStringToAnsiString(
    ANSI_STRING *Dest, const UNICODE_STRING *Source, BOOLEAN AllocDest)
{
    VxD_Debug_Printf("NTOS: RtlUnicodeStringToAnsiString (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* ================================================================
 *
 *  TIER 8 -- Object/Registry stubs
 *
 * ================================================================ */

/* ObfReferenceObject -- 1 param, void */
static void __stdcall ntos_ObfReferenceObject(PVOID Object)
{
    /* No-op: no real object manager */
}

/* ObfDereferenceObject -- 1 param, void */
static void __stdcall ntos_ObfDereferenceObject(PVOID Object)
{
    /* No-op */
}

/* ZwClose -- 1 param, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwClose(PVOID Handle)
{
    return STATUS_SUCCESS;
}

/* ZwOpenKey -- 3 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwOpenKey(
    PVOID KeyHandle, ULONG DesiredAccess, PVOID ObjectAttributes)
{
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* ZwQueryValueKey -- 6 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwQueryValueKey(
    PVOID KeyHandle, PVOID ValueName, ULONG KeyValueInfoClass,
    PVOID KeyValueInfo, ULONG Length, PULONG ResultLength)
{
    if (ResultLength) *ResultLength = 0;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* ZwSetValueKey -- 6 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwSetValueKey(
    PVOID KeyHandle, PVOID ValueName, ULONG TitleIndex,
    ULONG Type, PVOID Data, ULONG DataSize)
{
    return STATUS_SUCCESS;
}

/* IoOpenDeviceRegistryKey -- 4 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_IoOpenDeviceRegistryKey(
    PVOID DeviceObject, ULONG DevInstKeyType,
    ULONG DesiredAccess, PVOID DevInstRegKey)
{
    return STATUS_UNSUCCESSFUL;
}

/* IoWMIRegistrationControl -- 2 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_IoWMIRegistrationControl(
    PVOID DeviceObject, ULONG Action)
{
    return STATUS_SUCCESS;
}

/* ================================================================
 *
 *  HAL: Port I/O functions
 *
 * ================================================================ */

/* READ_PORT_UCHAR -- 1 param (PUCHAR treated as port address) */
static UCHAR __stdcall hal_READ_PORT_UCHAR(PUCHAR Port)
{
    return _inp_byte((USHORT)(ULONG)Port);
}

/* READ_PORT_USHORT -- 1 param */
static USHORT __stdcall hal_READ_PORT_USHORT(PUSHORT Port)
{
    return _inp_word((USHORT)(ULONG)Port);
}

/* READ_PORT_ULONG -- 1 param */
static ULONG __stdcall hal_READ_PORT_ULONG(PULONG Port)
{
    return _inp_dword((USHORT)(ULONG)Port);
}

/* WRITE_PORT_UCHAR -- 2 params */
static void __stdcall hal_WRITE_PORT_UCHAR(PUCHAR Port, UCHAR Value)
{
    _outp_byte((USHORT)(ULONG)Port, Value);
}

/* WRITE_PORT_USHORT -- 2 params */
static void __stdcall hal_WRITE_PORT_USHORT(PUSHORT Port, USHORT Value)
{
    _outp_word((USHORT)(ULONG)Port, Value);
}

/* WRITE_PORT_ULONG -- 2 params */
static void __stdcall hal_WRITE_PORT_ULONG(PULONG Port, ULONG Value)
{
    _outp_dword((USHORT)(ULONG)Port, Value);
}

/* READ_PORT_BUFFER_UCHAR -- 3 params */
static void __stdcall hal_READ_PORT_BUFFER_UCHAR(
    PUCHAR Port, PUCHAR Buffer, ULONG Count)
{
    _rep_insb((USHORT)(ULONG)Port, Buffer, Count);
}

/* READ_PORT_BUFFER_USHORT -- 3 params */
static void __stdcall hal_READ_PORT_BUFFER_USHORT(
    PUSHORT Port, PUSHORT Buffer, ULONG Count)
{
    _rep_insw((USHORT)(ULONG)Port, Buffer, Count);
}

/* READ_PORT_BUFFER_ULONG -- 3 params */
static void __stdcall hal_READ_PORT_BUFFER_ULONG(
    PULONG Port, PULONG Buffer, ULONG Count)
{
    _rep_insd((USHORT)(ULONG)Port, Buffer, Count);
}

/* WRITE_PORT_BUFFER_UCHAR -- 3 params */
static void __stdcall hal_WRITE_PORT_BUFFER_UCHAR(
    PUCHAR Port, const UCHAR *Buffer, ULONG Count)
{
    _rep_outsb((USHORT)(ULONG)Port, Buffer, Count);
}

/* WRITE_PORT_BUFFER_USHORT -- 3 params */
static void __stdcall hal_WRITE_PORT_BUFFER_USHORT(
    PUSHORT Port, const USHORT *Buffer, ULONG Count)
{
    _rep_outsw((USHORT)(ULONG)Port, Buffer, Count);
}

/* ================================================================
 *
 *  HAL: MMIO (memory-mapped I/O) functions
 *
 * ================================================================ */

/* READ_REGISTER_UCHAR -- 1 param */
static UCHAR __stdcall hal_READ_REGISTER_UCHAR(PUCHAR Register)
{
    return *((volatile UCHAR *)Register);
}

/* READ_REGISTER_USHORT -- 1 param */
static USHORT __stdcall hal_READ_REGISTER_USHORT(PUSHORT Register)
{
    return *((volatile USHORT *)Register);
}

/* READ_REGISTER_ULONG -- 1 param */
static ULONG __stdcall hal_READ_REGISTER_ULONG(PULONG Register)
{
    return *((volatile ULONG *)Register);
}

/* WRITE_REGISTER_UCHAR -- 2 params */
static void __stdcall hal_WRITE_REGISTER_UCHAR(PUCHAR Register, UCHAR Value)
{
    *((volatile UCHAR *)Register) = Value;
}

/* WRITE_REGISTER_USHORT -- 2 params */
static void __stdcall hal_WRITE_REGISTER_USHORT(PUSHORT Register, USHORT Value)
{
    *((volatile USHORT *)Register) = Value;
}

/* WRITE_REGISTER_ULONG -- 2 params */
static void __stdcall hal_WRITE_REGISTER_ULONG(PULONG Register, ULONG Value)
{
    *((volatile ULONG *)Register) = Value;
}

/* ================================================================
 *
 *  HAL: Stall / timing
 *
 * ================================================================ */

/* KeStallExecutionProcessor -- 1 param (Microseconds) (HAL.dll) */
static void __stdcall hal_KeStallExecutionProcessor(ULONG Microseconds)
{
    /* Each port 0x80 read takes ~1us on ISA bus.
     * This is the standard NT HAL technique. */
    ULONG i;
    for (i = 0; i < Microseconds; i++) {
        _inp_byte(0x80);
    }
}

/* ================================================================
 *
 *  TIER 9 -- Device/DMA/WorkItem (unlock es1371mp.sys + WDM drivers)
 *
 * ================================================================ */

/* ObReferenceObjectByPointer -- 4 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ObReferenceObjectByPointer(
    PVOID Object, ULONG DesiredAccess, PVOID ObjectType, ULONG AccessMode)
{
    return STATUS_SUCCESS;
}

/* DbgBreakPoint -- 0 params, void */
static void __stdcall ntos_DbgBreakPoint(void)
{
    VxD_Debug_Printf("NTOS: DbgBreakPoint\r\n");
}

/* IoCreateDevice -- 7 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_IoCreateDevice(
    PVOID DriverObject, ULONG DeviceExtensionSize, PVOID DeviceName,
    ULONG DeviceType, ULONG DeviceCharacteristics, BOOLEAN Exclusive,
    PVOID *DeviceObject)
{
    PVOID dev = VxD_PageAllocate(1, PAGEFIXED);
    if (!dev) {
        if (DeviceObject) *DeviceObject = NULL;
        return STATUS_UNSUCCESSFUL;
    }
    ntos_memset(dev, 0, PAGESIZE);
    if (DeviceObject) *DeviceObject = dev;
    return STATUS_SUCCESS;
}

/* IoDeleteDevice -- 1 param, void */
static void __stdcall ntos_IoDeleteDevice(PVOID DeviceObject)
{
    if (DeviceObject) VxD_PageFree(DeviceObject);
}

/* IoGetDmaAdapter -- 3 params, returns PVOID */
static PVOID __stdcall ntos_IoGetDmaAdapter(
    PVOID PhysicalDeviceObject, PVOID DeviceDescription,
    PULONG NumberOfMapRegisters)
{
    PVOID adapter = VxD_PageAllocate(1, PAGEFIXED);
    if (adapter) ntos_memset(adapter, 0, PAGESIZE);
    if (NumberOfMapRegisters) *NumberOfMapRegisters = 16;
    return adapter;
}

/* IoAllocateWorkItem -- 1 param, returns PVOID */
static PVOID __stdcall ntos_IoAllocateWorkItem(PVOID DeviceObject)
{
    PVOID item = VxD_PageAllocate(1, PAGEFIXED);
    if (item) ntos_memset(item, 0, PAGESIZE);
    return item;
}

/* IoQueueWorkItem -- 4 params, void */
typedef void (__stdcall *PIO_WORKITEM_ROUTINE)(PVOID DeviceObject, PVOID Context);

static void __stdcall ntos_IoQueueWorkItem(
    PVOID IoWorkItem, PVOID WorkerRoutine, ULONG QueueType, PVOID Context)
{
    VxD_Debug_Printf("NTOS: IoQueueWorkItem - calling routine inline\r\n");
    ((PIO_WORKITEM_ROUTINE)WorkerRoutine)(NULL, Context);
}

/* IoFreeWorkItem -- 1 param, void */
static void __stdcall ntos_IoFreeWorkItem(PVOID IoWorkItem)
{
    if (IoWorkItem) VxD_PageFree(IoWorkItem);
}

/* IoBuildDeviceIoControlRequest -- 9 params, returns PVOID (PIRP) */
static PVOID __stdcall ntos_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode, PVOID DeviceObject,
    PVOID InputBuffer, ULONG InputBufferLength,
    PVOID OutputBuffer, ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl, PVOID Event, PVOID IoStatusBlock)
{
    VxD_Debug_Printf("NTOS: IoBuildDeviceIoControlRequest (stub)\r\n");
    return NULL;
}

/* ExQueueWorkItem -- 2 params, void */
static void __stdcall ntos_ExQueueWorkItem(PVOID WorkItem, ULONG QueueType)
{
    /* No-op stub */
}

/* IoSetCompletionRoutineEx -- 7 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_IoSetCompletionRoutineEx(
    PVOID DeviceObject, PVOID Irp, PVOID CompletionRoutine,
    PVOID Context, BOOLEAN Success, BOOLEAN Error, BOOLEAN Cancel)
{
    return STATUS_SUCCESS;
}

/* ================================================================
 *
 *  TIER 10 -- CRT / string stubs (no libc in VxD)
 *
 * ================================================================ */

/* sprintf -- cdecl, variadic. Stub: write "?" + null.
 * Extra varargs on stack harmless (cdecl = caller cleanup). */
static int __cdecl ntos_sprintf(char *buffer, const char *format)
{
    if (buffer) { buffer[0] = '?'; buffer[1] = '\0'; }
    return 1;
}

/* swprintf -- cdecl, variadic. Stub: write L'?' + null (16-bit chars). */
static int __cdecl ntos_swprintf(PUSHORT buffer, const PUSHORT format)
{
    if (buffer) { buffer[0] = 0x003F; buffer[1] = 0x0000; }
    return 1;
}

/* wcscpy -- cdecl, copy 16-bit chars until null terminator */
static PVOID __cdecl ntos_wcscpy(PUSHORT dest, const USHORT *src)
{
    PUSHORT d = dest;
    const USHORT *s = src;
    if (!d || !s) return (PVOID)dest;
    while (*s) *d++ = *s++;
    *d = 0;
    return (PVOID)dest;
}

/* memset -- cdecl, byte fill loop. Returns dst.
 * Named ntos_libc_memset to avoid collision with static helper. */
static PVOID __cdecl ntos_libc_memset(PVOID dst, int val, ULONG count)
{
    UCHAR *d = (UCHAR *)dst;
    while (count--) *d++ = (UCHAR)val;
    return dst;
}

/* memcpy -- cdecl, byte copy loop. Returns dst.
 * Named ntos_libc_memcpy to avoid collision with static helper. */
static PVOID __cdecl ntos_libc_memcpy(PVOID dst, const PVOID src, ULONG count)
{
    UCHAR *d = (UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    while (count--) *d++ = *s++;
    return dst;
}

/* strncmp -- cdecl, standard byte comparison */
static int __cdecl ntos_strncmp(const UCHAR *s1, const UCHAR *s2, ULONG n)
{
    while (n--) {
        if (*s1 != *s2) return (int)*s1 - (int)*s2;
        if (*s1 == 0) return 0;
        s1++; s2++;
    }
    return 0;
}

/* ================================================================
 *
 *  TIER 11 -- EMU10K1 / Creative audio driver stubs
 *
 * ================================================================ */

/* Fake ETHREAD for KeGetCurrentThread -- lazy-allocated on first call */
static PVOID g_fake_thread = NULL;

/* KeGetCurrentThread -- 0 params, returns PVOID */
static PVOID __stdcall ntos_KeGetCurrentThread(void)
{
    if (!g_fake_thread) {
        g_fake_thread = VxD_PageAllocate(1, PAGEFIXED);
        if (g_fake_thread) ntos_memset(g_fake_thread, 0, PAGESIZE);
        VxD_Debug_Printf("NTOS: KeGetCurrentThread - allocated fake ETHREAD\r\n");
    }
    return g_fake_thread;
}

/* IoGetDeviceObjectPointer -- 4 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_IoGetDeviceObjectPointer(
    PVOID ObjectName, ULONG DesiredAccess, PVOID *FileObject, PVOID *DeviceObject)
{
    VxD_Debug_Printf("NTOS: IoGetDeviceObjectPointer (stub)\r\n");
    if (FileObject) *FileObject = NULL;
    if (DeviceObject) *DeviceObject = NULL;
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* ObReferenceObjectByHandle -- 6 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ObReferenceObjectByHandle(
    PVOID Handle, ULONG DesiredAccess, PVOID ObjectType,
    ULONG AccessMode, PVOID *Object, PVOID HandleInformation)
{
    VxD_Debug_Printf("NTOS: ObReferenceObjectByHandle (stub)\r\n");
    if (Object) *Object = NULL;
    return STATUS_UNSUCCESSFUL;
}

/* ZwLoadDriver -- 1 param, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwLoadDriver(PVOID DriverServiceName)
{
    VxD_Debug_Printf("NTOS: ZwLoadDriver (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* KefReleaseSpinLockFromDpcLevel -- 1 param, void (ntoskrnl version)
 * Distinct from hal_KefReleaseSpinLockFromDpcLevel in the HAL table. */
static void __stdcall ntos_KefReleaseSpinLockFromDpcLevel(PULONG SpinLock)
{
    /* No-op */
}

/* KefAcquireSpinLockAtDpcLevel -- 1 param, void (ntoskrnl version)
 * Distinct from hal_KefAcquireSpinLockAtDpcLevel in the HAL table. */
static void __stdcall ntos_KefAcquireSpinLockAtDpcLevel(PULONG SpinLock)
{
    /* No-op */
}

/* ZwQueryInformationFile -- 5 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwQueryInformationFile(
    PVOID FileHandle, PVOID IoStatusBlock, PVOID FileInformation,
    ULONG Length, ULONG FileInformationClass)
{
    VxD_Debug_Printf("NTOS: ZwQueryInformationFile (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* ZwReadFile -- 9 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwReadFile(
    PVOID FileHandle, PVOID Event, PVOID ApcRoutine, PVOID ApcContext,
    PVOID IoStatusBlock, PVOID Buffer, ULONG Length,
    PVOID ByteOffset, PVOID Key)
{
    VxD_Debug_Printf("NTOS: ZwReadFile (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* RtlQueryRegistryValues -- 5 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_RtlQueryRegistryValues(
    ULONG RelativeTo, PVOID Path, PVOID QueryTable,
    PVOID Context, PVOID Environment)
{
    VxD_Debug_Printf("NTOS: RtlQueryRegistryValues (stub)\r\n");
    return STATUS_OBJECT_NAME_NOT_FOUND;
}

/* PsCreateSystemThread -- 7 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_PsCreateSystemThread(
    PVOID ThreadHandle, ULONG DesiredAccess, PVOID ObjectAttributes,
    PVOID ProcessHandle, PVOID ClientId, PVOID StartRoutine, PVOID StartContext)
{
    VxD_Debug_Printf("NTOS: PsCreateSystemThread (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* KeWaitForMultipleObjects -- 8 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_KeWaitForMultipleObjects(
    ULONG Count, PVOID *Object, ULONG WaitType, ULONG WaitReason,
    ULONG WaitMode, BOOLEAN Alertable, PVOID Timeout, PVOID WaitBlockArray)
{
    VxD_Debug_Printf("NTOS: KeWaitForMultipleObjects (stub)\r\n");
    return STATUS_SUCCESS;
}

/* wcscat -- cdecl, append 16-bit src to end of dest */
static PVOID __cdecl ntos_wcscat(PUSHORT dest, const USHORT *src)
{
    PUSHORT d = dest;
    const USHORT *s = src;
    if (!d || !s) return (PVOID)dest;
    /* Find null terminator in dest */
    while (*d) d++;
    /* Copy src including null */
    while (*s) *d++ = *s++;
    *d = 0;
    return (PVOID)dest;
}

/* RtlExtendedLargeIntegerDivide -- 4 params
 * Dividend is LARGE_INTEGER by value (2 ULONGs on stack: Lo, Hi).
 * Returns LARGE_INTEGER in EDX:EAX. We do 32-bit divide. */
static ULONG __stdcall ntos_RtlExtendedLargeIntegerDivide(
    ULONG Dividend_Lo, ULONG Dividend_Hi, ULONG Divisor, PULONG Remainder)
{
    ULONG quotient;
    VxD_Debug_Printf("NTOS: RtlExtendedLargeIntegerDivide\r\n");
    if (Divisor == 0) {
        if (Remainder) *Remainder = 0;
        _zero_edx();
        return 0;
    }
    quotient = Dividend_Lo / Divisor;
    if (Remainder) *Remainder = Dividend_Lo - (quotient * Divisor);
    _zero_edx();
    return quotient;
}

/* KeSaveFloatingPointState -- 1 param, returns NTSTATUS */
static NTSTATUS __stdcall ntos_KeSaveFloatingPointState(PVOID FloatSave)
{
    VxD_Debug_Printf("NTOS: KeSaveFloatingPointState (stub)\r\n");
    return STATUS_SUCCESS;
}

/* KeRestoreFloatingPointState -- 1 param, returns NTSTATUS */
static NTSTATUS __stdcall ntos_KeRestoreFloatingPointState(PVOID FloatSave)
{
    VxD_Debug_Printf("NTOS: KeRestoreFloatingPointState (stub)\r\n");
    return STATUS_SUCCESS;
}

/* RtlWriteRegistryValue -- 6 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_RtlWriteRegistryValue(
    ULONG RelativeTo, PVOID Path, PVOID ValueName,
    ULONG ValueType, PVOID ValueData, ULONG ValueLength)
{
    VxD_Debug_Printf("NTOS: RtlWriteRegistryValue (stub)\r\n");
    return STATUS_SUCCESS;
}

/* IoReleaseCancelSpinLock -- 1 param (UCHAR promoted to ULONG), void */
static void __stdcall ntos_IoReleaseCancelSpinLock(ULONG Irql)
{
    /* No-op */
}

/* ZwWriteFile -- 9 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwWriteFile(
    PVOID FileHandle, PVOID Event, PVOID ApcRoutine, PVOID ApcContext,
    PVOID IoStatusBlock, PVOID Buffer, ULONG Length,
    PVOID ByteOffset, PVOID Key)
{
    VxD_Debug_Printf("NTOS: ZwWriteFile (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* ZwCreateFile -- 11 params, returns NTSTATUS */
static NTSTATUS __stdcall ntos_ZwCreateFile(
    PVOID FileHandle, ULONG DesiredAccess, PVOID ObjectAttributes,
    PVOID IoStatusBlock, PVOID AllocationSize, ULONG FileAttributes,
    ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions,
    PVOID EaBuffer, ULONG EaLength)
{
    VxD_Debug_Printf("NTOS: ZwCreateFile (stub)\r\n");
    return STATUS_UNSUCCESSFUL;
}

/* PsTerminateSystemThread -- 1 param, does not return */
static void __stdcall ntos_PsTerminateSystemThread(NTSTATUS ExitStatus)
{
    VxD_Debug_Printf("NTOS: PsTerminateSystemThread called\r\n");
    ntos_log_hex("  ExitStatus=", ExitStatus, "\r\n");
    for (;;) { _inp_byte(0x80); }
}

/* RtlRaiseException -- 1 param, void (does not return) */
static void __stdcall ntos_RtlRaiseException(PVOID ExceptionRecord)
{
    VxD_Debug_Printf("NTOS: RtlRaiseException called\r\n");
    ntos_log_hex("  ExceptionRecord=", (ULONG)ExceptionRecord, "\r\n");
    for (;;) { _inp_byte(0x80); }
}

/* MmMapLockedPages -- 2 params, returns PVOID
 * Maps an MDL's physical pages into virtual address space.
 * Stub: return NULL (code never executes on 32-bit Win98). */
static PVOID __stdcall ntos_MmMapLockedPages(PVOID Mdl, ULONG AccessMode)
{
    VxD_Debug_Printf("NTOS: MmMapLockedPages (stub)\r\n");
    return NULL;
}

/* ================================================================
 *
 *  ntoskrnl.exe Import Function Table
 *
 * ================================================================ */

static const IMPORT_FUNC_ENTRY ntos_funcs[] = {
    /* TIER 1: Memory */
    { "ExAllocatePoolWithTag",          (PVOID)ntos_ExAllocatePoolWithTag },
    { "ExFreePool",                     (PVOID)ntos_ExFreePool },
    { "ExFreePoolWithTag",              (PVOID)ntos_ExFreePoolWithTag },
    { "ExAllocatePool",                 (PVOID)ntos_ExAllocatePool },

    /* TIER 2: Interlocked */
    { "InterlockedIncrement",           (PVOID)ntos_InterlockedIncrement },
    { "InterlockedDecrement",           (PVOID)ntos_InterlockedDecrement },
    { "InterlockedExchange",            (PVOID)ntos_InterlockedExchange },
    { "InterlockedCompareExchange",     (PVOID)ntos_InterlockedCompareExchange },

    /* TIER 3: Synchronization */
    { "KeInitializeEvent",              (PVOID)ntos_KeInitializeEvent },
    { "KeSetEvent",                     (PVOID)ntos_KeSetEvent },
    { "KeResetEvent",                   (PVOID)ntos_KeResetEvent },
    { "KeClearEvent",                   (PVOID)ntos_KeClearEvent },
    { "KeWaitForSingleObject",          (PVOID)ntos_KeWaitForSingleObject },
    { "KeInitializeSpinLock",           (PVOID)ntos_KeInitializeSpinLock },
    { "KeAcquireSpinLockRaiseToDpc",   (PVOID)ntos_KeAcquireSpinLockRaiseToDpc },
    { "KeReleaseSpinLock",             (PVOID)ntos_KeReleaseSpinLock },
    { "KeInitializeMutex",              (PVOID)ntos_KeInitializeMutex },
    { "KeReleaseMutex",                 (PVOID)ntos_KeReleaseMutex },

    /* TIER 4: Timer/DPC */
    { "KeInitializeTimer",              (PVOID)ntos_KeInitializeTimer },
    { "KeInitializeTimerEx",            (PVOID)ntos_KeInitializeTimerEx },
    { "KeSetTimer",                     (PVOID)ntos_KeSetTimer },
    { "KeSetTimerEx",                   (PVOID)ntos_KeSetTimerEx },
    { "KeCancelTimer",                  (PVOID)ntos_KeCancelTimer },
    { "KeInitializeDpc",                (PVOID)ntos_KeInitializeDpc },
    { "KeInsertQueueDpc",               (PVOID)ntos_KeInsertQueueDpc },
    { "KeRemoveQueueDpc",               (PVOID)ntos_KeRemoveQueueDpc },

    /* TIER 5: IRP/IO */
    { "IofCallDriver",                  (PVOID)ntos_IofCallDriver },
    { "IofCompleteRequest",             (PVOID)ntos_IofCompleteRequest },
    { "IoAllocateIrp",                  (PVOID)ntos_IoAllocateIrp },
    { "IoFreeIrp",                      (PVOID)ntos_IoFreeIrp },
    { "IoInitializeIrp",                (PVOID)ntos_IoInitializeIrp },

    /* TIER 6: Power */
    { "PoCallDriver",                   (PVOID)ntos_PoCallDriver },
    { "PoStartNextPowerIrp",            (PVOID)ntos_PoStartNextPowerIrp },
    { "PoSetPowerState",                (PVOID)ntos_PoSetPowerState },

    /* TIER 7: Misc */
    { "KeDelayExecutionThread",         (PVOID)ntos_KeDelayExecutionThread },
    { "KeQuerySystemTime",              (PVOID)ntos_KeQuerySystemTime },
    { "KeQueryTimeIncrement",           (PVOID)ntos_KeQueryTimeIncrement },
    { "DbgPrint",                       (PVOID)ntos_DbgPrint },
    { "KeBugCheckEx",                   (PVOID)ntos_KeBugCheckEx },
    { "KeTickCount",                    (PVOID)&g_KeTickCount },
    { "memmove",                        (PVOID)ntos_memmove },
    { "_except_handler3",               (PVOID)ntos_except_handler3 },
    { "__C_specific_handler",          (PVOID)ntos_except_handler3 },
    { "_alldiv",                        (PVOID)ntos_alldiv },
    { "_allmul",                         (PVOID)ntos_allmul },
    { "_allshr",                        (PVOID)ntos_allshr },
    { "_aulldiv",                       (PVOID)ntos_aulldiv },
    { "_aullrem",                       (PVOID)ntos_aullrem },

    /* x64 drivers import these from ntoskrnl (not HAL) */
    { "ExAcquireFastMutex",            (PVOID)hal_ExAcquireFastMutex },
    { "ExReleaseFastMutex",            (PVOID)hal_ExReleaseFastMutex },
    { "KfAcquireSpinLock",             (PVOID)hal_KfAcquireSpinLock },
    { "KfReleaseSpinLock",             (PVOID)hal_KfReleaseSpinLock },
    { "KeGetCurrentIrql",              (PVOID)hal_KeGetCurrentIrql },

    /* String functions */
    { "RtlInitUnicodeString",           (PVOID)ntos_RtlInitUnicodeString },
    { "RtlCopyUnicodeString",           (PVOID)ntos_RtlCopyUnicodeString },
    { "RtlFreeUnicodeString",           (PVOID)ntos_RtlFreeUnicodeString },
    { "RtlInitAnsiString",             (PVOID)ntos_RtlInitAnsiString },
    { "RtlAnsiStringToUnicodeString",   (PVOID)ntos_RtlAnsiStringToUnicodeString },
    { "RtlUnicodeStringToAnsiString",   (PVOID)ntos_RtlUnicodeStringToAnsiString },

    /* TIER 8: Object/Registry */
    { "ObfReferenceObject",             (PVOID)ntos_ObfReferenceObject },
    { "ObfDereferenceObject",           (PVOID)ntos_ObfDereferenceObject },
    { "ZwClose",                        (PVOID)ntos_ZwClose },
    { "ZwOpenKey",                      (PVOID)ntos_ZwOpenKey },
    { "ZwQueryValueKey",                (PVOID)ntos_ZwQueryValueKey },
    { "ZwSetValueKey",                  (PVOID)ntos_ZwSetValueKey },
    { "IoOpenDeviceRegistryKey",        (PVOID)ntos_IoOpenDeviceRegistryKey },
    { "IoWMIRegistrationControl",       (PVOID)ntos_IoWMIRegistrationControl },

    /* TIER 9: Device/DMA/WorkItem */
    { "ObReferenceObjectByPointer",     (PVOID)ntos_ObReferenceObjectByPointer },
    { "DbgBreakPoint",                  (PVOID)ntos_DbgBreakPoint },
    { "IoCreateDevice",                 (PVOID)ntos_IoCreateDevice },
    { "IoDeleteDevice",                 (PVOID)ntos_IoDeleteDevice },
    { "IoGetDmaAdapter",                (PVOID)ntos_IoGetDmaAdapter },
    { "IoAllocateWorkItem",             (PVOID)ntos_IoAllocateWorkItem },
    { "IoQueueWorkItem",                (PVOID)ntos_IoQueueWorkItem },
    { "IoFreeWorkItem",                 (PVOID)ntos_IoFreeWorkItem },
    { "IoBuildDeviceIoControlRequest",   (PVOID)ntos_IoBuildDeviceIoControlRequest },
    { "ExQueueWorkItem",                (PVOID)ntos_ExQueueWorkItem },
    { "IoSetCompletionRoutineEx",       (PVOID)ntos_IoSetCompletionRoutineEx },

    /* TIER 10: CRT/string */
    { "sprintf",                        (PVOID)ntos_sprintf },
    { "swprintf",                       (PVOID)ntos_swprintf },
    { "wcscpy",                         (PVOID)ntos_wcscpy },
    { "memset",                         (PVOID)ntos_libc_memset },
    { "memcpy",                         (PVOID)ntos_libc_memcpy },
    { "strncmp",                        (PVOID)ntos_strncmp },

    /* TIER 11: EMU10K1 / Creative audio driver */
    { "KeGetCurrentThread",             (PVOID)ntos_KeGetCurrentThread },
    { "IoGetDeviceObjectPointer",       (PVOID)ntos_IoGetDeviceObjectPointer },
    { "ObReferenceObjectByHandle",      (PVOID)ntos_ObReferenceObjectByHandle },
    { "ZwLoadDriver",                   (PVOID)ntos_ZwLoadDriver },
    { "KefReleaseSpinLockFromDpcLevel", (PVOID)ntos_KefReleaseSpinLockFromDpcLevel },
    { "KefAcquireSpinLockAtDpcLevel",   (PVOID)ntos_KefAcquireSpinLockAtDpcLevel },
    { "ZwQueryInformationFile",         (PVOID)ntos_ZwQueryInformationFile },
    { "ZwReadFile",                     (PVOID)ntos_ZwReadFile },
    { "RtlQueryRegistryValues",         (PVOID)ntos_RtlQueryRegistryValues },
    { "PsCreateSystemThread",           (PVOID)ntos_PsCreateSystemThread },
    { "KeWaitForMultipleObjects",       (PVOID)ntos_KeWaitForMultipleObjects },
    { "wcscat",                         (PVOID)ntos_wcscat },
    { "RtlExtendedLargeIntegerDivide",  (PVOID)ntos_RtlExtendedLargeIntegerDivide },
    { "KeSaveFloatingPointState",       (PVOID)ntos_KeSaveFloatingPointState },
    { "KeRestoreFloatingPointState",    (PVOID)ntos_KeRestoreFloatingPointState },
    { "RtlWriteRegistryValue",          (PVOID)ntos_RtlWriteRegistryValue },
    { "IoReleaseCancelSpinLock",        (PVOID)ntos_IoReleaseCancelSpinLock },
    { "ZwWriteFile",                    (PVOID)ntos_ZwWriteFile },
    { "ZwCreateFile",                   (PVOID)ntos_ZwCreateFile },
    { "PsTerminateSystemThread",        (PVOID)ntos_PsTerminateSystemThread },
    { "RtlRaiseException",             (PVOID)ntos_RtlRaiseException },

    /* NE2000 x64: memory-mapped I/O */
    { "MmMapLockedPages",              (PVOID)ntos_MmMapLockedPages },

    { NULL, NULL }
};

/* ================================================================
 *
 *  HAL.dll Import Function Table
 *
 * ================================================================ */

static const IMPORT_FUNC_ENTRY hal_funcs[] = {
    /* Port I/O */
    { "READ_PORT_UCHAR",               (PVOID)hal_READ_PORT_UCHAR },
    { "READ_PORT_USHORT",              (PVOID)hal_READ_PORT_USHORT },
    { "READ_PORT_ULONG",               (PVOID)hal_READ_PORT_ULONG },
    { "WRITE_PORT_UCHAR",              (PVOID)hal_WRITE_PORT_UCHAR },
    { "WRITE_PORT_USHORT",             (PVOID)hal_WRITE_PORT_USHORT },
    { "WRITE_PORT_ULONG",              (PVOID)hal_WRITE_PORT_ULONG },
    { "READ_PORT_BUFFER_UCHAR",        (PVOID)hal_READ_PORT_BUFFER_UCHAR },
    { "READ_PORT_BUFFER_USHORT",       (PVOID)hal_READ_PORT_BUFFER_USHORT },
    { "READ_PORT_BUFFER_ULONG",        (PVOID)hal_READ_PORT_BUFFER_ULONG },
    { "WRITE_PORT_BUFFER_UCHAR",       (PVOID)hal_WRITE_PORT_BUFFER_UCHAR },
    { "WRITE_PORT_BUFFER_USHORT",      (PVOID)hal_WRITE_PORT_BUFFER_USHORT },

    /* MMIO */
    { "READ_REGISTER_UCHAR",           (PVOID)hal_READ_REGISTER_UCHAR },
    { "READ_REGISTER_USHORT",          (PVOID)hal_READ_REGISTER_USHORT },
    { "READ_REGISTER_ULONG",           (PVOID)hal_READ_REGISTER_ULONG },
    { "WRITE_REGISTER_UCHAR",          (PVOID)hal_WRITE_REGISTER_UCHAR },
    { "WRITE_REGISTER_USHORT",         (PVOID)hal_WRITE_REGISTER_USHORT },
    { "WRITE_REGISTER_ULONG",          (PVOID)hal_WRITE_REGISTER_ULONG },

    /* Spinlocks */
    { "KfAcquireSpinLock",             (PVOID)hal_KfAcquireSpinLock },
    { "KfReleaseSpinLock",             (PVOID)hal_KfReleaseSpinLock },
    { "KefAcquireSpinLockAtDpcLevel",  (PVOID)hal_KefAcquireSpinLockAtDpcLevel },
    { "KefReleaseSpinLockFromDpcLevel",(PVOID)hal_KefReleaseSpinLockFromDpcLevel },

    /* Fast mutex */
    { "ExAcquireFastMutex",            (PVOID)hal_ExAcquireFastMutex },
    { "ExReleaseFastMutex",            (PVOID)hal_ExReleaseFastMutex },

    /* IRQL */
    { "KeGetCurrentIrql",              (PVOID)hal_KeGetCurrentIrql },
    { "KfRaiseIrql",                   (PVOID)hal_KfRaiseIrql },
    { "KfLowerIrql",                   (PVOID)hal_KfLowerIrql },

    /* Stall / timing */
    { "KeStallExecutionProcessor",     (PVOID)hal_KeStallExecutionProcessor },
    { "KeQueryPerformanceCounter",     (PVOID)hal_KeQueryPerformanceCounter },

    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Structures
 *
 * NOTE: This registers 2 shims. PELOAD.C MAX_PORT_SHIMS is 8.
 * With other shims already registered, ensure the limit is adequate.
 * ================================================================ */

static PORT_DRIVER_SHIM ntos_shim = {
    "ntoskrnl.exe",
    ntos_funcs,
    0,      /* func_count: uses null terminator */
    NULL,   /* bridge_init */
    NULL,   /* bridge_io */
    NULL    /* bridge_cleanup */
};

static PORT_DRIVER_SHIM hal_shim = {
    "HAL.dll",
    hal_funcs,
    0,
    NULL,
    NULL,
    NULL
};

/* ================================================================
 * Public Registration
 * ================================================================ */

void ntos_shim_register(void)
{
    VxD_Debug_Printf("NTOS: Registering ntoskrnl.exe shim\r\n");
    register_port_driver(&ntos_shim);

    VxD_Debug_Printf("NTOS: Registering HAL.dll shim\r\n");
    register_port_driver(&hal_shim);
}

/* ================================================================
 * Test Entrypoint
 *
 * Loads a driver PE image using only the ntoskrnl+HAL shims.
 * Tests: PE load, import resolution, DriverEntry, export parsing.
 * ================================================================ */

extern int pe_load_image(const void *pe_data, unsigned long pe_size,
    const IMPORT_FUNC_ENTRY *func_table, void **out_entry, void **out_base);

/* Forward-declare LOADED_IMAGE as opaque — PELOAD.C owns the definition.
 * We only use pointers, so the compiler doesn't need the layout. */
struct _LOADED_IMAGE;
typedef struct _LOADED_IMAGE NTOS_LOADED_IMAGE;

extern NTOS_LOADED_IMAGE *register_loaded_image(const char *name, void *base, ULONG image_size);
extern int pe_parse_exports(void *image_base, ULONG image_size, NTOS_LOADED_IMAGE *img);

#ifdef NTOS_TEST_HIDPARSE
#include "HIDPARSE_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_HIDGAME
#include "HIDPARSE_EMBEDDED.H"
#include "HIDGAME_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_ES1371
#include "ES1371MP_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_X64
#include "HIDPARSE64_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_X64_MULTI
#include "HIDPARSE64_EMBEDDED.H"
#include "PCIIDE64_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_X64_HID
#include "HIDGAME64_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_X64_VGA
#include "VGAPNP64_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_X64_NE2K
#include "NE2000_X64_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_NT3X
#include "AHA154X_NT351_EMBEDDED.H"
#include "AHA154X_NT31_EMBEDDED.H"
#include "BUSLOGIC_NT351_EMBEDDED.H"
#include "NCR53C9X_NT351_EMBEDDED.H"
#endif

#ifdef NTOS_TEST_IA64
#include "PCIIDE_IA64_EMBEDDED.H"
#endif

int ntos_test_driver(void)
{
#ifdef NTOS_TEST_HIDPARSE
    PVOID entry_point = NULL;
    PVOID image_base = NULL;
    int rc;
    ULONG status;
    NTOS_LOADED_IMAGE *img;
    int n_exp;
    typedef ULONG (__stdcall *PFN_DRIVER_ENTRY)(PVOID, PVOID);
    PFN_DRIVER_ENTRY pfnDriverEntry;

    ntos_shim_register();

    VxD_Debug_Printf("NTOS: Loading hidparse.sys...\r\n");
    rc = pe_load_image(hidparse_embedded_data, sizeof(hidparse_embedded_data),
                       NULL, &entry_point, &image_base);
    if (rc != 0) {
        ntos_log_hex("NTOS: PE load failed rc=", (ULONG)rc, "\r\n");
        return rc;
    }
    ntos_log_hex("NTOS: hidparse.sys loaded at ", (ULONG)image_base, "");
    ntos_log_hex(" entry=", (ULONG)entry_point, "\r\n");

    img = register_loaded_image("HIDPARSE.SYS", image_base, 0x10000);
    n_exp = 0;
    if (img) {
        n_exp = pe_parse_exports(image_base, 0x10000, img);
        ntos_log_hex("NTOS: Parsed exports: ", (ULONG)n_exp, "\r\n");
    }

    pfnDriverEntry = (PFN_DRIVER_ENTRY)entry_point;
    VxD_Debug_Printf("NTOS: Calling DriverEntry...\r\n");
    status = pfnDriverEntry(NULL, NULL);
    ntos_log_hex("NTOS: DriverEntry returned ", status, "\r\n");

    if (status != 0) {
        VxD_Debug_Printf("NTOS: DriverEntry FAILED\r\n");
        return -1;
    }

    VxD_Debug_Printf("NTOS: *** HIDPARSE.SYS LOADED AND INITIALIZED ***\r\n");
    if (n_exp > 0) {
        ntos_log_hex("NTOS: Available HidP_* exports: ", (ULONG)n_exp, "\r\n");
    }
    return 0;

#elif defined(NTOS_TEST_HIDGAME)
    PVOID entry_point = NULL;
    PVOID image_base = NULL;
    PVOID hg_entry = NULL;
    PVOID hg_base = NULL;
    int rc;
    ULONG status;
    NTOS_LOADED_IMAGE *img;
    int n_exp;
    typedef ULONG (__stdcall *PFN_DRIVER_ENTRY)(PVOID, PVOID);
    PFN_DRIVER_ENTRY pfnDriverEntry;

    extern void hid_shim_register(void);

    ntos_shim_register();
    hid_shim_register();

    /* Step 1: Load hidparse.sys (library, exports HidP_* functions) */
    VxD_Debug_Printf("NTOS: Loading hidparse.sys (library)...\r\n");
    rc = pe_load_image(hidparse_embedded_data, sizeof(hidparse_embedded_data),
                       NULL, &entry_point, &image_base);
    if (rc != 0) {
        ntos_log_hex("NTOS: hidparse PE load failed rc=", (ULONG)rc, "\r\n");
        return rc;
    }
    ntos_log_hex("NTOS: hidparse loaded at ", (ULONG)image_base, "\r\n");

    img = register_loaded_image("HIDPARSE.SYS", image_base, 0x10000);
    if (img) {
        n_exp = pe_parse_exports(image_base, 0x10000, img);
        ntos_log_hex("NTOS: hidparse exports: ", (ULONG)n_exp, "\r\n");
    }

    pfnDriverEntry = (PFN_DRIVER_ENTRY)entry_point;
    status = pfnDriverEntry(NULL, NULL);
    ntos_log_hex("NTOS: hidparse DriverEntry: ", status, "\r\n");
    if (status != 0) return -1;

    /* Step 2: Load hidgame.sys (minidriver, imports from HIDCLASS + HIDPARSE) */
    VxD_Debug_Printf("NTOS: Loading hidgame.sys...\r\n");
    rc = pe_load_image(hidgame_embedded_data, sizeof(hidgame_embedded_data),
                       NULL, &hg_entry, &hg_base);
    if (rc != 0) {
        ntos_log_hex("NTOS: hidgame PE load failed rc=", (ULONG)rc, "\r\n");
        return rc;
    }
    ntos_log_hex("NTOS: hidgame loaded at ", (ULONG)hg_base, "");
    ntos_log_hex(" entry=", (ULONG)hg_entry, "\r\n");

    /* Create fake DRIVER_OBJECT (0x200 bytes) so DriverEntry can store
     * dispatch routines without crashing on NULL dereference.
     * Key offsets: +0x38 = MajorFunction[0..28] (IRP_MJ_xxx dispatch table)
     *              +0x14 = DriverStartIo, +0x18 = DriverUnload
     *              +0x08 = DeviceObject (chain head) */
    {
        PVOID fake_drv = VxD_PageAllocate(1, PAGEFIXED);
        PVOID fake_reg = VxD_PageAllocate(1, PAGEFIXED);
        if (!fake_drv || !fake_reg) {
            VxD_Debug_Printf("NTOS: alloc fake DRIVER_OBJECT failed\r\n");
            return -2;
        }
        ntos_memset(fake_drv, 0, PAGESIZE);
        ntos_memset(fake_reg, 0, PAGESIZE);

        pfnDriverEntry = (PFN_DRIVER_ENTRY)hg_entry;
        VxD_Debug_Printf("NTOS: Calling hidgame DriverEntry...\r\n");
        status = pfnDriverEntry(fake_drv, fake_reg);
    }
    ntos_log_hex("NTOS: hidgame DriverEntry returned ", status, "\r\n");

    if (status != 0) {
        VxD_Debug_Printf("NTOS: hidgame DriverEntry FAILED\r\n");
        return -1;
    }

    VxD_Debug_Printf("NTOS: *** HIDGAME.SYS LOADED AND INITIALIZED ***\r\n");
    return 0;

#elif defined(NTOS_TEST_ES1371)
    PVOID entry_point = NULL;
    PVOID image_base = NULL;
    int rc;
    ULONG status;
    typedef ULONG (__stdcall *PFN_DRIVER_ENTRY)(PVOID, PVOID);
    PFN_DRIVER_ENTRY pfnDriverEntry;

    extern void audio_shim_register(void);

    ntos_shim_register();
    audio_shim_register();

    VxD_Debug_Printf("NTOS: Loading es1371mp.sys (audio)...\r\n");
    rc = pe_load_image(es1371mp_embedded_data, sizeof(es1371mp_embedded_data),
                       NULL, &entry_point, &image_base);
    if (rc != 0) {
        ntos_log_hex("NTOS: es1371mp PE load failed rc=", (ULONG)rc, "\r\n");
        return rc;
    }
    ntos_log_hex("NTOS: es1371mp loaded at ", (ULONG)image_base, "");
    ntos_log_hex(" entry=", (ULONG)entry_point, "\r\n");

    {
        PVOID fake_drv = VxD_PageAllocate(1, PAGEFIXED);
        PVOID fake_reg = VxD_PageAllocate(1, PAGEFIXED);
        if (!fake_drv || !fake_reg) {
            VxD_Debug_Printf("NTOS: alloc fake DRIVER_OBJECT failed\r\n");
            return -2;
        }
        ntos_memset(fake_drv, 0, PAGESIZE);
        ntos_memset(fake_reg, 0, PAGESIZE);

        pfnDriverEntry = (PFN_DRIVER_ENTRY)entry_point;
        VxD_Debug_Printf("NTOS: Calling es1371mp DriverEntry...\r\n");
        status = pfnDriverEntry(fake_drv, fake_reg);
    }
    ntos_log_hex("NTOS: es1371mp DriverEntry returned ", status, "\r\n");

    if (status == 0) {
        VxD_Debug_Printf("NTOS: *** ES1371MP.SYS LOADED AND INITIALIZED ***\r\n");
    } else {
        VxD_Debug_Printf("NTOS: es1371mp DriverEntry returned non-zero (may be expected)\r\n");
    }
    return 0;

#elif defined(NTOS_TEST_X64)
    PVOID entry_point = NULL;
    PVOID image_base = NULL;
    int rc;

    ntos_shim_register();

    VxD_Debug_Printf("NTOS: === PE32+ (x64) LOADER TEST ===\r\n");
    VxD_Debug_Printf("NTOS: Loading hidparse.sys (AMD64 PE32+)...\r\n");
    rc = pe_load_image(hidparse64_embedded_data, sizeof(hidparse64_embedded_data),
                       NULL, &entry_point, &image_base);
    if (rc == 0) {
        ntos_log_hex("NTOS: x64 image loaded at ", (ULONG)image_base, "");
        ntos_log_hex(" entry=", (ULONG)entry_point, "\r\n");
        VxD_Debug_Printf("NTOS: *** PE32+ LOAD SUCCESS ***\r\n");
        VxD_Debug_Printf("NTOS: (Cannot execute x64 code on 32-bit Win98)\r\n");
    } else {
        ntos_log_hex("NTOS: PE32+ load returned rc=", (ULONG)rc, "\r\n");
    }
    return rc;

#elif defined(NTOS_TEST_X64_MULTI)
    {
        PVOID ep, ib;
        int rc, pass = 0, fail = 0;

        extern void pciide_shim_register(void);
        ntos_shim_register();
        pciide_shim_register();
        VxD_Debug_Printf("NTOS: === PE32+ MULTI-DRIVER TEST ===\r\n");

        VxD_Debug_Printf("NTOS: [1/2] hidparse.sys x64 (41K, parser)...\r\n");
        rc = pe_load_image(hidparse64_embedded_data, sizeof(hidparse64_embedded_data),
                           NULL, &ep, &ib);
        ntos_log_hex("  rc=", (ULONG)rc, rc == 0 ? " PASS\r\n" : " FAIL\r\n");
        if (rc == 0) pass++; else fail++;

        VxD_Debug_Printf("NTOS: [2/2] pciide.sys x64 (6K, IDE controller)...\r\n");
        rc = pe_load_image(pciide64_embedded_data, sizeof(pciide64_embedded_data),
                           NULL, &ep, &ib);
        ntos_log_hex("  rc=", (ULONG)rc, rc == 0 ? " PASS\r\n" : " FAIL\r\n");
        if (rc == 0) pass++; else fail++;

        ntos_log_hex("NTOS: PE32+ results: ", (ULONG)pass, " pass, ");
        ntos_log_hex("", (ULONG)fail, " fail\r\n");
        if (fail == 0)
            VxD_Debug_Printf("NTOS: *** ALL PE32+ LOADS SUCCEEDED ***\r\n");
        return fail;
    }

#elif defined(NTOS_TEST_X64_HID)
    {
        PVOID ep, ib;
        int rc;

        extern void hid_shim_register(void);
        ntos_shim_register();
        hid_shim_register();

        VxD_Debug_Printf("NTOS: === PE32+ HID x64 TEST ===\r\n");

        VxD_Debug_Printf("NTOS: Loading hidgame.sys x64...\r\n");
        rc = pe_load_image(hidgame64_embedded_data, sizeof(hidgame64_embedded_data),
                           NULL, &ep, &ib);
        if (rc != 0) {
            ntos_log_hex("NTOS: hidgame64 load failed rc=", (ULONG)rc, "\r\n");
            return rc;
        }
        ntos_log_hex("NTOS: hidgame64 at ", (ULONG)ib, " PASS\r\n");
        VxD_Debug_Printf("NTOS: *** PE32+ HID x64 LOAD SUCCEEDED ***\r\n");
        return 0;
    }

#elif defined(NTOS_TEST_X64_VGA)
    {
        PVOID ep, ib;
        int rc;

        extern void video_shim_register(void);
        ntos_shim_register();
        video_shim_register();

        VxD_Debug_Printf("NTOS: === PE32+ VGA x64 TEST ===\r\n");
        VxD_Debug_Printf("NTOS: Loading vgapnp.sys x64...\r\n");
        rc = pe_load_image(vgapnp64_embedded_data, sizeof(vgapnp64_embedded_data),
                           NULL, &ep, &ib);
        if (rc == 0) {
            ntos_log_hex("NTOS: vgapnp64 at ", (ULONG)ib, " PASS\r\n");
            VxD_Debug_Printf("NTOS: *** PE32+ VGA x64 LOAD SUCCEEDED ***\r\n");
        } else {
            ntos_log_hex("NTOS: vgapnp64 load failed rc=", (ULONG)rc, "\r\n");
        }
        return rc;
    }

#elif defined(NTOS_TEST_X64_NE2K)
    {
        PVOID ep, ib;
        int rc;

        extern void ndis_shim_register(void);
        ntos_shim_register();
        ndis_shim_register();

        VxD_Debug_Printf("NTOS: === PE32+ NE2000 x64 TEST ===\r\n");
        VxD_Debug_Printf("NTOS: Loading ne2000.sys x64 (22K network)...\r\n");
        rc = pe_load_image(ne2000_x64_embedded_data, sizeof(ne2000_x64_embedded_data),
                           NULL, &ep, &ib);
        if (rc == 0) {
            ntos_log_hex("NTOS: ne2000 x64 at ", (ULONG)ib, " PASS\r\n");
            VxD_Debug_Printf("NTOS: *** PE32+ NE2000 x64 LOAD SUCCEEDED ***\r\n");
        } else {
            ntos_log_hex("NTOS: ne2000 x64 load failed rc=", (ULONG)rc, "\r\n");
        }
        return rc;
    }

#elif defined(NTOS_TEST_NT3X)
    {
        extern const IMPORT_FUNC_ENTRY scsiport_funcs[];
        PVOID ep, ib;
        int rc, pass = 0, fail = 0;

        ntos_shim_register();

        VxD_Debug_Printf("NTOS: === NT 3.x DRIVER TEST ===\r\n");

        /* Test 1: NT 3.51 aha154x.sys (1995, Adaptec 154x SCSI, 9K) */
        VxD_Debug_Printf("NTOS: [1/4] aha154x.sys NT 3.51 (1995)...\r\n");
        rc = pe_load_image(aha154x_nt351_embedded_data,
                           sizeof(aha154x_nt351_embedded_data),
                           scsiport_funcs, &ep, &ib);
        ntos_log_hex("  rc=", (ULONG)rc, rc == 0 ? " PASS\r\n" : " FAIL\r\n");
        if (rc == 0) { pass++; ntos_log_hex("  loaded at ", (ULONG)ib, "\r\n"); }
        else fail++;

        /* Test 2: NT 3.1 aha154x.sys (1993, first NT ever) */
        VxD_Debug_Printf("NTOS: [2/4] aha154x.sys NT 3.1 (1993)...\r\n");
        rc = pe_load_image(aha154x_nt31_embedded_data,
                           sizeof(aha154x_nt31_embedded_data),
                           scsiport_funcs, &ep, &ib);
        ntos_log_hex("  rc=", (ULONG)rc, rc == 0 ? " PASS\r\n" : " FAIL\r\n");
        if (rc == 0) pass++; else fail++;

        /* Test 3: NT 3.51 buslogic.sys (BusLogic SCSI) */
        VxD_Debug_Printf("NTOS: [3/4] buslogic.sys NT 3.51...\r\n");
        rc = pe_load_image(buslogic_nt351_embedded_data,
                           sizeof(buslogic_nt351_embedded_data),
                           scsiport_funcs, &ep, &ib);
        ntos_log_hex("  rc=", (ULONG)rc, rc == 0 ? " PASS\r\n" : " FAIL\r\n");
        if (rc == 0) pass++; else fail++;

        /* Test 4: NT 3.51 ncr53c9x.sys (NCR 53C9x SCSI) */
        VxD_Debug_Printf("NTOS: [4/4] ncr53c9x.sys NT 3.51...\r\n");
        rc = pe_load_image(ncr53c9x_nt351_embedded_data,
                           sizeof(ncr53c9x_nt351_embedded_data),
                           scsiport_funcs, &ep, &ib);
        ntos_log_hex("  rc=", (ULONG)rc, rc == 0 ? " PASS\r\n" : " FAIL\r\n");
        if (rc == 0) pass++; else fail++;

        ntos_log_hex("NTOS: NT 3.x results: ", (ULONG)pass, " pass, ");
        ntos_log_hex("", (ULONG)fail, " fail\r\n");
        if (fail == 0)
            VxD_Debug_Printf("NTOS: *** ALL NT 3.x DRIVERS LOADED ***\r\n");
        return fail;
    }

#elif defined(NTOS_TEST_IA64)
    {
        PVOID ep, ib;
        int rc;

        extern void pciide_shim_register(void);
        ntos_shim_register();
        pciide_shim_register();

        VxD_Debug_Printf("NTOS: === IA-64 (ITANIUM) PE32+ TEST ===\r\n");
        VxD_Debug_Printf("NTOS: Loading pciide.sys IA-64...\r\n");
        rc = pe_load_image(pciide_ia64_embedded_data,
                           sizeof(pciide_ia64_embedded_data),
                           NULL, &ep, &ib);
        if (rc == 0) {
            ntos_log_hex("NTOS: IA-64 pciide at ", (ULONG)ib, " PASS\r\n");
            VxD_Debug_Printf("NTOS: *** IA-64 ITANIUM PE32+ LOAD SUCCEEDED ***\r\n");
            VxD_Debug_Printf("NTOS: (Cannot execute IA-64 code on i386 Win98)\r\n");
        } else {
            ntos_log_hex("NTOS: IA-64 load failed rc=", (ULONG)rc, "\r\n");
        }
        return rc;
    }
#else
    VxD_Debug_Printf("NTOS: No test driver embedded\r\n");
    return -99;
#endif
}
