;============================================================================
;   VXDWRAP_V4.ASM - All segments in LCODE, single LE object
;   COM1 serial debug output + IFSMgr_Ring0_FileIO for file I/O
;============================================================================

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
%define Sys_Dynamic_Device_Init 0x001B
%define Sys_Dynamic_Device_Exit 0x001C

%define UNDEFINED_DEVICE_ID     0x0000
%define UNDEFINED_INIT_ORDER    0x80000000

extern _ntmini_init
extern _ntmini_cleanup
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
    ; Write 'V' to port 0xE9 on every control message (for debugcon)
    push    eax
    mov     al, 'V'
    out     0xE9, al
    push    edx
    mov     dx, 0x402
    out     dx, al
    pop     edx
    pop     eax

    cmp     eax, Device_Init
    je      .do_init
    cmp     eax, Sys_Critical_Init
    je      .do_serial_only
    cmp     eax, Init_Complete
    je      .do_init_complete
    cmp     eax, Sys_Dynamic_Device_Init
    je      .do_dynamic_init
    cmp     eax, Sys_Dynamic_Device_Exit
    je      .do_exit
    clc
    ret

.do_serial_only:
    ; Sys_Critical_Init: just init serial (standalone VxD path)
    pushad
    call    VxD_Serial_Init
    popad
    clc
    ret

.do_dynamic_init:
    ; Sys_Dynamic_Device_Init: loaded by IOS from IOSUBSYS.
    ; Do MINIMAL init - just serial + log message. No PE load.
    ; The full miniport init would crash during early IOS phase.
    pushad
    call    VxD_Serial_Init
    popad
    ; Write a marker so we know we were loaded dynamically
    push    eax
    mov     al, 'D'             ; 'D' = dynamic load marker
    out     0xE9, al
    push    edx
    mov     dx, 0x402
    out     dx, al
    pop     edx
    pop     eax
    ; Log to serial
    pushad
    extern  _ntmini_dynamic_init
    call    _ntmini_dynamic_init
    popad
    clc
    ret

.do_init_complete:
    ; Init_Complete: IOS registration fallback + MSCDEX hook
    pushad
    call    _ntmini_ios_init     ; skips if already registered
    popad
    ; Hook INT 2Fh for MSCDEX CD-ROM access
    pushad
    mov     eax, 0x2F
    mov     esi, mscdex_v86_handler
    int     0x20
    dd      VMM_Hook_V86_Int_Chain
    popad
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
;   IFSMgr_Ring0_FileIO is a cdecl function. Parameters pushed on stack.
;   First parameter (leftmost) = function code.
;   Returns result in EAX. Negative/carry = error.
;========================================================================

;------------------------------------------------------------------------
; int VxD_File_Open(const char *filename)
;
; Opens a file for reading using IFSMgr_Ring0_FileIO.
; Returns file handle (>= 0) or -1 on error.
;
; R0_OPENCREATFILE calling convention (cdecl):
;   IFSMgr_Ring0_FileIO(R0_OPENCREATFILE, pFilename, flags, action, attr, pAction)
;   flags:  access mode + share mode
;   action: 1 = open existing
;   attr:   0 = normal
;   pAction: NULL
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

    ; Full cdecl (all params on stack, right-to-left)
    ; This convention completed previously (returned EAX=1)
    push    dword 0                         ; pAction (NULL)
    push    dword 0                         ; attr (normal)
    push    dword 1                         ; action: open existing only
    push    dword R0_OPEN_ACCESS_READONLY | R0_OPEN_SHARE_DENYWRITE | R0_NO_CACHE
    push    dword [ebp+8]                   ; pFilename
    push    dword R0_OPENCREATFILE           ; function code

    int     0x20
    dd      IFSMgr_Ring0_FileIO
    add     esp, 24

    ; Diagnostic: store all register values
    mov     [_diag_regs + 0], eax           ; diag_regs[0] = EAX
    mov     [_diag_regs + 4], ebx           ; diag_regs[1] = EBX
    mov     [_diag_regs + 8], ecx           ; diag_regs[2] = ECX
    mov     [_diag_regs + 12], edx          ; diag_regs[3] = EDX

    ; EAX should be the file handle now
    test    eax, eax
    jz      .open_fail
    cmp     eax, 0
    jl      .open_fail
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
; int VxD_File_Read(int handle, void *buffer, int count)
;
; Reads from a file using IFSMgr_Ring0_FileIO.
; Returns bytes read, or -1 on error.
;
; R0_READFILE calling convention (cdecl):
;   IFSMgr_Ring0_FileIO(R0_READFILE, hFile, pBuffer, count, position)
;   position: 0xFFFFFFFF = current position
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

    ; Full cdecl — try alt order: (func, pBuffer, hFile, count, position)
    push    dword 0xFFFFFFFF                ; position
    push    dword [ebp+16]                  ; count
    push    dword [ebp+8]                   ; hFile
    push    dword [ebp+12]                  ; pBuffer
    push    dword R0_READFILE               ; function code

    int     0x20
    dd      IFSMgr_Ring0_FileIO
    add     esp, 20

    ; EAX = bytes read (or try ECX)
    ; Previous test: EAX returned count but buffer was zeros
    ; The data might have been written to a different location
    ; Let's check both EAX and ECX
    jc      .read_fail
    ; If EAX == 0 try ECX
    test    eax, eax
    jnz     .read_have_count
    mov     eax, ecx
.read_have_count:
    cmp     eax, 0
    jl      .read_fail
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
; Closes a file using IFSMgr_Ring0_FileIO,
; and also closes the boot log.
;------------------------------------------------------------------------
global VxD_File_Close
global _VxD_File_Close
VxD_File_Close:
_VxD_File_Close:
    push    ebp
    mov     ebp, esp
    push    ebx

    ; Close via IFSMgr if handle > 1 (real file handle)
    mov     eax, [ebp+8]
    cmp     eax, 1
    jle     .close_bootlog

    ; Full cdecl
    push    dword [ebp+8]                   ; hFile
    push    dword R0_CLOSEFILE              ; function code
    int     0x20
    dd      IFSMgr_Ring0_FileIO
    add     esp, 8
    jmp     .close_done

.close_bootlog:
    ; Close boot log
    int     0x20
    dd      VMM_Close_Boot_Log

.close_done:
    pop     ebx
    pop     ebp
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
; VPICD services
%define VPICD_Virtualize_IRQ    0x00440001
%define VPICD_Phys_EOI          0x00440006

; CONFIGMG (Configuration Manager) services - Device ID 0x0033
; Ordinals from inc/CONFIGMG.INC service table (0-indexed from line 156)
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
; Service ordinals from IOS.INC service table ordering:
;   0=Get_Version, 1=Register, 2=Requestor_Service, 3=Exclusive_Access,
;   4=SendCommand, 5=BD_Command_Complete, 6=Synchronous_Command,
;   7=Set_Async_Time_Out, ...
; IOS VxD services (device 0x0010) - CORRECTED per Ralf Brown Interrupt List
; and confirmed by ESDI_506.PDR disassembly (uses ordinals 7, 16, 18).
; Previous ordinals were WRONG: IOS_Register was at 1 (actually Register_Device),
; IOS_Set_Async_Time_Out was at 7 (actually IOS_Register!).
; Format: 0x00100000 | ordinal
%define IOS_Get_Version         0x00100000  ; ordinal 0 - Get IOS version
%define IOS_Register_Device     0x00100001  ; ordinal 1 - Register a device (NOT IOS_Register!)
%define IOS_Find_Int13_Drive    0x00100002  ; ordinal 2 - Find INT 13h drive
%define IOS_Get_Device_List     0x00100003  ; ordinal 3 - Get device list
%define IOS_Send_Command        0x00100004  ; ordinal 4 - Send I/O command
%define IOS_BD_Command_Complete 0x00100005  ; ordinal 5 - Signal command complete
%define IOS_Synchronous_Command 0x00100006  ; ordinal 6 - Synchronous command
%define IOS_Register            0x00100007  ; ordinal 7 - Register driver with IOS (THE real one)
%define IOS_Requestor_Service   0x00100008  ; ordinal 8 - ISP dispatch (ISP_INSERT_CALLDOWN etc)
%define IOS_Exclusive_Access    0x00100009  ; ordinal 9 - Exclusive device access
%define IOS_Send_Next_Command   0x0010000A  ; ordinal 10 - Send next queued command
%define IOS_Set_Async_Time_Out  0x0010000B  ; ordinal 11 - Set async timeout
%define IOS_Signal_Semaphore    0x0010000C  ; ordinal 12 - Signal semaphore (no switch)
%define IOS_Idle_Status         0x0010000D  ; ordinal 13 - Idle status check
%define IOS_Map_IORS_To_I24     0x0010000E  ; ordinal 14 - Map IOR status to INT 24h
%define IOS_Map_IORS_To_I21     0x0010000F  ; ordinal 15 - Map IOR status to INT 21h
%define IOS_Print_Log           0x00100010  ; ordinal 16 - Debug print log
; ordinal 17 = unknown
%define IOS_Mem_Alloc           0x00100012  ; ordinal 18 - IOS memory allocator (from ESDI_506)
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
; Registers port driver with IOS using the CORRECT ordinal 7.
; Previous code used ordinal 1 (IOS_Register_Device) which is WRONG.
; ESDI_506.PDR and NTMAPHLP.PDR both use ordinal 7 for registration.
; Returns 0 on success (carry clear), non-zero on failure.
; VxD services signal success via carry flag, not EAX.
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
    push    eax                 ; also on stack (ESDI_506 pattern)
    int     0x20
    dd      IOS_Register        ; 0x00100007 (ordinal 7 - CORRECTED)
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
; Confirmed by debug: [ESP+4] has valid AEP structs with correct func codes.
global _ios_aep_bridge
global ios_aep_bridge
_ios_aep_bridge:
ios_aep_bridge:
    pushad
    ; After pushad (32 bytes): original [ESP+4] is now at [ESP+36]
    push    dword [esp+36]      ; AEP pointer (from IOS stack)
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
    push    esi
    push    edi
    int     0x20
    dd      IOS_Get_Device_List  ; 0x00100003
    ; Result might be in EAX, EDX, or ESI
    test    eax, eax
    jnz     .gdl_done
    mov     eax, edx            ; try EDX
    test    eax, eax
    jnz     .gdl_done
    mov     eax, esi            ; try ESI
.gdl_done:
    pop     edi
    pop     esi
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
; Convention: EDX = ISP packet pointer, also pushed on stack (belt+suspenders).
; ESDI_506 disassembly shows: lea edx,[isp]; push edx; call [ilb_svc]; add esp,4
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
    push    edx                  ; also on stack (ESDI_506 pattern)
    mov     eax, [ebp+8]        ; ILB_service_rtn address
    call    eax                  ; call the service
    add     esp, 4               ; clean ISP from stack
    pop     ecx
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
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
; ESDI_506.PDR uses this with sizes 0x78, 0x28, 0xC8.
; Returns pointer to allocated memory in EAX (or 0 on failure).
; Calling convention from ESDI_506: push size, int 20h, add esp 4
global _IOS_Mem_Alloc_Wrapper
global IOS_Mem_Alloc_Wrapper
_IOS_Mem_Alloc_Wrapper:
IOS_Mem_Alloc_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+8]       ; size on stack (matches ESDI_506 pattern)
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
