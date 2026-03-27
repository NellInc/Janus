;============================================================================
;
;   VXDWRAP_V3.ASM - Version 3: VxD shell + debug + heap + file I/O + PageAlloc
;
;   Based on VXDWRAP_V2.ASM. Adds:
;     - _VxD_File_Create (R0_OpenCreateFile with create/truncate)
;     - _VxD_File_Write (R0_WriteFile)
;     - _VxD_PageAllocate / _VxD_PageFree (VMM page allocation)
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
%define VMM_HeapAllocate        0x00010256
%define VMM_HeapFree            0x00010257
%define VMM_R0_OpenCreateFile   0x000102DE
%define VMM_R0_ReadFile         0x000102E1
%define VMM_R0_WriteFile        0x000102E2
%define VMM_R0_CloseFile        0x000102E0
%define VMM__PageAllocate       0x00010053
%define VMM__PageFree           0x00010054

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
; External C functions (from NTMINI_V3.C, Watcom cdecl)
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
    ; Write "V3!" to QEMU debug port 0xE9 BEFORE calling any C code
    push    edx
    push    eax
    mov     dx, 0x402
    mov     al, 'V'
    out     dx, al
    mov     al, '3'
    out     dx, al
    mov     al, '!'
    out     dx, al
    mov     al, 0x0A
    out     dx, al
    pop     eax
    pop     edx

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
; void VxD_Debug_Printf(const char *msg)
; Writes each character to QEMU debug port 0xE9 (captured by -debugcon)
; Also calls Out_Debug_String for kernel debugger

global VxD_Debug_Printf
global _VxD_Debug_Printf
VxD_Debug_Printf:
_VxD_Debug_Printf:
    push    ebp
    mov     ebp, esp
    push    esi
    push    edx
    push    eax

    ; Write to QEMU debug port 0xE9
    mov     esi, [ebp+8]
    mov     dx, 0x402
.dbg_loop:
    lodsb
    test    al, al
    jz      .dbg_done
    out     dx, al
    jmp     .dbg_loop
.dbg_done:

    ; Also call Out_Debug_String for kernel debugger
    mov     esi, [ebp+8]
    int     0x20
    dd      VMM_Out_Debug_String

    pop     eax
    pop     edx
    pop     esi
    pop     ebp
    ret


;--- Heap Allocate ----------------------------------------------------------
; void *VxD_HeapAllocate(unsigned long size, unsigned long flags)
; _HeapAllocate: push flags, push size, INT 20h
; Returns pointer in EAX, or 0 on failure

global VxD_HeapAllocate
global _VxD_HeapAllocate
VxD_HeapAllocate:
_VxD_HeapAllocate:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+12]      ; flags
    push    dword [ebp+8]       ; size
    int     0x20
    dd      VMM_HeapAllocate
    add     esp, 8
    pop     ebp
    ret


;--- Heap Free --------------------------------------------------------------
; void VxD_HeapFree(void *ptr, unsigned long flags)
; _HeapFree: push flags, push ptr, INT 20h

global VxD_HeapFree
global _VxD_HeapFree
VxD_HeapFree:
_VxD_HeapFree:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+12]      ; flags
    push    dword [ebp+8]       ; ptr
    int     0x20
    dd      VMM_HeapFree
    add     esp, 8
    pop     ebp
    ret


;--- File Open (read existing) ---------------------------------------------
; int VxD_File_Open(const char *filename)
; R0_OpenCreateFile(pFilename, access, sharemode, action, attrs, flags, hContext)
; Returns file handle in EAX, or -1 on error (CF set)
;
; We use: access=0 (read), share=0, action=1 (open existing),
;         attrs=0, flags=0, hContext=0

global VxD_File_Open
global _VxD_File_Open
VxD_File_Open:
_VxD_File_Open:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    ecx
    push    edx

    push    dword 0             ; hContext
    push    dword 0             ; flags
    push    dword 0             ; attributes
    push    dword 1             ; action = OPEN_EXISTING
    push    dword 0             ; share mode
    push    dword 0             ; access mode (read)
    push    dword [ebp+8]      ; pFilename
    int     0x20
    dd      VMM_R0_OpenCreateFile
    add     esp, 28
    jc      .open_fail

    ; EAX = file handle
    pop     edx
    pop     ecx
    pop     ebx
    pop     ebp
    ret

.open_fail:
    mov     eax, -1
    pop     edx
    pop     ecx
    pop     ebx
    pop     ebp
    ret


;--- File Create (create/truncate for writing) ------------------------------
; int VxD_File_Create(const char *filename)
; R0_OpenCreateFile(pFilename, access, sharemode, action, attrs, flags, hContext)
; Returns file handle in EAX, or -1 on error (CF set)
;
; We use: access=2 (read/write), share=0, action=0x01 (open existing),
;         attrs=0x20 (archive), flags=0, hContext=0
; File must already exist on disk (pre-created by deploy script)

global VxD_File_Create
global _VxD_File_Create
VxD_File_Create:
_VxD_File_Create:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    ecx
    push    edx

    push    dword 0             ; hContext
    push    dword 0             ; flags
    push    dword 0x20          ; attributes (archive)
    push    dword 1             ; action = OPEN_EXISTING
    push    dword 0             ; share mode
    push    dword 2             ; access mode (read/write)
    push    dword [ebp+8]      ; pFilename
    int     0x20
    dd      VMM_R0_OpenCreateFile
    add     esp, 28
    jc      .create_fail

    ; EAX = file handle
    pop     edx
    pop     ecx
    pop     ebx
    pop     ebp
    ret

.create_fail:
    mov     eax, -1
    pop     edx
    pop     ecx
    pop     ebx
    pop     ebp
    ret


;--- File Read --------------------------------------------------------------
; int VxD_File_Read(int handle, void *buffer, int count)
; R0_ReadFile(bBlockAlign, handle, buffer, count, offset_lo, offset_hi, pBytesRead, hContext)
; Returns bytes read, or -1 on error

global VxD_File_Read
global _VxD_File_Read
VxD_File_Read:
_VxD_File_Read:
    push    ebp
    mov     ebp, esp
    sub     esp, 4              ; local var for bytes_read at [ebp-4]
    push    ebx
    push    ecx
    push    edx

    mov     dword [ebp-4], 0   ; initialize bytes_read

    lea     eax, [ebp-4]       ; &bytes_read

    push    dword 0             ; hContext
    push    eax                 ; pBytesRead
    push    dword 0             ; offset_hi
    push    dword 0xFFFFFFFF    ; offset_lo = current position
    push    dword [ebp+16]     ; count (cbRead)
    push    dword [ebp+12]     ; buffer (pBuffer)
    push    dword [ebp+8]      ; handle
    push    dword 0             ; bBlockAlign = FALSE
    int     0x20
    dd      VMM_R0_ReadFile
    add     esp, 32
    jc      .read_fail

    mov     eax, [ebp-4]       ; return bytes_read
    pop     edx
    pop     ecx
    pop     ebx
    add     esp, 4
    pop     ebp
    ret

.read_fail:
    mov     eax, -1
    pop     edx
    pop     ecx
    pop     ebx
    add     esp, 4
    pop     ebp
    ret


;--- File Write -------------------------------------------------------------
; int VxD_File_Write(int handle, const void *buffer, int count)
; R0_WriteFile(bBlockAlign, handle, buffer, count, offset_lo, offset_hi, pBytesWritten, hContext)
; Returns bytes written, or -1 on error

global VxD_File_Write
global _VxD_File_Write
VxD_File_Write:
_VxD_File_Write:
    push    ebp
    mov     ebp, esp
    sub     esp, 4              ; local var for bytes_written at [ebp-4]
    push    ebx
    push    ecx
    push    edx

    mov     dword [ebp-4], 0   ; initialize bytes_written

    lea     eax, [ebp-4]       ; &bytes_written

    push    dword 0             ; hContext
    push    eax                 ; pBytesWritten
    push    dword 0             ; offset_hi
    push    dword 0xFFFFFFFF    ; offset_lo = current position
    push    dword [ebp+16]     ; count (cbWrite)
    push    dword [ebp+12]     ; buffer (pBuffer)
    push    dword [ebp+8]      ; handle
    push    dword 0             ; bBlockAlign = FALSE
    int     0x20
    dd      VMM_R0_WriteFile
    add     esp, 32
    jc      .write_fail

    mov     eax, [ebp-4]       ; return bytes_written
    pop     edx
    pop     ecx
    pop     ebx
    add     esp, 4
    pop     ebp
    ret

.write_fail:
    mov     eax, -1
    pop     edx
    pop     ecx
    pop     ebx
    add     esp, 4
    pop     ebp
    ret


;--- File Close -------------------------------------------------------------
; void VxD_File_Close(int handle)
; R0_CloseFile(handle)

global VxD_File_Close
global _VxD_File_Close
VxD_File_Close:
_VxD_File_Close:
    push    ebp
    mov     ebp, esp
    push    dword [ebp+8]      ; handle
    int     0x20
    dd      VMM_R0_CloseFile
    add     esp, 4
    pop     ebp
    ret


;--- Page Allocate ----------------------------------------------------------
; void *VxD_PageAllocate(unsigned long nPages, unsigned long flags)
; VMM _PageAllocate(nPages, pType, VM, AlignMask, minPhys, maxPhys, PhysAddr, flags)
; We use: pType=2(PG_SYS), VM=0, AlignMask=0, minPhys=0, maxPhys=0, PhysAddr=0
;         flags = caller_flags | 0x28 (PC_FIXED|PC_WIRED)
; Returns linear address in EDX (EAX=phys), we return EDX (linear)

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

    mov     eax, [ebp+12]      ; caller flags
    or      eax, 0x28           ; PC_FIXED(0x08) | PC_WIRED(0x20)

    push    eax                 ; flags
    push    dword 0             ; PhysAddr (0 = don't care)
    push    dword 0             ; maxPhys
    push    dword 0             ; minPhys
    push    dword 0             ; AlignMask
    push    dword 0             ; VM handle (0 = system)
    push    dword 2             ; pType = PG_SYS
    push    dword [ebp+8]      ; nPages
    int     0x20
    dd      VMM__PageAllocate
    add     esp, 32

    ; _PageAllocate returns: EAX=physical addr, EDX=linear addr
    ; On failure both are 0
    mov     eax, edx            ; return linear address

    pop     edi
    pop     esi
    pop     ecx
    pop     ebx
    pop     ebp
    ret


;--- Page Free --------------------------------------------------------------
; void VxD_PageFree(void *addr)
; VMM _PageFree(hMem, flags)
; hMem = linear address, flags = 0

global VxD_PageFree
global _VxD_PageFree
VxD_PageFree:
_VxD_PageFree:
    push    ebp
    mov     ebp, esp
    push    dword 0             ; flags
    push    dword [ebp+8]      ; hMem (linear address)
    int     0x20
    dd      VMM__PageFree
    add     esp, 8
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
;----------------------------------------------------------------------------
global NTMINI_DDB
NTMINI_DDB:
    dd      0                       ; 0x00 DDB_Next
    dw      0x0400                  ; 0x04 DDB_SDK_Version (4.00)
    dw      UNDEFINED_DEVICE_ID     ; 0x06 DDB_Req_Device_Number
    db      3                       ; 0x08 DDB_Dev_Major_Version (V3)
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
;   CODE (in LCODE object to avoid cross-object fixups)
;
;============================================================================
segment _TEXT public use32 class=LCODE align=4
    ; C code from Watcom goes here

;============================================================================
;
;   DATA (in LCODE object to avoid cross-object fixups)
;
;============================================================================
segment _DATA public use32 class=LCODE align=4
    ; C initialized data goes here

;============================================================================
;
;   BSS (in LCODE object)
;
;============================================================================
segment _BSS public use32 class=LCODE align=4
    ; C BSS goes here
