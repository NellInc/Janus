/* NTMINI_V3.C - Version 3: Load NT4 atapi.sys PE image
 *
 * Tests:
 *   1. Create C:\NTMINI.LOG and write progress to it
 *   2. Open and read C:\WINDOWS\SYSTEM\ATAPI.SYS into heap buffer
 *   3. Call pe_load_image() with stub ScsiPort function table
 *   4. Log results (entry point, success/failure)
 *
 * ALL paths return 0 (success). Boot is never blocked.
 *
 * Build with: wcc386 -bt=windows -3s -s -zl -zu -d0 NTMINI_V3.C
 * Link with:  VXDWRAP_V3.ASM, PELOAD.obj
 */

/* ================================================================
 * Type definitions
 * ================================================================ */

typedef unsigned char       UCHAR;
typedef unsigned short      USHORT;
typedef unsigned long       ULONG;
typedef long                LONG;
typedef void               *PVOID;

/* ================================================================
 * Import function table entry (must match PELOAD.C)
 * ================================================================ */

typedef struct {
    const char *name;
    void       *func;
} IMPORT_FUNC_ENTRY;

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

extern void VxD_Debug_Printf(const char *msg);
extern void *VxD_HeapAllocate(unsigned long size, unsigned long flags);
extern void VxD_HeapFree(void *ptr, unsigned long flags);
extern int VxD_File_Open(const char *filename);
extern int VxD_File_Create(const char *filename);
extern int VxD_File_Read(int handle, void *buffer, int count);
extern int VxD_File_Write(int handle, const void *buffer, int count);
extern void VxD_File_Close(int handle);
extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void VxD_PageFree(PVOID addr);
extern ULONG diag_regs[4];  /* EAX, EBX, ECX, EDX from last open call */

/* ================================================================
 * PELOAD.C externals
 * ================================================================ */

extern int pe_load_image(
    const void *pe_data,
    unsigned long pe_size,
    const IMPORT_FUNC_ENTRY *func_table,
    void **out_entry,
    void **out_base);

extern void pe_unload_image(void *image_base);

/* ================================================================
 * Simple string helpers (no libc available)
 * ================================================================ */

static ULONG my_strlen(const char *s)
{
    ULONG n = 0;
    while (*s++) n++;
    return n;
}

static void my_strcpy(char *dst, const char *src)
{
    while (*src) *dst++ = *src++;
    *dst = 0;
}

/* Convert unsigned long to hex string */
static void ulong_to_hex(ULONG val, char *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 7; i >= 0; i--) {
        buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    }
    buf[10] = 0;
}

/* Convert unsigned long to decimal string */
static void ulong_to_dec(ULONG val, char *buf)
{
    char tmp[12];
    int i = 0;
    int j;

    if (val == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return;
    }
    while (val > 0) {
        tmp[i++] = (char)('0' + (val % 10));
        val /= 10;
    }
    for (j = 0; j < i; j++) {
        buf[j] = tmp[i - 1 - j];
    }
    buf[i] = 0;
}

/* Convert signed int to decimal string */
static void int_to_dec(int val, char *buf)
{
    if (val < 0) {
        *buf++ = '-';
        val = -val;
    }
    ulong_to_dec((ULONG)val, buf);
}

/* ================================================================
 * Log file writing
 * ================================================================ */

static int g_log_handle = -1;

static void log_write(const char *msg)
{
    ULONG len;

    /* Always write to debug output too */
    VxD_Debug_Printf(msg);

    if (g_log_handle < 0) return;

    len = my_strlen(msg);
    if (len > 0) {
        VxD_File_Write(g_log_handle, msg, (int)len);
    }
}

static void log_str_hex(const char *prefix, ULONG val, const char *suffix)
{
    char hex[12];
    ulong_to_hex(val, hex);
    log_write(prefix);
    log_write(hex);
    log_write(suffix);
}

static void log_str_dec(const char *prefix, ULONG val, const char *suffix)
{
    char dec[12];
    ulong_to_dec(val, dec);
    log_write(prefix);
    log_write(dec);
    log_write(suffix);
}

static void log_str_int(const char *prefix, int val, const char *suffix)
{
    char dec[14];
    int_to_dec(val, dec);
    log_write(prefix);
    log_write(dec);
    log_write(suffix);
}

/* ================================================================
 * ScsiPort stub functions - all return 0
 * ================================================================ */

static ULONG stub_ScsiPortGetDeviceBase(void) { return 0; }
static ULONG stub_ScsiPortFreeDeviceBase(void) { return 0; }
static ULONG stub_ScsiPortReadPortUchar(void) { return 0; }
static ULONG stub_ScsiPortReadPortUshort(void) { return 0; }
static ULONG stub_ScsiPortReadPortUlong(void) { return 0; }
static ULONG stub_ScsiPortReadPortBufferUshort(void) { return 0; }
static ULONG stub_ScsiPortReadPortBufferUlong(void) { return 0; }
static ULONG stub_ScsiPortWritePortUchar(void) { return 0; }
static ULONG stub_ScsiPortWritePortBufferUshort(void) { return 0; }
static ULONG stub_ScsiPortWritePortBufferUlong(void) { return 0; }
static ULONG stub_ScsiPortWritePortUlong(void) { return 0; }
static ULONG stub_ScsiPortStallExecution(void) { return 0; }
static ULONG stub_ScsiPortMoveMemory(void) { return 0; }
static ULONG stub_ScsiPortGetPhysicalAddress(void) { return 0; }
static ULONG stub_ScsiPortGetUncachedExtension(void) { return 0; }
static ULONG stub_ScsiPortNotification(void) { return 0; }
static ULONG stub_ScsiPortCompleteRequest(void) { return 0; }
static ULONG stub_ScsiPortLogError(void) { return 0; }
static ULONG stub_ScsiPortInitialize(void) { return 0; }
static ULONG stub_ScsiPortConvertUlongToPhysicalAddress(void) { return 0; }
static ULONG stub_ScsiPortGetBusData(void) { return 0; }
static ULONG stub_ScsiPortSetBusDataByOffset(void) { return 0; }

/* 22 entries + NULL terminator */
static const IMPORT_FUNC_ENTRY scsiport_stubs[] = {
    { "ScsiPortGetDeviceBase",                (void *)stub_ScsiPortGetDeviceBase },
    { "ScsiPortFreeDeviceBase",               (void *)stub_ScsiPortFreeDeviceBase },
    { "ScsiPortReadPortUchar",                (void *)stub_ScsiPortReadPortUchar },
    { "ScsiPortReadPortUshort",               (void *)stub_ScsiPortReadPortUshort },
    { "ScsiPortReadPortUlong",                (void *)stub_ScsiPortReadPortUlong },
    { "ScsiPortReadPortBufferUshort",         (void *)stub_ScsiPortReadPortBufferUshort },
    { "ScsiPortReadPortBufferUlong",          (void *)stub_ScsiPortReadPortBufferUlong },
    { "ScsiPortWritePortUchar",               (void *)stub_ScsiPortWritePortUchar },
    { "ScsiPortWritePortBufferUshort",        (void *)stub_ScsiPortWritePortBufferUshort },
    { "ScsiPortWritePortBufferUlong",         (void *)stub_ScsiPortWritePortBufferUlong },
    { "ScsiPortWritePortUlong",               (void *)stub_ScsiPortWritePortUlong },
    { "ScsiPortStallExecution",               (void *)stub_ScsiPortStallExecution },
    { "ScsiPortMoveMemory",                   (void *)stub_ScsiPortMoveMemory },
    { "ScsiPortGetPhysicalAddress",           (void *)stub_ScsiPortGetPhysicalAddress },
    { "ScsiPortGetUncachedExtension",         (void *)stub_ScsiPortGetUncachedExtension },
    { "ScsiPortNotification",                 (void *)stub_ScsiPortNotification },
    { "ScsiPortCompleteRequest",              (void *)stub_ScsiPortCompleteRequest },
    { "ScsiPortLogError",                     (void *)stub_ScsiPortLogError },
    { "ScsiPortInitialize",                   (void *)stub_ScsiPortInitialize },
    { "ScsiPortConvertUlongToPhysicalAddress",(void *)stub_ScsiPortConvertUlongToPhysicalAddress },
    { "ScsiPortGetBusData",                   (void *)stub_ScsiPortGetBusData },
    { "ScsiPortSetBusDataByOffset",           (void *)stub_ScsiPortSetBusDataByOffset },
    { (const char *)0,                        (void *)0 }
};

/* ================================================================
 * Main init function
 * ================================================================ */

int _ntmini_init(void)
{
    /*
     * DIAGNOSTIC BUILD: Progressively test functionality.
     * Each step writes a marker byte to NTMINI.LOG so we can see
     * exactly how far execution got.
     */
    int log_h;
    int atapi_h;
    void *buf;
    int n;
    ULONG total;
    void *entry_point;
    void *image_base;
    int rc;

    /* All output goes to QEMU debug port 0xE9 via VxD_Debug_Printf */
    /* AND to NTMINI.LOG if we can open it */

    VxD_Debug_Printf("NTMINI-V3-A: ALIVE\r\n");

    /* Step B: Open NTMINI.LOG for writing */
    log_h = VxD_File_Create("C:\\NTMINI.LOG");
    if (log_h < 0) {
        VxD_Debug_Printf("NTMINI-V3-B: LOG OPEN FAIL\r\n");
    } else {
        VxD_Debug_Printf("NTMINI-V3-B: LOG OPEN OK\r\n");
        VxD_File_Write(log_h, "STEP-C: Log opened\r\n", 20);
    }

    /* Step D: Open ATAPI.SYS */
    VxD_Debug_Printf("NTMINI-V3-D: Opening ATAPI\r\n");
    atapi_h = VxD_File_Open("C:\\WINDOWS\\SYSTEM\\ATAPI.SYS");
    log_str_hex("D:eax=", diag_regs[0], " ");
    log_str_hex("ebx=", diag_regs[1], " ");
    log_str_hex("ecx=", diag_regs[2], " ");
    log_str_hex("edx=", diag_regs[3], "\r\n");
    if (atapi_h < 0) {
        VxD_Debug_Printf("NTMINI-V3-D: ATAPI FAIL\r\n");
        if (log_h >= 0) { VxD_File_Write(log_h, "ATAPI open FAIL\r\n", 17); VxD_File_Close(log_h); }
        return 0;
    }
    VxD_Debug_Printf("NTMINI-V3-D: ATAPI OK\r\n");
    if (log_h >= 0) VxD_File_Write(log_h, "STEP-D: ATAPI OK\r\n", 18);

    /* Step E: Page alloc (HeapAllocate ordinal 0x256 doesn't exist in this VMM) */
    VxD_Debug_Printf("NTMINI-V3-E: Page alloc\r\n");
    buf = VxD_PageAllocate(32, 0);  /* 32 pages = 128KB */
    if (!buf) {
        VxD_Debug_Printf("NTMINI-V3-E: Heap FAIL\r\n");
        VxD_File_Close(atapi_h);
        if (log_h >= 0) { VxD_File_Write(log_h, "Heap FAIL\r\n", 11); VxD_File_Close(log_h); }
        return 0;
    }
    VxD_Debug_Printf("NTMINI-V3-E: Heap OK\r\n");
    if (log_h >= 0) VxD_File_Write(log_h, "STEP-E: Heap OK\r\n", 17);

    /* Step F: Read ATAPI.SYS */
    VxD_Debug_Printf("NTMINI-V3-F: Reading\r\n");
    total = 0;
    {
        ULONG chunk = 4096;  /* try small read first */
        n = VxD_File_Read(atapi_h, (UCHAR *)buf, (int)chunk);
        log_str_int("NTMINI-V3-F: read1=", n, " ");
        /* Check if first bytes changed from zero */
        {
            UCHAR *p = (UCHAR *)buf;
            log_str_hex("b[0]=", (ULONG)p[0], " ");
            log_str_hex("b[1]=", (ULONG)p[1], "\r\n");
        }
        if (n > 0) {
            total = (ULONG)n;
            /* Continue reading */
            while (total < 131072) {
                chunk = 131072 - total;
                if (chunk > 32768) chunk = 32768;
                n = VxD_File_Read(atapi_h, (UCHAR *)buf + total, (int)chunk);
                if (n <= 0) break;
                total += (ULONG)n;
            }
        }
    }
    VxD_File_Close(atapi_h);
    log_str_dec("NTMINI-V3-F: total=", total, " bytes\r\n");

    if (log_h >= 0) {
        char msg[40];
        char dec[12];
        int i;
        msg[0]='S'; msg[1]='T'; msg[2]='E'; msg[3]='P'; msg[4]='-';
        msg[5]='F'; msg[6]=':'; msg[7]=' ';
        ulong_to_dec(total, dec);
        i = 8;
        { int j = 0; while (dec[j]) { msg[i++] = dec[j++]; } }
        msg[i++] = '\r'; msg[i++] = '\n';
        VxD_File_Write(log_h, msg, i);
    }

    /* Step G: MZ check + hex dump of first 32 bytes */
    {
        UCHAR *p = (UCHAR *)buf;
        ULONG j;
        VxD_Debug_Printf("G:hex:");
        for (j = 0; j < 32 && j < total; j++) {
            char hx[4];
            hx[0] = "0123456789ABCDEF"[(p[j] >> 4) & 0xF];
            hx[1] = "0123456789ABCDEF"[p[j] & 0xF];
            hx[2] = ' ';
            hx[3] = 0;
            VxD_Debug_Printf(hx);
        }
        VxD_Debug_Printf("\r\n");
        if (total > 1 && p[0] == 'M' && p[1] == 'Z') {
            VxD_Debug_Printf("NTMINI-V3-G: MZ OK\r\n");
            if (log_h >= 0) VxD_File_Write(log_h, "STEP-G: MZ OK\r\n", 15);
        } else {
            log_str_hex("G:b0=", (ULONG)p[0], " ");
            log_str_hex("b1=", (ULONG)p[1], "\r\n");
            VxD_Debug_Printf("NTMINI-V3-G: No MZ\r\n");
            if (log_h >= 0) VxD_File_Write(log_h, "STEP-G: No MZ\r\n", 15);
        }
    }

    /* Step H: PE load */
    VxD_Debug_Printf("NTMINI-V3-H: PE load start\r\n");
    if (log_h >= 0) VxD_File_Write(log_h, "STEP-H: PE...\r\n", 15);

    entry_point = (void *)0;
    image_base = (void *)0;
    rc = pe_load_image(buf, total, scsiport_stubs, &entry_point, &image_base);

    if (rc == 0) {
        VxD_Debug_Printf("NTMINI-V3-H: PE SUCCESS\r\n");
        if (log_h >= 0) {
            char msg[60];
            char hex[12];
            int i;
            VxD_File_Write(log_h, "STEP-H: OK\r\n", 12);
            msg[0]='e'; msg[1]='=';
            ulong_to_hex((ULONG)entry_point, hex);
            i = 2;
            { int j = 0; while (hex[j]) { msg[i++] = hex[j++]; } }
            msg[i++] = '\r'; msg[i++] = '\n';
            VxD_File_Write(log_h, msg, i);
            msg[0]='b'; msg[1]='=';
            ulong_to_hex((ULONG)image_base, hex);
            i = 2;
            { int j = 0; while (hex[j]) { msg[i++] = hex[j++]; } }
            msg[i++] = '\r'; msg[i++] = '\n';
            VxD_File_Write(log_h, msg, i);
        }
        pe_unload_image(image_base);
    } else {
        VxD_Debug_Printf("NTMINI-V3-H: PE FAIL\r\n");
        if (log_h >= 0) {
            char msg[30];
            char dec[14];
            int i;
            msg[0]='F'; msg[1]='A'; msg[2]='I'; msg[3]='L'; msg[4]='=';
            int_to_dec(rc, dec);
            i = 5;
            { int j = 0; while (dec[j]) { msg[i++] = dec[j++]; } }
            msg[i++] = '\r'; msg[i++] = '\n';
            VxD_File_Write(log_h, msg, i);
        }
    }

    /* Cleanup */
    VxD_PageFree(buf);
    VxD_Debug_Printf("NTMINI-V3: DONE\r\n");
    if (log_h >= 0) {
        VxD_File_Write(log_h, "DONE\r\n", 6);
        VxD_File_Close(log_h);
    }
    return 0;
}

/* Called by VxD control procedure on Sys_Dynamic_Device_Exit */
void _ntmini_cleanup(void)
{
    VxD_Debug_Printf("NTMINI V3: VxD unloaded\r\n");
}
