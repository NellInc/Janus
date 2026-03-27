;============================================================================
;
;   VXDWRAP_V1.ASM - Version 1: VxD shell + debug print
;
;   Stripped-down version of VXDWRAP_NASM.ASM containing:
;     - The fixed DDB (no extra SDK_Version_Minor word)
;     - The control procedure dispatching to _ntmini_init / _ntmini_cleanup
;     - VxD_Debug_Printf wrapper (Out_Debug_String service)
;
;   Nothing else: no memory, no file I/O, no VPICD, no IOS, no ISR thunks.
;   Link with NTMINI_V1.C to produce a VxD that loads and prints a message.
;
;   Assembler: NASM 2.x+ with -f obj (OMF output)
;   Linker:    Open Watcom wlink, "format windows vxd"
;
;============================================================================

[BITS 32]

;============================================================================
; VxD Service IDs
;============================================================================
%define VMM_Out_Debug_String    0x0001001D

; VxD control messages
%define Sys_Critical_Init           0x0000
%define Device_Init                 0x0001
%define Init_Complete               0x0002
%define Sys_Dynamic_Device_Init     0x001B
%define Sys_Dynamic_Device_Exit     0x001C

; DDB constants
%define UNDEFINED_DEVICE_ID     0x0000
%define UNDEFINED_INIT_ORDER    0x80000000


;============================================================================
; External C functions (from NTMINI_V1.C, Watcom cdecl)
;============================================================================
extern _ntmini_init
extern _ntmini_cleanup


;============================================================================
;
;   LOCKED CODE SEGMENT
;
;============================================================================
segment _LTEXT public use32 class=LCODE align=4

;----------------------------------------------------------------------------
; NTMINI_Control - VMM control message dispatcher
;
; On entry: EAX = control message
; Return:   CF clear = success, CF set = failure
;----------------------------------------------------------------------------
global NTMINI_Control
NTMINI_Control:
    cmp     eax, Device_Init
    je      .do_init
    cmp     eax, Sys_Dynamic_Device_Init
    je      .do_init
    cmp     eax, Init_Complete
    je      .do_init_complete
    cmp     eax, Sys_Critical_Init
    je      .do_init
    cmp     eax, Sys_Dynamic_Device_Exit
    je      .do_exit
    ; Unknown message - return success (CF clear)
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

.do_init_complete:
    ; Just succeed
    clc
    ret

.do_exit:
    pushad
    call    _ntmini_cleanup
    popad
    clc
    ret


;============================================================================
; VxD Service Wrappers
;============================================================================

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


;============================================================================
;
;   LOCKED DATA SEGMENT
;
;============================================================================
segment _LDATA public use32 class=LCODE align=4

;----------------------------------------------------------------------------
; VxD Device Descriptor Block (DDB)
;
; Total: 0x44 = 68 bytes.
;
; IMPORTANT: NO separate "SDK minor version" field. DDB_SDK_Version is a
; single WORD at +04, followed immediately by DDB_Req_Device_Number at +06.
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
