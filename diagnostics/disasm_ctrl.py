#!/usr/bin/env python3
"""Disassemble and annotate the V5REAL.VXD control procedure."""

import struct, sys

# Read raw bytes from file offset 0x1644 (page data + 0x44), ~200 bytes
with open("V5REAL.VXD", "rb") as f:
    # First read the DDB at 0x1600 to confirm key fields
    f.seek(0x1600)
    ddb = f.read(0x50)
    print("=== DDB (Device Descriptor Block) at file offset 0x1600 ===")
    print(f"  DDB_Next:          0x{struct.unpack_from('<I', ddb, 0x00)[0]:08X}")
    print(f"  DDB_SDK_Version:   0x{struct.unpack_from('<H', ddb, 0x04)[0]:04X}")
    print(f"  DDB_Req_Device_Num:0x{struct.unpack_from('<H', ddb, 0x06)[0]:04X}")
    print(f"  DDB_Dev_Major_Ver: {ddb[0x08]}")
    print(f"  DDB_Dev_Minor_Ver: {ddb[0x09]}")
    print(f"  DDB_Flags:         0x{struct.unpack_from('<H', ddb, 0x0A)[0]:04X}")
    name_bytes = ddb[0x0C:0x14]
    print(f"  DDB_Name:          {name_bytes!r} -> '{name_bytes.decode('ascii', errors='replace').rstrip()}'")
    print(f"  DDB_Init_Order:    0x{struct.unpack_from('<I', ddb, 0x14)[0]:08X}")
    ctrl_off = struct.unpack_from('<I', ddb, 0x18)[0]
    print(f"  DDB_Control_Proc:  0x{ctrl_off:08X}")
    v86_api = struct.unpack_from('<I', ddb, 0x1C)[0]
    pm_api = struct.unpack_from('<I', ddb, 0x20)[0]
    print(f"  DDB_V86_API_Proc:  0x{v86_api:08X}")
    print(f"  DDB_PM_API_Proc:   0x{pm_api:08X}")

    # Now read control procedure bytes
    f.seek(0x1644)
    code = f.read(300)

print(f"\n=== Control Procedure at file offset 0x1644 (object offset 0x44) ===")
print(f"  Raw bytes ({len(code)}): {code[:64].hex()}")
print()

# Simple x86 32-bit disassembler using capstone if available, else manual
try:
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True

    # Known VxD service call patterns:
    # INT 20h followed by 4-byte service ID = VxD service call (VMMCall/VxDCall)
    # The 4 bytes after INT 20h encode: service_num(16 bits) | device_id(16 bits)

    print("=== Disassembly ===")
    instructions = list(md.disasm(code, 0x44))

    i = 0
    while i < len(instructions) and i < 80:
        insn = instructions[i]
        raw = code[insn.address - 0x44 : insn.address - 0x44 + insn.size]
        line = f"  {insn.address:04X}: {raw.hex():<20s} {insn.mnemonic:<8s} {insn.op_str}"

        # Annotate known patterns
        annotation = ""

        # Check for INT 20h (VxD service call)
        if insn.mnemonic == "int" and "0x20" in insn.op_str:
            # Next 4 bytes are the service descriptor
            svc_offset = insn.address - 0x44 + insn.size
            if svc_offset + 4 <= len(code):
                svc_dword = struct.unpack_from('<I', code, svc_offset)[0]
                svc_num = svc_dword & 0xFFFF
                dev_id = (svc_dword >> 16) & 0xFFFF

                # Known device IDs
                dev_names = {
                    0x0001: "VMM",
                    0x0002: "Debug",
                    0x0003: "VPICD",
                    0x0004: "VDMAD",
                    0x0005: "VTD",
                    0x0006: "V86MMGR",
                    0x0007: "PageSwap",
                    0x0008: "Pager",
                    0x0009: "Reboot",
                    0x000A: "VDD",
                    0x000C: "VMD",
                    0x000D: "VKD",
                    0x000E: "VCD",
                    0x000F: "VPD",
                    0x0010: "IOS",      # I/O Supervisor
                    0x0011: "VMCPD",
                    0x0012: "EBIOS",
                    0x0014: "VNETBIOS",
                    0x0015: "DOSMGR",
                    0x0017: "Shell",
                    0x0018: "VMPoll",
                    0x001A: "Dosnet",
                    0x0020: "Int13",
                    0x0021: "PAGEFILE",
                    0x0026: "VCOMM",
                    0x0027: "SPOOLER",
                    0x0028: "Win32s",
                    0x002A: "VXDLDR",
                    0x0033: "CONFIGMG",
                    0x0034: "DWCFGMG",
                    0x0040: "IFSMgr",
                    0x0048: "PERF",
                    0x0484: "IFSMGR",
                }

                # Known VMM services
                vmm_services = {
                    0x0001: "Get_VMM_Version",
                    0x0002: "Get_Cur_VM_Handle",
                    0x0003: "Test_Cur_VM_Handle",
                    0x0004: "Get_Sys_VM_Handle",
                    0x0010: "Get_Profile_String",
                    0x0019: "Hook_Device_Service",
                    0x0033: "Allocate_Device_CB_Area",
                    0x0046: "Get_Machine_Info",
                    0x0048: "Fatal_Error_Handler",
                    0x004E: "Hook_Device_V86_API",
                    0x009C: "_HeapAllocate",
                    0x00A0: "_HeapFree",
                }

                # Known IOS services
                ios_services = {
                    0x0000: "IOS_Get_Version",
                    0x0001: "IOS_Register",
                    0x0003: "IOS_Requestor_Service",
                    0x0004: "IOS_Exclusive_Access",
                    0x0005: "IOS_Send_Command",
                    0x0007: "IOS_BD_Register_Device",
                    0x0008: "IOS_Find_Int13_Drive",
                    0x0009: "IOS_Get_Device_List",
                    0x000A: "IOS_SendCommand",
                    0x000B: "IOS_BD_Command_Complete",
                    0x000F: "IOS_Register_VSD",
                    0x0010: "IOS_Register_Port_Driver",
                }

                dev_name = dev_names.get(dev_id, f"Dev_{dev_id:04X}")
                if dev_id == 0x0001:
                    svc_name = vmm_services.get(svc_num, f"Svc_{svc_num:04X}")
                elif dev_id == 0x0010:
                    svc_name = ios_services.get(svc_num, f"Svc_{svc_num:04X}")
                else:
                    svc_name = f"Svc_{svc_num:04X}"

                annotation = f"  ; VxDCall {dev_name}.{svc_name} (0x{svc_dword:08X})"

        # Annotate CMP EAX with message IDs
        if insn.mnemonic == "cmp" and "eax" in insn.op_str:
            if "0" in insn.op_str:
                try:
                    # Extract immediate value
                    parts = insn.op_str.split(",")
                    if len(parts) == 2:
                        val_str = parts[1].strip()
                        val = int(val_str, 0)
                        msg_names = {
                            0x00: "Sys_Critical_Init",
                            0x01: "Device_Init",
                            0x02: "Init_Complete",
                            0x03: "Sys_Critical_Exit",
                            0x04: "Device_Exit (never in W98)",
                            0x05: "Create_VM",
                            0x06: "VM_Critical_Init",
                            0x07: "VM_Init",
                            0x08: "VM_Terminate",
                            0x09: "VM_Not_Executeable",
                            0x0B: "Destroy_VM",
                            0x0D: "VM_Suspend",
                            0x0E: "VM_Resume",
                            0x10: "Set_Device_Focus",
                            0x11: "Begin_Message_Mode",
                            0x12: "End_Message_Mode",
                            0x13: "Reboot_Processor",
                            0x17: "Set_Device_Focus",
                            0x1B: "Power_Event",
                            0x1D: "Sys_Dynamic_Device_Init",
                            0x1E: "Sys_Dynamic_Device_Exit",
                            0x23: "PnP_New_DevNode",
                        }
                        if val in msg_names:
                            annotation = f"  ; msg={msg_names[val]}"
                except:
                    pass

        # CLC / STC annotations
        if insn.mnemonic == "clc":
            annotation = "  ; return SUCCESS"
        elif insn.mnemonic == "stc":
            annotation = "  ; return FAILURE"

        print(line + annotation)
        i += 1

    # Also dump hex of area around potential VxDCall sites
    print("\n=== Raw hex dump of control proc (first 200 bytes) ===")
    for off in range(0, min(200, len(code)), 16):
        hex_str = ' '.join(f'{code[off+j]:02X}' if off+j < len(code) else '  ' for j in range(16))
        ascii_str = ''.join(chr(code[off+j]) if 32 <= code[off+j] < 127 else '.' for j in range(16) if off+j < len(code))
        print(f"  {0x44+off:04X}: {hex_str}  {ascii_str}")

except ImportError:
    print("capstone not available, using ndisasm via subprocess...")
    import subprocess
    # Write raw bytes to temp file
    with open("/tmp/v5real_ctrl.bin", "wb") as f:
        f.write(code)
    result = subprocess.run(
        ["ndisasm", "-b", "32", "-o", "0x44", "/tmp/v5real_ctrl.bin"],
        capture_output=True, text=True
    )
    if result.returncode == 0:
        print(result.stdout[:5000])
    else:
        print(f"ndisasm failed: {result.stderr}")
        print("Falling back to raw hex dump:")
        for off in range(0, min(200, len(code)), 16):
            hex_str = ' '.join(f'{code[off+j]:02X}' if off+j < len(code) else '  ' for j in range(16))
            ascii_str = ''.join(chr(code[off+j]) if 32 <= code[off+j] < 127 else '.' for j in range(16) if off+j < len(code))
            print(f"  {0x44+off:04X}: {hex_str}  {ascii_str}")
