;------------------------------------------------------------------------
; VXDWRAP_V4.ASM - Win98 VxD Assembly Wrapper
;
; Provides the VxD entry points, DDB, and service call wrappers
; for the NTMINI CD-ROM file system driver.
;
; Assembler: NASM (nasm -f obj)
; Released under the MIT License. See LICENSE for details.
;------------------------------------------------------------------------

[BITS 32]

; VMM services
%define VMM_Out_Debug_String    0x0001001D
%define VMM_HeapAllocate        0x00010256
%define VMM_HeapFree            0x00010257
%define VMM__PageAllocate       0x00010053
%define VMM__PageFree           0x00010054

; Boot Log services (available during init)
%define VMM_Open_Boot_Log       0x00010188
%define VMM_Write_Boot_Log      0x00010189
%define VMM_Close_Boot_Log      0x0001018A

; V86 interrupt hooking
%define VMM_Hook_V86_Int_Chain  0x00010069
%define VMM_Get_Cur_VM_Handle   0x00010001

; IFSMgr Ring-0 File I/O (cdecl, stack-based)
%define IFSMgr_Ring0_FileIO     0x00400032

; Ring0_FileIO function codes
%define R0_OPENCREATFILE        0xD500
%define R0_READFILE             0xD600
%define R0_WRITEFILE            0xD601
%define R0_CLOSEFILE            0xD700
%define R0_GETFILESIZE          0xD800

; Open flags
%define R0_NO_CACHE             0x0100
%define R0_OPEN_ACCESS_READONLY 0x0000
%define R0_OPEN_SHARE_DENYWRITE 0x0020

; Control messages
%define Sys_Critical_Init       0x0000
%define Device_Init             0x0001
%define Init_Complete           0x0002
%define Sys_VM_Init             0x0003
%define Sys_Dynamic_Device_Init 0x001B
%define Sys_Dynamic_Device_Exit 0x001C

%define UNDEFINED_DEVICE_ID     0x0000
%define UNDEFINED_INIT_ORDER    0x80000000

extern _ntmini_init
extern _ntmini_cleanup
extern _ntmini_mount_cdrom
extern _ntmini_ios_init
extern _ntmini_aep_handler

;============================================================================
;   ALL CODE - single LCODE segment
;   DDB MUST be first: IOS loader requires the DDB in page 0 of the object.
;   If code before the DDB exceeds 4096 bytes, IOS will not load the VxD.
;============================================================================
segment _LTEXT public use32 class=LCODE align=4

;--- DDB at offset 0 so it's always in page 0 ---
global NTMINI_DDB
NTMINI_DDB:
    dd      0                     ; DDB_Next
    dw      0x0400                ; DDB_SDK_Version
    dw      UNDEFINED_DEVICE_ID   ; DDB_Req_Device_Number
    db      3                     ; DDB_Dev_Major_Version
    db      0                     ; DDB_Dev_Minor_Version
    dw      0                     ; DDB_Flags
    db      'NTMINI  '            ; DDB_Name (8 bytes, space-padded)
    dd      UNDEFINED_INIT_ORDER  ; DDB_Init_Order
    dd      NTMINI_Control        ; DDB_Control_Proc
    dd      0                     ; DDB_V86_API_Proc
    dd      0                     ; DDB_PM_API_Proc
    dd      0                     ; DDB_V86_API_CSIP
    dd      0                     ; DDB_PM_API_CSIP
    dd      0                     ; DDB_Reference_Data
    dd      0                     ; DDB_Service_Table_Ptr
    dd      0                     ; DDB_Service_Table_Size
    dd      0                     ; DDB_Win32_Service_Table
    dd      0                     ; DDB_Prev
    dd      0x44                  ; DDB_Size

global NTMINI_Control
NTMINI_Control:
    cmp     eax, Device_Init
    je      .do_init
    cmp     eax, Sys_Critical_Init
    je      .do_serial_only
    cmp     eax, Init_Complete
    je      .do_init_complete
    cmp     eax, Sys_VM_Init
    je      .do_sys_vm_init
    cmp     eax, Sys_Dynamic_Device_Init
    je      .do_dynamic_init
    cmp     eax, Sys_Dynamic_Device_Exit
    je      .do_exit
    clc
    ret

.do_serial_only:
    ; Sys_Critical_Init: init serial port
    pushad
    call    VxD_Serial_Init
    popad
    clc
    ret

.do_dynamic_init:
    ; Sys_Dynamic_Device_Init: loaded by IOS from IOSUBSYS.
    ; Minimal init only (serial + log). Full miniport init is
    ; deferred because it would crash during early IOS phase.
    pushad
    call    VxD_Serial_Init
    popad
    pushad
    extern  _ntmini_dynamic_init
    call    _ntmini_dynamic_init
    popad
    clc
    ret

.do_sys_vm_init:
    ; Sys_VM_Init: deferred timer setup + fallback init path.
    ; Always call deferred timer setup (even if Init_Complete ran).
    ; Set_Global_Time_Out needs system VM context, which Sys_VM_Init provides.
    extern  _ntmini_deferred_timer_setup
    pushad
    call    _ntmini_deferred_timer_setup
    popad
    ; Check if Init_Complete already ran (g_init_complete_ran flag)
    cmp     byte [g_init_complete_ran], 0
    jne     .svm_skip
    ; Init_Complete hasn't run yet, so do the work from here
    mov     byte [g_sys_vm_init_ran], 1
    ; Hook INT 2Fh for MSCDEX CD-ROM access
    pushad
    mov     eax, 0x2F
    mov     esi, mscdex_v86_handler
    int     0x20
    dd      VMM_Hook_V86_Int_Chain
    popad
    ; Attach CDFS mount
    pushad
    call    _ntmini_mount_cdrom
    popad
    ; File I/O test
    pushad
    call    _fileio_test
    popad
.svm_skip:
    clc
    ret

.do_init_complete:
    mov     byte [g_init_complete_ran], 1
    ; Skip work if Sys_VM_Init already did it
    cmp     byte [g_sys_vm_init_ran], 0
    jne     .ic_skip
    ; Hook INT 2Fh for MSCDEX CD-ROM access
    pushad
    mov     eax, 0x2F
    mov     esi, mscdex_v86_handler
    int     0x20
    dd      VMM_Hook_V86_Int_Chain
    popad
    ; Attach CDFS mount
    pushad
    call    _ntmini_mount_cdrom
    popad
    ; File I/O test
    pushad
    call    _fileio_test
    popad
.ic_skip:
    clc
    ret

.do_init:
    ; Device_Init / Sys_Dynamic_Device_Init:
    ; Init serial first (in case we're loaded dynamically by IOS,
    ; where Sys_Critical_Init is never sent)
    pushad
    call    VxD_Serial_Init
    popad
    ; Then do main init (PE load, miniport, IOS registration)
    pushad
    call    _ntmini_init
    popad
    ; Register with IOS early to get the ILB callback table.
    ; Full DCB creation is deferred to Init_Complete.
    pushad
    extern  _ntmini_ios_register_early
    call    _ntmini_ios_register_early
    popad
    ; Always return success so VxD stays loaded for Init_Complete
    clc
    ret

.do_exit:
    pushad
    call    _ntmini_cleanup
    popad
    clc
    ret


;--- COM1 Serial Init ---
; Must be called once before serial output
global VxD_Serial_Init
global _VxD_Serial_Init
VxD_Serial_Init:
_VxD_Serial_Init:
    push    edx
    push    eax
    ; Set DLAB to configure baud rate
    mov     dx, 0x3FB       ; LCR
    mov     al, 0x80        ; DLAB=1
    out     dx, al
    ; 9600 baud (divisor = 12)
    mov     dx, 0x3F8       ; Divisor low
    mov     al, 12
    out     dx, al
    mov     dx, 0x3F9       ; Divisor high
    xor     al, al
    out     dx, al
    ; 8N1, clear DLAB
    mov     dx, 0x3FB       ; LCR
    mov     al, 0x03        ; 8 data bits, no parity, 1 stop bit
    out     dx, al
    ; Enable FIFO
    mov     dx, 0x3FA       ; FCR
    mov     al, 0x07        ; Enable + clear FIFOs
    out     dx, al
    pop     eax
    pop     edx
    ret


;--- Debug Printf (COM1 serial + Out_Debug_String) ---
global VxD_Debug_Printf
global _VxD_Debug_Printf
VxD_Debug_Printf:
_VxD_Debug_Printf:
    push    ebp
    mov     ebp, esp
    push    esi
    push    edx
    push    eax
    ; Write to COM1 serial
    mov     esi, [ebp+8]
.ser_loop:
    lodsb
    test    al, al
    jz      .ser_done
.ser_wait:
    push    eax
    mov     dx, 0x3FD       ; LSR
    in      al, dx
    test    al, 0x20        ; THRE
    pop     eax
    jz      .ser_wait
    mov     dx, 0x3F8       ; THR
    out     dx, al
    jmp     .ser_loop
.ser_done:
    ; Also write to port 0xE9 (debugcon)
    mov     esi, [ebp+8]
.e9_loop:
    lodsb
    test    al, al
    jz      .e9_done
    out     0xE9, al
    push    edx
    mov     dx, 0x402
    out     dx, al
    pop     edx
    jmp     .e9_loop
.e9_done:
    ; Also call Out_Debug_String
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


;========================================================================
;   Ring-0 File I/O via IFSMgr_Ring0_FileIO
;
;   Register-based calling convention:
;   EAX = function code. Other params in EBX/ECX/EDX/ESI.
;   Returns result in EAX. CF set on error.
;========================================================================

;------------------------------------------------------------------------
; int VxD_File_Open(const char *filename)
;
; Opens a file for reading using IFSMgr_Ring0_FileIO.
; Returns file handle (>= 0) or -1 on error.
;
; Register convention:
;   EAX = R0_OPENCREATFILE (0xD500)
;   ESI = pointer to filename
;   EBX = access/share flags
;   ECX = file attributes (0 = normal)
;   EDX = action (1 = open existing)
;   Returns: EAX = file handle, CF set on error
;------------------------------------------------------------------------
global VxD_File_Open
global _VxD_File_Open
VxD_File_Open:
_VxD_File_Open:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi

    ; Register-based calling convention
    mov     esi, [ebp+8]                   ; ESI = filename pointer
    mov     eax, R0_OPENCREATFILE           ; EAX = function code 0xD500
    mov     ebx, R0_OPEN_ACCESS_READONLY | R0_OPEN_SHARE_DENYWRITE | R0_NO_CACHE
    xor     ecx, ecx                        ; ECX = attributes (normal)
    mov     edx, 1                          ; EDX = action: open existing

    int     0x20
    dd      IFSMgr_Ring0_FileIO

    ; Diagnostic: store all register values after call
    mov     [_diag_regs + 0], eax           ; diag_regs[0] = EAX (should be handle)
    mov     [_diag_regs + 4], ebx           ; diag_regs[1] = EBX
    mov     [_diag_regs + 8], ecx           ; diag_regs[2] = ECX
    mov     [_diag_regs + 12], edx          ; diag_regs[3] = EDX

    jc      .open_fail

    ; EAX = file handle from Ring0_FileIO.
    ; Win9x kernel handles can be above 0x80000000 (kernel pointers).
    ; Only reject zero handles, not "negative" ones.
    test    eax, eax
    jz      .open_fail

    jmp     .open_done

.open_fail:
    mov     eax, -1

.open_done:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret


;------------------------------------------------------------------------
; int VxD_File_Create(const char *filename)
;
; Opens the boot log for debug output (same as V3).
;------------------------------------------------------------------------
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


;------------------------------------------------------------------------
; int VxD_File_GetSize(int handle)
;
; Gets file size using IFSMgr_Ring0_FileIO (diagnostic).
; Returns file size or -1 on error.
;
; Register convention:
;   EAX = R0_GETFILESIZE (0xD800)
;   EBX = file handle
;   Returns: EAX = file size, CF set on error
;------------------------------------------------------------------------
global VxD_File_GetSize
global _VxD_File_GetSize
VxD_File_GetSize:
_VxD_File_GetSize:
    push    ebp
    mov     ebp, esp
    push    ebx

    mov     eax, R0_GETFILESIZE             ; EAX = function code 0xD800
    mov     ebx, [ebp+8]                   ; EBX = file handle

    int     0x20
    dd      IFSMgr_Ring0_FileIO

    jc      .gsize_fail
    jmp     .gsize_done

.gsize_fail:
    mov     eax, -1

.gsize_done:
    pop     ebx
    pop     ebp
    ret


;------------------------------------------------------------------------
; int VxD_File_Read(int handle, void *buffer, int count)
;
; Reads from a file using IFSMgr_Ring0_FileIO.
; Returns bytes read, or -1 on error.
;
; Register convention:
;   EAX = R0_READFILE (0xD600)
;   EBX = file handle
;   ECX = byte count to read
;   EDX = file position (0xFFFFFFFF = current position)
;   ESI = pointer to destination buffer
;   Returns: EAX = bytes actually read, CF set on error
;------------------------------------------------------------------------
global VxD_File_Read
global _VxD_File_Read
VxD_File_Read:
_VxD_File_Read:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi

    ; Register-based calling convention
    mov     ebx, [ebp+8]                   ; EBX = file handle
    mov     esi, [ebp+12]                  ; ESI = buffer pointer
    mov     ecx, [ebp+16]                  ; ECX = byte count
    mov     edx, 0                          ; EDX = position 0 (start of file)
    mov     eax, R0_READFILE                ; EAX = function code 0xD600

    int     0x20
    dd      IFSMgr_Ring0_FileIO

    ; Store post-call registers for diagnostic
    mov     [_diag_regs + 0], eax           ; bytes read (EAX convention)
    mov     [_diag_regs + 4], ebx
    mov     [_diag_regs + 8], ecx           ; bytes read (ECX convention)
    mov     [_diag_regs + 12], edx

    jc      .read_fail

    ; EAX = bytes actually read. Fall back to ECX if EAX is zero,
    ; since some Win98 builds return the count in ECX instead.
    test    eax, eax
    jnz     .read_done
    mov     eax, ecx                        ; fallback: try ECX
    jmp     .read_done

.read_fail:
    mov     eax, -1

.read_done:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret


;------------------------------------------------------------------------
; int VxD_File_Write(int handle, void *buffer, int count)
;
; Writes to the boot log (for debug output).
;------------------------------------------------------------------------
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


;------------------------------------------------------------------------
; void VxD_File_Close(int handle)
;
; Closes a file using IFSMgr_Ring0_FileIO (register convention),
; and also closes the boot log.
;------------------------------------------------------------------------
global VxD_File_Close
global _VxD_File_Close
VxD_File_Close:
_VxD_File_Close:
    push    ebp
    mov     ebp, esp
    push    ebx

    ; Close via IFSMgr if handle is a valid kernel pointer (> 1).
    ; Handle 0 is invalid, handle 1 is the boot log.
    mov     ebx, [ebp+8]
    test    ebx, ebx
    jz      .close_done
    cmp     ebx, 1
    je      .close_bootlog

    ; Register convention: EAX = func, EBX = handle
    mov     eax, R0_CLOSEFILE               ; EAX = 0xD700
    ; EBX already set above
    int     0x20
    dd      IFSMgr_Ring0_FileIO
    jmp     .close_done

.close_bootlog:
    ; Close boot log
    int     0x20
    dd      VMM_Close_Boot_Log

.close_done:
    pop     ebx
    pop     ebp
    ret


;========================================================================
;   File I/O test - exercises register-based IFSMgr_Ring0_FileIO
;   Called from Init_Complete when filesystem is ready.
;   Opens MSDOS.SYS, gets size, reads 16 bytes, logs via port 0xE9.
;========================================================================
global _fileio_test
global fileio_test
_fileio_test:
fileio_test:
    pushad

    ; --- Log banner ---
    mov     esi, .msg_banner
    call    .e9_print

    ; --- OPEN: C:\MSDOS.SYS ---
    mov     esi, .test_filename
    mov     eax, R0_OPENCREATFILE
    mov     ebx, R0_OPEN_ACCESS_READONLY | R0_OPEN_SHARE_DENYWRITE
    xor     ecx, ecx                ; attr = normal
    mov     edx, 1                  ; action = open existing
    int     0x20
    dd      IFSMgr_Ring0_FileIO
    jc      .ftest_open_fail

    ; EAX = file handle
    mov     [.ftest_handle], eax
    mov     esi, .msg_open_ok
    call    .e9_print
    mov     eax, [.ftest_handle]
    call    .e9_hex32               ; print handle value
    call    .e9_crlf

    ; --- GETFILESIZE ---
    mov     eax, R0_GETFILESIZE
    mov     ebx, [.ftest_handle]
    int     0x20
    dd      IFSMgr_Ring0_FileIO
    jc      .ftest_size_fail

    mov     [.ftest_size], eax
    mov     esi, .msg_size
    call    .e9_print
    mov     eax, [.ftest_size]
    call    .e9_hex32
    call    .e9_crlf

    ; --- READ 16 bytes from offset 0 ---
    mov     eax, R0_READFILE
    mov     ebx, [.ftest_handle]
    mov     ecx, 16                 ; read 16 bytes
    xor     edx, edx                ; from offset 0
    mov     esi, .ftest_buf
    int     0x20
    dd      IFSMgr_Ring0_FileIO
    jc      .ftest_read_fail

    ; EAX = bytes read
    mov     [.ftest_nread], eax
    mov     esi, .msg_read
    call    .e9_print
    mov     eax, [.ftest_nread]
    call    .e9_hex32
    call    .e9_crlf

    ; Dump first 16 bytes of buffer
    mov     esi, .msg_data
    call    .e9_print
    mov     ecx, 16
    mov     esi, .ftest_buf
.ftest_dump:
    movzx   eax, byte [esi]
    call    .e9_hex8
    mov     al, ' '
    out     0xE9, al
    inc     esi
    loop    .ftest_dump
    call    .e9_crlf

    ; --- CLOSE ---
    mov     eax, R0_CLOSEFILE
    mov     ebx, [.ftest_handle]
    int     0x20
    dd      IFSMgr_Ring0_FileIO

    mov     esi, .msg_done
    call    .e9_print
    jmp     .ftest_exit

.ftest_open_fail:
    mov     esi, .msg_open_fail
    call    .e9_print
    jmp     .ftest_exit

.ftest_size_fail:
    mov     esi, .msg_size_fail
    call    .e9_print
    ; still close
    mov     eax, R0_CLOSEFILE
    mov     ebx, [.ftest_handle]
    int     0x20
    dd      IFSMgr_Ring0_FileIO
    jmp     .ftest_exit

.ftest_read_fail:
    mov     esi, .msg_read_fail
    call    .e9_print
    ; Log EAX for error info
    call    .e9_hex32
    call    .e9_crlf
    mov     eax, R0_CLOSEFILE
    mov     ebx, [.ftest_handle]
    int     0x20
    dd      IFSMgr_Ring0_FileIO
    jmp     .ftest_exit

.ftest_exit:
    popad
    ret

;--- Helper: print NUL-terminated string to port 0xE9 ---
.e9_print:
    push    eax
.e9p_loop:
    lodsb
    test    al, al
    jz      .e9p_done
    out     0xE9, al
    jmp     .e9p_loop
.e9p_done:
    pop     eax
    ret

;--- Helper: print EAX as 8-digit hex to port 0xE9 ---
.e9_hex32:
    push    ecx
    push    eax
    mov     ecx, 8
.h32_loop:
    rol     eax, 4
    push    eax
    and     al, 0x0F
    add     al, '0'
    cmp     al, '9'
    jle     .h32_digit
    add     al, 7               ; 'A'-'9'-1
.h32_digit:
    out     0xE9, al
    pop     eax
    loop    .h32_loop
    pop     eax
    pop     ecx
    ret

;--- Helper: print AL as 2-digit hex to port 0xE9 ---
.e9_hex8:
    push    eax
    push    ecx
    mov     cl, al
    shr     al, 4
    and     al, 0x0F
    add     al, '0'
    cmp     al, '9'
    jle     .h8_hi
    add     al, 7
.h8_hi:
    out     0xE9, al
    mov     al, cl
    and     al, 0x0F
    add     al, '0'
    cmp     al, '9'
    jle     .h8_lo
    add     al, 7
.h8_lo:
    out     0xE9, al
    pop     ecx
    pop     eax
    ret

;--- Helper: print CR LF ---
.e9_crlf:
    push    eax
    mov     al, 0x0D
    out     0xE9, al
    mov     al, 0x0A
    out     0xE9, al
    pop     eax
    ret

;--- Test data ---
.test_filename:  db 'C:\MSDOS.SYS', 0
.msg_banner:     db 0x0D, 0x0A, '=== FILEIO TEST (register conv) ===', 0x0D, 0x0A, 0
.msg_open_ok:    db 'OPEN OK handle=', 0
.msg_open_fail:  db 'OPEN FAIL', 0x0D, 0x0A, 0
.msg_size:       db 'SIZE=', 0
.msg_size_fail:  db 'GETSIZE FAIL', 0x0D, 0x0A, 0
.msg_read:       db 'READ bytes=', 0
.msg_read_fail:  db 'READ FAIL eax=', 0
.msg_data:       db 'DATA: ', 0
.msg_done:       db 'FILEIO TEST OK', 0x0D, 0x0A, 0

;--- Test variables (BSS-like, in code segment for simplicity) ---
.ftest_handle:   dd 0
.ftest_size:     dd 0
.ftest_nread:    dd 0
.ftest_buf:      times 32 db 0


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
    ; _PageAllocate(nPages, pType, hvm, AlignMask, minPhys, maxPhys, PhysAddr, flags)
    ; cdecl: push right-to-left
    push    dword 0x09               ; flags: PAGEFIXED(0x08) | PAGEZEROINIT(0x01)
    push    dword 0                  ; PhysAddr (don't care)
    push    dword 0                  ; maxPhys (any)
    push    dword 0                  ; minPhys (any)
    push    dword 0                  ; AlignMask (page-aligned by default)
    push    dword 0                  ; hvm (0 = system VM)
    push    dword 1                  ; pType: PG_SYS=1 (not 2)
    push    dword [ebp+8]            ; nPages
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

;--- Page Free (no-op: we pass linear addr but _PageFree needs handle) ---
;--- Memory stays allocated - fine for permanent driver ---
global VxD_PageFree
global _VxD_PageFree
VxD_PageFree:
_VxD_PageFree:
    ret


;============================================================================
;   Data segment (DDB moved to top of _LTEXT for IOS compatibility)
;============================================================================
segment _LDATA public use32 class=LCODE align=4

;============================================================================
;   C code and data segments - ALL class=LCODE for single object
;============================================================================
segment _TEXT public use32 class=LCODE align=4
segment _DATA public use32 class=LCODE align=4

; Diagnostic registers from IFSMgr_Ring0_FileIO calls
global _diag_regs
global diag_regs
_diag_regs:
diag_regs:
    dd      0, 0, 0, 0      ; EAX, EBX, ECX, EDX after open

; IOS_Get_Device_List register dump (all regs after call)
global _gdl_regs
global gdl_regs
_gdl_regs:
gdl_regs:
    dd      0, 0, 0, 0, 0, 0   ; EAX, EBX, ECX, EDX, ESI, EDI
; VPICD services
%define VPICD_Virtualize_IRQ    0x00440001
%define VPICD_Phys_EOI          0x00440006

; CONFIGMG (Configuration Manager) services - Device ID 0x0033
%define CM_Locate_DevNode       0x00330002  ; ordinal 2
%define CM_Create_DevNode       0x0033000B  ; ordinal 11
%define CM_Register_Device_Driver 0x0033000E ; ordinal 14
%define CM_Register_DevLoader   0x00330032  ; ordinal 50
%define CM_Reenumerate_DevNode  0x00330033  ; ordinal 51
%define CM_Setup_DevNode        0x00330034  ; ordinal 52
%define CM_Get_DevNode_Status   0x00330036  ; ordinal 54
%define CM_CallBack_Device_Driver 0x00330039 ; ordinal 57
%define CM_Read_Registry_Value  0x0033003E  ; ordinal 62
%define CM_Reprocess_DevNode    0x0033005C  ; ordinal 92

; IOS (I/O Supervisor) services - Device ID 0x0010
; Format: 0x00100000 | ordinal
%define IOS_Get_Version         0x00100000  ; ordinal 0 - Get IOS version
%define IOS_Register_Device     0x00100001  ; ordinal 1 - Register a device
%define IOS_Find_Int13_Drive    0x00100002  ; ordinal 2 - Find INT 13h drive
%define IOS_Get_Device_List     0x00100003  ; ordinal 3 - Get device list
%define IOS_Send_Command        0x00100004  ; ordinal 4 - Send I/O command
%define IOS_BD_Command_Complete 0x00100005  ; ordinal 5 - Signal command complete
%define IOS_Synchronous_Command 0x00100006  ; ordinal 6 - Synchronous command
%define IOS_Register            0x00100007  ; ordinal 7 - Register driver with IOS
%define IOS_Requestor_Service   0x00100008  ; ordinal 8 - ISP dispatch (ISP_INSERT_CALLDOWN etc)
%define IOS_Exclusive_Access    0x00100009  ; ordinal 9 - Exclusive device access
%define IOS_Send_Next_Command   0x0010000A  ; ordinal 10 - Send next queued command
%define IOS_Set_Async_Time_Out  0x0010000B  ; ordinal 11 - Set async timeout
%define IOS_Signal_Semaphore    0x0010000C  ; ordinal 12 - Signal semaphore (no switch)
%define IOS_Idle_Status         0x0010000D  ; ordinal 13 - Idle status check
%define IOS_Map_IORS_To_I24     0x0010000E  ; ordinal 14 - Map IOR status to INT 24h
%define IOS_Map_IORS_To_I21     0x0010000F  ; ordinal 15 - Map IOR status to INT 21h
%define IOS_Print_Log           0x00100010  ; ordinal 16 - Debug print log
%define IOS_Mem_Alloc           0x00100012  ; ordinal 18 - IOS memory allocator
; Alias: ISP_INSERT_CALLDOWN is dispatched through IOS_Requestor_Service (ordinal 8)
%define ISP_INSERT_CALLDOWN     IOS_Requestor_Service

;--- IOS_Get_Version_Test() -> EAX = version or sentinel ---
global _IOS_Get_Version_Test
global IOS_Get_Version_Test
_IOS_Get_Version_Test:
IOS_Get_Version_Test:
    mov     eax, 0xFEEDFACE
    int     0x20
    dd      IOS_Get_Version     ; 0x00100000
    ret

;--- ULONG IOS_Call_Service_N(ULONG ordinal, void *param) ---
; Calls IOS service at given ordinal with param in EAX and on stack.
; Returns: low word = carry (0=clear/success, 1=set/fail), high word = EAX result shifted
; Actually: returns 0 if carry clear, 1 if carry set. Caller checks AEP side effects.
global _IOS_Call_Service_N
global IOS_Call_Service_N
_IOS_Call_Service_N:
IOS_Call_Service_N:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     ecx, [ebp+8]       ; ordinal
    mov     eax, [ebp+12]      ; param (DDB pointer)
    ; Build service ID: 0x00100000 | ordinal
    or      ecx, 0x00100000
    ; Self-modify the int 0x20 dd to use the right service ID
    mov     [.svc_id], ecx
    push    eax                 ; param on stack too
    int     0x20
.svc_id:
    dd      0x00100000          ; placeholder, patched above
    add     esp, 4
    jc      .svc_fail
    xor     eax, eax            ; carry clear = success
    jmp     .svc_done
.svc_fail:
    mov     eax, 1              ; carry set = failure
.svc_done:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG IOS_Register_Driver(DDB *ddb) ---
; Registers port driver with IOS (ordinal 7).
; Returns 0 on success (carry clear), non-zero on failure.
global _IOS_Register_Driver
global IOS_Register_Driver
_IOS_Register_Driver:
IOS_Register_Driver:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]       ; DDB pointer in EAX (register convention)
    push    eax                 ; also on stack
    int     0x20
    dd      IOS_Register        ; 0x00100007
    add     esp, 4              ; clean stack
    jc      .ios_reg_fail       ; carry set = failure
    xor     eax, eax            ; success: return 0
    jmp     .ios_reg_done
.ios_reg_fail:
    ; EAX may contain error code, or set a default
    test    eax, eax
    jnz     .ios_reg_done       ; if EAX non-zero, use it as error
    mov     eax, 1              ; else return 1 (generic failure)
.ios_reg_done:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG ISP_Insert_Calldown(DCB *dcb, CALLDOWN *cd, DDB *ddb, ULONG flags) ---
; Insert our handler into a DCB's calldown chain.
; Ordinal 8 = IOS_Requestor_Service, which dispatches ISP requests.
; NOTE: ISP_INSERT_CALLDOWN is dispatched through IOS_Requestor_Service.
; The params on stack form an ISP structure that IOS_Requestor_Service reads.
; Returns 0 on success (carry clear).
global _ISP_Insert_Calldown
global ISP_Insert_Calldown
_ISP_Insert_Calldown:
ISP_Insert_Calldown:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    ; Push all 4 params on stack for the service
    push    dword [ebp+20]      ; flags
    push    dword [ebp+16]      ; ddb
    push    dword [ebp+12]      ; cd (calldown)
    push    dword [ebp+8]       ; dcb
    ; Also set EAX = dcb (register convention backup)
    mov     eax, [ebp+8]
    int     0x20
    dd      ISP_INSERT_CALLDOWN ; 0x00100008
    add     esp, 16             ; clean 4 params
    jc      .isp_fail
    xor     eax, eax
    jmp     .isp_done
.isp_fail:
    mov     eax, 1
.isp_done:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- IOR handler bridge ---
; Called by IOS when an I/O request arrives for our DCB.
; DDK: port driver calldown uses cdecl convention.
; First param (IOP pointer) is on stack at [ESP+4].
; After pushad (32 bytes), original [ESP+4] is at [ESP+36].
extern _ntmini_ior_handler
global _ios_ior_bridge
global ios_ior_bridge
_ios_ior_bridge:
ios_ior_bridge:
    pushad
    push    dword [esp+36]      ; IOP/IOR pointer (cdecl first param)
    call    _ntmini_ior_handler
    add     esp, 4
    popad
    ret

;--- IOS AEP handler bridge ---
; IOS calls the AER (DRP_aer) with cdecl convention.
; AEP pointer is on stack at [ESP+4] (first param after return address).
global _ios_aep_bridge
global ios_aep_bridge
_ios_aep_bridge:
ios_aep_bridge:
    pushad
    ; After pushad (32 bytes): original [ESP+4] is now at [ESP+36]
    push    dword [esp+36]      ; AEP pointer
    call    _ntmini_aep_handler
    add     esp, 4
    popad
    clc
    ret

;--- ULONG IOS_Get_Device_List_Wrapper() ---
; Returns pointer to first DCB in IOS's device list.
; IOS_Get_Device_List (ordinal 3) returns device list head.
; Return value might be in EAX or EDX depending on IOS version.
; We return EAX (try EDX if EAX is 0).
%define VMM_Get_DDB             0x00010146
global _IOS_Get_Device_List_Wrapper
global IOS_Get_Device_List_Wrapper
_IOS_Get_Device_List_Wrapper:
IOS_Get_Device_List_Wrapper:
    push    ebx
    push    ecx
    push    esi
    push    edi
    int     0x20
    dd      IOS_Get_Device_List  ; 0x00100003
    ; Save ALL register outputs for C code to inspect
    mov     [gdl_regs+0], eax
    mov     [gdl_regs+4], ebx
    mov     [gdl_regs+8], ecx
    mov     [gdl_regs+12], edx
    mov     [gdl_regs+16], esi
    mov     [gdl_regs+20], edi
    ; Try each register for valid kernel pointer (>= 0xC0000000).
    cmp     eax, 0xC0000000
    jae     .gdl_done
    mov     eax, edx
    cmp     eax, 0xC0000000
    jae     .gdl_done
    mov     eax, esi
    cmp     eax, 0xC0000000
    jae     .gdl_done
    mov     eax, ecx
    cmp     eax, 0xC0000000
    jae     .gdl_done
    mov     eax, ebx
    cmp     eax, 0xC0000000
    jae     .gdl_done
    xor     eax, eax            ; no valid pointer found
.gdl_done:
    pop     edi
    pop     esi
    pop     ecx
    pop     ebx
    ret

;--- ULONG VMM_Get_DDB_Wrapper(USHORT device_id) ---
; Gets DDB pointer for a loaded VxD by device ID.
; Returns ECX = DDB pointer, or 0 if not found.
global _VMM_Get_DDB_Wrapper
global VMM_Get_DDB_Wrapper
_VMM_Get_DDB_Wrapper:
VMM_Get_DDB_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    movzx   eax, word [ebp+8]   ; device_id
    xor     edx, edx             ; any version
    int     0x20
    dd      VMM_Get_DDB          ; 0x00010146
    ; ECX = DDB pointer (or 0)
    mov     eax, ecx
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- void Call_ILB_Service(ULONG service_rtn, PVOID isp_packet) ---
; Calls ILB_service_rtn with ISP packet.
; Convention: EDX = ISP packet pointer, also pushed on stack.
global _Call_ILB_Service
global Call_ILB_Service
_Call_ILB_Service:
Call_ILB_Service:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    ecx
    mov     edx, [ebp+12]       ; ISP packet pointer in EDX
    push    edx                  ; also on stack
    mov     eax, [ebp+8]        ; ILB_service_rtn address
    call    eax                  ; call the service
    add     esp, 4               ; clean ISP from stack
    pop     ecx
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- void Call_ILB_Internal_Request(ULONG func_addr, ULONG dcb, ULONG iop, ULONG start_cd) ---
; Calls ILB_internal_request with register-based calling convention.
; Register-based calling convention:
;   EBX = DCB pointer
;   EDI = IOP pointer
;   EDX = starting calldown handler addr (0 = chain head, else skips to node after match)
; The function JMPs to chain executor which eventually RETs to caller.
global _Call_ILB_Internal_Request
global Call_ILB_Internal_Request
_Call_ILB_Internal_Request:
Call_ILB_Internal_Request:
    push    ebp
    mov     ebp, esp
    push    esi
    push    edi
    push    ebx
    mov     ebx, [ebp+12]       ; DCB pointer in EBX
    mov     edi, [ebp+16]       ; IOP pointer in EDI
    mov     edx, [ebp+20]       ; starting calldown (0 = chain head)
    mov     eax, [ebp+8]        ; function address
    call    eax                  ; call ILB_internal_request
    pop     ebx
    pop     edi
    pop     esi
    pop     ebp
    ret

;--- VMM_Set_Global_Time_Out wrapper ---
; void Set_Global_Timeout(ULONG ms, void (*callback)(ULONG refdata), ULONG refdata)
; VMM Set_Global_Time_Out: EAX=ms, ESI=callback, EDX=refdata
; Returns timeout handle in EAX (ignored here).
; Set_Time_Out (ordinal 0x001C) - per-VM timer callback
; Set_Global_Time_Out is ordinal 0x005C but might not work.
; Set_Time_Out fires in context of current VM - fine for system VM.
%define VMM_Set_Time_Out       0x0001001C
global _Set_Global_Timeout
global Set_Global_Timeout
_Set_Global_Timeout:
Set_Global_Timeout:
    push    ebp
    mov     ebp, esp
    push    esi
    push    ebx
    ; Get current VM handle (required for Set_Time_Out)
    int     0x20
    dd      VMM_Get_Cur_VM_Handle   ; EBX = current VM handle
    mov     eax, [ebp+8]       ; timeout in milliseconds
    mov     esi, [ebp+12]      ; callback function pointer
    mov     edx, [ebp+16]      ; reference data
    int     0x20
    dd      VMM_Set_Time_Out
    ; ESI = timeout handle on return (0 = error)
    mov     eax, esi            ; return handle to caller
    pop     ebx
    pop     esi
    pop     ebp
    ret

;--- Timer callback bridge: calls C function _ntmini_timer_callback(ULONG refdata) ---
; VMM calls with EDX=refdata, must return via RET.
extern _ntmini_timer_callback
global _timer_callback_bridge
global timer_callback_bridge
_timer_callback_bridge:
timer_callback_bridge:
    pushad
    push    edx                 ; refdata as parameter to C function
    call    _ntmini_timer_callback
    add     esp, 4
    popad
    ret

;--- IOS_BD_Complete(IOP *iop) ---
; Signals IOS that an I/O request is complete.
global _IOS_BD_Complete
global IOS_BD_Complete
_IOS_BD_Complete:
IOS_BD_Complete:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    edx
    mov     ebx, [ebp+8]       ; IOP pointer
    int     0x20
    dd      IOS_BD_Command_Complete
    pop     edx
    pop     ebx
    pop     ebp
    ret

;--- IOS_Requestor(ISP *isp) ---
; Calls IOS requestor service for creating DCBs etc.
global _IOS_Requestor
global IOS_Requestor
_IOS_Requestor:
IOS_Requestor:
    push    ebp
    mov     ebp, esp
    push    edx
    mov     edx, [ebp+8]       ; ISP pointer
    int     0x20
    dd      IOS_Requestor_Service
    pop     edx
    pop     ebp
    ret

;--- PVOID IOS_Mem_Alloc_Wrapper(ULONG size) ---
; IOS memory allocator (ordinal 18 = 0x00100012).
; Returns pointer to allocated memory in EAX (or 0 on failure).
global _IOS_Mem_Alloc_Wrapper
global IOS_Mem_Alloc_Wrapper
_IOS_Mem_Alloc_Wrapper:
IOS_Mem_Alloc_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+8]       ; size on stack
    int     0x20
    dd      IOS_Mem_Alloc       ; 0x00100012
    add     esp, 4
    ; EAX should contain pointer or 0
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG CM_Locate_DevNode_Wrapper(ULONG *pdevnode, char *device_id, ULONG flags) ---
; Find a devnode by device ID string.
; Returns 0 (CR_SUCCESS) on success, devnode handle written to *pdevnode.
global _CM_Locate_DevNode_Wrapper
global CM_Locate_DevNode_Wrapper
_CM_Locate_DevNode_Wrapper:
CM_Locate_DevNode_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+16]      ; flags
    push    dword [ebp+12]      ; device_id string
    push    dword [ebp+8]       ; pdevnode (output pointer)
    int     0x20
    dd      CM_Locate_DevNode   ; 0x00330002
    add     esp, 12
    ; EAX = CR_xxx result code
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG CM_Setup_DevNode_Wrapper(ULONG devnode, ULONG flags) ---
; Trigger driver loading for a device node.
; flags: 0=CM_SETUP_DEVNODE_READY, 1=CM_SETUP_DOWNLOAD
global _CM_Setup_DevNode_Wrapper
global CM_Setup_DevNode_Wrapper
_CM_Setup_DevNode_Wrapper:
CM_Setup_DevNode_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+12]      ; flags
    push    dword [ebp+8]       ; devnode
    int     0x20
    dd      CM_Setup_DevNode    ; 0x00330034
    add     esp, 8
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG CM_Create_DevNode_Wrapper(ULONG *pdevnode, char *device_id, ULONG parent, ULONG flags) ---
global _CM_Create_DevNode_Wrapper
global CM_Create_DevNode_Wrapper
_CM_Create_DevNode_Wrapper:
CM_Create_DevNode_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+20]      ; flags
    push    dword [ebp+16]      ; parent devnode
    push    dword [ebp+12]      ; device_id string
    push    dword [ebp+8]       ; pdevnode output
    int     0x20
    dd      CM_Create_DevNode   ; 0x0033000B
    add     esp, 16
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG CM_Register_Device_Driver_Wrapper(ULONG devnode, PVOID handler, ULONG refdata, ULONG flags) ---
; Register our VxD as the driver for a devnode. This tells CONFIGMG
; "I am the driver" and should change status from NOT_CONFIGURED to DRIVER_LOADED.
global _CM_Register_Device_Driver_Wrapper
global CM_Register_Device_Driver_Wrapper
_CM_Register_Device_Driver_Wrapper:
CM_Register_Device_Driver_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+20]      ; flags
    push    dword [ebp+16]      ; refdata
    push    dword [ebp+12]      ; handler (config handler callback)
    push    dword [ebp+8]       ; devnode
    int     0x20
    dd      CM_Register_Device_Driver ; 0x0033000E
    add     esp, 16
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG CM_Get_DevNode_Status_Wrapper(ULONG *pstatus, ULONG *pproblem, ULONG devnode, ULONG flags) ---
global _CM_Get_DevNode_Status_Wrapper
global CM_Get_DevNode_Status_Wrapper
_CM_Get_DevNode_Status_Wrapper:
CM_Get_DevNode_Status_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+20]      ; flags
    push    dword [ebp+16]      ; devnode
    push    dword [ebp+12]      ; pproblem
    push    dword [ebp+8]       ; pstatus
    int     0x20
    dd      CM_Get_DevNode_Status   ; 0x00330036
    add     esp, 16
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG CM_CallBack_Device_Loader_Wrapper(ULONG devnode, ULONG functype) ---
; Explicitly call the device's DevLoader. functype: 1=load, 2=unload.
global _CM_CallBack_Device_Loader_Wrapper
global CM_CallBack_Device_Loader_Wrapper
_CM_CallBack_Device_Loader_Wrapper:
CM_CallBack_Device_Loader_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+12]      ; functype
    push    dword [ebp+8]       ; devnode
    int     0x20
    dd      CM_CallBack_Device_Driver  ; 0x00330039
    add     esp, 8
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG CM_Reenumerate_DevNode_Wrapper(ULONG devnode, ULONG flags) ---
; Force re-enumeration of a device node's children.
global _CM_Reenumerate_DevNode_Wrapper
global CM_Reenumerate_DevNode_Wrapper
_CM_Reenumerate_DevNode_Wrapper:
CM_Reenumerate_DevNode_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+12]      ; flags
    push    dword [ebp+8]       ; devnode
    int     0x20
    dd      CM_Reenumerate_DevNode ; 0x00330033
    add     esp, 8
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- VPICD IRQ handler globals ---
global _g_irq_hw_int_func
global g_irq_hw_int_func
global _g_irq_devext
global g_irq_devext
global _g_irq_handle
global g_irq_handle
_g_irq_hw_int_func:
g_irq_hw_int_func:     dd 0    ; HwInterrupt function pointer (__stdcall)
_g_irq_devext:
g_irq_devext:           dd 0    ; DeviceExtension pointer
_g_irq_handle:
g_irq_handle:           dd 0    ; IRQ handle from VPICD

;--- IOSBRIDGE-compatible globals ---
; These are populated by NTMINI_V5.C after miniport initialization.
; IOSBRIDGE.C references them as extern to call miniport functions.
global _g_HwStartIo
global g_HwStartIo
global _g_HwResetBus
global g_HwResetBus
global _g_HwInterrupt
global g_HwInterrupt
global _g_DeviceExtension
global g_DeviceExtension
global _g_SrbExtensionSize
global g_SrbExtensionSize
global _g_SrbCompleted
global g_SrbCompleted
global _g_ReadyForNext
global g_ReadyForNext
_g_HwStartIo:
g_HwStartIo:            dd 0    ; BOOLEAN (__stdcall *)(PVOID, PSRB)
_g_HwResetBus:
g_HwResetBus:           dd 0    ; BOOLEAN (__stdcall *)(PVOID, ULONG)
_g_HwInterrupt:
g_HwInterrupt:          dd 0    ; BOOLEAN (__stdcall *)(PVOID)
_g_DeviceExtension:
g_DeviceExtension:      dd 0    ; PVOID
_g_SrbExtensionSize:
g_SrbExtensionSize:     dd 0    ; ULONG
_g_SrbCompleted:
g_SrbCompleted:         dd 0    ; volatile BOOLEAN
_g_ReadyForNext:
g_ReadyForNext:         dd 0    ; volatile BOOLEAN

;--- Init dispatch flags (prevent double execution) ---
g_init_complete_ran:    db 0    ; set to 1 when Init_Complete handler runs
g_sys_vm_init_ran:      db 0    ; set to 1 when Sys_VM_Init handler runs
    align 4

;--- IDE hardware interrupt handler ---
; Called by VMM when IRQ fires. EAX=IRQ handle, EBX=VM handle.
_ide_irq_handler:
    pushad
    ; Call HwInterrupt(__stdcall)(DeviceExtension)
    mov ecx, [g_irq_hw_int_func]
    test ecx, ecx
    jz .no_handler
    push dword [g_irq_devext]
    call ecx                    ; __stdcall: callee cleans stack
.no_handler:
    popad
    ; Physical EOI
    int     0x20
    dd      VPICD_Phys_EOI
    clc
    ret

;--- VxD_Hook_IRQ(irq_number) -> 0=fail, nonzero=handle ---
global _VxD_Hook_IRQ
global VxD_Hook_IRQ
_VxD_Hook_IRQ:
VxD_Hook_IRQ:
    push    ebp
    mov     ebp, esp
    push    edi
    push    ebx

    ; Build VPICD_IRQ_Descriptor on stack (32 bytes)
    sub     esp, 32
    mov     edi, esp
    ; Zero it first
    xor     eax, eax
    mov     [edi+0], eax
    mov     [edi+4], eax
    mov     [edi+8], eax
    mov     [edi+12], eax
    mov     [edi+16], eax
    mov     [edi+20], eax
    mov     [edi+24], eax
    mov     [edi+28], eax
    ; Fill fields
    mov     ax, [ebp+8]         ; IRQ number (word)
    mov     [edi+0], ax          ; VID_IRQ_Number
    mov     word [edi+2], 0      ; VID_Options (default)
    lea     eax, [_ide_irq_handler]
    mov     [edi+4], eax         ; VID_Hw_Int_Proc

    ; Call VPICD_Virtualize_IRQ (EDI -> descriptor)
    int     0x20
    dd      VPICD_Virtualize_IRQ
    jc      .hook_fail

    ; Success: EAX = IRQ handle
    mov     [g_irq_handle], eax
    jmp     .hook_done

.hook_fail:
    xor     eax, eax

.hook_done:
    add     esp, 32
    pop     ebx
    pop     edi
    pop     ebp
    ret

segment _BSS public use32 class=LCODE align=4
segment CONST public use32 class=LCODE align=4
segment CONST2 public use32 class=LCODE align=4
segment _TLS public use32 class=LCODE align=4

;============================================================================
;   ScsiPort struct-return functions in ASM
;   MSVC struct-return ABI: hidden first param = pointer to result storage
;   These MUST match MSVC's calling convention exactly.
;============================================================================
segment _LTEXT

;--- ScsiPortConvertUlongToPhysicalAddress ---
; MSVC returns 8-byte union in EDX:EAX (LowPart=EAX, HighPart=EDX)
; Caller pushes: [Addr(4)] = 4 bytes
; __stdcall: callee cleans, ret 4
; REMAP: primary IDE (0x1F0/0x3F6) → secondary (0x170/0x376)
global _sp_ConvertUlong_asm
global sp_ConvertUlong_asm
_sp_ConvertUlong_asm:
sp_ConvertUlong_asm:
    push    ebp
    mov     ebp, esp
    mov     eax, [ebp+8]       ; Addr - NO remapping (passthrough)
    ; Port remapping is done in sp_ReadPortUchar/sp_WritePortUchar instead.
    ; This keeps the miniport's internal address as 0x1F0 (primary channel 0).
    xor     edx, edx            ; HighPart = 0 in EDX
    pop     ebp
    ret     4                   ; clean 1 param (4 bytes)

;--- ScsiPortGetPhysicalAddress ---
; MSVC returns 8-byte union in EDX:EAX
; Caller pushes: [Length(4)] [VirtualAddr(4)] [Srb(4)] [HwExt(4)] = 16 bytes
; __stdcall: callee cleans, ret 16
global _sp_GetPhysAddr_asm
global sp_GetPhysAddr_asm
_sp_GetPhysAddr_asm:
sp_GetPhysAddr_asm:
    push    ebp
    mov     ebp, esp
    mov     eax, [ebp+16]      ; VirtualAddr → LowPart
    xor     edx, edx            ; HighPart = 0
    ; Write *Length = 0x10000
    mov     ecx, [ebp+20]      ; Length pointer
    test    ecx, ecx
    jz      .no_len
    mov     dword [ecx], 0x10000
.no_len:
    pop     ebp
    ret     16                  ; clean 4 params (16 bytes)

;============================================================================
; MSCDEX V86 INT 2Fh handler
;
; Called by VMM when V86 mode code executes INT 2Fh.
; Hook_V86_Int_Chain callback convention:
;   EBX = current VM handle
;   EBP = pointer to Client_Reg_Struc
;   Return: carry clear = handled (don't chain), carry set = not handled (chain)
;
; Client_Reg_Struc offsets:
;   +0x00: Client_EDI    +0x04: Client_ESI
;   +0x08: Client_EBP    +0x0C: reserved
;   +0x10: Client_EBX    +0x14: Client_EDX
;   +0x18: Client_ECX    +0x1C: Client_EAX
;   +0x20: Client_Error  +0x24: Client_EIP
;   +0x28: Client_CS     +0x2C: Client_EFlags
;   +0x30: Client_ESP    +0x34: Client_SS
;   +0x38: Client_ES     +0x3C: Client_DS
;============================================================================
extern _ntmini_mscdex_handler

mscdex_v86_handler:
    ; Check if this is an MSCDEX call (AH = 0x15)
    mov     eax, [ebp+0x1C]     ; Client_EAX
    cmp     ah, 0x15
    jne     .not_mscdex

    ; It's MSCDEX! Call C handler with pointer to client regs.
    ; ULONG ntmini_mscdex_handler(void *client_regs)
    ; Returns 0 if handled, non-zero if not handled.
    pushad
    push    ebp                 ; client_regs pointer
    call    _ntmini_mscdex_handler
    add     esp, 4
    test    eax, eax
    popad
    jnz     .not_mscdex         ; C handler said "not handled"

    ; Handled: clear carry to stop chaining
    clc
    ret

.not_mscdex:
    ; Not our call: set carry to continue chain
    stc
    ret

;============================================================================
; IFSMgr VxD Service Wrappers
;============================================================================

;--- ULONG IFSMgr_InstallFSHook_Wrapper(ULONG pfnHandler) ---
; Installs an IFS file system API hook. Returns previous hook ptr.
%define IFSMgr_InstallFileSystemApiHook 0x00400067

global _IFSMgr_InstallFSHook_Wrapper
global IFSMgr_InstallFSHook_Wrapper
_IFSMgr_InstallFSHook_Wrapper:
IFSMgr_InstallFSHook_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]           ; pfnHandler
    push    eax
    int     0x20
    dd      IFSMgr_InstallFileSystemApiHook
    add     esp, 4
    ; EAX = previous hook function pointer
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

%define IFSMgr_CDROM_Attach  0x0040002F
%define NotifyVolumeArrival  0x0040004D

;--- ULONG IFSMgr_CDROM_Attach_Wrapper(ULONG drive_letter, ULONG *pvrp_out) ---
; Forces CDFS mount on a drive. Returns 0 on success.
; cdecl: push pvrp_out, push drive_letter, int 20h.
global _IFSMgr_CDROM_Attach_Wrapper
global IFSMgr_CDROM_Attach_Wrapper
_IFSMgr_CDROM_Attach_Wrapper:
IFSMgr_CDROM_Attach_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+12]       ; pvrp_out
    push    eax
    mov     eax, [ebp+8]        ; drive_letter (0-based)
    push    eax
    int     0x20
    dd      IFSMgr_CDROM_Attach ; 0x0040002F
    add     esp, 8
    ; EAX = 0 success, else error
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- void NotifyVolumeArrival_Wrapper(ULONG drive_letter) ---
; Notifies IFSMgr that a new volume appeared.
global _NotifyVolumeArrival_Wrapper
global NotifyVolumeArrival_Wrapper
_NotifyVolumeArrival_Wrapper:
NotifyVolumeArrival_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]        ; drive_letter (0-based)
    push    eax
    int     0x20
    dd      NotifyVolumeArrival  ; 0x0040004D
    add     esp, 4
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG IFSMgr_RegisterMount_Wrapper(ULONG pfnMount, ULONG version, ULONG is_default) ---
; Registers an FSD mount routine with IFSMgr. Returns provider ID (-1 on error).
; cdecl: push is_default, push version, push pfnMount, int 20h.
%define IFSMgr_RegisterMount 0x00400001

global _IFSMgr_RegisterMount_Wrapper
global IFSMgr_RegisterMount_Wrapper
_IFSMgr_RegisterMount_Wrapper:
IFSMgr_RegisterMount_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+16]       ; is_default (0 or 1)
    push    eax
    mov     eax, [ebp+12]       ; version (IFSMGRVERSION = 0x22)
    push    eax
    mov     eax, [ebp+8]        ; pfnMount entry point
    push    eax
    int     0x20
    dd      IFSMgr_RegisterMount ; 0x00400001
    add     esp, 12
    ; EAX = Provider ID (-1 if error)
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG IFSMgr_FSDAttachSFT_Wrapper(ULONG pioreq) ---
; Creates an SFT entry for the given ioreq. Returns 0 on success.
; Ordinal 95 in service table = 0x0040005F
%define IFSMgr_FSDAttachSFT 0x0040005F

global _IFSMgr_FSDAttachSFT_Wrapper
global IFSMgr_FSDAttachSFT_Wrapper
_IFSMgr_FSDAttachSFT_Wrapper:
IFSMgr_FSDAttachSFT_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]        ; pioreq
    push    eax
    int     0x20
    dd      IFSMgr_FSDAttachSFT  ; 0x0040005F
    add     esp, 4
    ; EAX = result
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG IFSMgr_GetHeap_Wrapper(ULONG size) ---
; Allocate from IFSMgr's heap. Returns pointer or 0 on failure.
; Ordinal 13 in service table = 0x0040000D
%define IFSMgr_GetHeap 0x0040000D

global _IFSMgr_GetHeap_Wrapper
global IFSMgr_GetHeap_Wrapper
_IFSMgr_GetHeap_Wrapper:
IFSMgr_GetHeap_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]        ; size
    push    eax
    int     0x20
    dd      IFSMgr_GetHeap       ; 0x0040000D
    add     esp, 4
    ; EAX = pointer or 0
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- void IFSMgr_RetHeap_Wrapper(ULONG ptr) ---
; Free IFSMgr heap memory.
; Ordinal 14 in service table = 0x0040000E
%define IFSMgr_RetHeap 0x0040000E

global _IFSMgr_RetHeap_Wrapper
global IFSMgr_RetHeap_Wrapper
_IFSMgr_RetHeap_Wrapper:
IFSMgr_RetHeap_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]        ; ptr
    push    eax
    int     0x20
    dd      IFSMgr_RetHeap       ; 0x0040000E
    add     esp, 4
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG IFSMgr_RegisterFSDWithPriority_Wrapper(ULONG type, ULONG entry, ULONG prio, ULONG param, ULONG attr, ULONG ver) ---
; Register an FSD with specific priority. Lower priority number = tried first.
; Ordinal 118 in service table = 0x00400076
; Type 0 = local FSD (same as RegisterMount)
%define IFSMgr_RegisterFSDWithPriority 0x00400076

global _IFSMgr_RegisterFSDWithPriority_Wrapper
global IFSMgr_RegisterFSDWithPriority_Wrapper
_IFSMgr_RegisterFSDWithPriority_Wrapper:
IFSMgr_RegisterFSDWithPriority_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    ; cdecl: push right-to-left
    ; C prototype: (type, entry, priority, param, attr, version)
    ; [ebp+8]=type [ebp+12]=entry [ebp+16]=prio [ebp+20]=param [ebp+24]=attr [ebp+28]=ver
    mov     eax, [ebp+28]       ; version
    push    eax
    mov     eax, [ebp+24]       ; attributes
    push    eax
    mov     eax, [ebp+20]       ; param (normal/default FSD)
    push    eax
    mov     eax, [ebp+16]       ; priority
    push    eax
    mov     eax, [ebp+12]       ; entry point
    push    eax
    mov     eax, [ebp+8]        ; FSD type (0=local)
    push    eax
    int     0x20
    dd      IFSMgr_RegisterFSDWithPriority ; 0x00400076
    add     esp, 24
    ; EAX = Provider ID (-1 if error)
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG IFSMgr_CallProvider_Wrapper(ULONG provider_id, ULONG fn, ULONG pioreq) ---
; Calls a specific FSD's entry point for a given function.
; Ordinal 41 in service table = 0x00400029
%define IFSMgr_CallProvider 0x00400029

global _IFSMgr_CallProvider_Wrapper
global IFSMgr_CallProvider_Wrapper
_IFSMgr_CallProvider_Wrapper:
IFSMgr_CallProvider_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+16]       ; pioreq
    push    eax
    mov     eax, [ebp+12]       ; fn (function number)
    push    eax
    mov     eax, [ebp+8]        ; provider_id
    push    eax
    int     0x20
    dd      IFSMgr_CallProvider  ; 0x00400029
    add     esp, 12
    ; EAX = result
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

;--- ULONG IFSMgr_Get_Drive_Info_Wrapper(ULONG drive_letter) ---
; Returns drive info flags for a given volume (0=A:).
; Uses register calling: al = drive letter (0-based).
; Ordinal 52 in service table = 0x00400034
%define IFSMgr_Get_Drive_Info 0x00400034

global _IFSMgr_Get_Drive_Info_Wrapper
global IFSMgr_Get_Drive_Info_Wrapper
_IFSMgr_Get_Drive_Info_Wrapper:
IFSMgr_Get_Drive_Info_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    mov     eax, [ebp+8]        ; drive_letter (0-based)
    ; al = drive letter for this register-based service
    int     0x20
    dd      IFSMgr_Get_Drive_Info ; 0x00400034
    ; EAX = flags on carry clear, undefined on carry set
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret
