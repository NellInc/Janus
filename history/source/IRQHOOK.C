/****************************************************************************
 *
 *  IRQHOOK.C
 *
 *  IRQ virtualization and hardware interrupt handling for NTMINI.VXD.
 *
 *  This module hooks a SCSI/ATAPI controller's IRQ line via VPICD so that
 *  incoming hardware interrupts are forwarded to the NT miniport driver's
 *  HwInterrupt routine. It acts as the bridge between Win98's VPICD
 *  interrupt model and NT4's SCSI miniport interrupt model.
 *
 *  NT miniport drivers expect:
 *    - Their HwInterrupt callback to be invoked at DIRQL
 *    - A BOOLEAN return indicating whether the interrupt was theirs
 *    - The port driver (us) to handle EOI and unmask
 *
 *  Win98 VPICD provides:
 *    - A virtualized IRQ hook (Hw_Int_Proc) called with interrupts disabled
 *    - EOI/mask/unmask services we must call explicitly
 *
 *  Compiler: Microsoft Visual C++ 4.x or Watcom C (flat model, /Gs /Ox)
 *  Environment: Windows 98 DDK VxD build environment
 *
 *  Copyright (c) 2026 Nell Watson / PA research project.
 *
 ****************************************************************************/

/*--------------------------------------------------------------------------
 * Type definitions
 *
 * We define our own base types rather than pulling in the full NT DDK
 * headers, since we are building a VxD, not an NT driver.
 *--------------------------------------------------------------------------*/

typedef unsigned long   ULONG;
typedef unsigned short  USHORT;
typedef unsigned char   UCHAR;
typedef int             BOOLEAN;
typedef void *          PVOID;

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

/*--------------------------------------------------------------------------
 * VPICD_IRQ_Descriptor
 *
 * This structure is passed to VPICD_Virtualize_IRQ to hook an IRQ line.
 * The layout must exactly match what VPICD expects (see vpicd.inc in the
 * Windows 98 DDK).
 *
 * Fields marked (0) are unused by us and should be set to zero, which
 * tells VPICD to use default behavior for those callbacks.
 *--------------------------------------------------------------------------*/
#pragma pack(1)
typedef struct _VPICD_IRQ_DESCRIPTOR {
    USHORT  VID_IRQ_Number;         /* IRQ line number (0-15)               */
    USHORT  VID_Options;            /* Option flags (see below)             */
    ULONG   VID_Hw_Int_Proc;       /* Address of our hardware ISR thunk    */
    ULONG   VID_Virt_Int_Proc;     /* Virtual interrupt handler    (0)     */
    ULONG   VID_EOI_Proc;          /* EOI callback                (0)     */
    ULONG   VID_Mask_Change_Proc;  /* Mask change notification    (0)     */
    ULONG   VID_IRET_Proc;         /* IRET handler                (0)     */
    ULONG   VID_IRET_Time_Out;     /* IRET timeout in ms (500 default)    */
    ULONG   VID_Hw_Int_Ref;        /* Reference data passed to ISR (unused)*/
} VPICD_IRQ_DESCRIPTOR;
#pragma pack()

/*
 * VPICD option flags (from vpicd.inc).
 * VPICD_OPT_READ_HW_IRR: VPICD reads the PIC's In-Service Register
 *   before dispatching to verify the IRQ is genuinely asserted. Reduces
 *   spurious interrupt dispatches on shared IRQ lines.
 * VPICD_OPT_CAN_SHARE: Allow this IRQ to be shared with other
 *   virtualized handlers.
 */
#define VPICD_OPT_READ_HW_IRR   0x0001
#define VPICD_OPT_CAN_SHARE     0x0002

/*--------------------------------------------------------------------------
 * External functions provided by VXDWRAP.ASM
 *
 * These are thin assembly wrappers around the corresponding VxD services.
 * All use __cdecl calling convention.
 *--------------------------------------------------------------------------*/

extern ULONG __cdecl VxD_VPICD_Virtualize_IRQ(VPICD_IRQ_DESCRIPTOR *vid);
extern void  __cdecl VxD_VPICD_Force_Default_Behavior(ULONG irq_handle);
extern void  __cdecl VxD_VPICD_Phys_EOI(ULONG irq_handle);
extern void  __cdecl VxD_VPICD_Physically_Unmask(ULONG irq_handle);
extern void  __cdecl VxD_VPICD_Physically_Mask(ULONG irq_handle);
extern void  __cdecl VxD_Debug_Printf(char *msg);

/*
 * ISR thunk address, defined in VXDWRAP.ASM locked data segment.
 * This is the address of the assembly-level ISR entry point that VPICD
 * will call; it in turn calls our C-level irq_hw_isr().
 */
extern ULONG ISR_Thunk_Addr;

/*--------------------------------------------------------------------------
 * Module-level state
 *
 * All fields are in the locked (non-pageable) data segment. This is
 * critical: if any of this were pageable, a page fault inside an ISR
 * would triple-fault the machine.
 *--------------------------------------------------------------------------*/

/* Handle returned by VPICD_Virtualize_IRQ. Zero means not hooked. */
static ULONG g_irq_handle = 0;

/* IRQ number we are hooked on (for debug messages). */
static ULONG g_irq_number = 0;

/* Pointer to the NT miniport's HwInterrupt callback.
 *
 * NT4 SCSI miniport HwInterrupt prototype:
 *   BOOLEAN (*HW_INTERRUPT)(PVOID DeviceExtension);
 *
 * The miniport returns TRUE if the interrupt was from its device,
 * FALSE otherwise (for shared IRQ support).
 */
typedef BOOLEAN (__cdecl *PFN_HW_INTERRUPT)(PVOID DeviceExtension);
static PFN_HW_INTERRUPT g_pfn_hw_interrupt = NULL;

/* Pointer to the miniport's device extension, passed to HwInterrupt. */
static PVOID g_device_extension = NULL;

/* Interrupt statistics (debug/diagnostic). */
static ULONG g_isr_count       = 0;   /* Total times our ISR was called    */
static ULONG g_isr_claimed     = 0;   /* Times miniport claimed the IRQ    */
static ULONG g_isr_unclaimed   = 0;   /* Times miniport did not claim      */

/*--------------------------------------------------------------------------
 * irq_hook_install
 *
 * Hooks the specified IRQ line via VPICD and registers our ISR thunk as
 * the hardware interrupt handler.
 *
 * Parameters:
 *   irq_number    - IRQ line to hook (e.g., 14 for primary IDE, 15 for
 *                   secondary IDE, or whatever the PCI SCSI controller
 *                   is assigned).
 *   hw_interrupt  - Pointer to the NT miniport's HwInterrupt function.
 *   dev_ext       - Pointer to the miniport's device extension. This is
 *                   the opaque context block the miniport allocated during
 *                   HwFindAdapter / HwInitialize.
 *
 * Returns:
 *   0 on success, nonzero on failure.
 *
 * Notes:
 *   - Only one IRQ hook is supported at a time. Call irq_hook_remove()
 *     before hooking a different IRQ.
 *   - The IRQ is physically unmasked after virtualization so the
 *     controller can actually fire interrupts.
 *--------------------------------------------------------------------------*/
int __cdecl irq_hook_install(ULONG irq_number,
                             BOOLEAN (__cdecl *hw_interrupt)(PVOID),
                             PVOID dev_ext)
{
    VPICD_IRQ_DESCRIPTOR vid;
    ULONG handle;

    /* Sanity checks. */
    if (hw_interrupt == NULL) {
        VxD_Debug_Printf("NTMINI IRQ: NULL HwInterrupt callback\r\n");
        return -1;
    }
    if (g_irq_handle != 0) {
        VxD_Debug_Printf("NTMINI IRQ: already hooked, remove first\r\n");
        return -2;
    }
    if (irq_number > 15) {
        VxD_Debug_Printf("NTMINI IRQ: invalid IRQ number\r\n");
        return -3;
    }

    /* Store the miniport callback and context. */
    g_pfn_hw_interrupt = (PFN_HW_INTERRUPT)hw_interrupt;
    g_device_extension = dev_ext;
    g_irq_number       = irq_number;

    /* Reset statistics. */
    g_isr_count     = 0;
    g_isr_claimed   = 0;
    g_isr_unclaimed = 0;

    /*
     * Fill in the VPICD IRQ descriptor.
     *
     * VID_Hw_Int_Proc is set to the address of our assembly thunk
     * (_ISR_Thunk in VXDWRAP.ASM), which saves registers and calls
     * our C-level irq_hw_isr() below.
     *
     * We request CAN_SHARE because SCSI controllers on PCI are almost
     * always on shared IRQ lines. We also request READ_HW_IRR to let
     * VPICD verify the IRQ is genuinely asserted before dispatching,
     * which reduces spurious calls on busy shared lines.
     */
    vid.VID_IRQ_Number       = (USHORT)irq_number;
    vid.VID_Options          = VPICD_OPT_CAN_SHARE | VPICD_OPT_READ_HW_IRR;
    vid.VID_Hw_Int_Proc      = ISR_Thunk_Addr;
    vid.VID_Virt_Int_Proc    = 0;
    vid.VID_EOI_Proc         = 0;
    vid.VID_Mask_Change_Proc = 0;
    vid.VID_IRET_Proc        = 0;
    vid.VID_IRET_Time_Out    = 500;   /* ms, standard default */
    vid.VID_Hw_Int_Ref       = 0;

    /* Ask VPICD to virtualize this IRQ. */
    handle = VxD_VPICD_Virtualize_IRQ(&vid);
    if (handle == 0) {
        VxD_Debug_Printf("NTMINI IRQ: VPICD_Virtualize_IRQ failed\r\n");
        g_pfn_hw_interrupt = NULL;
        g_device_extension = NULL;
        return -4;
    }

    g_irq_handle = handle;

    /*
     * Physically unmask the IRQ so the controller's interrupts can
     * reach us. Without this, the PIC will hold the line masked and
     * no interrupts will fire.
     */
    VxD_VPICD_Physically_Unmask(g_irq_handle);

    VxD_Debug_Printf("NTMINI IRQ: hooked successfully\r\n");
    return 0;
}

/*--------------------------------------------------------------------------
 * irq_hook_remove
 *
 * Unhooks the IRQ, restoring VPICD's default behavior for this line.
 * Safe to call even if no IRQ is currently hooked (no-op in that case).
 *--------------------------------------------------------------------------*/
void __cdecl irq_hook_remove(void)
{
    if (g_irq_handle == 0) {
        return;  /* Nothing hooked. */
    }

    /*
     * Mask the IRQ first so no more interrupts arrive while we are
     * tearing down.
     */
    VxD_VPICD_Physically_Mask(g_irq_handle);

    /*
     * Release the virtualization. VPICD will restore its default
     * handler for this IRQ line.
     */
    VxD_VPICD_Force_Default_Behavior(g_irq_handle);

    VxD_Debug_Printf("NTMINI IRQ: unhooked\r\n");

    /* Clear state. */
    g_irq_handle       = 0;
    g_irq_number       = 0;
    g_pfn_hw_interrupt = NULL;
    g_device_extension = NULL;
}

/*--------------------------------------------------------------------------
 * irq_hw_isr
 *
 * C-level hardware ISR. Called from the assembly thunk (_ISR_Thunk in
 * VXDWRAP.ASM) whenever VPICD dispatches our virtualized IRQ.
 *
 * Calling context:
 *   - Ring 0, interrupts DISABLED (VPICD holds IF clear).
 *   - We must be fast. No blocking, no page faults, no VMM services
 *     that might block.
 *   - The assembly thunk passes the IRQ handle as the sole argument.
 *
 * What we do:
 *   1. Call the NT miniport's HwInterrupt(DeviceExtension).
 *   2. The miniport reads its device's interrupt status register and
 *      completes any pending I/O. It returns TRUE if the interrupt
 *      was from its device.
 *   3. We issue a physical EOI via VPICD regardless of the return
 *      value, because on a shared PCI IRQ line the interrupt is
 *      level-triggered and we need to clear it at the PIC even if
 *      it wasn't ours (another handler in the chain will service it).
 *   4. Return nonzero if the miniport claimed the interrupt, zero
 *      otherwise. The assembly thunk uses this to set/clear carry
 *      for VPICD's chain logic.
 *
 * Parameters:
 *   irq_handle - VPICD IRQ handle (same as g_irq_handle, but passed
 *                by the thunk for efficiency so we don't re-load it).
 *
 * Returns:
 *   Nonzero if the interrupt was claimed by the miniport, zero if not.
 *--------------------------------------------------------------------------*/
ULONG __cdecl irq_hw_isr(ULONG irq_handle)
{
    BOOLEAN claimed;

    g_isr_count++;

    /*
     * Guard against a race where the IRQ fires after we have begun
     * unhooking but before VPICD has fully deregistered us.
     */
    if (g_pfn_hw_interrupt == NULL) {
        VxD_VPICD_Phys_EOI(irq_handle);
        return 0;
    }

    /*
     * Call the NT miniport's HwInterrupt entry point.
     *
     * The miniport was written expecting to be called at DIRQL with
     * its device extension pointer. It will:
     *   - Read the device's interrupt status register
     *   - If the interrupt is from this device, service it (copy data
     *     from the device FIFO, advance the SRB, etc.)
     *   - Return TRUE if it was this device's interrupt
     *
     * For ATAPI.SYS specifically, HwInterrupt reads the IDE status
     * register (port base + 7), which clears the device's interrupt
     * assertion. This is essential: on a level-triggered PCI IRQ, if
     * we EOI the PIC without clearing the device's interrupt, the IRQ
     * will fire again immediately in an infinite loop.
     */
    claimed = g_pfn_hw_interrupt(g_device_extension);

    if (claimed) {
        g_isr_claimed++;
    } else {
        g_isr_unclaimed++;
    }

    /*
     * Issue physical EOI to the PIC.
     *
     * We MUST do this for every interrupt dispatch, whether claimed or
     * not. On a shared IRQ line, multiple devices may be asserting
     * simultaneously. If we don't EOI, the PIC will hold the line in
     * the In-Service state and no further interrupts (on this line or
     * lower-priority lines) will be delivered.
     *
     * For ISA (edge-triggered) IRQs this is straightforward. For PCI
     * (level-triggered) IRQs, the device must deassert its interrupt
     * line before we EOI, otherwise the PIC will immediately re-trigger.
     * The miniport's HwInterrupt is responsible for deasserting by
     * reading the device status register.
     */
    VxD_VPICD_Phys_EOI(irq_handle);

    return claimed ? 1 : 0;
}

/*--------------------------------------------------------------------------
 * irq_get_stats
 *
 * Returns interrupt statistics for diagnostic purposes.
 * Callable from the W32_DeviceIOControl path for a user-mode diagnostic
 * tool.
 *
 * Parameters:
 *   total     - [out] Total ISR invocations
 *   claimed   - [out] Miniport-claimed interrupts
 *   unclaimed - [out] Unclaimed (shared line, not our device)
 *--------------------------------------------------------------------------*/
void __cdecl irq_get_stats(ULONG *total, ULONG *claimed, ULONG *unclaimed)
{
    if (total)     *total     = g_isr_count;
    if (claimed)   *claimed   = g_isr_claimed;
    if (unclaimed) *unclaimed = g_isr_unclaimed;
}

/*--------------------------------------------------------------------------
 * irq_is_hooked
 *
 * Returns TRUE if an IRQ is currently virtualized by this module.
 *--------------------------------------------------------------------------*/
BOOLEAN __cdecl irq_is_hooked(void)
{
    return (g_irq_handle != 0) ? TRUE : FALSE;
}

/*--------------------------------------------------------------------------
 * irq_get_handle
 *
 * Returns the current VPICD IRQ handle, or 0 if not hooked.
 * Used by other modules that need to issue VPICD calls on this IRQ.
 *--------------------------------------------------------------------------*/
ULONG __cdecl irq_get_handle(void)
{
    return g_irq_handle;
}
