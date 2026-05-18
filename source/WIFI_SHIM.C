/*
 * WIFI_SHIM.C - 802.11 OID Handler Extension for NDIS_SHIM.C
 *
 * Provides WiFi-specific OID handling for XP 802.11 miniport drivers
 * running on Win98 via the VxD PE loader. XP WiFi miniports use
 * standard NDIS 5.1 but query additional 802.11 OIDs that must be
 * handled to prevent crashes.
 *
 * Architecture:
 *   - Called by NDIS_SHIM when OID falls in 0x0D000000 range
 *   - Phase 1: returns NDIS_STATUS_NOT_SUPPORTED for all WiFi OIDs
 *     (prevents crashes; actual WiFi requires scan/association state)
 *   - Provides wifi_handle_oid() for integration into NDIS_SHIM's
 *     QueryInformation/SetInformation path
 *
 * Build: Open Watcom C 2.0 (wcc386 -bt=windows -3s -s -zl -d0)
 * Link:  with existing VxD (PELOAD.C, VxD wrapper, NDIS_SHIM.C)
 *
 * Calling convention: all functions use __stdcall where they interface
 * with NT miniport drivers.
 *
 * Known limitations (Phase 1):
 *   - All WiFi OIDs return NOT_SUPPORTED
 *   - No scan result generation
 *   - No WEP/WPA key management
 *   - No association state machine
 *   - No beacon/probe response parsing
 */

/* ================================================================
 * Basic type definitions (must match PELOAD.C / NTMINI_V5.C)
 * ================================================================ */

typedef unsigned char       UCHAR;
typedef unsigned short      USHORT;
typedef unsigned long       ULONG;
typedef signed long         LONG;
typedef unsigned char       BOOLEAN;
typedef void                VOID;
typedef void               *PVOID;
typedef UCHAR              *PUCHAR;
typedef USHORT             *PUSHORT;
typedef ULONG              *PULONG;

#define TRUE  1
#define FALSE 0
#define NULL  ((void*)0)

typedef ULONG NDIS_STATUS;

#define NDIS_STATUS_SUCCESS             0x00000000UL
#define NDIS_STATUS_NOT_SUPPORTED       0xC00000BBUL
#define NDIS_STATUS_INVALID_LENGTH      0xC0010014UL

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
extern void  VxD_Debug_Printf(const char *fmt, ...);

#define PAGESIZE    4096
#define PAGEFIXED   0x00000001

/* PE loader externals */
typedef struct { const char *name; PVOID func; } IMPORT_FUNC_ENTRY;

typedef struct _PORT_DRIVER_SHIM {
    const char              *dll_name;
    const IMPORT_FUNC_ENTRY *func_table;
    int                      func_count;
    int  (*bridge_init)(void *context);
    int  (*bridge_io)(void *request);
    void (*bridge_cleanup)(void);
} PORT_DRIVER_SHIM;

extern int register_port_driver(PORT_DRIVER_SHIM *shim);

/* ================================================================
 * 802.11 OID Definitions (from ntddndis.h / XP DDK)
 *
 * All WiFi OIDs live in the 0x0D000000 range.
 * Query OIDs: 0x0D010101 - 0x0D0102FF
 * Set OIDs:   0x0D010101 - 0x0D0102FF (overlapping, direction in IRP)
 * Statistics: 0x0D020201 - 0x0D0202FF
 * ================================================================ */

#define OID_802_11_BSSID                    0x0D010101UL
#define OID_802_11_SSID                     0x0D010102UL
#define OID_802_11_NETWORK_TYPES_SUPPORTED  0x0D010203UL
#define OID_802_11_NETWORK_TYPE_IN_USE      0x0D010204UL
#define OID_802_11_TX_POWER_LEVEL           0x0D010205UL
#define OID_802_11_RSSI                     0x0D010206UL
#define OID_802_11_RSSI_TRIGGER             0x0D010207UL
#define OID_802_11_INFRASTRUCTURE_MODE      0x0D010108UL
#define OID_802_11_FRAGMENTATION_THRESHOLD  0x0D010209UL
#define OID_802_11_RTS_THRESHOLD            0x0D01020AUL
#define OID_802_11_NUMBER_OF_ANTENNAS       0x0D01020BUL
#define OID_802_11_RX_ANTENNA_SELECTED      0x0D01020CUL
#define OID_802_11_TX_ANTENNA_SELECTED      0x0D01020DUL
#define OID_802_11_SUPPORTED_RATES          0x0D01020EUL
#define OID_802_11_DESIRED_RATES            0x0D010210UL
#define OID_802_11_CONFIGURATION            0x0D010211UL
#define OID_802_11_STATISTICS               0x0D020212UL
#define OID_802_11_ADD_WEP                  0x0D010113UL
#define OID_802_11_REMOVE_WEP              0x0D010114UL
#define OID_802_11_DISASSOCIATE             0x0D010115UL
#define OID_802_11_POWER_MODE               0x0D010216UL
#define OID_802_11_BSSID_LIST               0x0D010217UL
#define OID_802_11_AUTHENTICATION_MODE      0x0D010118UL
#define OID_802_11_PRIVACY_FILTER           0x0D010119UL
#define OID_802_11_BSSID_LIST_SCAN          0x0D01011AUL
#define OID_802_11_WEP_STATUS               0x0D01011BUL
#define OID_802_11_ENCRYPTION_STATUS        0x0D01011BUL  /* alias */
#define OID_802_11_RELOAD_DEFAULTS          0x0D01011CUL
#define OID_802_11_ADD_KEY                  0x0D01011DUL
#define OID_802_11_REMOVE_KEY               0x0D01011EUL
#define OID_802_11_ASSOCIATION_INFORMATION  0x0D01011FUL
#define OID_802_11_TEST                     0x0D010120UL

/* ================================================================
 * WiFi Adapter State (Phase 1: minimal stubs)
 * ================================================================ */

/* NDIS_802_11_NETWORK_INFRASTRUCTURE */
#define Ndis802_11IBSS              0
#define Ndis802_11Infrastructure    1
#define Ndis802_11AutoUnknown       2

/* NDIS_802_11_AUTHENTICATION_MODE */
#define Ndis802_11AuthModeOpen      0
#define Ndis802_11AuthModeShared    1

/* NDIS_802_11_NETWORK_TYPE */
#define Ndis802_11FH                0
#define Ndis802_11DS                1

static struct {
    ULONG   InfrastructureMode;
    ULONG   AuthenticationMode;
    ULONG   NetworkTypeInUse;
    UCHAR   CurrentBSSID[6];
    UCHAR   CurrentSSID[32];
    ULONG   CurrentSSIDLength;
    BOOLEAN Associated;
} g_wifi_state;

/* ================================================================
 * Debug helper
 * ================================================================ */

static void wifi_log_oid(const char *prefix, ULONG oid) {
    static const char hx[] = "0123456789ABCDEF";
    char h[12];
    int i;
    h[0] = '0'; h[1] = 'x';
    for (i = 7; i >= 0; i--) h[2 + (7 - i)] = hx[(oid >> (i * 4)) & 0xF];
    h[10] = 0;
    VxD_Debug_Printf(prefix);
    VxD_Debug_Printf(h);
    VxD_Debug_Printf("\r\n");
}

/* ================================================================
 * WiFi OID Handler
 *
 * Returns TRUE if the OID was handled (even if unsupported),
 * FALSE if it's not a WiFi OID and should pass through to
 * standard NDIS handling.
 *
 * Parameters:
 *   AdapterContext - Miniport adapter context (from NdisMSetAttributes)
 *   Oid           - The OID being queried/set
 *   InfoBuf       - Buffer for data (query: output; set: input)
 *   InfoBufLen    - Size of InfoBuf in bytes
 *   BytesWritten  - [out] Bytes written to InfoBuf (query)
 *   BytesNeeded   - [out] Bytes needed if buffer too small
 *
 * Return: TRUE = handled (check Status), FALSE = not a WiFi OID
 * Status is written via the BytesWritten pointer convention:
 *   *BytesWritten = 0 and return TRUE means NOT_SUPPORTED
 * ================================================================ */

BOOLEAN __stdcall wifi_handle_oid(
    PVOID   AdapterContext,
    ULONG   Oid,
    PVOID   InfoBuf,
    ULONG   InfoBufLen,
    PULONG  BytesWritten,
    PULONG  BytesNeeded)
{
    /* Check if this is an 802.11 OID (0x0Dxxxxxx range) */
    if ((Oid & 0xFF000000UL) != 0x0D000000UL) {
        return FALSE;  /* Not a WiFi OID, pass through */
    }

    (void)AdapterContext;
    (void)InfoBuf;
    (void)InfoBufLen;

    /* Log the OID for diagnostics */
    wifi_log_oid("WIFI: OID query/set ", Oid);

    /*
     * Phase 1: All WiFi OIDs return NOT_SUPPORTED.
     *
     * This prevents crashes when a WiFi miniport queries these OIDs
     * during initialization. The miniport will see NOT_SUPPORTED and
     * either fail gracefully or skip WiFi-specific features.
     *
     * Future phases would implement:
     *   - OID_802_11_BSSID_LIST_SCAN: trigger scan, populate results
     *   - OID_802_11_BSSID_LIST: return cached scan results
     *   - OID_802_11_SSID: set/get current SSID
     *   - OID_802_11_ADD_WEP: store WEP key
     *   - OID_802_11_AUTHENTICATION_MODE: set auth mode
     *   - OID_802_11_INFRASTRUCTURE_MODE: set BSS/IBSS
     */

    switch (Oid) {
    case OID_802_11_BSSID:
    case OID_802_11_SSID:
    case OID_802_11_NETWORK_TYPE_IN_USE:
    case OID_802_11_NETWORK_TYPES_SUPPORTED:
    case OID_802_11_INFRASTRUCTURE_MODE:
    case OID_802_11_TX_POWER_LEVEL:
    case OID_802_11_RSSI:
    case OID_802_11_RSSI_TRIGGER:
    case OID_802_11_FRAGMENTATION_THRESHOLD:
    case OID_802_11_RTS_THRESHOLD:
    case OID_802_11_NUMBER_OF_ANTENNAS:
    case OID_802_11_RX_ANTENNA_SELECTED:
    case OID_802_11_TX_ANTENNA_SELECTED:
    case OID_802_11_SUPPORTED_RATES:
    case OID_802_11_DESIRED_RATES:
    case OID_802_11_CONFIGURATION:
    case OID_802_11_STATISTICS:
    case OID_802_11_ADD_WEP:
    case OID_802_11_REMOVE_WEP:
    case OID_802_11_DISASSOCIATE:
    case OID_802_11_POWER_MODE:
    case OID_802_11_BSSID_LIST:
    case OID_802_11_AUTHENTICATION_MODE:
    case OID_802_11_PRIVACY_FILTER:
    case OID_802_11_BSSID_LIST_SCAN:
    case OID_802_11_WEP_STATUS:
    case OID_802_11_RELOAD_DEFAULTS:
    case OID_802_11_ADD_KEY:
    case OID_802_11_REMOVE_KEY:
    case OID_802_11_ASSOCIATION_INFORMATION:
    case OID_802_11_TEST:
        VxD_Debug_Printf("WIFI:   -> NOT_SUPPORTED (Phase 1 stub)\r\n");
        if (BytesWritten) *BytesWritten = 0;
        if (BytesNeeded)  *BytesNeeded  = 0;
        return TRUE;  /* Handled: NOT_SUPPORTED */

    default:
        /* Unknown 802.11 OID in the 0x0D range */
        wifi_log_oid("WIFI:   -> Unknown WiFi OID, NOT_SUPPORTED: ", Oid);
        if (BytesWritten) *BytesWritten = 0;
        if (BytesNeeded)  *BytesNeeded  = 0;
        return TRUE;  /* Handled: NOT_SUPPORTED */
    }
}

/*
 * wifi_get_oid_status
 *
 * Helper for NDIS_SHIM integration. After wifi_handle_oid returns TRUE,
 * the caller needs the NDIS_STATUS to return. Phase 1 always returns
 * NOT_SUPPORTED.
 */
NDIS_STATUS __stdcall wifi_get_oid_status(ULONG Oid)
{
    (void)Oid;
    return NDIS_STATUS_NOT_SUPPORTED;
}

/* ================================================================
 * Import Function Table (WiFi extends NDIS, no separate DLL)
 *
 * This shim doesn't resolve imports from a separate DLL. It extends
 * the NDIS_SHIM's OID handling. The port driver registration is for
 * diagnostic purposes and future direct-load scenarios.
 * ================================================================ */

static const IMPORT_FUNC_ENTRY wifi_funcs[] = {
    { NULL, NULL }
};

/* ================================================================
 * Port Driver Shim Structure
 * ================================================================ */

static PORT_DRIVER_SHIM wifi_shim_struct = {
    "WIFI_802_11",      /* dll_name (diagnostic identifier) */
    wifi_funcs,         /* func_table */
    0,                  /* func_count */
    NULL,               /* bridge_init */
    NULL,               /* bridge_io */
    NULL                /* bridge_cleanup */
};

/* ================================================================
 * Public Registration
 * ================================================================ */

void wifi_shim_register(void)
{
    /* Initialize default state */
    g_wifi_state.InfrastructureMode = Ndis802_11Infrastructure;
    g_wifi_state.AuthenticationMode = Ndis802_11AuthModeOpen;
    g_wifi_state.NetworkTypeInUse   = Ndis802_11DS;
    g_wifi_state.CurrentSSIDLength  = 0;
    g_wifi_state.Associated         = FALSE;

    VxD_Debug_Printf("WIFI: Registering 802.11 OID handler shim\r\n");
    register_port_driver(&wifi_shim_struct);
}
