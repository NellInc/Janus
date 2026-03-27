PAGE 58,132
TITLE VXDWRAP.ASM - VxD wrapper/boilerplate for NT SCSI miniport shim
;****************************************************************************
;
;   VXDWRAP.ASM
;
;   VxD assembly boilerplate and service wrappers for NTMINI.VXD.
;   This is a Windows 9x Port Driver (PDR) that loads NT4 SCSI miniport
;   drivers (e.g. ATAPI.SYS) and translates IOS requests into miniport
;   calls.
;
;   Assembler: MASM 6.11+ (ml /coff /W3 /c /Cx)
;   Environment: Windows 98 DDK
;
;   Copyright (c) 2026 Nell Watson / PA research project.
;
;****************************************************************************

        .386p

;------------------------------------------------------------------------
; DDK includes
;------------------------------------------------------------------------
        include vmm.inc         ; VMM services, VxD macros
        include debug.inc       ; Out_Debug_String, Trace_Out
        include vpicd.inc       ; VPICD interrupt virtualization
        include vwin32.inc      ; W32_DeviceIOControl equates
        include ifsmgr.inc      ; IFSMgr Ring-0 file I/O

; IOS equates -- ios.inc may not ship with all DDK versions, so we
; define the bare minimum ourselves if the include is absent.
ifndef IOS_Register
IOS_Register    equ     00010040h   ; IOS service ordinal
endif

;------------------------------------------------------------------------
; Externs -- C functions compiled with __cdecl in NTMINI.C / IRQHOOK.C
;------------------------------------------------------------------------
extern _NtMini_DeviceInit@0:NEAR       ; NTMINI.C  -- stdcall, 0 args
extern _NtMini_DeviceExit@0:NEAR       ; NTMINI.C  -- stdcall, 0 args
extern _NtMini_W32Ioctl@4:NEAR         ; NTMINI.C  -- stdcall, 1 arg (PDIOCPARAMETERS)
extern _irq_hw_isr:NEAR                ; IRQHOOK.C -- cdecl ISR thunk target

;------------------------------------------------------------------------
; Device Descriptor Block
;
; Declare_Virtual_Device macro expands the DDB, the segment map, and the
; DEVICE_ID / INIT_ORDER constants.
;------------------------------------------------------------------------
Declare_Virtual_Device NTMINI, 1, 0, NTMINI_Control, \
        UNDEFINED_DEVICE_ID, UNDEFINED_INIT_ORDER

;========================================================================
;
;   LOCKED (non-pageable) CODE
;
;========================================================================
VxD_LOCKED_CODE_SEG

;------------------------------------------------------------------------
; NTMINI_Control -- VMM control message dispatcher
;
; Called by VMM whenever a system event occurs. We handle:
;   Sys_Dynamic_Device_Init  -- dynamic VxD load  (LoadDevice)
;   Device_Init              -- static  VxD load  (SYSTEM.INI)
;   Sys_Dynamic_Device_Exit  -- dynamic unload
;   W32_DeviceIOControl      -- DeviceIoControl from ring-3
;
; All other messages: clc (return success / "not handled").
;------------------------------------------------------------------------
BeginProc NTMINI_Control

        Control_Dispatch Sys_Dynamic_Device_Init, _NTMINI_OnInit
        Control_Dispatch Device_Init,             _NTMINI_OnInit
        Control_Dispatch Sys_Dynamic_Device_Exit, _NTMINI_OnExit
        Control_Dispatch W32_DeviceIOControl,     _NTMINI_OnIoctl

        ; Default: message not handled -- return carry clear (success).
        clc
        ret

EndProc NTMINI_Control

;------------------------------------------------------------------------
; _NTMINI_OnInit -- Forwarded from control proc for init messages.
;------------------------------------------------------------------------
BeginProc _NTMINI_OnInit

        pushad
        call    _NtMini_DeviceInit@0    ; C init -- returns 0 on success
        test    eax, eax
        popad
        jnz     short @init_fail
        clc                             ; success
        ret
@init_fail:
        stc                             ; failure -- VMM will unload us
        ret

EndProc _NTMINI_OnInit

;------------------------------------------------------------------------
; _NTMINI_OnExit -- Forwarded from control proc for unload.
;------------------------------------------------------------------------
BeginProc _NTMINI_OnExit

        pushad
        call    _NtMini_DeviceExit@0
        popad
        clc
        ret

EndProc _NTMINI_OnExit

;------------------------------------------------------------------------
; _NTMINI_OnIoctl -- W32_DeviceIOControl handler.
;   ESI -> DIOC_Registers (DIOCParams pointer)
;------------------------------------------------------------------------
BeginProc _NTMINI_OnIoctl

        pushad
        push    esi                     ; arg: PDIOCPARAMETERS
        call    _NtMini_W32Ioctl@4     ; stdcall -- callee cleans stack
        mov     [esp+1Ch], eax          ; patch saved EAX in PUSHAD frame
        popad
        clc
        ret

EndProc _NTMINI_OnIoctl

;========================================================================
;   VxD service wrappers -- callable from C (__cdecl convention)
;========================================================================

;------------------------------------------------------------------------
; ULONG _VxD_PageAllocate(ULONG nPages, ULONG flags);
;
; Allocates locked, non-pageable physical memory.
; Returns linear address of the allocation, or 0 on failure.
;
; Internally calls VMM _PageAllocate:
;   _PageAllocate(nPages, pType, hVM, AlignMask, minPhys, maxPhys,
;                 pPhysAddr, flags)
; We hardcode most parameters for simplicity:
;   pType      = PG_SYS (1)
;   hVM        = 0 (system VM)
;   AlignMask  = 0
;   minPhys    = 0
;   maxPhys    = FFFFFFFFh
;   pPhysAddr  = 0 (don't need physical addr)
;   flags      = caller-supplied (typically PageFixed + PageZeroInit)
;------------------------------------------------------------------------
        public _VxD_PageAllocate
BeginProc _VxD_PageAllocate

        push    ebp
        mov     ebp, esp
        push    ebx
        push    edi
        push    esi

        ; Build the _PageAllocate parameter block on the stack.
        ; Push in reverse order (rightmost arg first).
        push    [ebp+12]               ; flags (caller's flags)
        push    0                       ; PhysAddr ptr (NULL)
        push    0FFFFFFFFh              ; maxPhys
        push    0                       ; minPhys
        push    0                       ; AlignMask
        push    0                       ; hVM (system)
        push    PG_SYS                  ; pType
        push    [ebp+8]                ; nPages

        VMMCall _PageAllocate
        add     esp, 32                 ; 8 dwords * 4

        ; EAX = linear address (0 on failure), EDX = physical address
        pop     esi
        pop     edi
        pop     ebx
        pop     ebp
        ret

EndProc _VxD_PageAllocate

;------------------------------------------------------------------------
; void _VxD_PageFree(ULONG linAddr, ULONG flags);
;
; Frees memory allocated by _VxD_PageAllocate.
;------------------------------------------------------------------------
        public _VxD_PageFree
BeginProc _VxD_PageFree

        push    ebp
        mov     ebp, esp

        push    [ebp+12]               ; flags (0)
        push    [ebp+8]                ; linear address

        VMMCall _PageFree
        add     esp, 8

        pop     ebp
        ret

EndProc _VxD_PageFree

;------------------------------------------------------------------------
; ULONG _VxD_HeapAllocate(ULONG nBytes, ULONG flags);
;
; Allocates from the non-pageable heap. Better for small allocations
; than full-page granularity.
;------------------------------------------------------------------------
        public _VxD_HeapAllocate
BeginProc _VxD_HeapAllocate

        push    ebp
        mov     ebp, esp

        push    [ebp+12]               ; flags (HEAPZEROINIT etc.)
        push    [ebp+8]                ; nBytes

        VMMCall _HeapAllocate
        add     esp, 8                 ; returns address in EAX

        pop     ebp
        ret

EndProc _VxD_HeapAllocate

;------------------------------------------------------------------------
; void _VxD_HeapFree(ULONG addr, ULONG flags);
;------------------------------------------------------------------------
        public _VxD_HeapFree
BeginProc _VxD_HeapFree

        push    ebp
        mov     ebp, esp

        push    [ebp+12]               ; flags
        push    [ebp+8]                ; addr

        VMMCall _HeapFree
        add     esp, 8

        pop     ebp
        ret

EndProc _VxD_HeapFree

;========================================================================
;   IOS registration wrapper
;========================================================================

;------------------------------------------------------------------------
; ULONG _VxD_IOS_Register(PVOID pDRP);
;
; Registers this driver with the I/O Supervisor by passing a pointer
; to a filled-in Driver Registration Packet (DRP).
;
; Returns: IOS error code (0 = success).
;------------------------------------------------------------------------
        public _VxD_IOS_Register
BeginProc _VxD_IOS_Register

        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi

        mov     eax, [ebp+8]           ; pDRP

        ; IOS_Register expects the DRP pointer in EAX on some DDK versions,
        ; or as a stack parameter on others. We do both for safety.
        push    eax
        VxDCall IOS_Register
        add     esp, 4

        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

EndProc _VxD_IOS_Register

;========================================================================
;   VPICD interrupt virtualization wrappers
;========================================================================

;------------------------------------------------------------------------
; ULONG _VxD_VPICD_Virtualize_IRQ(PVOID pVID);
;
; Virtualizes an IRQ so our ISR gets called instead of the default
; handler. Returns IRQ handle on success, 0 on failure.
;
; Arg: pointer to VPICD_IRQ_Descriptor structure.
;------------------------------------------------------------------------
        public _VxD_VPICD_Virtualize_IRQ
BeginProc _VxD_VPICD_Virtualize_IRQ

        push    ebp
        mov     ebp, esp
        push    ebx
        push    edi
        push    esi

        mov     edi, [ebp+8]           ; EDI -> VPICD_IRQ_Descriptor

        VxDCall VPICD_Virtualize_IRQ
        jnc     short @virt_ok
        xor     eax, eax               ; failure: return 0
        jmp     short @virt_done
@virt_ok:
        mov     eax, eax               ; EAX = IRQ handle (already there)
@virt_done:
        pop     esi
        pop     edi
        pop     ebx
        pop     ebp
        ret

EndProc _VxD_VPICD_Virtualize_IRQ

;------------------------------------------------------------------------
; void _VxD_VPICD_Force_Default_Behavior(ULONG irqHandle);
;
; Releases a virtualized IRQ, restoring default handling.
;------------------------------------------------------------------------
        public _VxD_VPICD_Force_Default_Behavior
BeginProc _VxD_VPICD_Force_Default_Behavior

        push    ebp
        mov     ebp, esp
        push    ebx

        mov     eax, [ebp+8]           ; IRQ handle

        VxDCall VPICD_Force_Default_Behavior

        pop     ebx
        pop     ebp
        ret

EndProc _VxD_VPICD_Force_Default_Behavior

;------------------------------------------------------------------------
; void _VxD_VPICD_Phys_EOI(ULONG irqHandle);
;
; Sends physical End-Of-Interrupt to the PIC for this IRQ.
;------------------------------------------------------------------------
        public _VxD_VPICD_Phys_EOI
BeginProc _VxD_VPICD_Phys_EOI

        push    ebp
        mov     ebp, esp
        push    ebx

        mov     eax, [ebp+8]           ; IRQ handle

        VxDCall VPICD_Phys_EOI

        pop     ebx
        pop     ebp
        ret

EndProc _VxD_VPICD_Phys_EOI

;------------------------------------------------------------------------
; void _VxD_VPICD_Physically_Unmask(ULONG irqHandle);
;
; Unmasks the physical IRQ line on the PIC.
;------------------------------------------------------------------------
        public _VxD_VPICD_Physically_Unmask
BeginProc _VxD_VPICD_Physically_Unmask

        push    ebp
        mov     ebp, esp
        push    ebx

        mov     eax, [ebp+8]

        VxDCall VPICD_Physically_Unmask

        pop     ebx
        pop     ebp
        ret

EndProc _VxD_VPICD_Physically_Unmask

;------------------------------------------------------------------------
; void _VxD_VPICD_Physically_Mask(ULONG irqHandle);
;
; Masks the physical IRQ line on the PIC.
;------------------------------------------------------------------------
        public _VxD_VPICD_Physically_Mask
BeginProc _VxD_VPICD_Physically_Mask

        push    ebp
        mov     ebp, esp
        push    ebx

        mov     eax, [ebp+8]

        VxDCall VPICD_Physically_Mask

        pop     ebx
        pop     ebp
        ret

EndProc _VxD_VPICD_Physically_Mask

;========================================================================
;   IRQ ISR thunk
;
;   VPICD calls our Hw_Int_Proc with:
;       EAX = IRQ handle
;       EBX = current VM handle
;   We must preserve all registers except EAX and set carry flag to
;   indicate whether we handled the interrupt.
;
;   This thunk bridges from VPICD's register-based calling convention
;   to our C __cdecl ISR in IRQHOOK.C.
;========================================================================

        public _ISR_Thunk
BeginProc _ISR_Thunk

        pushad
        pushfd

        ; Save the IRQ handle so the C code can issue EOI.
        push    eax                     ; arg: IRQ handle

        call    _irq_hw_isr             ; C function, returns BOOL in EAX
        add     esp, 4                  ; clean up cdecl arg

        ; Save return value for carry-flag decision.
        mov     edx, eax

        popfd
        popad

        ; If the C ISR returned nonzero, we claimed the interrupt.
        ; VPICD expects: carry set = handled, carry clear = not ours.
        ; However, with VPICD_OPT_READ_HW_IRR we always EOI in the C code,
        ; so we just return carry clear (let VPICD continue chain if needed).
        clc
        ret

EndProc _ISR_Thunk

;========================================================================
;   Debug output wrapper
;========================================================================

;------------------------------------------------------------------------
; void _VxD_Debug_Printf(char *fmt);
;
; Outputs a null-terminated string to the debug console via
; Out_Debug_String. Only functional with debug builds of VMM32;
; on retail builds this is a no-op.
;
; Note: This is NOT printf-style. Pass a pre-formatted string.
;------------------------------------------------------------------------
        public _VxD_Debug_Printf
BeginProc _VxD_Debug_Printf

        push    ebp
        mov     ebp, esp
        push    esi

        mov     esi, [ebp+8]           ; ESI -> string
        VMMCall Out_Debug_String

        pop     esi
        pop     ebp
        ret

EndProc _VxD_Debug_Printf

;========================================================================
;   Ring-0 file I/O wrappers
;
;   Used to load ATAPI.SYS (the NT miniport binary) from disk at init
;   time. We use the IFSMgr Ring-0 file I/O services which are
;   available during Device_Init and later.
;========================================================================

;------------------------------------------------------------------------
; ULONG _VxD_File_Open(char *filename, ULONG *pFileSize);
;
; Opens a file for reading. Returns a file handle, or 0 on failure.
; If pFileSize is non-NULL, writes the file size there.
;
; Uses R0_OpenCreateFile (VMM service):
;   EAX = action (0 = open existing)
;   ECX = attributes (0 = normal)
;   EDX = flags: OPEN_FLAGS_NO_CACHE | OPEN_FLAGS_NO_COMPRESS
;   ESI = pointer to filename (null-terminated)
;   Returns: EAX = handle, ECX = action taken, carry on error
;------------------------------------------------------------------------

; R0_OpenCreateFile flags
R0_OPEN_ACCESS_READONLY equ 00000000h
R0_OPEN_SHARE_DENYWRITE equ 00000020h
R0_OPEN_FLAGS_EXISTING  equ 00000001h   ; open existing only
R0_NO_CACHE             equ 00000100h

        public _VxD_File_Open
BeginProc _VxD_File_Open

        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi

        ; R0_OpenCreateFile parameters:
        mov     esi, [ebp+8]           ; filename
        mov     eax, R0_OPEN_ACCESS_READONLY OR R0_OPEN_SHARE_DENYWRITE
        mov     ecx, 0                 ; attributes (normal)
        mov     edx, R0_OPEN_FLAGS_EXISTING OR R0_NO_CACHE
        mov     ebx, 0                 ; pAction (don't care)

        VMMCall R0_OpenCreateFile
        jnc     short @fo_ok

        ; Open failed.
        xor     eax, eax
        jmp     short @fo_done

@fo_ok:
        ; EAX = file handle. Get file size if caller wants it.
        mov     edi, [ebp+12]          ; pFileSize
        test    edi, edi
        jz      short @fo_done

        ; Use R0_GetFileSize to retrieve file length.
        push    eax                     ; save handle
        VMMCall R0_GetFileSize          ; EAX = handle in, returns size in EAX
        mov     [edi], eax             ; store file size
        pop     eax                     ; restore handle

@fo_done:
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

EndProc _VxD_File_Open

;------------------------------------------------------------------------
; ULONG _VxD_File_Read(ULONG hFile, PVOID buffer, ULONG nBytes);
;
; Reads nBytes from the current file position into buffer.
; Returns the number of bytes actually read, or 0 on failure.
;
; Uses R0_ReadFile:
;   EAX = file handle
;   ECX = byte count
;   EDX = file position (0FFFFFFFFh = current pos)
;   ESI = buffer
;   Returns: ECX = bytes read, carry on error
;------------------------------------------------------------------------
        public _VxD_File_Read
BeginProc _VxD_File_Read

        push    ebp
        mov     ebp, esp
        push    ebx
        push    esi
        push    edi

        mov     eax, [ebp+8]           ; hFile
        mov     esi, [ebp+12]          ; buffer
        mov     ecx, [ebp+16]          ; nBytes
        mov     edx, 0FFFFFFFFh        ; current file position

        VMMCall R0_ReadFile
        jnc     short @fr_ok

        xor     eax, eax               ; error: return 0
        jmp     short @fr_done

@fr_ok:
        mov     eax, ecx               ; bytes actually read

@fr_done:
        pop     edi
        pop     esi
        pop     ebx
        pop     ebp
        ret

EndProc _VxD_File_Read

;------------------------------------------------------------------------
; void _VxD_File_Close(ULONG hFile);
;
; Closes a file opened with _VxD_File_Open.
;------------------------------------------------------------------------
        public _VxD_File_Close
BeginProc _VxD_File_Close

        push    ebp
        mov     ebp, esp
        push    ebx

        mov     eax, [ebp+8]           ; hFile

        VMMCall R0_CloseFile

        pop     ebx
        pop     ebp
        ret

EndProc _VxD_File_Close

VxD_LOCKED_CODE_ENDS

;========================================================================
;
;   INIT-ONLY CODE (freed after boot completes)
;
;========================================================================
VxD_ICODE_SEG

; (Reserved for future use. Heavy init work is done in C code which
;  lives in the locked segment since we support dynamic loading.)

VxD_ICODE_ENDS

;========================================================================
;
;   LOCKED DATA (non-pageable)
;
;========================================================================
VxD_LOCKED_DATA_SEG

; Storage for the ISR thunk address, exported so C code can fill in
; the VPICD_IRQ_Descriptor's Hw_Int_Proc field.
        public _ISR_Thunk_Addr
_ISR_Thunk_Addr dd OFFSET32 _ISR_Thunk

VxD_LOCKED_DATA_ENDS

;========================================================================
;
;   INIT-ONLY DATA (freed after boot completes)
;
;========================================================================
VxD_IDATA_SEG

; Init banner string
_szInitBanner db 'NTMINI: NT Miniport Compatibility Shim v1.0', 0Dh, 0Ah, 0

VxD_IDATA_ENDS

        END
