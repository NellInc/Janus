/* NTMINI_V4.C - Version 4: Embedded ATAPI.SYS + PE load
 *
 * Bypasses file I/O entirely by embedding ATAPI.SYS data.
 * Tests PE loader with real NT4 miniport driver binary.
 *
 * Build: wcc386 -bt=windows -3s -s -zl -d0 -i=. NTMINI_V4.C
 */

typedef unsigned char       UCHAR;
typedef unsigned short      USHORT;
typedef unsigned long       ULONG;
typedef long                LONG;
typedef void               *PVOID;

typedef struct {
    const char *name;
    void       *func;
} IMPORT_FUNC_ENTRY;

extern void VxD_Debug_Printf(const char *msg);
extern void VxD_Serial_Init(void);
extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void VxD_PageFree(PVOID addr);

extern int pe_load_image(
    const void *pe_data,
    unsigned long pe_size,
    const IMPORT_FUNC_ENTRY *func_table,
    void **out_entry,
    void **out_base);

extern void pe_unload_image(void *image_base);

/* Embedded ATAPI.SYS binary */
#include "ATAPI_EMBEDDED.H"

/* Simple string helpers */
static void ulong_to_hex(ULONG val, char *buf)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 7; i >= 0; i--)
        buf[2 + (7 - i)] = hex[(val >> (i * 4)) & 0xF];
    buf[10] = 0;
}

static void log_hex(const char *prefix, ULONG val, const char *suffix)
{
    char hex[12];
    ulong_to_hex(val, hex);
    VxD_Debug_Printf(prefix);
    VxD_Debug_Printf(hex);
    VxD_Debug_Printf(suffix);
}

/* ScsiPort stub functions */
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
static ULONG stub_ScsiPortWritePortUshort(void) { return 0; }
static ULONG stub_ScsiPortReadRegisterUchar(void) { return 0; }
static ULONG stub_ScsiPortReadRegisterUshort(void) { return 0; }
static ULONG stub_ScsiPortReadRegisterUlong(void) { return 0; }
static ULONG stub_ScsiPortWriteRegisterUchar(void) { return 0; }
static ULONG stub_ScsiPortWriteRegisterUshort(void) { return 0; }
static ULONG stub_ScsiPortWriteRegisterUlong(void) { return 0; }

static const IMPORT_FUNC_ENTRY scsiport_stubs[] = {
    { "ScsiPortGetDeviceBase",                (void *)stub_ScsiPortGetDeviceBase },
    { "ScsiPortFreeDeviceBase",               (void *)stub_ScsiPortFreeDeviceBase },
    { "ScsiPortReadPortUchar",                (void *)stub_ScsiPortReadPortUchar },
    { "ScsiPortReadPortUshort",               (void *)stub_ScsiPortReadPortUshort },
    { "ScsiPortReadPortUlong",                (void *)stub_ScsiPortReadPortUlong },
    { "ScsiPortReadPortBufferUshort",         (void *)stub_ScsiPortReadPortBufferUshort },
    { "ScsiPortReadPortBufferUlong",          (void *)stub_ScsiPortReadPortBufferUlong },
    { "ScsiPortWritePortUchar",               (void *)stub_ScsiPortWritePortUchar },
    { "ScsiPortWritePortUshort",              (void *)stub_ScsiPortWritePortUshort },
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
    { "ScsiPortReadRegisterUchar",            (void *)stub_ScsiPortReadRegisterUchar },
    { "ScsiPortReadRegisterUshort",           (void *)stub_ScsiPortReadRegisterUshort },
    { "ScsiPortReadRegisterUlong",            (void *)stub_ScsiPortReadRegisterUlong },
    { "ScsiPortWriteRegisterUchar",           (void *)stub_ScsiPortWriteRegisterUchar },
    { "ScsiPortWriteRegisterUshort",          (void *)stub_ScsiPortWriteRegisterUshort },
    { "ScsiPortWriteRegisterUlong",           (void *)stub_ScsiPortWriteRegisterUlong },
    { (const char *)0,                        (void *)0 }
};

int _ntmini_init(void)
{
    void *entry_point;
    void *image_base;
    int rc;

    VxD_Debug_Printf("NTMINI-V4: ALIVE\r\n");

    /* Verify embedded data */
    log_hex("V4: embedded size=", ATAPI_EMBEDDED_SIZE, "\r\n");
    log_hex("V4: MZ check=", (ULONG)((atapi_embedded_data[1] << 8) | atapi_embedded_data[0]), "\r\n");

    if (atapi_embedded_data[0] != 'M' || atapi_embedded_data[1] != 'Z') {
        VxD_Debug_Printf("V4: ERROR: embedded data has no MZ header!\r\n");
        return 0;
    }
    VxD_Debug_Printf("V4: MZ OK\r\n");

    /* PE load from embedded data */
    VxD_Debug_Printf("V4: PE load start\r\n");
    entry_point = (void *)0;
    image_base = (void *)0;
    rc = pe_load_image(atapi_embedded_data, ATAPI_EMBEDDED_SIZE,
                       scsiport_stubs, &entry_point, &image_base);

    if (rc == 0) {
        ULONG status;
        typedef ULONG (__cdecl *PFN_DRIVER_ENTRY)(void *, void *);
        PFN_DRIVER_ENTRY DriverEntry;

        VxD_Debug_Printf("V4: PE SUCCESS!\r\n");
        log_hex("V4: entry=", (ULONG)entry_point, "\r\n");
        log_hex("V4: base=", (ULONG)image_base, "\r\n");

        /* Call DriverEntry!
         * NT miniport DriverEntry(DriverObject, RegistryPath)
         * Both can be NULL for initial testing - DriverEntry will call
         * ScsiPortInitialize which is our stub returning 0.
         */
        VxD_Debug_Printf("V4: Calling DriverEntry...\r\n");
        DriverEntry = (PFN_DRIVER_ENTRY)entry_point;
        status = DriverEntry((void *)0, (void *)0);
        log_hex("V4: DriverEntry returned ", status, "\r\n");
        VxD_Debug_Printf("V4: DriverEntry SURVIVED!\r\n");
        /* Don't unload - driver stays resident */
    } else {
        VxD_Debug_Printf("V4: PE FAIL rc=");
        {
            char dec[14]; char *p = dec;
            int v = rc;
            if (v < 0) { *p++ = '-'; v = -v; }
            { char tmp[12]; int i=0,j;
              if (v==0) { tmp[i++]='0'; }
              else { while(v>0){tmp[i++]=(char)('0'+(v%10));v/=10;} }
              for(j=0;j<i;j++) *p++=tmp[i-1-j];
            }
            *p++ = '\r'; *p++ = '\n'; *p = 0;
            VxD_Debug_Printf(dec);
        }
    }

    VxD_Debug_Printf("V4: DONE\r\n");
    return 0;  /* always succeed - don't block boot */
}

void _ntmini_cleanup(void)
{
    VxD_Debug_Printf("V4: unloaded\r\n");
}
