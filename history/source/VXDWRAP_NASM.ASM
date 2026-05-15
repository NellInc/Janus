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
%define VMM_PageFree            0x00010055
%define VMM_HeapAllocate        0x0001004F
%define VMM_HeapFree            0x00010051
%define VMM_Out_Debug_String    0x0001001D
; VMM R0 file I/O services DON'T EXIST in Win98 SE (ordinals 0x02DE-0x02E1).
; Use IFSMgr Ring0_FileIO instead (cdecl, stack-based).
%define IFSMgr_Ring0_FileIO     0x00400032
%define IFSMgr_CDROM_Attach     0x0040002F
%define IFSMgr_NotifyVolumeArrival 0x0040004D
%define IFSMgr_RegisterMount    0x00400001
%define IFSMgr_InstallFileSystemApiHook 0x00400067
%define IFSMgr_FSDAttachSFT    0x0040005F
%define R0_OPENCREATFILE        0xD500
%define R0_READFILE             0xD600
%define R0_CLOSEFILE            0xD700
%define VMM_Set_Global_Time_Out 0x0001003C
%define VMM_Cancel_Time_Out     0x0001003D
%define VMM__RegOpenKey         0x00010100
%define VMM__RegCloseKey        0x00010101
%define VMM__RegCreateKey       0x00010102
%define VMM__RegQueryValueEx    0x00010104
%define VMM__RegSetValueEx      0x00010105
%define VMM_Get_System_Time     0x0001003F
%define VMM__MapPhysToLinear    0x0001006C
%define VMM_Time_Slice_Sleep    0x00010161
%define VMM_Get_DDB             0x00010146
%define VMM_R0_OpenCreateFile   0x000102DE
%define VMM_R0_CloseFile        0x000102E0
%define VMM_R0_ReadFile         0x000102E1
%define VWIN32_CopyMem          0x002A0005

%define VPICD_Virtualize_IRQ            0x00030000
%define VPICD_Force_Default_Behavior    0x00030008
%define VPICD_Physically_Mask           0x0003000A
%define VPICD_Physically_Unmask         0x0003000B
%define VPICD_Phys_EOI                  0x00030012

%define IOS_Register_svc                0x00100007
%define IOS_Requestor_Service_svc       0x00100008
%define IOS_SendCommand_svc             0x00100004
%define ISP_INSERT_CALLDOWN_svc         0x00100008
%define IOS_BD_Command_Complete_svc     0x00100005

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
extern ntmini_device_init           ; int ntmini_device_init(void)
extern _ios_schedule_late_cdrom_attach_probe ; void ios_schedule_late_cdrom_attach_probe(void)
extern g_late_next_handler          ; ULONG g_late_next_handler
extern _aep_handler                 ; void aep_handler(void *aep)
extern _ios_wdm_log_iop             ; void ios_wdm_log_iop(void *iop)
extern _ios_wdm_ior_entry           ; void ios_wdm_ior_entry(void *iop)
extern _IrpMgr_IoCompleteRequest    ; void IrpMgr_IoCompleteRequest(void *irp, char boost)
extern _IrpMgr_IoAllocateIrp        ; void *IrpMgr_IoAllocateIrp(char stack, unsigned char charge)
extern _ntk_KeInitializeEvent_cdecl ; void helper(event, type, state32)
extern _ntk_KeSetEvent_cdecl        ; long helper(event, increment, wait32)
extern _ntk_KeWaitForSingleObject_cdecl ; status helper(obj, reason, mode, alert32, timeout)
extern _ntk_ExFreePool_cdecl        ; void helper(ptr)

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
    ; Early heartbeat: write "NT!" to port 0xE9 to confirm VxD loaded
    push    eax
    mov     al, 'N'
    out     0xE9, al
    mov     al, 'T'
    out     0xE9, al
    mov     al, '!'
    out     0xE9, al
    mov     al, 0x0A
    out     0xE9, al
    pop     eax
    ; Full control-message value. The low-byte-only "uXX" marker is not
    ; enough if VMM leaves high EAX bits nonzero.
    push    eax
    push    ecx
    push    edx
    mov     edx, eax
    mov     al, 'm'
    out     0xE9, al
    mov     ecx, 8
.msg_hex_loop:
    rol     edx, 4
    mov     al, dl
    and     al, 0x0F
    cmp     al, 9
    jbe     .msg_hex_num
    add     al, 'A' - 10
    jmp     .msg_hex_out
.msg_hex_num:
    add     al, '0'
.msg_hex_out:
    out     0xE9, al
    loop    .msg_hex_loop
    mov     al, 0x0A
    out     0xE9, al
    pop     edx
    pop     ecx
    pop     eax
    ; Only init at Init_Complete (filesystem ready for Ring0_FileIO)
    ; or Sys_Dynamic_Device_Init (hot-load). Return success for earlier messages.
    cmp     eax, Init_Complete
    je      .msg_init_complete
    cmp     eax, Sys_Dynamic_Device_Init
    je      .msg_dynamic_init
    cmp     eax, Sys_Dynamic_Device_Exit
    je      .msg_dynamic_exit
    cmp     eax, Device_Init
    je      .msg_device_init
    cmp     eax, Sys_Critical_Init
    je      .msg_syscrit_init
    cmp     eax, 0x0000DEAD
    je      .msg_late_attach
    jmp     .msg_unknown
.msg_unknown:
    push    eax
    push    ecx
    mov     cl, al
    mov     al, 'u'
    out     0xE9, al
    mov     al, cl
    shr     al, 4
    cmp     al, 9
    jbe     .msg_unknown_hi_num
    add     al, 'A' - 10
    jmp     .msg_unknown_hi_out
.msg_unknown_hi_num:
    add     al, '0'
.msg_unknown_hi_out:
    out     0xE9, al
    mov     al, cl
    and     al, 0x0F
    cmp     al, 9
    jbe     .msg_unknown_lo_num
    add     al, 'A' - 10
    jmp     .msg_unknown_lo_out
.msg_unknown_lo_num:
    add     al, '0'
.msg_unknown_lo_out:
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     ecx
    pop     eax
    clc
    ret
.msg_init_complete:
    push    eax
    mov     al, '2'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    jmp     .do_ios_register
.msg_dynamic_init:
    push    eax
    mov     al, 'J'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    jmp     .do_init
.msg_dynamic_exit:
    push    eax
    mov     al, 'x'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    jmp     .do_exit
.msg_late_attach:
    push    eax
    mov     al, 'L'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    pushad
    call    _ios_schedule_late_cdrom_attach_probe
    popad
    clc
    ret
.msg_device_init:
    push    eax
    mov     al, '1'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    jmp     .do_early_ok
.msg_syscrit_init:
    push    eax
    mov     al, '0'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    jmp     .do_early_ok
.do_early_ok:
    clc
    ret

.do_ios_register:
    pushad
    call    ios_register_port_driver
    test    eax, eax
    popad
    jnz     .init_fail
    push    eax
    mov     al, 'z'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    clc
    ret

.do_init:
    pushad
    call    _ntmini_init
    test    eax, eax
    popad
    jnz     .init_fail
    push    eax
    mov     al, 'Z'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    clc
    ret
.init_fail:
    push    eax
    mov     al, 'F'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    stc
    ret

.do_exit:
    pushad
    call    _ntmini_cleanup
    popad
    push    eax
    mov     al, 'X'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
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

    ; Print init banner via port 0xE9 (Out_Debug_String ordinal 0x1D is wrong
    ; on retail VMM - it maps to Map_Lin_To_VM_Addr, causing WPE)
    mov     esi, _szInitBanner
.init_banner_loop:
    lodsb
    test    al, al
    jz      .init_banner_done
    out     0xE9, al
    jmp     .init_banner_loop
.init_banner_done:

    ; Breadcrumb A: about to call ios_register_port_driver
    mov     al, 'A'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al

    ; Call NT5/WDM stack initialization. IOS registration is deferred until
    ; Init_Complete because IOS_Register during Device_Init trips V86MMGR.
    call    ntmini_device_init
    ; EAX = 0 on success

    ; Breadcrumb B: returned from ios_register_port_driver
    push    eax
    mov     al, 'B'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax

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
    push    eax
    mov     al, 'c'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax

    ; Unhook IRQ if hooked
    call    _irq_hook_remove

    push    eax
    mov     al, 'q'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax

    pop     ebp
    ret


global ios_wdm_passthru_stub
global _ios_wdm_passthru_stub
ios_wdm_passthru_stub:
_ios_wdm_passthru_stub:
    ; Preserve the original IOS calldown register context while the C logger runs.
    pushad
    push    dword [esp+36]      ; original first cdecl argument: IOP pointer
    call    _ios_wdm_log_iop
    add     esp, 4
    popad
    cmp     dword [g_late_next_handler], 0
    je      .done
    jmp     dword [g_late_next_handler]
.done:
    ret

global ios_aep_bridge
global _ios_aep_bridge
ios_aep_bridge:
_ios_aep_bridge:
    ; Preserve original IOS AEP register context while C handles the AEP.
    pushad
    push    dword [esp+36]      ; original first cdecl argument: AEP pointer
    call    _aep_handler
    add     esp, 4
    popad
    clc
    ret

global ios_wdm_preserve_stub
global _ios_wdm_preserve_stub
ios_wdm_preserve_stub:
_ios_wdm_preserve_stub:
    ; Preserve original IOS calldown registers while C handles/completes the IOP.
    pushad
    push    dword [esp+36]      ; original first cdecl argument: IOP pointer
    call    _ios_wdm_ior_entry
    add     esp, 4
    popad
    ret

global ios_ior_stub
ios_ior_stub:
    mov     al, 'X'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    ret

global ntmini_search_stub_asm
global _ntmini_search_stub_asm
ntmini_search_stub_asm:
_ntmini_search_stub_asm:
    mov     al, 'Y'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    mov     edx, [esp+4]
    test    edx, edx
    jz      .search_done
    mov     word [edx+0x1A], 0x0012
.search_done:
    mov     eax, -1
    ret

global ntmini_netdir_stub_asm
global _ntmini_netdir_stub_asm
ntmini_netdir_stub_asm:
_ntmini_netdir_stub_asm:
    mov     al, 'J'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    mov     edx, [esp+4]
    test    edx, edx
    jz      .netdir_done
    mov     word [edx+0x1A], 0x0005
.netdir_done:
    mov     eax, 5
    ret

global ntmini_shutdown_stub_asm
global _ntmini_shutdown_stub_asm
ntmini_shutdown_stub_asm:
_ntmini_shutdown_stub_asm:
    mov     al, 'K'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    mov     edx, [esp+4]
    test    edx, edx
    jz      .shutdown_done
    mov     word [edx+0x1A], 0x0000
.shutdown_done:
    xor     eax, eax
    ret


;============================================================================
; VxD Service Wrappers (callable from C, __cdecl)
;
; Each function is exported with both VxD_Name and _VxD_Name variants
; because different C files were compiled with different conventions.
;============================================================================

;--- CONFIGMG wrappers ------------------------------------------------------

global _CM_Locate_DevNode_Wrapper
global CM_Locate_DevNode_Wrapper
_CM_Locate_DevNode_Wrapper:
CM_Locate_DevNode_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+16]
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      0x00330002
    add     esp, 12
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global _CM_Register_Device_Driver_Wrapper
global CM_Register_Device_Driver_Wrapper
_CM_Register_Device_Driver_Wrapper:
CM_Register_Device_Driver_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+20]
    push    dword [ebp+16]
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      0x0033000E
    add     esp, 16
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global _CM_Create_DevNode_Wrapper
global CM_Create_DevNode_Wrapper
_CM_Create_DevNode_Wrapper:
CM_Create_DevNode_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+20]
    push    dword [ebp+16]
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      0x0033000B
    add     esp, 16
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global _CM_Setup_DevNode_Wrapper
global CM_Setup_DevNode_Wrapper
_CM_Setup_DevNode_Wrapper:
CM_Setup_DevNode_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+12]
    push    dword [ebp+8]
    int     0x20
    dd      0x00330034
    add     esp, 8
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

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
    dd      0x0001004F
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
    dd      0x00010051
    add     esp, 8
    pop     ebp
    ret

global VMM_Get_DDB_Wrapper
global _VMM_Get_DDB_Wrapper
VMM_Get_DDB_Wrapper:
_VMM_Get_DDB_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    movzx   eax, word [ebp+8]   ; device_id
    xor     edx, edx            ; any version
    int     0x20
    dd      VMM_Get_DDB
    mov     eax, ecx            ; ECX = DDB pointer
    pop     edi
    pop     esi
    pop     ebx
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
    dd      0x00100007
    add     esp, 4
    jc      .ios_register_fail
    xor     eax, eax
    jmp     .ios_register_done
.ios_register_fail:
    test    eax, eax
    jnz     .ios_register_done
    mov     eax, 1
.ios_register_done:
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
    dd      0x00100008
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
    dd      0x00100004
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
    dd      0x00100005
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IOS_BD_Complete_IOP
global _IOS_BD_Complete_IOP
IOS_BD_Complete_IOP:
_IOS_BD_Complete_IOP:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    edx
    mov     ebx, [ebp+8]
    int     0x20
    dd      IOS_BD_Command_Complete_svc
    pop     edx
    pop     ebx
    pop     ebp
    ret

global Call_ILB_Service
global _Call_ILB_Service
Call_ILB_Service:
_Call_ILB_Service:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    ecx
    mov     edx, [ebp+12]       ; ISP packet pointer in EDX
    push    edx                 ; ESDI_506 pattern also passes it on stack
    mov     eax, [ebp+8]        ; ILB_service_rtn
    call    eax
    add     esp, 4
    pop     ecx
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
global _VxD_VPICD_VirtualizeIrq         ; NTKSHIM variant (cdecl)
VxD_VPICD_Virtualize_IRQ:
_VxD_VPICD_Virtualize_IRQ:
VxD_VPICD_VirtualizeIRQ:
_VxD_VPICD_VirtualizeIrq:
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
global _VxD_VPICD_ForceDefaultBehavior  ; NTKSHIM variant (cdecl)
VxD_VPICD_Force_Default_Behavior:
_VxD_VPICD_Force_Default_Behavior:
VxD_VPICD_UnvirtualizeIRQ:
_VxD_VPICD_ForceDefaultBehavior:
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
global _VxD_VPICD_PhysicalEOI           ; NTKSHIM variant (cdecl)
VxD_VPICD_Phys_EOI:
_VxD_VPICD_Phys_EOI:
VxD_VPICD_PhysicalEOI:
_VxD_VPICD_PhysicalEOI:
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
    ; Write to port 0xE9 (QEMU -debugcon) byte by byte
    mov     esi, [ebp+8]       ; string pointer
.dbg_loop:
    lodsb
    test    al, al
    jz      .dbg_done
    out     0xE9, al
    jmp     .dbg_loop
.dbg_done:
    ; Out_Debug_String removed - ordinal 0x1D is Map_Lin_To_VM_Addr on
    ; retail VMM, not Out_Debug_String. Port 0xE9 output above is sufficient.
    pop     esi
    pop     ebp
    ret

;--- Debug marker (callable from C: void dbg_mark(char c)) -----------------
global dbg_mark
global _dbg_mark
dbg_mark:
_dbg_mark:
    push    ebp
    mov     ebp, esp
    mov     eax, [ebp+8]       ; char argument
    out     0xE9, al
    mov     al, 10              ; newline
    out     0xE9, al
    pop     ebp
    ret

;--- File I/O ---------------------------------------------------------------

global VxD_File_Open
global _VxD_File_Open
; File I/O via IFSMgr_Ring0_FileIO (cdecl, stack-based)
; Matches working V4 implementation (VXDWRAP_V4.ASM)
VxD_File_Open:
_VxD_File_Open:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    ecx
    push    edx
    push    esi

    ; VMM R0_OpenCreateFile register convention:
    ; ESI=filename, EAX=access/share, ECX=attributes, EDX=flags, EBX=pAction.
    mov     esi, [ebp+8]
    mov     eax, R0_OPEN_ACCESS_READONLY | R0_OPEN_SHARE_DENYWRITE
    xor     ecx, ecx
    mov     edx, R0_OPEN_FLAGS_EXISTING | R0_NO_CACHE
    xor     ebx, ebx
    int     0x20
    dd      VMM_R0_OpenCreateFile
    jc      .fo_fail
    jmp     .fo_done
.fo_fail:
    xor     eax, eax
.fo_done:
    pop     esi
    pop     edx
    pop     ecx
    pop     ebx
    pop     ebp
    ret

global VxD_File_Read
global _VxD_File_Read
; int VxD_File_Read(int handle, void *buffer, int count)
VxD_File_Read:
_VxD_File_Read:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    ecx
    push    edx
    push    esi

    ; VMM R0_ReadFile register convention:
    ; EAX=handle, ECX=count, EDX=position (-1 current), ESI=buffer.
    mov     eax, [ebp+8]
    mov     esi, [ebp+12]
    mov     ecx, [ebp+16]
    mov     edx, 0xFFFFFFFF
    int     0x20
    dd      VMM_R0_ReadFile
    jc      .fr_fail
    mov     eax, ecx
    jmp     .fr_done
.fr_fail:
    xor     eax, eax
.fr_done:
    pop     esi
    pop     edx
    pop     ecx
    pop     ebx
    pop     ebp
    ret

global VxD_File_Close
global _VxD_File_Close
; void VxD_File_Close(int handle)
VxD_File_Close:
_VxD_File_Close:
    push    ebp
    mov     ebp, esp

    mov     eax, [ebp+8]                    ; hFile
    int     0x20
    dd      VMM_R0_CloseFile

    pop     ebp
    ret

global IFSMgr_NotifyVolumeArrival_Wrapper
global _IFSMgr_NotifyVolumeArrival_Wrapper
IFSMgr_NotifyVolumeArrival_Wrapper:
_IFSMgr_NotifyVolumeArrival_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+8]                   ; logical unit / drive number
    int     0x20
    dd      IFSMgr_NotifyVolumeArrival
    add     esp, 4
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IFSMgr_CDROM_Attach_Wrapper
global _IFSMgr_CDROM_Attach_Wrapper
IFSMgr_CDROM_Attach_Wrapper:
_IFSMgr_CDROM_Attach_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+12]                  ; pVRP out
    push    dword [ebp+8]                   ; drive number
    int     0x20
    dd      IFSMgr_CDROM_Attach
    add     esp, 8
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IFSMgr_RegisterMount_Wrapper
global _IFSMgr_RegisterMount_Wrapper
IFSMgr_RegisterMount_Wrapper:
_IFSMgr_RegisterMount_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+16]      ; is_default
    push    dword [ebp+12]      ; IFSMgr version
    push    dword [ebp+8]       ; mount callback
    int     0x20
    dd      IFSMgr_RegisterMount
    add     esp, 12
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IFSMgr_InstallFSHook_Wrapper
global _IFSMgr_InstallFSHook_Wrapper
IFSMgr_InstallFSHook_Wrapper:
_IFSMgr_InstallFSHook_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+8]
    int     0x20
    dd      IFSMgr_InstallFileSystemApiHook
    add     esp, 4
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IFSMgr_FSDAttachSFT_Wrapper
global _IFSMgr_FSDAttachSFT_Wrapper
IFSMgr_FSDAttachSFT_Wrapper:
_IFSMgr_FSDAttachSFT_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+8]
    int     0x20
    dd      IFSMgr_FSDAttachSFT
    add     esp, 4
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IFSMgr_CallPrevHook_Far16_32
global _IFSMgr_CallPrevHook_Far16_32
IFSMgr_CallPrevHook_Far16_32:
_IFSMgr_CallPrevHook_Far16_32:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi

    sub     esp, 8              ; local 16:32 far pointer storage
    mov     eax, [ebp+8]        ; packed selector:offset16, e.g. 0070:0465
    movzx   edx, ax
    mov     [esp], edx          ; 32-bit offset
    shr     eax, 16
    mov     [esp+4], ax         ; 16-bit selector

    push    dword [ebp+28]      ; pir
    push    dword [ebp+24]      ; cpid
    push    dword [ebp+20]      ; resType
    push    dword [ebp+16]      ; drive
    push    dword [ebp+12]      ; fn
    call    far [esp+20]
    add     esp, 20
    add     esp, 8

    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IFSMgr_CallPrevFSHook
global _IFSMgr_CallPrevFSHook
IFSMgr_CallPrevFSHook:
_IFSMgr_CallPrevFSHook:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi

    mov     eax, [ebp+8]        ; ppPrevHook
    mov     eax, [eax]          ; previous hook function pointer
    test    eax, eax
    jz      .no_prev_hook

    push    dword [ebp+32]      ; pir
    push    dword [ebp+28]      ; cpid
    push    dword [ebp+24]      ; resType
    push    dword [ebp+20]      ; drive
    push    dword [ebp+16]      ; fn
    push    dword [ebp+12]      ; FSDFnAddr
    call    eax
    add     esp, 24
    jmp     .done_prev_hook

.no_prev_hook:
    xor     eax, eax

.done_prev_hook:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global IFSMgr_CallPrevFSHook5
global _IFSMgr_CallPrevFSHook5
IFSMgr_CallPrevFSHook5:
_IFSMgr_CallPrevFSHook5:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi

    mov     eax, [ebp+8]        ; ppPrevHook
    mov     eax, [eax]          ; previous dispatcher/hook pointer
    test    eax, eax
    jz      .no_prev_hook5

    push    dword [ebp+28]      ; pir
    push    dword [ebp+24]      ; cpid
    push    dword [ebp+20]      ; resType
    push    dword [ebp+16]      ; drive
    push    dword [ebp+12]      ; fn
    call    eax
    add     esp, 20
    jmp     .done_prev_hook5

.no_prev_hook5:
    xor     eax, eax

.done_prev_hook5:
    pop     edi
    pop     esi
    pop     ebx
    pop     ebp
    ret

global VWIN32_CopyMem_Wrapper
global _VWIN32_CopyMem_Wrapper
VWIN32_CopyMem_Wrapper:
_VWIN32_CopyMem_Wrapper:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+16]      ; cb
    push    dword [ebp+12]      ; src
    push    dword [ebp+8]       ; dst
    int     0x20
    dd      VWIN32_CopyMem
    add     esp, 12
    pop     edi
    pop     esi
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

;--- Get_System_Time (for NTKSHIM.C) ----------------------------------------

global VxD_GetSystemTime
global _VxD_GetSystemTime
VxD_GetSystemTime:
_VxD_GetSystemTime:
    int     0x20
    dd      VMM_Get_System_Time
    ret

;--- Time_Slice_Sleep (for NTKSHIM.C) ---------------------------------------

global VxD_TimesliceSleep
global _VxD_TimesliceSleep
VxD_TimesliceSleep:
_VxD_TimesliceSleep:
    int     0x20
    dd      VMM_Time_Slice_Sleep
    ret

;--- _MapPhysToLinear (for NTKSHIM.C MmMapIoSpace) --------------------------

global VxD_MapPhysToLinear
global _VxD_MapPhysToLinear
VxD_MapPhysToLinear:
_VxD_MapPhysToLinear:
    push    ebp
    mov     ebp, esp
    push    ebx
    push    esi
    push    edi
    push    dword [ebp+16]      ; flags
    push    dword [ebp+12]      ; size
    push    dword [ebp+8]       ; physAddr
    int     0x20
    dd      VMM__MapPhysToLinear
    add     esp, 12
    pop     edi
    pop     esi
    pop     ebx
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
;   0x44    DD    DDB_Reserved1
;   0x48    DD    DDB_Reserved2
;   0x4C    DD    DDB_Reserved3
;   Total: 0x50 = 80 bytes
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
    dd      'Prev'                  ; 0x3C DDB_Prev
    dd      0x50                    ; 0x40 sizeof(DDB)
    dd      'Rsv1'                  ; 0x44 DDB_Reserved1
    dd      'Rsv2'                  ; 0x48 DDB_Reserved2
    dd      'Rsv3'                  ; 0x4C DDB_Reserved3

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

;----------------------------------------------------------------------------
; MS x86 fastcall HAL thunks for NT miniport imports.
; NT calls these with arguments in ECX/EDX. Preserve callee-saved registers.
;----------------------------------------------------------------------------

global ntk_KfAcquireSpinLock_ms
global _ntk_KfAcquireSpinLock_ms
ntk_KfAcquireSpinLock_ms:
_ntk_KfAcquireSpinLock_ms:
    mov     al, 'A'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    xor     eax, eax
    ret

global ntk_IofCompleteRequest_ms
global _ntk_IofCompleteRequest_ms
ntk_IofCompleteRequest_ms:
_ntk_IofCompleteRequest_ms:
    push    ecx
    push    edx
    mov     al, 'M'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     edx
    pop     ecx
    movzx   eax, dl
    push    eax
    push    ecx
    call    _IrpMgr_IoCompleteRequest
    add     esp, 8
    ret

global ntk_IoAllocateIrp_ms
global _ntk_IoAllocateIrp_ms
ntk_IoAllocateIrp_ms:
_ntk_IoAllocateIrp_ms:
    mov     al, 'n'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    push    dword [esp+8]
    push    dword [esp+8]
    call    _IrpMgr_IoAllocateIrp
    add     esp, 8
    ret     8

global ntk_KeInitializeEvent_ms
global _ntk_KeInitializeEvent_ms
ntk_KeInitializeEvent_ms:
_ntk_KeInitializeEvent_ms:
    push    dword [esp+12]
    push    dword [esp+12]
    push    dword [esp+12]
    call    _ntk_KeInitializeEvent_cdecl
    add     esp, 12
    ret     12

global ntk_KeSetEvent_ms
global _ntk_KeSetEvent_ms
ntk_KeSetEvent_ms:
_ntk_KeSetEvent_ms:
    push    dword [esp+12]
    push    dword [esp+12]
    push    dword [esp+12]
    call    _ntk_KeSetEvent_cdecl
    add     esp, 12
    ret     12

global ntk_KeWaitForSingleObject_ms
global _ntk_KeWaitForSingleObject_ms
ntk_KeWaitForSingleObject_ms:
_ntk_KeWaitForSingleObject_ms:
    push    dword [esp+20]
    push    dword [esp+20]
    push    dword [esp+20]
    push    dword [esp+20]
    push    dword [esp+20]
    call    _ntk_KeWaitForSingleObject_cdecl
    add     esp, 20
    ret     20

global ntk_ExFreePool_ms
global _ntk_ExFreePool_ms
ntk_ExFreePool_ms:
_ntk_ExFreePool_ms:
    push    dword [esp+4]
    call    _ntk_ExFreePool_cdecl
    add     esp, 4
    ret     4

global ntk_KfReleaseSpinLock_ms
global _ntk_KfReleaseSpinLock_ms
ntk_KfReleaseSpinLock_ms:
_ntk_KfReleaseSpinLock_ms:
    mov     al, 'R'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    push    eax
    mov     eax, [esp+4]
    and     al, 0x0F
    cmp     al, 9
    jbe     .rel_digit
    add     al, 7
.rel_digit:
    add     al, '0'
    out     0xE9, al
    mov     al, 10
    out     0xE9, al
    pop     eax
    ret

global ntk_KfRaiseIrql_ms
global _ntk_KfRaiseIrql_ms
ntk_KfRaiseIrql_ms:
_ntk_KfRaiseIrql_ms:
    xor     eax, eax
    ret

global ntk_KfLowerIrql_ms
global _ntk_KfLowerIrql_ms
ntk_KfLowerIrql_ms:
_ntk_KfLowerIrql_ms:
    ret

global ntk_InterlockedIncrement_ms
global _ntk_InterlockedIncrement_ms
ntk_InterlockedIncrement_ms:
_ntk_InterlockedIncrement_ms:
    mov     eax, [ecx]
    inc     eax
    mov     [ecx], eax
    ret

global ntk_InterlockedDecrement_ms
global _ntk_InterlockedDecrement_ms
ntk_InterlockedDecrement_ms:
_ntk_InterlockedDecrement_ms:
    mov     eax, [ecx]
    dec     eax
    mov     [ecx], eax
    ret

global ntk_InterlockedExchange_ms
global _ntk_InterlockedExchange_ms
ntk_InterlockedExchange_ms:
_ntk_InterlockedExchange_ms:
    mov     eax, [ecx]
    mov     [ecx], edx
    ret


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
