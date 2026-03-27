;============================================================================
;   VXDWRAP_V3_FLAT.ASM - All segments in LCODE, single LE object
;   No ICODE/PCODE segments. Everything locked. No cross-object fixups.
;============================================================================

[BITS 32]

%define VMM_Out_Debug_String    0x0001001D
%define VMM_HeapAllocate        0x00010256
%define VMM_HeapFree            0x00010257
; VMM Boot Log services (available during init)
%define VMM_Open_Boot_Log       0x00010188
%define VMM_Write_Boot_Log      0x00010189
%define VMM_Close_Boot_Log      0x0001018A
; VMM OpenFile (ICODE - init only)
%define VMM_OpenFile            0x000100B8
; IFSMgr_Ring0_FileIO
%define IFSMgr_Ring0_FileIO     0x00400032
%define VMM__PageAllocate       0x00010053
%define VMM__PageFree           0x00010054

%define Sys_Critical_Init           0x0000
%define Device_Init                 0x0001
%define Init_Complete               0x0002
%define Sys_Dynamic_Device_Init     0x001B
%define Sys_Dynamic_Device_Exit     0x001C

%define UNDEFINED_DEVICE_ID     0x0000
%define UNDEFINED_INIT_ORDER    0x80000000

extern _ntmini_init
extern _ntmini_cleanup

;============================================================================
;   ALL CODE - single LCODE segment
;============================================================================
segment _LTEXT public use32 class=LCODE align=4

global NTMINI_Control
NTMINI_Control:
    ; Raw debug port test - write 'V' on every control message
    push    eax
    mov     al, 'V'
    out     0xE9, al
    pop     eax

    cmp     eax, Device_Init
    je      .do_init
    cmp     eax, Sys_Critical_Init
    je      .do_succeed
    cmp     eax, Init_Complete
    je      .do_succeed
    cmp     eax, Sys_Dynamic_Device_Init
    je      .do_init
    cmp     eax, Sys_Dynamic_Device_Exit
    je      .do_exit
    clc
    ret

.do_succeed:
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


;--- Debug Printf ---
global VxD_Debug_Printf
global _VxD_Debug_Printf
VxD_Debug_Printf:
_VxD_Debug_Printf:
    push    ebp
    mov     ebp, esp
    push    esi
    push    edx
    push    eax
    mov     esi, [ebp+8]
    mov     dx, 0xE9
.dbg_loop:
    lodsb
    test    al, al
    jz      .dbg_done
    out     dx, al
    jmp     .dbg_loop
.dbg_done:
    mov     esi, [ebp+8]
    int     0x20
    dd      VMM_Out_Debug_String
    pop     eax
    pop     edx
    pop     esi
    pop     ebp
    ret


;--- Heap Allocate ---
global VxD_HeapAllocate
global _VxD_HeapAllocate
VxD_HeapAllocate:
_VxD_HeapAllocate:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      VMM_HeapAllocate
    add     esp, 8
    pop     ebp
    ret

;--- Heap Free ---
global VxD_HeapFree
global _VxD_HeapFree
VxD_HeapFree:
_VxD_HeapFree:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      VMM_HeapFree
    add     esp, 8
    pop     ebp
    ret

;--- File Open (STUB - returns -1, not available in this VMM) ---
global VxD_File_Open
global _VxD_File_Open
VxD_File_Open:
_VxD_File_Open:
    mov     eax, -1
    ret

;--- File Create (STUB - uses boot log instead) ---
global VxD_File_Create
global _VxD_File_Create
VxD_File_Create:
_VxD_File_Create:
    ; Open the boot log for our debug output
    int     0x20
    dd      VMM_Open_Boot_Log
    jc      .bc_fail
    mov     eax, 1              ; return "handle" = 1 (fake, just means log is open)
    ret
.bc_fail:
    mov     eax, -1
    ret

;--- File Read (STUB) ---
global VxD_File_Read
global _VxD_File_Read
VxD_File_Read:
_VxD_File_Read:
    mov     eax, -1
    ret

;--- File Write (writes to boot log) ---
; VxD_File_Write(handle, buffer, count)
; Write_Boot_Log: ESI = ptr to string (null-terminated)
global VxD_File_Write
global _VxD_File_Write
VxD_File_Write:
_VxD_File_Write:
    push    ebp
    mov     ebp, esp
    push    esi
    mov     esi, [ebp+12]      ; buffer pointer
    int     0x20
    dd      VMM_Write_Boot_Log
    mov     eax, [ebp+16]      ; return count (pretend all written)
    pop     esi
    pop     ebp
    ret

;--- File Close (closes boot log) ---
global VxD_File_Close
global _VxD_File_Close
VxD_File_Close:
_VxD_File_Close:
    int     0x20
    dd      VMM_Close_Boot_Log
    ret

;--- Page Allocate ---
global VxD_PageAllocate
global _VxD_PageAllocate
VxD_PageAllocate:
_VxD_PageAllocate:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    ecx
    push    esi
    push    edi
    mov     eax, [ebp+12]
    or      eax, 0x28
    push    eax
    push    dword 0
    push    dword 0
    push    dword 0
    push    dword 0
    push    dword 0
    push    dword 2
    push    dword [ebp+8]
    int     0x20
    dd      VMM__PageAllocate
    add     esp, 32
    mov     eax, edx
    pop     edi
    pop     esi
    pop     ecx
    pop     ebx
    pop     ebp
    ret

;--- Page Free ---
global VxD_PageFree
global _VxD_PageFree
VxD_PageFree:
_VxD_PageFree:
    push    ebp
    mov     ebp, esp
    push    dword 0
    push    dword [ebp+8]
    int     0x20
    dd      VMM__PageFree
    add     esp, 8
    pop     ebp
    ret


;============================================================================
;   DDB - same LCODE segment
;============================================================================
segment _LDATA public use32 class=LCODE align=4

global NTMINI_DDB
NTMINI_DDB:
    dd      0
    dw      0x0400
    dw      UNDEFINED_DEVICE_ID
    db      3
    db      0
    dw      0
    db      'NTMINI  '
    dd      UNDEFINED_INIT_ORDER
    dd      NTMINI_Control
    dd      0
    dd      0
    dd      0
    dd      0
    dd      0
    dd      0
    dd      0
    dd      0
    dd      0
    dd      0x44

;============================================================================
;   C code and data segments - ALL class=LCODE for single object
;============================================================================
segment _TEXT public use32 class=LCODE align=4
segment _DATA public use32 class=LCODE align=4
segment _BSS public use32 class=LCODE align=4
segment CONST public use32 class=LCODE align=4
segment CONST2 public use32 class=LCODE align=4
segment _TLS public use32 class=LCODE align=4
