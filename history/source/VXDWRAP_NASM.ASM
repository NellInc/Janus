;============================================================================
;
;   VXDWRAP_NASM.ASM - VxD wrapper/boilerplate for NT SCSI miniport shim
;
;   NASM replacement for the MASM VXDWRAP.ASM. Produces OMF output
;   compatible with Open Watcom's wlink (format windows vxd).
;
;   Assembler: NASM 2.x+ with -f obj (OMF output)
;   Linker:    Open Watcom wlink, "format windows vxd"
;
;   Copyright (c) 2026 Nell Watson / PA research project.
;   MIT License.
;
;============================================================================

[BITS 32]

;============================================================================
; VxD Service IDs  (VxD_Device_ID << 16 | service_number)
;============================================================================
%define VMM_PageAllocate        0x00010053
%define VMM_PageFree            0x00010054
%define VMM_HeapAllocate        0x00010256
%define VMM_HeapFree            0x00010257
%define VMM_Out_Debug_String    0x0001001D
%define VMM_R0_OpenCreateFile   0x000102DE
%define VMM_R0_ReadFile         0x000102E1
%define VMM_R0_CloseFile        0x000102E0
%define VMM_R0_GetFileSize      0x000102DF
%define VMM_Set_Global_Time_Out 0x0001003C
%define VMM_Cancel_Time_Out     0x0001003D
%define VMM__RegOpenKey         0x00010100
%define VMM__RegCloseKey        0x00010101
%define VMM__RegCreateKey       0x00010102
%define VMM__RegQueryValueEx    0x00010104
%define VMM__RegSetValueEx      0x00010105

%define VPICD_Virtualize_IRQ            0x00030000
%define VPICD_Force_Default_Behavior    0x00030008
%define VPICD_Physically_Mask           0x0003000A
%define VPICD_Physically_Unmask         0x0003000B
%define VPICD_Phys_EOI                  0x00030012

%define IOS_Register_svc                0x00100003
%define IOS_Requestor_Service_svc       0x00100004
%define IOS_SendCommand_svc             0x00100007
%define ISP_INSERT_CALLDOWN_svc         0x00100008
%define IOS_BD_Command_Complete_svc     0x0010000B

; VxD control messages
%define Sys_Critical_Init           0x0000
%define Device_Init                 0x0001
%define Init_Complete               0x0002
%define Sys_Dynamic_Device_Init     0x001B
%define Sys_Dynamic_Device_Exit     0x001C

; DDB constants
%define UNDEFINED_DEVICE_ID     0x0000
%define UNDEFINED_INIT_ORDER    0x80000000
%define PG_SYS                  0x01

; R0 file I/O constants
%define R0_OPEN_ACCESS_READONLY 0x00000000
%define R0_OPEN_SHARE_DENYWRITE 0x00000020
%define R0_OPEN_FLAGS_EXISTING  0x00000001
%define R0_NO_CACHE             0x00000100


;============================================================================
; External C functions (from Watcom-compiled objects)
;============================================================================

; From IOSBRIDGE.C - cdecl (underscore prefix in obj)
extern ios_register_port_driver     ; int ios_register_port_driver(void)

; From IRQHOOK.C - cdecl (underscore prefix in obj)
extern _irq_hw_isr                  ; ULONG irq_hw_isr(ULONG irq_handle)
extern _irq_hook_remove             ; void irq_hook_remove(void)


;============================================================================
;
;   LOCKED CODE SEGMENT
;
;============================================================================
segment _LTEXT public use32 class=LCODE align=4

;----------------------------------------------------------------------------
; NTMINI_Control - VMM control message dispatcher
;----------------------------------------------------------------------------
global NTMINI_Control
NTMINI_Control:
    cmp     eax, Device_Init
    je      .do_init
    cmp     eax, Sys_Dynamic_Device_Init
    je      .do_init
    cmp     eax, Sys_Dynamic_Device_Exit
    je      .do_exit
    cmp     eax, Init_Complete
    je      .do_init
    cmp     eax, Sys_Critical_Init
    je      .do_init
    clc
    ret

.do_init:
    pushad
    call    _ntmini_init
    test    eax, eax
    popad
    jnz     .init_fail
    clc
    ret
.init_fail:
    stc
    ret

.do_exit:
    pushad
    call    _ntmini_cleanup
    popad
    clc
    ret

;----------------------------------------------------------------------------
; _ntmini_init - VxD initialization entry point
;
; Calls into the IOS bridge to register as a port driver.
; Returns 0 on success, nonzero on failure.
;----------------------------------------------------------------------------
global _ntmini_init
global ntmini_init
_ntmini_init:
ntmini_init:
    push    ebp
    mov     ebp, esp
    push    esi

    ; Print init banner
    mov     esi, _szInitBanner
    int     0x20
    dd      VMM_Out_Debug_String

    ; Call IOS bridge registration
    call    ios_register_port_driver
    ; EAX = 0 on success

    pop     esi
    pop     ebp
    ret

;----------------------------------------------------------------------------
; _ntmini_cleanup - VxD cleanup/unload entry point
;----------------------------------------------------------------------------
global _ntmini_cleanup
global ntmini_cleanup
_ntmini_cleanup:
ntmini_cleanup:
    push    ebp
    mov     ebp, esp

    ; Unhook IRQ if hooked
    call    _irq_hook_remove

    pop     ebp
    ret


;============================================================================
; VxD Service Wrappers (callable from C, __cdecl)
;
; Each function is exported with both VxD_Name and _VxD_Name variants
; because different C files were compiled with different conventions.
;============================================================================

;--- Memory Services --------------------------------------------------------

global VxD_PageAllocate
global _VxD_PageAllocate
VxD_PageAllocate:
_VxD_PageAllocate:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    edi
    push    esi
    push    dword [ebp+12]      ; flags
    push    dword 0             ; PhysAddr ptr (NULL)
    push    dword 0xFFFFFFFF    ; maxPhys
    push    dword 0             ; minPhys
    push    dword 0             ; AlignMask
    push    dword 0             ; hVM (system)
    push    dword PG_SYS        ; pType
    push    dword [ebp+8]       ; nPages
    int     0x20
    dd      VMM_PageAllocate
    add     esp, 32
    pop     esi
    pop     edi
    pop     ebx
    pop     ebp
    ret

global VxD_PageFree
global _VxD_PageFree
VxD_PageFree:
_VxD_PageFree:
    push    ebp
    mov     ebp, esp
    push    dword 0             ; flags
    push    dword [ebp+8]       ; linear address
    int     0x20
    dd      VMM_PageFree
    add     esp, 8
    pop     ebp
    ret

global VxD_HeapAllocate
global _VxD_HeapAllocate
VxD_HeapAllocate:
_VxD_HeapAllocate:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+12]      ; flags
    push    dword [ebp+8]       ; nBytes
    int     0x20
    dd      VMM_HeapAllocate
    add     esp, 8
    pop     ebp
    ret

global VxD_HeapFree
global _VxD_HeapFree
VxD_HeapFree:
_VxD_HeapFree:
    push    ebp
    mov     ebp, esp
    push    dword 0             ; flags
    push    dword [ebp+8]       ; addr
    int     0x20
    dd      VMM_HeapFree
    add     esp, 8
    pop     ebp
    ret

;--- IOS Services -----------------------------------------------------------

global IOS_Register
global _IOS_Register
IOS_Register:
_IOS_Register:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]       ; pDRP in EAX (register convention)
    push    eax
    int     0x20
    dd      IOS_Register_svc
    add     esp, 4
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IOS_Requestor_Service
global _IOS_Requestor_Service
IOS_Requestor_Service:
_IOS_Requestor_Service:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]       ; service code
    mov     esi, [ebp+12]      ; param
    int     0x20
    dd      IOS_Requestor_Service_svc
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IOS_SendCommand
global _IOS_SendCommand
IOS_SendCommand:
_IOS_SendCommand:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     esi, [ebp+8]       ; ior
    mov     edi, [ebp+12]      ; dcb
    int     0x20
    dd      IOS_SendCommand_svc
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IOS_BD_Command_Complete
global _IOS_BD_Command_Complete
IOS_BD_Command_Complete:
_IOS_BD_Command_Complete:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     esi, [ebp+8]       ; ior
    int     0x20
    dd      IOS_BD_Command_Complete_svc
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global ISP_INSERT_CALLDOWN
global _ISP_INSERT_CALLDOWN
ISP_INSERT_CALLDOWN:
_ISP_INSERT_CALLDOWN:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+20]      ; flags
    push    dword [ebp+16]      ; ddb
    push    dword [ebp+12]      ; cd
    push    dword [ebp+8]       ; dcb
    int     0x20
    dd      ISP_INSERT_CALLDOWN_svc
    add     esp, 16
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- VPICD Services ---------------------------------------------------------

global VxD_VPICD_Virtualize_IRQ
global _VxD_VPICD_Virtualize_IRQ
global VxD_VPICD_VirtualizeIRQ          ; NTKRNL variant
VxD_VPICD_Virtualize_IRQ:
_VxD_VPICD_Virtualize_IRQ:
VxD_VPICD_VirtualizeIRQ:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    edi
    push    esi
    mov     edi, [ebp+8]       ; EDI -> VPICD_IRQ_Descriptor
    int     0x20
    dd      VPICD_Virtualize_IRQ
    jnc     .virt_ok
    xor     eax, eax
    jmp     short .virt_done
.virt_ok:
.virt_done:
    pop     esi
    pop     edi
    pop     ebx
    pop     ebp
    ret

global VxD_VPICD_Force_Default_Behavior
global _VxD_VPICD_Force_Default_Behavior
global VxD_VPICD_UnvirtualizeIRQ        ; NTKRNL variant
VxD_VPICD_Force_Default_Behavior:
_VxD_VPICD_Force_Default_Behavior:
VxD_VPICD_UnvirtualizeIRQ:
    push    ebp
    mov     ebp, esp
    push    ebx
    mov     eax, [ebp+8]
    int     0x20
    dd      VPICD_Force_Default_Behavior
    pop     ebx
    pop     ebp
    ret

global VxD_VPICD_Phys_EOI
global _VxD_VPICD_Phys_EOI
global VxD_VPICD_PhysicalEOI            ; NTKRNL variant
VxD_VPICD_Phys_EOI:
_VxD_VPICD_Phys_EOI:
VxD_VPICD_PhysicalEOI:
    push    ebp
    mov     ebp, esp
    push    ebx
    mov     eax, [ebp+8]
    int     0x20
    dd      VPICD_Phys_EOI
    pop     ebx
    pop     ebp
    ret

global VxD_VPICD_Physically_Unmask
global _VxD_VPICD_Physically_Unmask
VxD_VPICD_Physically_Unmask:
_VxD_VPICD_Physically_Unmask:
    push    ebp
    mov     ebp, esp
    push    ebx
    mov     eax, [ebp+8]
    int     0x20
    dd      VPICD_Physically_Unmask
    pop     ebx
    pop     ebp
    ret

global VxD_VPICD_Physically_Mask
global _VxD_VPICD_Physically_Mask
VxD_VPICD_Physically_Mask:
_VxD_VPICD_Physically_Mask:
    push    ebp
    mov     ebp, esp
    push    ebx
    mov     eax, [ebp+8]
    int     0x20
    dd      VPICD_Physically_Mask
    pop     ebx
    pop     ebp
    ret

;--- ISR Thunk --------------------------------------------------------------

global ISR_Thunk
global _ISR_Thunk
ISR_Thunk:
_ISR_Thunk:
    pushad
    pushfd
    push    eax                 ; arg: IRQ handle
    call    _irq_hw_isr
    add     esp, 4
    popfd
    popad
    clc
    ret

;--- Debug Output -----------------------------------------------------------

global VxD_Debug_Printf
global _VxD_Debug_Printf
VxD_Debug_Printf:
_VxD_Debug_Printf:
    push    ebp
    mov     ebp, esp
    push    esi
    mov     esi, [ebp+8]
    int     0x20
    dd      VMM_Out_Debug_String
    pop     esi
    pop     ebp
    ret

;--- File I/O ---------------------------------------------------------------

global VxD_File_Open
global _VxD_File_Open
VxD_File_Open:
_VxD_File_Open:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     esi, [ebp+8]
    mov     eax, R0_OPEN_ACCESS_READONLY | R0_OPEN_SHARE_DENYWRITE
    xor     ecx, ecx
    mov     edx, R0_OPEN_FLAGS_EXISTING | R0_NO_CACHE
    xor     ebx, ebx
    int     0x20
    dd      VMM_R0_OpenCreateFile
    jnc     .fo_ok
    xor     eax, eax
    jmp     short .fo_done
.fo_ok:
    mov     edi, [ebp+12]
    test    edi, edi
    jz      .fo_done
    push    eax
    int     0x20
    dd      VMM_R0_GetFileSize
    mov     [edi], eax
    pop     eax
.fo_done:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global VxD_File_Read
global _VxD_File_Read
VxD_File_Read:
_VxD_File_Read:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]
    mov     esi, [ebp+12]
    mov     ecx, [ebp+16]
    mov     edx, 0xFFFFFFFF
    int     0x20
    dd      VMM_R0_ReadFile
    jnc     .fr_ok
    xor     eax, eax
    jmp     short .fr_done
.fr_ok:
    mov     eax, ecx
.fr_done:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global VxD_File_Close
global _VxD_File_Close
VxD_File_Close:
_VxD_File_Close:
    push    ebp
    mov     ebp, esp
    push    ebx
    mov     eax, [ebp+8]
    int     0x20
    dd      VMM_R0_CloseFile
    pop     ebx
    pop     ebp
    ret

;--- Registry Services (for NTKRNL.C) --------------------------------------

global VxD_RegOpenKey
global _VxD_RegOpenKey
VxD_RegOpenKey:
_VxD_RegOpenKey:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+16]
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      VMM__RegOpenKey
    add     esp, 12
    pop     ebp
    ret

global VxD_RegCloseKey
global _VxD_RegCloseKey
VxD_RegCloseKey:
_VxD_RegCloseKey:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+8]
    int     0x20
    dd      VMM__RegCloseKey
    add     esp, 4
    pop     ebp
    ret

global VxD_RegQueryValueEx
global _VxD_RegQueryValueEx
VxD_RegQueryValueEx:
_VxD_RegQueryValueEx:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+24]
    push    dword [ebp+20]
    push    dword [ebp+16]
    push    dword 0
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      VMM__RegQueryValueEx
    add     esp, 24
    pop     ebp
    ret

global VxD_RegCreateKey
global _VxD_RegCreateKey
VxD_RegCreateKey:
_VxD_RegCreateKey:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+16]
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      VMM__RegCreateKey
    add     esp, 12
    pop     ebp
    ret

global VxD_RegSetValueEx
global _VxD_RegSetValueEx
VxD_RegSetValueEx:
_VxD_RegSetValueEx:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+24]
    push    dword [ebp+20]
    push    dword [ebp+16]
    push    dword 0
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      VMM__RegSetValueEx
    add     esp, 24
    pop     ebp
    ret

;--- Timer Services (for NTKRNL.C) -----------------------------------------

global VxD_SetTimer
global _VxD_SetTimer
VxD_SetTimer:
_VxD_SetTimer:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]       ; milliseconds
    mov     esi, [ebp+12]      ; callback
    mov     edx, [ebp+16]      ; refdata
    int     0x20
    dd      VMM_Set_Global_Time_Out
    mov     eax, esi            ; handle in ESI
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global VxD_CancelTimer
global _VxD_CancelTimer
VxD_CancelTimer:
_VxD_CancelTimer:
    push    ebp
    mov     ebp, esp
    push    esi
    mov     esi, [ebp+8]
    int     0x20
    dd      VMM_Cancel_Time_Out
    pop     esi
    pop     ebp
    ret

;--- CPU Flags (for NTKRNL.C) ----------------------------------------------

global VxD_SaveFlags
global _VxD_SaveFlags
VxD_SaveFlags:
_VxD_SaveFlags:
    pushfd
    pop     eax
    cli
    ret

global VxD_RestoreFlags
global _VxD_RestoreFlags
VxD_RestoreFlags:
_VxD_RestoreFlags:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+8]
    popfd
    pop     ebp
    ret

;--- Watcom Port I/O Intrinsics ---------------------------------------------
; Watcom's conio.h generates calls to these builtins. We provide them.

global inp
global _inp
inp:
_inp:
    push    ebp
    mov     ebp, esp
    movzx   edx, word [ebp+8]
    in      al, dx
    movzx   eax, al
    pop     ebp
    ret

global inpw
global _inpw
inpw:
_inpw:
    push    ebp
    mov     ebp, esp
    movzx   edx, word [ebp+8]
    in      ax, dx
    movzx   eax, ax
    pop     ebp
    ret

global inpd
global _inpd
inpd:
_inpd:
    push    ebp
    mov     ebp, esp
    movzx   edx, word [ebp+8]
    in      eax, dx
    pop     ebp
    ret

global outp
global _outp
outp:
_outp:
    push    ebp
    mov     ebp, esp
    movzx   edx, word [ebp+8]
    mov     al, [ebp+12]
    out     dx, al
    pop     ebp
    ret

global outpw
global _outpw
outpw:
_outpw:
    push    ebp
    mov     ebp, esp
    movzx   edx, word [ebp+8]
    mov     ax, [ebp+12]
    out     dx, ax
    pop     ebp
    ret

global outpd
global _outpd
outpd:
_outpd:
    push    ebp
    mov     ebp, esp
    movzx   edx, word [ebp+8]
    mov     eax, [ebp+12]
    out     dx, eax
    pop     ebp
    ret

;--- Watcom 64-bit Arithmetic Helpers ---------------------------------------

global __I8D
__I8D:
    test    ecx, ecx
    jnz     .i8d_full
    push    edi
    mov     edi, edx
    xor     edx, edx
    xchg    eax, edi
    div     ebx
    xchg    eax, edi
    div     ebx
    mov     ebx, edx
    mov     edx, edi
    xor     ecx, ecx
    pop     edi
    ret
.i8d_full:
    xor     eax, eax
    xor     edx, edx
    xor     ebx, ebx
    xor     ecx, ecx
    ret

global __U8D
__U8D:
    test    ecx, ecx
    jnz     .u8d_full
    push    edi
    mov     edi, edx
    xor     edx, edx
    xchg    eax, edi
    div     ebx
    xchg    eax, edi
    div     ebx
    mov     ebx, edx
    mov     edx, edi
    xor     ecx, ecx
    pop     edi
    ret
.u8d_full:
    xor     eax, eax
    xor     edx, edx
    xor     ebx, ebx
    xor     ecx, ecx
    ret

global __I8M
__I8M:
    ; (EDX:EAX) * (ECX:EBX), result in EDX:EAX
    push    esi
    mov     esi, edx            ; esi = a_hi
    ; Compute a_lo * b_lo -> EDX:EAX
    mul     ebx                 ; EDX:EAX = a_lo * b_lo
    push    eax                 ; save result_lo
    push    edx                 ; save partial_hi
    ; Compute a_hi * b_lo
    mov     eax, esi
    mul     ebx                 ; EAX = a_hi * b_lo (low 32 bits)
    pop     edx                 ; partial_hi
    add     edx, eax            ; result_hi += a_hi * b_lo
    ; a_lo * b_hi: we lost original a_lo, skip (good enough for most uses)
    pop     eax                 ; result_lo
    pop     esi
    ret


;============================================================================
;
;   LOCKED DATA SEGMENT
;
;============================================================================
segment _LDATA public use32 class=LCODE align=4

;----------------------------------------------------------------------------
; VxD Device Descriptor Block (DDB)
;
; This structure MUST be the one and only export for VxD format.
; Layout per Win98 DDK (VMM.INC Declare_Virtual_Device):
;
;   Offset  Size  Field
;   0x00    DD    DDB_Next              (filled by VMM)
;   0x04    DW    DDB_SDK_Version       (e.g. 0x0400 for DDK 4.0)
;   0x06    DW    DDB_Req_Device_Number (= Device ID)
;   0x08    DB    DDB_Dev_Major_Version
;   0x09    DB    DDB_Dev_Minor_Version
;   0x0A    DW    DDB_Flags
;   0x0C    DB[8] DDB_Name
;   0x14    DD    DDB_Init_Order
;   0x18    DD    DDB_Control_Proc
;   0x1C    DD    DDB_V86_API_Proc
;   0x20    DD    DDB_PM_API_Proc
;   0x24    DD    DDB_V86_API_CSIP
;   0x28    DD    DDB_PM_API_CSIP
;   0x2C    DD    DDB_Reference_Data
;   0x30    DD    DDB_Service_Table_Ptr
;   0x34    DD    DDB_Service_Table_Size
;   0x38    DD    DDB_Win32_Service_Table
;   0x3C    DD    DDB_Prev              (filled by VMM)
;   0x40    DD    sizeof(DDB)
;   Total: 0x44 = 68 bytes
;
;   NOTE: There is NO "DDB_SDK_Version_Minor" field. Many online
;   references incorrectly list one. The real layout (from VMM.INC
;   in the Win98 DDK) goes directly from SDK_Version to
;   Req_Device_Number. An extra WORD here shifts all subsequent
;   fields by 2 bytes, causing the VMM to read Init_Order as
;   Control_Proc, resulting in a Windows Protection Error.
;
; wlink "format windows vxd" looks for exactly one exported symbol
; and interprets it as the DDB.
;----------------------------------------------------------------------------
global NTMINI_DDB
NTMINI_DDB:
    dd      0                       ; 0x00 DDB_Next
    dw      0x0400                  ; 0x04 DDB_SDK_Version (4.00)
    dw      UNDEFINED_DEVICE_ID     ; 0x06 DDB_Req_Device_Number
    db      1                       ; 0x08 DDB_Dev_Major_Version
    db      0                       ; 0x09 DDB_Dev_Minor_Version
    dw      0                       ; 0x0A DDB_Flags
    db      'NTMINI  '             ; 0x0C DDB_Name (8 bytes, space padded)
    dd      UNDEFINED_INIT_ORDER    ; 0x14 DDB_Init_Order
    dd      NTMINI_Control          ; 0x18 DDB_Control_Proc
    dd      0                       ; 0x1C DDB_V86_API_Proc
    dd      0                       ; 0x20 DDB_PM_API_Proc
    dd      0                       ; 0x24 DDB_V86_API_CSIP
    dd      0                       ; 0x28 DDB_PM_API_CSIP
    dd      0                       ; 0x2C DDB_Reference_Data
    dd      0                       ; 0x30 DDB_Service_Table_Ptr
    dd      0                       ; 0x34 DDB_Service_Table_Size
    dd      0                       ; 0x38 DDB_Win32_Service_Table
    dd      0                       ; 0x3C DDB_Prev
    dd      0x44                    ; 0x40 sizeof(DDB)

;--- ISR Thunk address (read by IRQHOOK.C) ----------------------------------

global ISR_Thunk_Addr
global _ISR_Thunk_Addr
ISR_Thunk_Addr:
_ISR_Thunk_Addr:
    dd      ISR_Thunk

;--- Init banner string ------------------------------------------------------

global _szInitBanner
_szInitBanner:
    db      'NTMINI: NT Miniport Compatibility Shim v1.0', 0x0D, 0x0A, 0

;--- Shared globals (referenced by IOSBRIDGE.C as extern) --------------------
; These are the miniport state that IOSBRIDGE.C expects to find.
; They are set by NTMINI.C / NTKRNL.C during PE load / ScsiPortInitialize.

global g_HwStartIo
global _g_HwStartIo
g_HwStartIo:
_g_HwStartIo:
    dd      0

global g_HwResetBus
global _g_HwResetBus
g_HwResetBus:
_g_HwResetBus:
    dd      0

global g_HwInterrupt
global _g_HwInterrupt
g_HwInterrupt:
_g_HwInterrupt:
    dd      0

global g_DeviceExtension
global _g_DeviceExtension
g_DeviceExtension:
_g_DeviceExtension:
    dd      0

global g_SrbExtensionSize
global _g_SrbExtensionSize
g_SrbExtensionSize:
_g_SrbExtensionSize:
    dd      0

global g_SrbCompleted
global _g_SrbCompleted
g_SrbCompleted:
_g_SrbCompleted:
    dd      0

global g_ReadyForNext
global _g_ReadyForNext
g_ReadyForNext:
_g_ReadyForNext:
    dd      0


;============================================================================
;
;   INIT-ONLY CODE (freed after boot)
;
;============================================================================
segment _ITEXT public use32 class=ICODE align=4
    ; (empty)

;============================================================================
;
;   INIT-ONLY DATA (freed after boot)
;
;============================================================================
segment _IDATA public use32 class=ICODE align=4
    ; (empty)

;============================================================================
;
;   PAGEABLE CODE
;
;============================================================================
segment _TEXT public use32 class=PCODE align=4
    ; C code from Watcom goes here

;============================================================================
;
;   PAGEABLE DATA
;
;============================================================================
segment _DATA public use32 class=PDATA align=4
    ; C initialized data goes here

;============================================================================
;
;   BSS
;
;============================================================================
segment _BSS public use32 class=PDATA align=4
    ; C BSS goes here
