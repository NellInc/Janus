/*
 * IRPMGR.H - NT I/O Manager Structures and IRP Infrastructure
 *
 * Defines the IRP (I/O Request Packet) structures, I/O stack locations,
 * device objects, driver objects, and all IoXxx function prototypes
 * needed to host an NT5 WDM driver stack on Windows 9x.
 *
 * This header provides the "I/O manager" abstraction layer. On real
 * NT, these structures live in ntoskrnl.exe. Here, we implement them
 * in IRPMGR.C using VxD heap allocation and a simple device object
 * tracking list.
 *
 * The structures must match the NT binary layout closely enough that
 * an NT5 driver compiled against the real DDK headers will work when
 * linked against our shim. Field offsets are critical.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#ifndef _IRPMGR_H_
#define _IRPMGR_H_

/* NT5 path: types come from NTKSHIM.H. Old NTKRNL.C path: from NTKRNL.H. */
#ifndef NTKSHIM_H
#include "NTKRNL.H"
#elif defined(_NTKRNL_H_)
/* NTKSHIM.H plus NTKRNL.H path: use the canonical NTKRNL.H binary
 * structure layouts. Do not redeclare IRP/DEVICE_OBJECT locally. */
#else
/* When using NTKSHIM.H path, provide the full struct definitions
 * that NTKRNL.H would normally supply. These must match the NTKRNL.H
 * layout exactly (field offsets matter for NT driver binary compat). */

/* SHORT is in NTKRNL.H but not NTKSHIM.H */
#ifndef _NTK_SHORT_DEFINED
#define _NTK_SHORT_DEFINED
typedef short SHORT;
#endif

/* IO_STACK_LOCATION forward + pointer typedef */
struct _IO_STACK_LOCATION;
typedef struct _IO_STACK_LOCATION IO_STACK_LOCATION, *PIO_STACK_LOCATION;

/* IO completion routine callback */
typedef NTSTATUS (NTAPI *PIO_COMPLETION_ROUTINE)(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp,
    PVOID Context
);

/* IRP dispatch, StartIo, AddDevice, Unload, Timer */
typedef NTSTATUS (NTAPI *PDRIVER_DISPATCH)(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
);
typedef VOID (NTAPI *PDRIVER_STARTIO)(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp
);
typedef NTSTATUS (NTAPI *PDRIVER_ADD_DEVICE)(
    PDRIVER_OBJECT DriverObject,
    PDEVICE_OBJECT PhysicalDeviceObject
);
typedef VOID (NTAPI *PDRIVER_UNLOAD)(
    PDRIVER_OBJECT DriverObject
);

/* IRP major function max */
#ifndef IRP_MJ_MAXIMUM_FUNCTION
#define IRP_MJ_MAXIMUM_FUNCTION 0x1B
#endif
#define IRP_MJ_MAXIMUM  (IRP_MJ_MAXIMUM_FUNCTION + 1)

/* FILE_DEVICE codes */
#ifndef FILE_DEVICE_CONTROLLER
#define FILE_DEVICE_CONTROLLER  0x00000004
#endif
#ifndef FILE_DEVICE_DISK
#define FILE_DEVICE_DISK        0x00000007
#endif

/* DO_xxx flags */
#ifndef DO_BUFFERED_IO
#define DO_BUFFERED_IO          0x00000004
#define DO_DIRECT_IO            0x00000010
#define DO_DEVICE_INITIALIZING  0x00000080
#define DO_POWER_PAGABLE        0x00002000
#endif

/* DRIVER_EXTENSION */
typedef struct _DRIVER_EXTENSION {
    PDRIVER_OBJECT      DriverObject;
    PDRIVER_ADD_DEVICE  AddDevice;
    ULONG               Count;
    UNICODE_STRING      ServiceKeyName;
    PVOID               ClientDriverExtension;
    PVOID               ClientIdentifier;
} DRIVER_EXTENSION, *PDRIVER_EXTENSION;

/* DRIVER_OBJECT */
struct _DRIVER_OBJECT {
    SHORT               Type;
    SHORT               Size;
    PDEVICE_OBJECT      DeviceObject;
    ULONG               Flags;
    PVOID               DriverStart;
    ULONG               DriverSize;
    PVOID               DriverSection;
    PDRIVER_EXTENSION    DriverExtension;
    UNICODE_STRING      DriverName;
    PUNICODE_STRING     HardwareDatabase;
    PVOID               FastIoDispatch;
    PVOID               DriverInit;
    PDRIVER_STARTIO     DriverStartIo;
    PDRIVER_UNLOAD      DriverUnload;
    PDRIVER_DISPATCH    MajorFunction[IRP_MJ_MAXIMUM];
};

/* DEVICE_OBJECT */
struct _DEVICE_OBJECT {
    SHORT               Type;
    USHORT              Size;
    LONG                ReferenceCount;
    PDRIVER_OBJECT      DriverObject;
    PDEVICE_OBJECT      NextDevice;
    PDEVICE_OBJECT      AttachedDevice;
    PIRP                CurrentIrp;
    PVOID               Timer;
    ULONG               Flags;
    ULONG               Characteristics;
    PVOID               Vpb;
    PVOID               DeviceExtension;
    ULONG               DeviceType;
    CHAR                StackSize;
    union {
        LIST_ENTRY          ListEntry;
        PVOID               Wcb;
    } Queue;
    ULONG               AlignmentRequirement;
    KDEVICE_QUEUE       DeviceQueue;
    KDPC                Dpc;
    ULONG               ActiveThreadCount;
    PVOID               SecurityDescriptor;
    KEVENT              DeviceLock;
    USHORT              SectorSize;
    USHORT              Spare1;
    PVOID               DeviceObjectExtension;
    PVOID               Reserved;
    PDEVICE_OBJECT      ShimLowerDevice;
    PIO_TIMER_ROUTINE   ShimTimerRoutine;
    PVOID               ShimTimerContext;
    ULONG               ShimTimerHandle;
    BOOLEAN             ShimTimerActive;
};

/* FILE_OBJECT (simplified) */
struct _FILE_OBJECT {
    SHORT               Type;
    SHORT               Size;
    PDEVICE_OBJECT      DeviceObject;
    UNICODE_STRING      FileName;
    LARGE_INTEGER       CurrentByteOffset;
    ULONG               Flags;
};

/* IRP_SIZE macro */
#define IRP_SIZE(StackSize) \
    ((USHORT)(sizeof(IRP) + ((StackSize) * sizeof(IO_STACK_LOCATION))))

/* IRP structure. Keep this layout in lock-step with NTKRNL.H and the
 * IRPMGR.C allocation/dispatch code. NT drivers touch nested Tail.Overlay
 * fields directly, so a smaller shim-local IRP layout corrupts dispatch. */
#pragma pack(push,4)
struct _IRP {
    SHORT               Type;
    USHORT              Size;
    PMDL                MdlAddress;
    ULONG               Flags;
    union {
        struct _IRP    *MasterIrp;
        LONG            IrpCount;
        PVOID           SystemBuffer;
    } AssociatedIrp;
    LIST_ENTRY          ThreadListEntry;
    IO_STATUS_BLOCK     IoStatus;
    CHAR                RequestorMode;
    UCHAR               PendingReturned;
    CHAR                StackCount;
    CHAR                CurrentLocation;
    UCHAR               Cancel;
    KIRQL               CancelIrql;
    CHAR                ApcEnvironment;
    UCHAR               AllocationFlags;
    PIO_STATUS_BLOCK    UserIosb;
    PKEVENT             UserEvent;
    union {
        struct {
            PVOID       UserApcRoutine;
            PVOID       UserApcContext;
        } AsynchronousParameters;
        LARGE_INTEGER   AllocationSize;
    } Overlay;
    PVOID               CancelRoutine;
    PVOID               UserBuffer;
    union {
        struct {
            union {
                KDEVICE_QUEUE_ENTRY DeviceQueueEntry;
                struct {
                    PVOID DriverContext[4];
                };
            };
            PVOID       Thread;
            PCHAR       AuxiliaryBuffer;
            struct {
                LIST_ENTRY  ListEntry;
                union {
                    PIO_STACK_LOCATION  CurrentStackLocation;
                    ULONG               PacketType;
                };
            };
            PFILE_OBJECT    OriginalFileObject;
        } Overlay;
    } Tail;
};

/* IO_STACK_LOCATION */
struct _IO_STACK_LOCATION {
    UCHAR               MajorFunction;
    UCHAR               MinorFunction;
    UCHAR               Flags;
    UCHAR               Control;
    union {
        struct {
            PVOID SecurityContext;
            ULONG Options;
            USHORT FileAttributes;
            USHORT ShareAccess;
            ULONG EaLength;
        } Create;
        struct {
            ULONG Length;
            ULONG Key;
            LARGE_INTEGER ByteOffset;
        } Read;
        struct {
            ULONG Length;
            ULONG Key;
            LARGE_INTEGER ByteOffset;
        } Write;
        struct {
            ULONG OutputBufferLength;
            ULONG InputBufferLength;
            ULONG IoControlCode;
            PVOID Type3InputBuffer;
        } DeviceIoControl;
        struct {
            PVOID Srb;
        } Scsi;
        struct {
            ULONG Type;
        } QueryDeviceRelations;
        struct {
            PVOID InterfaceType;
            USHORT Size;
            USHORT Version;
            PVOID Interface;
            PVOID InterfaceSpecificData;
        } QueryInterface;
        struct {
            PVOID Capabilities;
        } DeviceCapabilities;
        struct {
            ULONG WhichSpace;
            PVOID Buffer;
            ULONG Offset;
            ULONG Length;
        } ReadWriteConfig;
        struct {
            ULONG Type;
            ULONG State;
            ULONG ShutdownType;
        } Power;
        struct {
            PVOID Argument1;
            PVOID Argument2;
            PVOID Argument3;
            PVOID Argument4;
        } Others;
    } Parameters;
    PDEVICE_OBJECT      DeviceObject;
    PFILE_OBJECT        FileObject;
    PIO_COMPLETION_ROUTINE CompletionRoutine;
    PVOID               Context;
};
#pragma pack(pop)

/* IO_STACK_LOCATION Control flags.
 * NT drivers write these bits directly, so the values must match the
 * NT DDK layout. Atapi.sys sets 0xE0 for invoke-on-success/error/cancel. */
#define SL_PENDING_RETURNED     0x01
#define SL_ERROR_RETURNED       0x02
#define SL_INVOKE_ON_CANCEL     0x20
#define SL_INVOKE_ON_SUCCESS    0x40
#define SL_INVOKE_ON_ERROR      0x80

/* IRP stack location macros */
#define IoGetCurrentIrpStackLocation(Irp) ((Irp)->Tail.Overlay.CurrentStackLocation)
#define IoGetNextIrpStackLocation(Irp) \
    ((Irp)->Tail.Overlay.CurrentStackLocation - 1)
#define IoSetNextIrpStackLocation(Irp) do {  \
    (Irp)->CurrentLocation--;                 \
    (Irp)->Tail.Overlay.CurrentStackLocation--;            \
} while (0)
#define IoCopyCurrentIrpStackLocationToNext(Irp) do {     \
    PIO_STACK_LOCATION _irpSp;                             \
    PIO_STACK_LOCATION _nextIrpSp;                         \
    _irpSp = IoGetCurrentIrpStackLocation(Irp);            \
    _nextIrpSp = IoGetNextIrpStackLocation(Irp);           \
    RtlCopyMemory(_nextIrpSp, _irpSp,                     \
        (ULONG)((ULONG_PTR)&_irpSp->CompletionRoutine -   \
                (ULONG_PTR)_irpSp));                       \
    _nextIrpSp->CompletionRoutine = NULL;                  \
    _nextIrpSp->Context = NULL;                            \
    _nextIrpSp->Control = 0;                               \
} while (0)
#define IoSetCompletionRoutine(Irp, Routine, Ctx, Ok, Err, Cancel) do { \
    PIO_STACK_LOCATION _nextIrpSp;                              \
    _nextIrpSp = IoGetNextIrpStackLocation(Irp);                \
    _nextIrpSp->CompletionRoutine = (Routine);                  \
    _nextIrpSp->Context = (Ctx);                                \
    _nextIrpSp->Control = 0;                                    \
    if (Ok)     _nextIrpSp->Control |= SL_INVOKE_ON_SUCCESS;   \
    if (Err)    _nextIrpSp->Control |= SL_INVOKE_ON_ERROR;     \
    if (Cancel) _nextIrpSp->Control |= SL_INVOKE_ON_CANCEL;    \
} while (0)

/* Convenience macros mapping NT names to IRPMGR implementations */
#define IoCreateDevice          IrpMgr_IoCreateDevice
#define IoAllocateIrp           IrpMgr_IoAllocateIrp
#define IoFreeIrp               IrpMgr_IoFreeIrp
#define IoCallDriver            IrpMgr_IoCallDriver
#define IoCompleteRequest       IrpMgr_IoCompleteRequest
#define IoDeleteDevice          IrpMgr_IoDeleteDevice
#define IoAttachDeviceToDeviceStack IrpMgr_IoAttachDeviceToDeviceStack
#define IoDetachDevice          IrpMgr_IoDetachDevice

#endif

/* ================================================================
 * IRP MAJOR FUNCTION CODES
 *
 * These are indices into the DRIVER_OBJECT.MajorFunction dispatch
 * table. When IoCallDriver is called, the I/O manager looks up
 * the current stack location's MajorFunction and calls the
 * corresponding dispatch routine.
 *
 * Note: NTKRNL.H already defines these. We re-verify the values
 * here for documentation and ensure IRP_MJ_SCSI is defined.
 * ================================================================ */

#ifndef IRP_MJ_CREATE
#define IRP_MJ_CREATE                       0x00
#endif
#ifndef IRP_MJ_CLOSE
#define IRP_MJ_CLOSE                        0x02
#endif
#ifndef IRP_MJ_READ
#define IRP_MJ_READ                         0x03
#endif
#ifndef IRP_MJ_WRITE
#define IRP_MJ_WRITE                        0x04
#endif
#ifndef IRP_MJ_DEVICE_CONTROL
#define IRP_MJ_DEVICE_CONTROL               0x0E
#endif
#ifndef IRP_MJ_INTERNAL_DEVICE_CONTROL
#define IRP_MJ_INTERNAL_DEVICE_CONTROL      0x0F
#endif
#ifndef IRP_MJ_POWER
#define IRP_MJ_POWER                        0x16
#endif
#ifndef IRP_MJ_SYSTEM_CONTROL
#define IRP_MJ_SYSTEM_CONTROL               0x17
#endif
#ifndef IRP_MJ_PNP
#define IRP_MJ_PNP                          0x1B
#endif

/* IRP_MJ_SCSI is an alias for IRP_MJ_INTERNAL_DEVICE_CONTROL.
 * SCSI class drivers use this to send SRBs to port drivers. */
#ifndef IRP_MJ_SCSI
#define IRP_MJ_SCSI                         IRP_MJ_INTERNAL_DEVICE_CONTROL
#endif

/* Maximum number of major function slots in the dispatch table */
#ifndef IRP_MJ_DISPATCH_COUNT
#define IRP_MJ_DISPATCH_COUNT               28
#endif

/* ================================================================
 * IO_STATUS_BLOCK
 *
 * Embedded in every IRP. Contains the completion status and an
 * information field (usually bytes transferred).
 * ================================================================ */

/* Already defined in NTKRNL.H as IO_STATUS_BLOCK */

/* ================================================================
 * IRP STRUCTURE
 *
 * The I/O Request Packet is the fundamental I/O structure in NT.
 * Every I/O operation (read, write, IOCTL, PnP, power) flows
 * through an IRP. The IRP contains:
 *   - IoStatus: completion status and information
 *   - Stack locations: per-driver parameters
 *   - MdlAddress: memory descriptor list for DMA
 *   - AssociatedIrp.SystemBuffer: buffered I/O data
 *   - UserBuffer: direct I/O user buffer
 *   - Cancel support
 *
 * Defined in NTKRNL.H. We reference it here for clarity.
 * ================================================================ */

/* Already defined in NTKRNL.H as struct _IRP */

/* ================================================================
 * IO_STACK_LOCATION
 *
 * Each driver in a device stack gets its own stack location within
 * the IRP. The stack location contains:
 *   - MajorFunction / MinorFunction: what operation
 *   - Parameters: operation-specific data (union)
 *   - DeviceObject: target device for this stack entry
 *   - CompletionRoutine / Context: completion callback
 *
 * The Parameters union has members for:
 *   - Scsi: SRB pointer (for IRP_MJ_SCSI)
 *   - QueryDeviceRelations / PnP: PnP sub-parameters
 *   - Power: power state parameters
 *   - DeviceIoControl: IOCTL parameters
 *
 * Defined in NTKRNL.H. We reference it here for clarity.
 * ================================================================ */

/* Already defined in NTKRNL.H as struct _IO_STACK_LOCATION */

/* ================================================================
 * DEVICE_OBJECT
 *
 * Represents a device in the NT device tree. Contains:
 *   - DriverObject: owning driver
 *   - NextDevice: next device in the driver's device list
 *   - AttachedDevice: higher device in the stack
 *   - CurrentIrp: IRP being processed (StartIo model)
 *   - DeviceExtension: driver-private data
 *   - DeviceType: FILE_DEVICE_xxx
 *   - Flags: DO_xxx flags
 *   - AlignmentRequirement: buffer alignment
 *   - StackSize: number of stack locations needed
 *   - ReferenceCount: outstanding references
 *
 * Defined in NTKRNL.H. We reference it here for clarity.
 * ================================================================ */

/* Already defined in NTKRNL.H as struct _DEVICE_OBJECT */

/* ================================================================
 * DRIVER_OBJECT
 *
 * Represents a loaded driver. Contains:
 *   - DriverInit: DriverEntry function pointer
 *   - DriverUnload: cleanup function
 *   - MajorFunction[]: IRP dispatch table
 *   - DeviceObject: head of the device list
 *   - DriverExtension: contains AddDevice pointer
 *
 * Defined in NTKRNL.H. We reference it here for clarity.
 * ================================================================ */

/* Already defined in NTKRNL.H as struct _DRIVER_OBJECT */

/* ================================================================
 * DRIVER_EXTENSION
 *
 * Extension of DRIVER_OBJECT for WDM drivers. Contains:
 *   - AddDevice: called by PnP manager for each PDO
 *
 * Defined in NTKRNL.H. We reference it here for clarity.
 * ================================================================ */

/* Already defined in NTKRNL.H as struct _DRIVER_EXTENSION */

/* ================================================================
 * IRP STACK LOCATION ACCESS MACROS
 *
 * These macros navigate the IRP's stack location array.
 * CurrentLocation counts down from StackCount as the IRP
 * moves down the device stack.
 *
 * Defined in NTKRNL.H. Verified here for completeness.
 * ================================================================ */

/* IoGetCurrentIrpStackLocation(Irp) - defined in NTKRNL.H */
/* IoGetNextIrpStackLocation(Irp) - defined in NTKRNL.H */
/* IoSetNextIrpStackLocation(Irp) - defined in NTKRNL.H */
/* IoCopyCurrentIrpStackLocationToNext(Irp) - defined in NTKRNL.H */
/* IoSetCompletionRoutine(Irp, ...) - defined in NTKRNL.H */

/* ================================================================
 * DEVICE OBJECT TRACKING
 *
 * We maintain a simple list of all device objects created through
 * our I/O manager, for cleanup and enumeration purposes.
 * ================================================================ */

#define IRPMGR_MAX_DEVICES  32

/* ================================================================
 * I/O MANAGER FUNCTION PROTOTYPES
 *
 * These implement the NT IoXxx API. On real NT, they live in
 * ntoskrnl.exe. Here, they are implemented in IRPMGR.C using
 * VxD heap allocation.
 * ================================================================ */

/* --- IRP allocation and lifecycle --- */

/*
 * IoAllocateIrp - Allocate an IRP with the given stack size
 *
 * Parameters:
 *   StackSize    - Number of IO_STACK_LOCATION entries
 *   ChargeQuota  - Ignored (no quota tracking on Win9x)
 *
 * Returns:
 *   Pointer to allocated IRP, or NULL on failure
 */
PIRP __cdecl IrpMgr_IoAllocateIrp(CHAR StackSize, BOOLEAN ChargeQuota);

/*
 * IoFreeIrp - Free an IRP allocated with IoAllocateIrp
 *
 * Parameters:
 *   Irp - IRP to free
 */
VOID __cdecl IrpMgr_IoFreeIrp(PIRP Irp);

/* --- IRP dispatch --- */

/*
 * IoCallDriver - Send an IRP to a device's driver
 *
 * Decrements CurrentLocation, sets the stack's DeviceObject,
 * and calls the driver's MajorFunction dispatch routine.
 *
 * Parameters:
 *   DeviceObject - Target device
 *   Irp          - IRP to dispatch
 *
 * Returns:
 *   Status returned by the dispatch routine
 */
NTSTATUS __cdecl IrpMgr_IoCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS FASTCALL IrpMgr_IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);

/*
 * IoCompleteRequest - Complete an IRP
 *
 * Walks the stack backwards calling completion routines,
 * then frees the IRP.
 *
 * Parameters:
 *   Irp           - IRP to complete
 *   PriorityBoost - Scheduling boost (ignored on Win9x)
 */
VOID __cdecl IrpMgr_IoCompleteRequest(PIRP Irp, CHAR PriorityBoost);
VOID FASTCALL IrpMgr_IofCompleteRequest(PIRP Irp, CHAR PriorityBoost);
PIRP NTAPI IrpMgr_IoBuildDeviceIoControlRequest(
    ULONG IoControlCode,
    PDEVICE_OBJECT DeviceObject,
    PVOID InputBuffer,
    ULONG InputBufferLength,
    PVOID OutputBuffer,
    ULONG OutputBufferLength,
    BOOLEAN InternalDeviceIoControl,
    PKEVENT Event,
    PIO_STATUS_BLOCK IoStatusBlock);

/* --- IRP stack location helpers (function versions) --- */

PIO_STACK_LOCATION __cdecl IrpMgr_IoGetCurrentIrpStackLocation(PIRP Irp);
PIO_STACK_LOCATION __cdecl IrpMgr_IoGetNextIrpStackLocation(PIRP Irp);
VOID __cdecl IrpMgr_IoSetNextIrpStackLocation(PIRP Irp);
VOID __cdecl IrpMgr_IoCopyCurrentIrpStackLocationToNext(PIRP Irp);

/*
 * IoSetCompletionRoutine - Set completion callback on next stack location
 *
 * Parameters:
 *   Irp        - Target IRP
 *   Routine    - Completion callback
 *   Context    - Context passed to callback
 *   OnSuccess  - Call on STATUS_SUCCESS
 *   OnError    - Call on error status
 *   OnCancel   - Call on cancellation
 */
VOID __cdecl IrpMgr_IoSetCompletionRoutine(
    PIRP Irp,
    PIO_COMPLETION_ROUTINE Routine,
    PVOID Context,
    BOOLEAN OnSuccess,
    BOOLEAN OnError,
    BOOLEAN OnCancel);

/* --- Device object management --- */

/*
 * IoCreateDevice - Create a new device object
 *
 * Allocates a DEVICE_OBJECT and optional device extension
 * from the VxD heap. Links it into the driver's device list.
 *
 * Parameters:
 *   DriverObject     - Owning driver
 *   ExtensionSize    - Size of DeviceExtension in bytes
 *   DeviceName       - Optional device name (ignored in shim)
 *   DeviceType       - FILE_DEVICE_xxx
 *   Characteristics  - Device characteristics flags
 *   Exclusive        - Exclusive access flag
 *   DeviceObject     - Receives the created device object
 *
 * Returns:
 *   STATUS_SUCCESS or STATUS_INSUFFICIENT_RESOURCES
 */
NTSTATUS __cdecl IrpMgr_IoCreateDevice(
    PDRIVER_OBJECT DriverObject,
    ULONG ExtensionSize,
    PUNICODE_STRING DeviceName,
    ULONG DeviceType,
    ULONG Characteristics,
    BOOLEAN Exclusive,
    PDEVICE_OBJECT *DeviceObject);

/*
 * IoDeleteDevice - Delete a device object
 *
 * Removes the device from the driver's device list and frees
 * the memory.
 *
 * Parameters:
 *   DeviceObject - Device to delete
 */
VOID __cdecl IrpMgr_IoDeleteDevice(PDEVICE_OBJECT DeviceObject);

/*
 * IoAttachDeviceToDeviceStack - Attach a device to a device stack
 *
 * Sets SourceDevice as the top of the stack above TargetDevice.
 * Increments the stack size for IRP allocation.
 *
 * Parameters:
 *   SourceDevice - Device to attach (our FDO)
 *   TargetDevice - Device to attach to (PDO or lower filter)
 *
 * Returns:
 *   The device that was previously at the top of the stack
 *   (which SourceDevice is now above), or NULL on failure
 */
PDEVICE_OBJECT __cdecl IrpMgr_IoAttachDeviceToDeviceStack(
    PDEVICE_OBJECT SourceDevice,
    PDEVICE_OBJECT TargetDevice);

/*
 * IoDetachDevice - Detach a device from the device stack
 *
 * Clears the AttachedDevice pointer on TargetDevice.
 *
 * Parameters:
 *   TargetDevice - Device to detach from (the lower device)
 */
VOID __cdecl IrpMgr_IoDetachDevice(PDEVICE_OBJECT TargetDevice);

/*
 * IoGetAttachedDeviceReference - Walk to the top of the device stack
 *
 * Returns the topmost device in the stack. On real NT this
 * increments the reference count; our shim just returns the pointer.
 *
 * Parameters:
 *   DeviceObject - Any device in the stack
 *
 * Returns:
 *   Topmost device in the stack
 */
PDEVICE_OBJECT __cdecl IrpMgr_IoGetAttachedDeviceReference(
    PDEVICE_OBJECT DeviceObject);

/* --- Error logging (stubs) --- */

/*
 * IoAllocateErrorLogEntry - Allocate an error log entry
 *
 * Returns NULL (error logging is not supported on Win9x).
 */
PVOID __cdecl IrpMgr_IoAllocateErrorLogEntry(PVOID IoObject, UCHAR EntrySize);

/*
 * IoWriteErrorLogEntry - Write an error log entry
 *
 * No-op on Win9x.
 */
VOID __cdecl IrpMgr_IoWriteErrorLogEntry(PVOID ElEntry);

#endif /* _IRPMGR_H_ */
