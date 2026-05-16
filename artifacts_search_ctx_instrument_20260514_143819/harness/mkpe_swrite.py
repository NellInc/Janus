import struct, sys
out, outfile = sys.argv[1], sys.argv[2]
IMAGE_BASE=0x400000; FILE_ALIGN=0x200; SECT_ALIGN=0x1000

def align(x,a): return (x+a-1)//a*a
def u16(x): return struct.pack('<H',x)
def u32(x): return struct.pack('<I',x & 0xffffffff)
text_rva=0x1000; data_rva=0x3000; idata_rva=0x6000

data=bytearray()
def data_add(blob, align_to=4):
    off=len(data); data.extend(blob)
    while len(data)%align_to: data.append(0)
    return off
def cstr(s): return data_add(s.encode('ascii')+b'\0')
def msg(s):
    off=data_add(s.encode('ascii')+b'\r\n')
    return off, len(s)+2

def raw(s):
    off=data_add(s.encode('ascii'))
    return off, len(s)

out_fn_off=cstr(outfile)
write_fn_off=cstr('D:\\WRITETEST.TXT')
marker_str='SWRITE_MARKER: Written through IOS calldown chain OK\r\n'
marker_off=data_add(marker_str.encode('ascii'))
MARKER_LEN=len(marker_str)  # 54 bytes

m_start=msg('SWRITE_START')
m_wcreate=raw('WCREATE_HANDLE=0x')
m_wcreate_le=raw(' WCREATE_LE=0x')
m_write_ok=raw('WRITE_OK=0x')
m_write_n=raw(' WRITE_N=0x')
m_write_le=raw(' WRITE_LE=0x')
m_rcreate=raw('RCREATE_HANDLE=0x')
m_rcreate_le=raw(' RCREATE_LE=0x')
m_read_ok=raw('READ_OK=0x')
m_read_n=raw(' READ_N=0x')
m_read_le=raw(' READ_LE=0x')
m_text_begin=msg('TEXT_BEGIN')
m_text_end=msg('TEXT_END')
m_write_skip=msg('WRITE_SKIPPED_CREATE_FAILED')
m_read_skip=msg('READ_SKIPPED_CREATE_FAILED')
m_ops_done=msg('SWRITE_OPS_DONE')
m_end=msg('SWRITE_END')
m_fail_out=msg('FAIL_OUT')
newline=raw('\r\n')
ticks=[msg(f'TICK_{i:03d}') for i in range(10,121,10)]

hout_off=data_add(b'\0'*4)
hwrite_off=data_add(b'\0'*4)
hread_off=data_add(b'\0'*4)
written_off=data_add(b'\0'*4)
writeok_off=data_add(b'\0'*4)
writeerr_off=data_add(b'\0'*4)
wcrerr_off=data_add(b'\0'*4)
rcrerr_off=data_add(b'\0'*4)
readok_off=data_add(b'\0'*4)
nread_off=data_add(b'\0'*4)
readerr_off=data_add(b'\0'*4)
hexwork_off=data_add(b'\0'*4)
hexbuf_off=data_add(b'00000000', align_to=4)
readbuf_off=data_add(b'\0'*257, align_to=4)

idata=bytearray()
def add(blob): off=len(idata); idata.extend(blob); return off
funcs=['CreateFileA','WriteFile','CloseHandle','ExitProcess','GetFileAttributesA','GetLastError','SetLastError','ReadFile','FlushFileBuffers','Sleep']
desc_off=add(b'\0'*40)
int_off=add(b'\0'*(4*(len(funcs)+1)))
iat_off=add(b'\0'*(4*(len(funcs)+1)))
dll_off=add(b'KERNEL32.DLL\0')
name_rvas=[]
for fn in funcs:
    while len(idata)%2: idata.append(0)
    name_rvas.append(idata_rva+add(u16(0)+fn.encode('ascii')+b'\0'))
while len(idata)%4: idata.append(0)
for i,rva in enumerate(name_rvas):
    struct.pack_into('<I', idata, int_off+i*4, rva)
    struct.pack_into('<I', idata, iat_off+i*4, rva)
struct.pack_into('<IIIII', idata, desc_off, idata_rva+int_off,0,0,idata_rva+dll_off,idata_rva+iat_off)

code=bytearray(); rel_patches=[]; labels={}
def emit(b): code.extend(b)
def label(n): labels[n]=len(code)
def patch_rel32(n): rel_patches.append((len(code), n)); emit(b'\0\0\0\0')
def push8(v): emit(b'\x6a'+struct.pack('b',v))
def push(v): emit(b'\x68'+u32(v))
def push_mem(va): emit(b'\xff\x35'+u32(va))
def push_eax(): emit(b'\x50')
def call_iat(fn): emit(b'\xff\x15'+u32(IMAGE_BASE+idata_rva+iat_off+funcs.index(fn)*4))
def mov_mem_eax(va): emit(b'\xa3'+u32(va))
def mov_eax_mem(va): emit(b'\xa1'+u32(va))
def cmp_eax(v): emit(b'\x3d'+u32(v))
def je(n): emit(b'\x0f\x84'); patch_rel32(n)
def jmp(n): emit(b'\xe9'); patch_rel32(n)
va=lambda off: IMAGE_BASE+data_rva+off

def write_static(pair, flush=True):
    off, ln = pair
    push8(0); push(va(written_off)); push(ln); push(va(off)); push_mem(va(hout_off)); call_iat('WriteFile')
    if flush:
        push_mem(va(hout_off)); call_iat('FlushFileBuffers')

def write_readbuf():
    push8(0); push(va(written_off)); push_mem(va(nread_off)); push(va(readbuf_off)); push_mem(va(hout_off)); call_iat('WriteFile')
    push_mem(va(hout_off)); call_iat('FlushFileBuffers')

def write_hex_from(mem_off):
    mov_eax_mem(va(mem_off)); mov_mem_eax(va(hexwork_off))
    emit(b'\xbe'+u32(va(hexwork_off)))
    emit(b'\xbf'+u32(va(hexbuf_off)))
    emit(b'\xb9'+u32(8))
    loop_start=len(code)
    emit(b'\x8b\x06')          # mov eax,[esi]
    emit(b'\xc1\xe8\x1c')      # shr eax,28
    emit(b'\x3c\x09')          # cmp al,9
    emit(b'\x76\x04')          # jbe +4
    emit(b'\x04\x37')          # add al,55
    emit(b'\xeb\x02')          # jmp +2
    emit(b'\x04\x30')          # add al,48
    emit(b'\x88\x07')          # mov [edi],al
    emit(b'\x47')              # inc edi
    emit(b'\xc1\x26\x04')      # shl dword ptr [esi],4
    emit(b'\x49')              # dec ecx
    rel=loop_start-(len(code)+2)
    emit(b'\x75'+struct.pack('b', rel))
    push8(0); push(va(written_off)); push(8); push(va(hexbuf_off)); push_mem(va(hout_off)); call_iat('WriteFile')

# --- Open log file ---
push8(0); push(0x80); push8(2); push8(0); push8(3); push(0x40000000); push(va(out_fn_off)); call_iat('CreateFileA')
mov_mem_eax(va(hout_off)); cmp_eax(0xffffffff); je('fail_out')
write_static(m_start)

# --- Create D:\WRITETEST.TXT for writing (GENERIC_WRITE, CREATE_ALWAYS) ---
push8(0); call_iat('SetLastError')
push8(0); push(0x80); push8(2); push8(0); push8(3); push(0x40000000); push(va(write_fn_off)); call_iat('CreateFileA')
mov_mem_eax(va(hwrite_off)); call_iat('GetLastError'); mov_mem_eax(va(wcrerr_off))
write_static(m_wcreate, flush=False); write_hex_from(hwrite_off); write_static(m_wcreate_le, flush=False); write_hex_from(wcrerr_off); write_static(newline)
mov_eax_mem(va(hwrite_off)); cmp_eax(0xffffffff); je('skip_write')

# --- Write 54 bytes of marker content ---
push8(0); call_iat('SetLastError')
push8(0); push(va(written_off)); push(MARKER_LEN); push(va(marker_off)); push_mem(va(hwrite_off)); call_iat('WriteFile')
mov_mem_eax(va(writeok_off)); call_iat('GetLastError'); mov_mem_eax(va(writeerr_off))
write_static(m_write_ok, flush=False); write_hex_from(writeok_off); write_static(m_write_n, flush=False); write_hex_from(written_off); write_static(m_write_le, flush=False); write_hex_from(writeerr_off); write_static(newline)

# --- Flush and close write handle ---
push_mem(va(hwrite_off)); call_iat('FlushFileBuffers')
push_mem(va(hwrite_off)); call_iat('CloseHandle')
jmp('do_read')

label('skip_write')
write_static(m_write_skip)
jmp('after_ops')

label('do_read')
# --- Re-open D:\WRITETEST.TXT for reading (GENERIC_READ, OPEN_EXISTING) ---
push8(0); call_iat('SetLastError')
push8(0); push(0x80); push8(3); push8(0); push8(1); push(0x80000000); push(va(write_fn_off)); call_iat('CreateFileA')
mov_mem_eax(va(hread_off)); call_iat('GetLastError'); mov_mem_eax(va(rcrerr_off))
write_static(m_rcreate, flush=False); write_hex_from(hread_off); write_static(m_rcreate_le, flush=False); write_hex_from(rcrerr_off); write_static(newline)
mov_eax_mem(va(hread_off)); cmp_eax(0xffffffff); je('skip_read')

# --- Read back up to 64 bytes ---
push8(0); call_iat('SetLastError')
push8(0); push(va(nread_off)); push(64); push(va(readbuf_off)); push_mem(va(hread_off)); call_iat('ReadFile')
mov_mem_eax(va(readok_off)); call_iat('GetLastError'); mov_mem_eax(va(readerr_off))
write_static(m_read_ok, flush=False); write_hex_from(readok_off); write_static(m_read_n, flush=False); write_hex_from(nread_off); write_static(m_read_le, flush=False); write_hex_from(readerr_off); write_static(newline)
write_static(m_text_begin); write_readbuf(); write_static(newline); write_static(m_text_end)

# --- Close read handle ---
push_mem(va(hread_off)); call_iat('CloseHandle')
jmp('after_ops')

label('skip_read')
write_static(m_read_skip)

label('after_ops')
write_static(m_ops_done)
for t in ticks:
    push(100); call_iat('Sleep')
    write_static(t)
write_static(m_end)
push_mem(va(hout_off)); call_iat('CloseHandle')
push8(0); call_iat('ExitProcess')

label('fail_out')
push8(1); call_iat('ExitProcess')

for pos,n in rel_patches:
    target=labels[n]
    delta=target-(pos+4)
    struct.pack_into('<i', code, pos, delta)

headers_size=0x200
text_raw_size=align(len(code),FILE_ALIGN); data_raw_size=align(len(data),FILE_ALIGN); idata_raw_size=align(len(idata),FILE_ALIGN)
text_raw_ptr=headers_size; data_raw_ptr=text_raw_ptr+text_raw_size; idata_raw_ptr=data_raw_ptr+data_raw_size
size_image=align(idata_rva+align(len(idata),SECT_ALIGN),SECT_ALIGN)
mz=bytearray(0x80); mz[0:2]=b'MZ'; struct.pack_into('<I', mz, 0x3c, 0x80)
pe=bytearray(); pe+=b'PE\0\0'; pe+=struct.pack('<HHIIIHH',0x14c,3,0,0,0,0xE0,0x010F)
opt=bytearray(); opt+=u16(0x10b)+b'\x08\x00'; opt+=u32(text_raw_size)+u32(data_raw_size)+u32(0); opt+=u32(text_rva)+u32(text_rva)+u32(data_rva); opt+=u32(IMAGE_BASE)+u32(SECT_ALIGN)+u32(FILE_ALIGN); opt+=u16(4)+u16(0)+u16(0)+u16(0)+u16(4)+u16(0); opt+=u32(0)+u32(size_image)+u32(headers_size)+u32(0); opt+=u16(2)+u16(0); opt+=u32(0x100000)+u32(0x1000)+u32(0x100000)+u32(0x1000); opt+=u32(0)+u32(16)
for i in range(16): opt += (u32(idata_rva+desc_off)+u32(len(idata))) if i==1 else (u32(0)+u32(0))
assert len(opt)==0xE0
pe+=opt
def sect(name,vsize,rva,rawsize,rawptr,chars): return name.ljust(8,b'\0')+u32(vsize)+u32(rva)+u32(rawsize)+u32(rawptr)+u32(0)+u32(0)+u16(0)+u16(0)+u32(chars)
pe+=sect(b'.text',len(code),text_rva,text_raw_size,text_raw_ptr,0x60000020)
pe+=sect(b'.data',len(data),data_rva,data_raw_size,data_raw_ptr,0xC0000040)
pe+=sect(b'.idata',len(idata),idata_rva,idata_raw_size,idata_raw_ptr,0xC0000040)
image=(mz+pe).ljust(headers_size,b'\0') + code.ljust(text_raw_size,b'\0') + bytes(data).ljust(data_raw_size,b'\0') + bytes(idata).ljust(idata_raw_size,b'\0')
open(out,'wb').write(image)
print(f'out={out}')
print(f'outfile={outfile}')
print(f'bytes={len(image)}')
print(f'marker_len={MARKER_LEN}')
print(f'text_len={len(code)} data_len={len(data)} idata_len={len(idata)}')
print('imports=' + '|'.join(funcs))
