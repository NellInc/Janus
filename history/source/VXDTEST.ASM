;============================================================================
; VXDTEST.ASM - Absolute minimal VxD that writes to port 0x402
; No C code. No external dependencies. Pure diagnostic.
;============================================================================

[BITS 32]

%define UNDEFINED_DEVICE_ID     0x0000
%define UNDEFINED_INIT_ORDER    0x80000000
%define Device_Init             0x0001
%define Sys_Critical_Init       0x0000
%define Init_Complete           0x0002

;============================================================================
; LOCKED CODE SEGMENT
;============================================================================
segment _LTEXT public use32 class=LCODE align=4

global NTMINI_Control
NTMINI_Control:
    ; Write "OK" to port 0x402 for ANY message
    push    edx
    push    eax
    mov     dx, 0x402
    mov     al, 'O'
    out     dx, al
    mov     al, 'K'
    out     dx, al
    mov     al, 0x0A
    out     dx, al
    pop     eax
    pop     edx

    cmp     eax, Device_Init
    je      .ok
    cmp     eax, Sys_Critical_Init
    je      .ok
    cmp     eax, Init_Complete
    je      .ok
.ok:
    clc
    ret

;============================================================================
; LOCKED DATA SEGMENT
;============================================================================
segment _LDATA public use32 class=LCODE align=4

global NTMINI_DDB
NTMINI_DDB:
    dd      0                       ; DDB_Next
    dw      0x0400                  ; DDB_SDK_Version
    dw      UNDEFINED_DEVICE_ID     ; DDB_Req_Device_Number
    db      1                       ; DDB_Dev_Major_Version
    db      0                       ; DDB_Dev_Minor_Version
    dw      0                       ; DDB_Flags
    db      'NTMINI  '             ; DDB_Name
    dd      UNDEFINED_INIT_ORDER    ; DDB_Init_Order
    dd      NTMINI_Control          ; DDB_Control_Proc
    dd      0                       ; DDB_V86_API_Proc
    dd      0                       ; DDB_PM_API_Proc
    dd      0                       ; DDB_V86_API_CSIP
    dd      0                       ; DDB_PM_API_CSIP
    dd      0                       ; DDB_Reference_Data
    dd      0                       ; DDB_Service_Table_Ptr
    dd      0                       ; DDB_Service_Table_Size
    dd      0                       ; DDB_Win32_Service_Table
    dd      0                       ; DDB_Prev
    dd      0x44                    ; sizeof(DDB)

;============================================================================
; PAGEABLE segments (empty but needed for wlink)
;============================================================================
segment _ITEXT public use32 class=ICODE align=4
segment _IDATA public use32 class=ICODE align=4
segment _TEXT public use32 class=PCODE align=4
segment _DATA public use32 class=PDATA align=4
segment _BSS public use32 class=PDATA align=4
