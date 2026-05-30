/*
 * PELOAD.C - PE Image Loader for NT Kernel-Mode Drivers on Win9x
 *
 * Part of the NTMINI project: running NT4 SCSI miniport drivers on Win98.
 *
 * Loads a .sys PE image from a memory buffer into ring 0 memory,
 * processes relocations, resolves imports against registered port
 * driver shims, previously loaded images, or a caller-provided
 * function table (backward-compatible fallback), and returns the
 * DriverEntry address.
 *
 * Multi-DLL support: maintains a registry of loaded images and port
 * driver shims for cross-DLL import resolution. Loading order:
 *   1. Registered port driver shim (ScsiPort, VideoPort, NDIS, etc.)
 *   2. Previously loaded image exports
 *   3. Caller-provided func_table (backward compat fallback)
 *   4. Fail with PE_ERR_IMPORT_FAIL
 *
 * All memory allocation is done through VxD_PageAllocate (provided
 * by the VxD wrapper). No Win32 API calls are used.
 */

/* ================================================================
 * Basic type definitions
 * ================================================================ */

typedef unsigned char       UCHAR;
typedef unsigned short      USHORT;
typedef unsigned long       ULONG;
typedef long                LONG;
typedef void               *PVOID;
typedef char               *PCHAR;
typedef unsigned char      *PUCHAR;
typedef unsigned short     *PUSHORT;
typedef unsigned long      *PULONG;

/* ================================================================
 * VxD wrapper externals
 * ================================================================ */

/* Provided by VxD wrapper - allocates physically fixed ring 0 pages */
extern PVOID VxD_PageAllocate(ULONG nPages, ULONG flags);
extern void  VxD_PageFree(PVOID addr);
#define PAGESIZE    4096
#define PAGEFIXED   0x00000001

/* Debug output through VxD debug services */
extern void VxD_Debug_Printf(const char *fmt, ...);
#define DBGPRINT VxD_Debug_Printf

/* ================================================================
 * PE structure definitions
 * ================================================================ */

#define IMAGE_DOS_SIGNATURE     0x5A4D      /* MZ */
#define IMAGE_NT_SIGNATURE      0x00004550  /* PE\0\0 */

#define IMAGE_NUMBEROF_DIRECTORY_ENTRIES 16
#define IMAGE_SIZEOF_SHORT_NAME         8

/* Data directory indices */
#define IMAGE_DIRECTORY_ENTRY_EXPORT    0
#define IMAGE_DIRECTORY_ENTRY_IMPORT    1
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5

/* Relocation types */
#define IMAGE_REL_BASED_ABSOLUTE    0
#define IMAGE_REL_BASED_HIGH        1
#define IMAGE_REL_BASED_LOW         2
#define IMAGE_REL_BASED_HIGHLOW     3
#define IMAGE_REL_BASED_HIGHADJ     4
#define IMAGE_REL_BASED_DIR64      10  /* PE32+: 64-bit address fixup */

/* Import thunk flags */
#define IMAGE_ORDINAL_FLAG32        0x80000000UL
#define IMAGE_ORDINAL_FLAG64_HI     0x80000000UL  /* PE32+: bit 63 = bit 31 of Hi ULONG */

#pragma pack(push, 1)

typedef struct _IMAGE_DOS_HEADER {
    USHORT  e_magic;        /* Magic number (MZ) */
    USHORT  e_cblp;
    USHORT  e_cp;
    USHORT  e_crlc;
    USHORT  e_cparhdr;
    USHORT  e_minalloc;
    USHORT  e_maxalloc;
    USHORT  e_ss;
    USHORT  e_sp;
    USHORT  e_csum;
    USHORT  e_ip;
    USHORT  e_cs;
    USHORT  e_lfarlc;
    USHORT  e_ovno;
    USHORT  e_res[4];
    USHORT  e_oemid;
    USHORT  e_oeminfo;
    USHORT  e_res2[10];
    LONG    e_lfanew;       /* Offset to PE header */
} IMAGE_DOS_HEADER;

typedef struct _IMAGE_DATA_DIRECTORY {
    ULONG   VirtualAddress;
    ULONG   Size;
} IMAGE_DATA_DIRECTORY;

typedef struct _IMAGE_FILE_HEADER {
    USHORT  Machine;
    USHORT  NumberOfSections;
    ULONG   TimeDateStamp;
    ULONG   PointerToSymbolTable;
    ULONG   NumberOfSymbols;
    USHORT  SizeOfOptionalHeader;
    USHORT  Characteristics;
} IMAGE_FILE_HEADER;

typedef struct _IMAGE_OPTIONAL_HEADER32 {
    USHORT  Magic;
    UCHAR   MajorLinkerVersion;
    UCHAR   MinorLinkerVersion;
    ULONG   SizeOfCode;
    ULONG   SizeOfInitializedData;
    ULONG   SizeOfUninitializedData;
    ULONG   AddressOfEntryPoint;
    ULONG   BaseOfCode;
    ULONG   BaseOfData;
    ULONG   ImageBase;
    ULONG   SectionAlignment;
    ULONG   FileAlignment;
    USHORT  MajorOperatingSystemVersion;
    USHORT  MinorOperatingSystemVersion;
    USHORT  MajorImageVersion;
    USHORT  MinorImageVersion;
    USHORT  MajorSubsystemVersion;
    USHORT  MinorSubsystemVersion;
    ULONG   Win32VersionValue;
    ULONG   SizeOfImage;
    ULONG   SizeOfHeaders;
    ULONG   CheckSum;
    USHORT  Subsystem;
    USHORT  DllCharacteristics;
    ULONG   SizeOfStackReserve;
    ULONG   SizeOfStackCommit;
    ULONG   SizeOfHeapReserve;
    ULONG   SizeOfHeapCommit;
    ULONG   LoaderFlags;
    ULONG   NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER32;

typedef struct _IMAGE_NT_HEADERS {
    ULONG                   Signature;
    IMAGE_FILE_HEADER       FileHeader;
    IMAGE_OPTIONAL_HEADER32 OptionalHeader;
} IMAGE_NT_HEADERS;

/* ---- PE32+ (64-bit) optional header ---- */

typedef struct _IMAGE_OPTIONAL_HEADER64 {
    USHORT  Magic;                  /* 0x020B for PE32+ */
    UCHAR   MajorLinkerVersion;
    UCHAR   MinorLinkerVersion;
    ULONG   SizeOfCode;
    ULONG   SizeOfInitializedData;
    ULONG   SizeOfUninitializedData;
    ULONG   AddressOfEntryPoint;
    ULONG   BaseOfCode;
    /* NO BaseOfData in PE32+ */
    ULONG   ImageBase_Lo;           /* 64-bit ImageBase split */
    ULONG   ImageBase_Hi;
    ULONG   SectionAlignment;
    ULONG   FileAlignment;
    USHORT  MajorOperatingSystemVersion;
    USHORT  MinorOperatingSystemVersion;
    USHORT  MajorImageVersion;
    USHORT  MinorImageVersion;
    USHORT  MajorSubsystemVersion;
    USHORT  MinorSubsystemVersion;
    ULONG   Win32VersionValue;
    ULONG   SizeOfImage;
    ULONG   SizeOfHeaders;
    ULONG   CheckSum;
    USHORT  Subsystem;
    USHORT  DllCharacteristics;
    ULONG   SizeOfStackReserve_Lo;
    ULONG   SizeOfStackReserve_Hi;
    ULONG   SizeOfStackCommit_Lo;
    ULONG   SizeOfStackCommit_Hi;
    ULONG   SizeOfHeapReserve_Lo;
    ULONG   SizeOfHeapReserve_Hi;
    ULONG   SizeOfHeapCommit_Lo;
    ULONG   SizeOfHeapCommit_Hi;
    ULONG   LoaderFlags;
    ULONG   NumberOfRvaAndSizes;
    IMAGE_DATA_DIRECTORY DataDirectory[IMAGE_NUMBEROF_DIRECTORY_ENTRIES];
} IMAGE_OPTIONAL_HEADER64;

typedef struct _IMAGE_NT_HEADERS64 {
    ULONG                   Signature;
    IMAGE_FILE_HEADER       FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
} IMAGE_NT_HEADERS64;

/* ---- PE32+ 64-bit import thunk ---- */

typedef struct _IMAGE_THUNK_DATA64 {
    ULONG   Lo;     /* RVA to IMAGE_IMPORT_BY_NAME (for named imports) */
    ULONG   Hi;     /* ordinal flag at bit 31 (= bit 63 of full 64-bit value) */
} IMAGE_THUNK_DATA64;

typedef struct _IMAGE_SECTION_HEADER {
    UCHAR   Name[IMAGE_SIZEOF_SHORT_NAME];
    union {
        ULONG PhysicalAddress;
        ULONG VirtualSize;
    } Misc;
    ULONG   VirtualAddress;
    ULONG   SizeOfRawData;
    ULONG   PointerToRawData;
    ULONG   PointerToRelocations;
    ULONG   PointerToLinenumbers;
    USHORT  NumberOfRelocations;
    USHORT  NumberOfLinenumbers;
    ULONG   Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct _IMAGE_IMPORT_DESCRIPTOR {
    union {
        ULONG Characteristics;
        ULONG OriginalFirstThunk;   /* RVA to INT (Import Name Table) */
    } u;
    ULONG   TimeDateStamp;
    ULONG   ForwarderChain;
    ULONG   Name;                   /* RVA to DLL name string */
    ULONG   FirstThunk;             /* RVA to IAT */
} IMAGE_IMPORT_DESCRIPTOR;

typedef struct _IMAGE_THUNK_DATA32 {
    union {
        ULONG ForwarderString;
        ULONG Function;
        ULONG Ordinal;
        ULONG AddressOfData;        /* RVA to IMAGE_IMPORT_BY_NAME */
    } u1;
} IMAGE_THUNK_DATA32;

typedef struct _IMAGE_IMPORT_BY_NAME {
    USHORT  Hint;
    char    Name[1];                /* Variable length */
} IMAGE_IMPORT_BY_NAME;

typedef struct _IMAGE_BASE_RELOCATION {
    ULONG   VirtualAddress;
    ULONG   SizeOfBlock;
    /* USHORT TypeOffset[] follows */
} IMAGE_BASE_RELOCATION;

/* ---- PE export directory (for parsing loaded image exports) ---- */

typedef struct _IMAGE_EXPORT_DIRECTORY {
    ULONG   Characteristics;
    ULONG   TimeDateStamp;
    USHORT  MajorVersion;
    USHORT  MinorVersion;
    ULONG   Name;                   /* RVA to DLL name string */
    ULONG   Base;                   /* Ordinal base */
    ULONG   NumberOfFunctions;      /* Total exported functions */
    ULONG   NumberOfNames;          /* Number of named exports */
    ULONG   AddressOfFunctions;     /* RVA to function address array */
    ULONG   AddressOfNames;         /* RVA to name string RVA array */
    ULONG   AddressOfNameOrdinals;  /* RVA to ordinal index array */
} IMAGE_EXPORT_DIRECTORY;

#pragma pack(pop)

/* ================================================================
 * Import function table entry (provided by caller or port shim)
 * ================================================================ */

typedef struct {
    const char *name;
    void       *func;
} IMPORT_FUNC_ENTRY;

/* ================================================================
 * Port driver shim registry
 *
 * Each port driver shim (ScsiPort, VideoPort, NDIS, etc.) registers
 * a DLL name and a null-terminated function table. When a PE image
 * imports from that DLL name, the shim's table is used.
 * ================================================================ */

typedef struct _PORT_DRIVER_SHIM {
    const char              *dll_name;      /* e.g. "SCSIPORT.SYS" */
    const IMPORT_FUNC_ENTRY *func_table;    /* null-terminated */
    int                      func_count;    /* informational; lookup uses null terminator */
    int  (*bridge_init)(void *context);
    int  (*bridge_io)(void *request);
    void (*bridge_cleanup)(void);
} PORT_DRIVER_SHIM;

#define MAX_PORT_SHIMS 16
static PORT_DRIVER_SHIM *shim_registry[MAX_PORT_SHIMS];
static int shim_count = 0;

/* ================================================================
 * Loaded image registry
 *
 * Each loaded PE image can be registered so that subsequent images
 * can resolve imports against its exports. Export name pointers
 * reference into the loaded image's memory, so the image must remain
 * loaded for as long as any dependents are active.
 * ================================================================ */

#define MAX_LOADED_IMAGE_EXPORTS 64

typedef struct _LOADED_IMAGE {
    const char        *name;        /* DLL name (e.g. "pciidex.sys") */
    void              *base;        /* Loaded image base address */
    ULONG              image_size;  /* SizeOfImage for bounds checks */
    IMPORT_FUNC_ENTRY  exports[MAX_LOADED_IMAGE_EXPORTS];
    int                n_exports;
} LOADED_IMAGE;

#define MAX_LOADED_IMAGES 8
static LOADED_IMAGE loaded_images[MAX_LOADED_IMAGES];
static int loaded_image_count = 0;

/* ================================================================
 * Error codes
 * ================================================================ */

#define PE_OK                   0
#define PE_ERR_NULL_INPUT      -1
#define PE_ERR_TOO_SMALL       -2
#define PE_ERR_BAD_DOS_SIG     -3
#define PE_ERR_BAD_PE_OFFSET   -4
#define PE_ERR_BAD_PE_SIG      -5
#define PE_ERR_NOT_I386        -6
#define PE_ERR_NO_OPTHDR       -7
#define PE_ERR_ALLOC_FAIL      -8
#define PE_ERR_SECTION_OOB     -9
#define PE_ERR_IMPORT_FAIL     -10
#define PE_ERR_RELOC_FAIL      -11
#define PE_ERR_NO_ENTRY        -12
#define PE_ERR_REGISTRY_FULL   -13
#define PE_ERR_BAD_PE32PLUS    -14  /* PE32+ header validation failed */

/* ================================================================
 * Helper: case-insensitive string compare
 * ================================================================ */

static int strcmp_nocase(const char *a, const char *b)
{
    unsigned char ca, cb;

    for (;;) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;

        /* Convert to lowercase */
        if (ca >= 'A' && ca <= 'Z') ca += ('a' - 'A');
        if (cb >= 'A' && cb <= 'Z') cb += ('a' - 'A');

        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0)  return 0;
    }
}

/* ================================================================
 * Helper: memory copy
 * ================================================================ */

static void pe_memcpy(void *dst, const void *src, ULONG len)
{
    PUCHAR d = (PUCHAR)dst;
    const UCHAR *s = (const UCHAR *)src;
    while (len--) *d++ = *s++;
}

/* ================================================================
 * Helper: memory zero
 * ================================================================ */

static void pe_memzero(void *dst, ULONG len)
{
    PUCHAR d = (PUCHAR)dst;
    while (len--) *d++ = 0;
}

/* ================================================================
 * Helper: string length
 * ================================================================ */

static ULONG pe_strlen(const char *s)
{
    ULONG n = 0;
    while (*s++) n++;
    return n;
}

/* ================================================================
 * Helper: resolve an imported function name against a func table
 *
 * The table must be null-terminated (last entry has name == NULL).
 * ================================================================ */

static void *resolve_import(const char *name, const IMPORT_FUNC_ENTRY *table)
{
    ULONG i;

    if (!table || !name) return (void *)0;

    for (i = 0; table[i].name != (const char *)0; i++) {
        if (strcmp_nocase(name, table[i].name) == 0) {
            return table[i].func;
        }
    }
    return (void *)0;
}

/* ================================================================
 * Port driver shim registration
 * ================================================================ */

/*
 * register_port_driver - Register a port driver shim for import resolution.
 *
 * The shim's dll_name is used for case-insensitive matching against
 * PE import descriptors. The func_table must be null-terminated.
 *
 * Returns: 0 on success, PE_ERR_REGISTRY_FULL if registry is full.
 */
int register_port_driver(PORT_DRIVER_SHIM *shim)
{
    if (!shim || !shim->dll_name) {
        DBGPRINT("PELOAD: register_port_driver: null shim or dll_name\n");
        return PE_ERR_NULL_INPUT;
    }

    if (shim_count >= MAX_PORT_SHIMS) {
        DBGPRINT("PELOAD: port driver shim registry full (%d/%d)\n",
                 shim_count, MAX_PORT_SHIMS);
        return PE_ERR_REGISTRY_FULL;
    }

    shim_registry[shim_count] = shim;
    shim_count++;

    DBGPRINT("PELOAD: registered port driver shim: %s (%d/%d)\n",
             shim->dll_name, shim_count, MAX_PORT_SHIMS);
    return PE_OK;
}

/*
 * find_port_driver - Find a registered port driver shim by DLL name.
 *
 * Case-insensitive match. Returns NULL if not found.
 */
PORT_DRIVER_SHIM *find_port_driver(const char *dll_name)
{
    int i;

    if (!dll_name) return (PORT_DRIVER_SHIM *)0;

    for (i = 0; i < shim_count; i++) {
        if (shim_registry[i] &&
            strcmp_nocase(dll_name, shim_registry[i]->dll_name) == 0) {
            return shim_registry[i];
        }
    }
    return (PORT_DRIVER_SHIM *)0;
}

/* ================================================================
 * Loaded image registry
 * ================================================================ */

/*
 * find_loaded_image - Find a previously loaded image by DLL name.
 *
 * Case-insensitive match. Returns NULL if not found.
 */
LOADED_IMAGE *find_loaded_image(const char *dll_name)
{
    int i;

    if (!dll_name) return (LOADED_IMAGE *)0;

    for (i = 0; i < loaded_image_count; i++) {
        if (loaded_images[i].name &&
            strcmp_nocase(dll_name, loaded_images[i].name) == 0) {
            return &loaded_images[i];
        }
    }
    return (LOADED_IMAGE *)0;
}

/*
 * register_loaded_image - Register a loaded image for cross-DLL resolution.
 *
 * The caller provides the image name and base address. Exports should
 * be populated via pe_parse_exports() before calling this, or can be
 * left empty for images with no exports.
 *
 * Returns a pointer to the LOADED_IMAGE slot, or NULL if full.
 */
LOADED_IMAGE *register_loaded_image(const char *name, void *base, ULONG image_size)
{
    LOADED_IMAGE *img;

    if (!name || !base) {
        DBGPRINT("PELOAD: register_loaded_image: null name or base\n");
        return (LOADED_IMAGE *)0;
    }

    if (loaded_image_count >= MAX_LOADED_IMAGES) {
        DBGPRINT("PELOAD: loaded image registry full (%d/%d)\n",
                 loaded_image_count, MAX_LOADED_IMAGES);
        return (LOADED_IMAGE *)0;
    }

    img = &loaded_images[loaded_image_count];
    pe_memzero(img, sizeof(LOADED_IMAGE));
    img->name = name;
    img->base = base;
    img->image_size = image_size;
    img->n_exports = 0;

    loaded_image_count++;

    DBGPRINT("PELOAD: registered loaded image: %s at 0x%08lX (%d/%d)\n",
             name, (ULONG)base, loaded_image_count, MAX_LOADED_IMAGES);
    return img;
}

/* ================================================================
 * pe_parse_exports - Parse a PE image's export directory
 *
 * Reads the export directory of a loaded PE image and populates
 * the LOADED_IMAGE's exports array. Only named exports are stored;
 * ordinal-only exports are skipped. Forwarder entries (function RVA
 * falls inside the export directory) are skipped with a warning.
 *
 * Export name pointers reference into the loaded image's memory.
 * The image must remain loaded for as long as dependents need the
 * export table.
 *
 * image_base:  base address of the already-loaded PE image
 * image_size:  SizeOfImage of the PE
 * img:         LOADED_IMAGE entry to populate (exports[] and n_exports)
 *
 * Returns: number of exports parsed, or 0 if no export directory.
 * ================================================================ */

int pe_parse_exports(void *image_base, ULONG image_size, LOADED_IMAGE *img)
{
    PUCHAR                        base;
    const IMAGE_DOS_HEADER       *dos;
    const IMAGE_NT_HEADERS       *nt;
    const IMAGE_EXPORT_DIRECTORY *expdir;
    IMAGE_DATA_DIRECTORY          exp_dd;
    ULONG   exp_rva, exp_size;
    ULONG   num_names, max_parse;
    const ULONG   *name_rvas;
    const USHORT  *name_ords;
    const ULONG   *func_rvas;
    ULONG   i;
    int     count;

    if (!image_base || !img) return 0;

    base = (PUCHAR)image_base;
    dos = (const IMAGE_DOS_HEADER *)base;

    /* Validate DOS header */
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    if ((ULONG)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > image_size) return 0;

    nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    /* Check for export directory */
    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
        DBGPRINT("PELOAD: pe_parse_exports: no export directory entries\n");
        return 0;
    }

    exp_dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    exp_rva  = exp_dd.VirtualAddress;
    exp_size = exp_dd.Size;

    if (exp_rva == 0 || exp_size == 0) {
        DBGPRINT("PELOAD: pe_parse_exports: empty export directory\n");
        return 0;
    }

    /* Bounds check the export directory itself */
    if (exp_rva + sizeof(IMAGE_EXPORT_DIRECTORY) > image_size) {
        DBGPRINT("PELOAD: pe_parse_exports: export directory beyond image bounds\n");
        return 0;
    }

    expdir = (const IMAGE_EXPORT_DIRECTORY *)(base + exp_rva);
    num_names = expdir->NumberOfNames;

    DBGPRINT("PELOAD: pe_parse_exports: %lu named exports, %lu total functions\n",
             num_names, expdir->NumberOfFunctions);

    if (num_names == 0) return 0;

    /* Bounds check the three parallel arrays */
    if (expdir->AddressOfNames == 0 ||
        expdir->AddressOfNames + num_names * sizeof(ULONG) > image_size) {
        DBGPRINT("PELOAD: pe_parse_exports: AddressOfNames out of bounds\n");
        return 0;
    }
    if (expdir->AddressOfNameOrdinals == 0 ||
        expdir->AddressOfNameOrdinals + num_names * sizeof(USHORT) > image_size) {
        DBGPRINT("PELOAD: pe_parse_exports: AddressOfNameOrdinals out of bounds\n");
        return 0;
    }
    if (expdir->AddressOfFunctions == 0 ||
        expdir->AddressOfFunctions +
            expdir->NumberOfFunctions * sizeof(ULONG) > image_size) {
        DBGPRINT("PELOAD: pe_parse_exports: AddressOfFunctions out of bounds\n");
        return 0;
    }

    name_rvas = (const ULONG *)(base + expdir->AddressOfNames);
    name_ords = (const USHORT *)(base + expdir->AddressOfNameOrdinals);
    func_rvas = (const ULONG *)(base + expdir->AddressOfFunctions);

    /* Clamp to MAX_LOADED_IMAGE_EXPORTS to prevent overflow */
    max_parse = num_names;
    if (max_parse > MAX_LOADED_IMAGE_EXPORTS - 1) {
        DBGPRINT("PELOAD: pe_parse_exports: WARNING: %lu exports exceed cap of %d, "
                 "truncating\n", num_names, MAX_LOADED_IMAGE_EXPORTS - 1);
        max_parse = MAX_LOADED_IMAGE_EXPORTS - 1;
    }

    count = 0;
    for (i = 0; i < max_parse; i++) {
        ULONG name_rva;
        USHORT ord_idx;
        ULONG func_rva;
        const char *exp_name;

        name_rva = name_rvas[i];
        ord_idx  = name_ords[i];

        /* Bounds check name RVA */
        if (name_rva == 0 || name_rva >= image_size) {
            DBGPRINT("PELOAD: pe_parse_exports: name RVA 0x%08lX out of bounds, "
                     "skipping\n", name_rva);
            continue;
        }

        /* Bounds check ordinal index */
        if (ord_idx >= expdir->NumberOfFunctions) {
            DBGPRINT("PELOAD: pe_parse_exports: ordinal %u beyond function count "
                     "%lu, skipping\n", (unsigned)ord_idx, expdir->NumberOfFunctions);
            continue;
        }

        func_rva = func_rvas[ord_idx];

        /* Forwarder detection: if the function RVA falls inside the
         * export directory range, it's a forwarder string, not code.
         * Skip these; NT storage drivers rarely use them. */
        if (func_rva >= exp_rva && func_rva < exp_rva + exp_size) {
            exp_name = (const char *)(base + name_rva);
            DBGPRINT("PELOAD: pe_parse_exports: skipping forwarder: %s -> %s\n",
                     exp_name, (const char *)(base + func_rva));
            continue;
        }

        /* Bounds check function RVA */
        if (func_rva >= image_size) {
            DBGPRINT("PELOAD: pe_parse_exports: function RVA 0x%08lX out of "
                     "bounds, skipping\n", func_rva);
            continue;
        }

        exp_name = (const char *)(base + name_rva);

        img->exports[count].name = exp_name;
        img->exports[count].func = (void *)(base + func_rva);
        count++;

        DBGPRINT("PELOAD: pe_parse_exports: [%d] %s -> 0x%08lX\n",
                 count - 1, exp_name, (ULONG)(base + func_rva));
    }

    /* Null-terminate for compatibility with resolve_import() */
    if (count < MAX_LOADED_IMAGE_EXPORTS) {
        img->exports[count].name = (const char *)0;
        img->exports[count].func = (void *)0;
    }

    img->n_exports = count;

    DBGPRINT("PELOAD: pe_parse_exports: parsed %d named exports\n", count);
    return count;
}

/* ================================================================
 * resolve_dll_import - Resolve a function import against all sources
 *
 * Resolution order:
 *   1. Port driver shim matching the DLL name
 *   2. Loaded image exports matching the DLL name
 *   3. Caller-provided fallback func_table (backward compat)
 *
 * Returns the resolved function address, or NULL if unresolved.
 * ================================================================ */

static void *resolve_dll_import(
    const char *dll_name,
    const char *func_name,
    const IMPORT_FUNC_ENTRY *fallback_table)
{
    void *resolved;

    /* 1. Check registered port driver shims */
    {
        PORT_DRIVER_SHIM *shim = find_port_driver(dll_name);
        if (shim && shim->func_table) {
            resolved = resolve_import(func_name, shim->func_table);
            if (resolved) return resolved;
        }
    }

    /* 2. Check previously loaded image exports */
    {
        LOADED_IMAGE *img = find_loaded_image(dll_name);
        if (img && img->n_exports > 0) {
            resolved = resolve_import(func_name, img->exports);
            if (resolved) return resolved;
        }
    }

    /* 3. Fallback to caller-provided table (backward compat) */
    if (fallback_table) {
        resolved = resolve_import(func_name, fallback_table);
        if (resolved) return resolved;
    }

    return (void *)0;
}

/* ================================================================
 * pe_load_image64 - Forward declaration (PE32+ loader, defined below)
 * ================================================================ */

static int pe_load_image64(
    const void *pe_data,
    unsigned long pe_size,
    const IMPORT_FUNC_ENTRY *func_table,
    void **out_entry,
    void **out_base);

/* ================================================================
 * pe_load_image - Load a PE image from a memory buffer
 *
 * pe_data:     pointer to the entire .sys file contents in memory
 * pe_size:     size of the file in bytes
 * func_table:  array of {name, func_ptr} pairs, NULL-name terminated
 *              (backward-compat fallback; may be NULL if port shims
 *              are registered)
 * out_entry:   receives the entry point (DriverEntry) address
 * out_base:    receives the loaded image base address
 *
 * Auto-detects PE32 vs PE32+ (AMD64) and dispatches accordingly.
 *
 * Returns: 0 on success, negative error code on failure
 * ================================================================ */

int pe_load_image(
    const void *pe_data,
    unsigned long pe_size,
    const IMPORT_FUNC_ENTRY *func_table,
    void **out_entry,
    void **out_base)
{
    const UCHAR             *raw;
    const IMAGE_DOS_HEADER  *dos;
    const IMAGE_NT_HEADERS  *nt;
    const IMAGE_SECTION_HEADER *sec;
    PUCHAR                  image;
    ULONG                   image_size;
    ULONG                   num_pages;
    ULONG                   i;
    ULONG                   delta;
    int                     needs_reloc;

    /* ---- Validate inputs ---- */

    if (!pe_data || !out_entry || !out_base) {
        DBGPRINT("PELOAD: null input parameter\n");
        return PE_ERR_NULL_INPUT;
    }

    *out_entry = (void *)0;
    *out_base  = (void *)0;

    if (pe_size < sizeof(IMAGE_DOS_HEADER)) {
        DBGPRINT("PELOAD: file too small for DOS header (%lu bytes)\n", pe_size);
        return PE_ERR_TOO_SMALL;
    }

    raw = (const UCHAR *)pe_data;

    /* ---- Parse DOS header ---- */

    dos = (const IMAGE_DOS_HEADER *)raw;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        DBGPRINT("PELOAD: bad DOS signature: 0x%04X (expected 0x%04X)\n",
                 (unsigned)dos->e_magic, (unsigned)IMAGE_DOS_SIGNATURE);
        return PE_ERR_BAD_DOS_SIG;
    }

    DBGPRINT("PELOAD: DOS header OK, e_lfanew = 0x%08lX\n", (ULONG)dos->e_lfanew);

    if (dos->e_lfanew < 0 ||
        (ULONG)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > pe_size) {
        DBGPRINT("PELOAD: PE header offset out of bounds\n");
        return PE_ERR_BAD_PE_OFFSET;
    }

    /* ---- Parse PE/NT headers ---- */

    nt = (const IMAGE_NT_HEADERS *)(raw + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        DBGPRINT("PELOAD: bad PE signature: 0x%08lX (expected 0x%08lX)\n",
                 nt->Signature, (ULONG)IMAGE_NT_SIGNATURE);
        return PE_ERR_BAD_PE_SIG;
    }

    DBGPRINT("PELOAD: PE signature OK\n");

    /* Check machine type: dispatch PE32+ (AMD64 or IA-64) to 64-bit loader */
    if (nt->FileHeader.Machine == 0x8664 || nt->FileHeader.Machine == 0x0200) {
        DBGPRINT("PELOAD: 64-bit image detected (Machine=0x%04X), "
                 "dispatching to pe_load_image64\n",
                 (unsigned)nt->FileHeader.Machine);
        return pe_load_image64(pe_data, pe_size, func_table, out_entry, out_base);
    }

    /* Accept i386 (0x014C), MIPS R4000 (0x0166), Alpha AXP (0x0184), PPC (0x01F0) */
    if (nt->FileHeader.Machine == 0x0166) {
        DBGPRINT("PELOAD: MIPS R4000 (little-endian) image detected\n");
    } else if (nt->FileHeader.Machine == 0x0184) {
        DBGPRINT("PELOAD: DEC Alpha AXP image detected\n");
    } else if (nt->FileHeader.Machine == 0x01F0) {
        DBGPRINT("PELOAD: PowerPC (little-endian) image detected\n");
    }
    if (nt->FileHeader.Machine != 0x014C &&
        nt->FileHeader.Machine != 0x0166 &&
        nt->FileHeader.Machine != 0x0184 &&
        nt->FileHeader.Machine != 0x01F0) {
        DBGPRINT("PELOAD: unsupported machine type (Machine=0x%04X)\n",
                 (unsigned)nt->FileHeader.Machine);
        return PE_ERR_NOT_I386;
    }

    if (nt->FileHeader.SizeOfOptionalHeader == 0) {
        DBGPRINT("PELOAD: no optional header present\n");
        return PE_ERR_NO_OPTHDR;
    }

    DBGPRINT("PELOAD: Machine=0x%04X, Sections=%u, OptHdrSize=%u\n",
             (unsigned)nt->FileHeader.Machine,
             (unsigned)nt->FileHeader.NumberOfSections,
             (unsigned)nt->FileHeader.SizeOfOptionalHeader);

    DBGPRINT("PELOAD: ImageBase=0x%08lX, SizeOfImage=0x%08lX, EntryPoint=0x%08lX\n",
             nt->OptionalHeader.ImageBase,
             nt->OptionalHeader.SizeOfImage,
             nt->OptionalHeader.AddressOfEntryPoint);

    image_size = nt->OptionalHeader.SizeOfImage;
    if (image_size == 0) {
        DBGPRINT("PELOAD: SizeOfImage is zero\n");
        return PE_ERR_TOO_SMALL;
    }

    /* ---- Allocate ring 0 memory for image ---- */

    num_pages = (image_size + PAGESIZE - 1) / PAGESIZE;
    num_pages += 4; /* guard pages: RTL8029 accesses 0x1613 past SizeOfImage */
    DBGPRINT("PELOAD: allocating %lu pages (%lu bytes) for image\n",
             num_pages, num_pages * PAGESIZE);

    image = (PUCHAR)VxD_PageAllocate(num_pages, PAGEFIXED);
    if (!image) {
        DBGPRINT("PELOAD: VxD_PageAllocate failed\n");
        return PE_ERR_ALLOC_FAIL;
    }

    DBGPRINT("PELOAD: image allocated at 0x%08lX\n", (ULONG)image);

    /* Zero out the entire image region */
    pe_memzero(image, num_pages * PAGESIZE);

    /* ---- Copy PE headers ---- */

    {
        ULONG hdr_size = nt->OptionalHeader.SizeOfHeaders;
        if (hdr_size > pe_size) hdr_size = pe_size;
        if (hdr_size > image_size) hdr_size = image_size;
        pe_memcpy(image, raw, hdr_size);
        DBGPRINT("PELOAD: copied %lu bytes of headers\n", hdr_size);
    }

    /* ---- Map sections ---- */

    sec = (const IMAGE_SECTION_HEADER *)(
        (const UCHAR *)&nt->OptionalHeader +
        nt->FileHeader.SizeOfOptionalHeader
    );

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        ULONG vaddr    = sec[i].VirtualAddress;
        ULONG vsize    = sec[i].Misc.VirtualSize;
        ULONG raw_off  = sec[i].PointerToRawData;
        ULONG raw_size = sec[i].SizeOfRawData;
        ULONG copy_size;

        DBGPRINT("PELOAD: section [%.8s] VA=0x%08lX VSize=0x%08lX "
                 "RawOff=0x%08lX RawSize=0x%08lX\n",
                 sec[i].Name, vaddr, vsize, raw_off, raw_size);

        /* Bounds check: virtual address + size must fit in image */
        if (vaddr + vsize > image_size) {
            /* Use the larger of VirtualSize and SizeOfRawData for check */
            ULONG actual_end = vaddr + (vsize > raw_size ? vsize : raw_size);
            if (vaddr >= image_size) {
                DBGPRINT("PELOAD: section %.8s VA beyond image bounds\n",
                         sec[i].Name);
                VxD_PageFree(image);
                return PE_ERR_SECTION_OOB;
            }
            /* Clamp if slightly over (some linkers do this) */
            if (vsize > image_size - vaddr) {
                vsize = image_size - vaddr;
            }
        }

        /* Copy raw data if present */
        if (raw_off > 0 && raw_size > 0) {
            if (raw_off + raw_size > pe_size) {
                DBGPRINT("PELOAD: section %.8s raw data beyond file bounds\n",
                         sec[i].Name);
                VxD_PageFree(image);
                return PE_ERR_SECTION_OOB;
            }

            copy_size = raw_size;
            if (copy_size > vsize && vsize > 0) {
                copy_size = vsize;
            }
            if (vaddr + copy_size > image_size) {
                copy_size = image_size - vaddr;
            }

            pe_memcpy(image + vaddr, raw + raw_off, copy_size);
            DBGPRINT("PELOAD: copied %lu bytes for section %.8s\n",
                     copy_size, sec[i].Name);
        }

        /* If VirtualSize > SizeOfRawData, the remainder is already zeroed */
    }

    /* ---- Process base relocations ---- */

    delta = (ULONG)image - nt->OptionalHeader.ImageBase;
    needs_reloc = (delta != 0);

    DBGPRINT("PELOAD: preferred base=0x%08lX, actual=0x%08lX, delta=0x%08lX\n",
             nt->OptionalHeader.ImageBase, (ULONG)image, delta);

    if (needs_reloc) {
        IMAGE_DATA_DIRECTORY reloc_dir;
        const IMAGE_BASE_RELOCATION *reloc;
        ULONG reloc_rva;
        ULONG reloc_size;
        ULONG offset;

        if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC) {
            DBGPRINT("PELOAD: image needs relocation but has no reloc directory\n");
            VxD_PageFree(image);
            return PE_ERR_RELOC_FAIL;
        }

        reloc_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        reloc_rva  = reloc_dir.VirtualAddress;
        reloc_size = reloc_dir.Size;

        if (reloc_rva == 0 || reloc_size == 0) {
            DBGPRINT("PELOAD: image needs relocation but reloc directory is empty\n");
            DBGPRINT("PELOAD: WARNING: proceeding without relocations (may crash)\n");
        } else {
            DBGPRINT("PELOAD: processing relocations at RVA 0x%08lX, size %lu\n",
                     reloc_rva, reloc_size);

            if (reloc_rva + reloc_size > image_size) {
                DBGPRINT("PELOAD: relocation directory extends beyond image\n");
                VxD_PageFree(image);
                return PE_ERR_RELOC_FAIL;
            }

            offset = 0;
            while (offset < reloc_size) {
                ULONG block_va;
                ULONG block_sz;
                ULONG num_entries;
                const USHORT *entries;
                ULONG j;

                reloc = (const IMAGE_BASE_RELOCATION *)(image + reloc_rva + offset);
                block_va = reloc->VirtualAddress;
                block_sz = reloc->SizeOfBlock;

                if (block_sz == 0) {
                    DBGPRINT("PELOAD: reloc block size is zero, stopping\n");
                    break;
                }

                if (block_sz < sizeof(IMAGE_BASE_RELOCATION)) {
                    DBGPRINT("PELOAD: reloc block too small (%lu), stopping\n", block_sz);
                    break;
                }

                num_entries = (block_sz - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(USHORT);
                entries = (const USHORT *)((const UCHAR *)reloc +
                                          sizeof(IMAGE_BASE_RELOCATION));

                for (j = 0; j < num_entries; j++) {
                    USHORT entry = entries[j];
                    USHORT type   = (entry >> 12) & 0x0F;
                    USHORT roff   = entry & 0x0FFF;
                    ULONG  target = block_va + roff;

                    switch (type) {
                    case IMAGE_REL_BASED_ABSOLUTE:
                        /* Padding entry, skip */
                        break;

                    case IMAGE_REL_BASED_HIGHLOW:
                        if (target + 4 <= image_size) {
                            PULONG patchaddr = (PULONG)(image + target);
                            *patchaddr += delta;
                        } else {
                            DBGPRINT("PELOAD: HIGHLOW reloc target 0x%08lX out of bounds\n",
                                     target);
                        }
                        break;

                    case IMAGE_REL_BASED_HIGH:
                        if (target + 2 <= image_size) {
                            PUSHORT patchaddr = (PUSHORT)(image + target);
                            ULONG val = ((ULONG)*patchaddr << 16) + delta;
                            *patchaddr = (USHORT)(val >> 16);
                        } else {
                            DBGPRINT("PELOAD: HIGH reloc target 0x%08lX out of bounds\n",
                                     target);
                        }
                        break;

                    case IMAGE_REL_BASED_LOW:
                        if (target + 2 <= image_size) {
                            PUSHORT patchaddr = (PUSHORT)(image + target);
                            *patchaddr = (USHORT)((ULONG)*patchaddr + (USHORT)delta);
                        } else {
                            DBGPRINT("PELOAD: LOW reloc target 0x%08lX out of bounds\n",
                                     target);
                        }
                        break;

                    case IMAGE_REL_BASED_HIGHADJ:
                        /*
                         * HIGHADJ uses two slots: current entry has the high
                         * 16 bits, next entry has the low 16 bits for rounding.
                         */
                        if (target + 2 <= image_size && j + 1 < num_entries) {
                            PUSHORT patchaddr = (PUSHORT)(image + target);
                            USHORT low_part = entries[j + 1];
                            ULONG val = ((ULONG)*patchaddr << 16) + (ULONG)low_part;
                            val += delta;
                            val += 0x8000; /* Round */
                            *patchaddr = (USHORT)(val >> 16);
                            j++; /* Consume the extra entry */
                        } else {
                            DBGPRINT("PELOAD: HIGHADJ reloc at 0x%08lX invalid\n",
                                     target);
                        }
                        break;

                    default:
                        DBGPRINT("PELOAD: unknown reloc type %u at offset 0x%08lX\n",
                                 (unsigned)type, target);
                        break;
                    }
                }

                offset += block_sz;
            }

            DBGPRINT("PELOAD: relocations complete\n");
        }
    } else {
        DBGPRINT("PELOAD: image loaded at preferred base, no relocations needed\n");
    }

    /* ---- Resolve imports ---- */

    if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        IMAGE_DATA_DIRECTORY import_dir;
        const IMAGE_IMPORT_DESCRIPTOR *imp;
        ULONG import_rva;

        import_dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        import_rva = import_dir.VirtualAddress;

        if (import_rva != 0 && import_dir.Size != 0) {
            DBGPRINT("PELOAD: processing imports at RVA 0x%08lX, size %lu\n",
                     import_rva, import_dir.Size);

            if (import_rva + sizeof(IMAGE_IMPORT_DESCRIPTOR) > image_size) {
                DBGPRINT("PELOAD: import directory beyond image bounds\n");
                VxD_PageFree(image);
                return PE_ERR_IMPORT_FAIL;
            }

            imp = (const IMAGE_IMPORT_DESCRIPTOR *)(image + import_rva);

            /* Walk import descriptors until a null entry */
            while (imp->u.OriginalFirstThunk != 0 || imp->FirstThunk != 0) {
                const char *dll_name;

                /* Validate DLL name RVA */
                if (imp->Name == 0 || imp->Name >= image_size) {
                    DBGPRINT("PELOAD: import descriptor has invalid Name RVA 0x%08lX\n",
                             imp->Name);
                    VxD_PageFree(image);
                    return PE_ERR_IMPORT_FAIL;
                }

                dll_name = (const char *)(image + imp->Name);
                DBGPRINT("PELOAD: importing from DLL: %s\n", dll_name);

                /*
                 * Use OriginalFirstThunk (INT) for names if available,
                 * otherwise fall back to FirstThunk.
                 */
                {
                    ULONG int_rva = imp->u.OriginalFirstThunk;
                    ULONG iat_rva = imp->FirstThunk;
                    const IMAGE_THUNK_DATA32 *name_thunk;
                    PULONG iat_entry;

                    if (int_rva == 0) int_rva = iat_rva;

                    if (int_rva >= image_size || iat_rva >= image_size) {
                        DBGPRINT("PELOAD: import thunk RVAs out of bounds "
                                 "(INT=0x%08lX, IAT=0x%08lX)\n",
                                 int_rva, iat_rva);
                        VxD_PageFree(image);
                        return PE_ERR_IMPORT_FAIL;
                    }

                    name_thunk = (const IMAGE_THUNK_DATA32 *)(image + int_rva);
                    iat_entry  = (PULONG)(image + iat_rva);

                    while (name_thunk->u1.AddressOfData != 0) {
                        void *resolved;

                        if (name_thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG32) {
                            ULONG ordinal = name_thunk->u1.Ordinal & 0xFFFF;
                            DBGPRINT("PELOAD: ERROR: ordinal import #%lu not supported\n",
                                     ordinal);
                            VxD_PageFree(image);
                            return PE_ERR_IMPORT_FAIL;
                        }

                        /* Named import */
                        {
                            ULONG hint_rva = name_thunk->u1.AddressOfData;
                            const IMAGE_IMPORT_BY_NAME *hint_name;
                            const char *func_name;

                            if (hint_rva >= image_size) {
                                DBGPRINT("PELOAD: import name RVA 0x%08lX out of bounds\n",
                                         hint_rva);
                                VxD_PageFree(image);
                                return PE_ERR_IMPORT_FAIL;
                            }

                            hint_name = (const IMAGE_IMPORT_BY_NAME *)(image + hint_rva);
                            func_name = hint_name->Name;

                            DBGPRINT("PELOAD:   resolving: %s!%s (hint %u)\n",
                                     dll_name, func_name, (unsigned)hint_name->Hint);

                            /*
                             * Multi-source resolution:
                             *   1. Port driver shim matching dll_name
                             *   2. Loaded image exports matching dll_name
                             *   3. Caller's func_table (backward compat)
                             */
                            resolved = resolve_dll_import(
                                dll_name, func_name, func_table);

                            if (!resolved) {
                                DBGPRINT("PELOAD: UNRESOLVED: ");
                                DBGPRINT(dll_name);
                                DBGPRINT("!");
                                DBGPRINT(func_name);
                                DBGPRINT("\r\n");
                                VxD_PageFree(image);
                                return PE_ERR_IMPORT_FAIL;
                            }

                            DBGPRINT("PELOAD: resolved ");
                            DBGPRINT(func_name);
                            DBGPRINT("\r\n");

                            *iat_entry = (ULONG)resolved;
                        }

                        name_thunk++;
                        iat_entry++;
                    }
                }

                imp++;
            }

            DBGPRINT("PELOAD: all imports resolved\n");
        } else {
            DBGPRINT("PELOAD: no import directory present\n");
        }
    } else {
        DBGPRINT("PELOAD: no import data directory entries\n");
    }

    /* ---- Compute entry point ---- */

    if (nt->OptionalHeader.AddressOfEntryPoint == 0) {
        DBGPRINT("PELOAD: WARNING: AddressOfEntryPoint is zero\n");
        VxD_PageFree(image);
        return PE_ERR_NO_ENTRY;
    }

    *out_entry = (void *)(image + nt->OptionalHeader.AddressOfEntryPoint);
    *out_base  = (void *)image;

    DBGPRINT("PELOAD: load complete\n");
    DBGPRINT("PELOAD:   image base  = 0x%08lX\n", (ULONG)image);
    DBGPRINT("PELOAD:   image size  = 0x%08lX (%lu bytes)\n", image_size, image_size);
    DBGPRINT("PELOAD:   entry point = 0x%08lX\n", (ULONG)*out_entry);

    return PE_OK;
}

/* ================================================================
 * pe_load_image64 - Load a PE32+ (64-bit) image from a memory buffer
 *
 * Parses PE32+ headers, maps sections, processes DIR64 relocations,
 * and resolves imports using 8-byte thunks. The image is loaded into
 * 32-bit ring 0 memory (VxD_PageAllocate), so 64-bit code cannot
 * actually execute on 32-bit Win98. This enables header analysis,
 * import/export listing, and compatibility reporting.
 *
 * 64-bit ImageBase is truncated to lower 32 bits for delta
 * computation. DIR64 relocations zero the upper 4 bytes since our
 * load address is 32-bit. IAT entries are 8 bytes wide; resolved
 * 32-bit shim addresses are written to the lower 4 bytes with the
 * upper 4 bytes zeroed.
 *
 * pe_data:     pointer to the entire .sys file contents in memory
 * pe_size:     size of the file in bytes
 * func_table:  backward-compat fallback import table (may be NULL)
 * out_entry:   receives the entry point address
 * out_base:    receives the loaded image base address
 *
 * Returns: 0 on success, negative error code on failure
 * ================================================================ */

static int pe_load_image64(
    const void *pe_data,
    unsigned long pe_size,
    const IMPORT_FUNC_ENTRY *func_table,
    void **out_entry,
    void **out_base)
{
    const UCHAR               *raw;
    const IMAGE_DOS_HEADER    *dos;
    const IMAGE_NT_HEADERS64  *nt64;
    const IMAGE_SECTION_HEADER *sec;
    PUCHAR                    image;
    ULONG                     image_size;
    ULONG                     num_pages;
    ULONG                     i;
    ULONG                     delta;
    int                       needs_reloc;

    /* ---- Validate inputs ---- */

    if (!pe_data || !out_entry || !out_base) {
        DBGPRINT("PELOAD64: null input parameter\n");
        return PE_ERR_NULL_INPUT;
    }

    *out_entry = (void *)0;
    *out_base  = (void *)0;

    if (pe_size < sizeof(IMAGE_DOS_HEADER)) {
        DBGPRINT("PELOAD64: file too small for DOS header (%lu bytes)\n", pe_size);
        return PE_ERR_TOO_SMALL;
    }

    raw = (const UCHAR *)pe_data;

    /* ---- Parse DOS header ---- */

    dos = (const IMAGE_DOS_HEADER *)raw;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        DBGPRINT("PELOAD64: bad DOS signature: 0x%04X\n", (unsigned)dos->e_magic);
        return PE_ERR_BAD_DOS_SIG;
    }

    DBGPRINT("PELOAD64: DOS header OK, e_lfanew = 0x%08lX\n", (ULONG)dos->e_lfanew);

    /* Bounds check with PE32+ header size (larger than PE32) */
    if (dos->e_lfanew < 0 ||
        (ULONG)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > pe_size) {
        DBGPRINT("PELOAD64: PE header offset out of bounds\n");
        return PE_ERR_BAD_PE_OFFSET;
    }

    /* ---- Parse PE/NT headers (PE32+ layout) ---- */

    /* Struct size sanity check (catches pack/alignment errors at runtime) */
    if (sizeof(IMAGE_OPTIONAL_HEADER64) != 240 ||
        sizeof(IMAGE_NT_HEADERS64) != 264) {
        DBGPRINT("PELOAD64: FATAL: struct size mismatch! "
                 "OptHdr64=%lu (expect 240), NtHdr64=%lu (expect 264)\n",
                 (ULONG)sizeof(IMAGE_OPTIONAL_HEADER64),
                 (ULONG)sizeof(IMAGE_NT_HEADERS64));
        return PE_ERR_BAD_PE32PLUS;
    }

    nt64 = (const IMAGE_NT_HEADERS64 *)(raw + dos->e_lfanew);

    if (nt64->Signature != IMAGE_NT_SIGNATURE) {
        DBGPRINT("PELOAD64: bad PE signature: 0x%08lX\n", nt64->Signature);
        return PE_ERR_BAD_PE_SIG;
    }

    /* Validate Machine type: AMD64 (0x8664) or IA-64 Itanium (0x0200) */
    if (nt64->FileHeader.Machine != 0x8664 &&
        nt64->FileHeader.Machine != 0x0200) {
        DBGPRINT("PELOAD64: not a 64-bit image (Machine=0x%04X)\n",
                 (unsigned)nt64->FileHeader.Machine);
        return PE_ERR_BAD_PE32PLUS;
    }
    if (nt64->FileHeader.Machine == 0x0200) {
        DBGPRINT("PELOAD64: IA-64 (Itanium) image detected\n");
    }

    /* Validate PE32+ magic */
    if (nt64->OptionalHeader.Magic != 0x020B) {
        DBGPRINT("PELOAD64: not PE32+ magic (Magic=0x%04X, expected 0x020B)\n",
                 (unsigned)nt64->OptionalHeader.Magic);
        return PE_ERR_BAD_PE32PLUS;
    }

    if (nt64->FileHeader.SizeOfOptionalHeader == 0) {
        DBGPRINT("PELOAD64: no optional header present\n");
        return PE_ERR_NO_OPTHDR;
    }

    DBGPRINT("PELOAD64: Machine=0x%04X, Sections=%u, OptHdrSize=%u\n",
             (unsigned)nt64->FileHeader.Machine,
             (unsigned)nt64->FileHeader.NumberOfSections,
             (unsigned)nt64->FileHeader.SizeOfOptionalHeader);

    DBGPRINT("PELOAD64: ImageBase=0x%08lX:%08lX, SizeOfImage=0x%08lX, "
             "EntryPoint=0x%08lX\n",
             nt64->OptionalHeader.ImageBase_Hi,
             nt64->OptionalHeader.ImageBase_Lo,
             nt64->OptionalHeader.SizeOfImage,
             nt64->OptionalHeader.AddressOfEntryPoint);

    if (nt64->OptionalHeader.ImageBase_Hi != 0) {
        DBGPRINT("PELOAD64: WARNING: ImageBase upper 32 bits non-zero (0x%08lX), "
                 "image designed for >4GB address space\n",
                 nt64->OptionalHeader.ImageBase_Hi);
    }

    image_size = nt64->OptionalHeader.SizeOfImage;
    if (image_size == 0) {
        DBGPRINT("PELOAD64: SizeOfImage is zero\n");
        return PE_ERR_TOO_SMALL;
    }

    /* ---- Allocate ring 0 memory for image ---- */

    num_pages = (image_size + PAGESIZE - 1) / PAGESIZE;
    DBGPRINT("PELOAD64: allocating %lu pages (%lu bytes) for image\n",
             num_pages, num_pages * PAGESIZE);

    image = (PUCHAR)VxD_PageAllocate(num_pages, PAGEFIXED);
    if (!image) {
        DBGPRINT("PELOAD64: VxD_PageAllocate failed\n");
        return PE_ERR_ALLOC_FAIL;
    }

    DBGPRINT("PELOAD64: image allocated at 0x%08lX\n", (ULONG)image);

    /* Zero out the entire image region */
    pe_memzero(image, num_pages * PAGESIZE);

    /* ---- Copy PE headers ---- */

    {
        ULONG hdr_size = nt64->OptionalHeader.SizeOfHeaders;
        if (hdr_size > pe_size) hdr_size = pe_size;
        if (hdr_size > image_size) hdr_size = image_size;
        pe_memcpy(image, raw, hdr_size);
        DBGPRINT("PELOAD64: copied %lu bytes of headers\n", hdr_size);
    }

    /* ---- Map sections (identical to PE32: RVAs and sizes are 32-bit) ---- */

    sec = (const IMAGE_SECTION_HEADER *)(
        (const UCHAR *)&nt64->OptionalHeader +
        nt64->FileHeader.SizeOfOptionalHeader
    );

    for (i = 0; i < nt64->FileHeader.NumberOfSections; i++) {
        ULONG vaddr    = sec[i].VirtualAddress;
        ULONG vsize    = sec[i].Misc.VirtualSize;
        ULONG raw_off  = sec[i].PointerToRawData;
        ULONG raw_size = sec[i].SizeOfRawData;
        ULONG copy_size;

        DBGPRINT("PELOAD64: section [%.8s] VA=0x%08lX VSize=0x%08lX "
                 "RawOff=0x%08lX RawSize=0x%08lX\n",
                 sec[i].Name, vaddr, vsize, raw_off, raw_size);

        /* Bounds check: virtual address must fit in image */
        if (vaddr + vsize > image_size) {
            if (vaddr >= image_size) {
                DBGPRINT("PELOAD64: section %.8s VA beyond image bounds\n",
                         sec[i].Name);
                VxD_PageFree(image);
                return PE_ERR_SECTION_OOB;
            }
            /* Clamp if slightly over (some linkers do this) */
            if (vsize > image_size - vaddr) {
                vsize = image_size - vaddr;
            }
        }

        /* Copy raw data if present */
        if (raw_off > 0 && raw_size > 0) {
            if (raw_off + raw_size > pe_size) {
                DBGPRINT("PELOAD64: section %.8s raw data beyond file bounds\n",
                         sec[i].Name);
                VxD_PageFree(image);
                return PE_ERR_SECTION_OOB;
            }

            copy_size = raw_size;
            if (copy_size > vsize && vsize > 0) {
                copy_size = vsize;
            }
            if (vaddr + copy_size > image_size) {
                copy_size = image_size - vaddr;
            }

            pe_memcpy(image + vaddr, raw + raw_off, copy_size);
            DBGPRINT("PELOAD64: copied %lu bytes for section %.8s\n",
                     copy_size, sec[i].Name);
        }
    }

    /* ---- Process base relocations (PE32+) ---- */

    /*
     * Delta computation: our load address is 32-bit, the preferred
     * ImageBase may be 64-bit. We use only the lower 32 bits of
     * ImageBase for the delta. For DIR64 fixups, the upper 4 bytes
     * of each 8-byte slot are zeroed (our address space is 32-bit).
     */
    delta = (ULONG)image - nt64->OptionalHeader.ImageBase_Lo;
    needs_reloc = (delta != 0) || (nt64->OptionalHeader.ImageBase_Hi != 0);

    DBGPRINT("PELOAD64: preferred base=0x%08lX:%08lX, actual=0x%08lX, delta=0x%08lX\n",
             nt64->OptionalHeader.ImageBase_Hi,
             nt64->OptionalHeader.ImageBase_Lo,
             (ULONG)image, delta);

    if (needs_reloc) {
        IMAGE_DATA_DIRECTORY reloc_dir;
        const IMAGE_BASE_RELOCATION *reloc;
        ULONG reloc_rva;
        ULONG reloc_size;
        ULONG offset;

        if (nt64->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_BASERELOC) {
            DBGPRINT("PELOAD64: image needs relocation but has no reloc directory\n");
            VxD_PageFree(image);
            return PE_ERR_RELOC_FAIL;
        }

        reloc_dir = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        reloc_rva  = reloc_dir.VirtualAddress;
        reloc_size = reloc_dir.Size;

        if (reloc_rva == 0 || reloc_size == 0) {
            DBGPRINT("PELOAD64: image needs relocation but reloc directory is empty\n");
            DBGPRINT("PELOAD64: WARNING: proceeding without relocations (may crash)\n");
        } else {
            DBGPRINT("PELOAD64: processing relocations at RVA 0x%08lX, size %lu\n",
                     reloc_rva, reloc_size);

            if (reloc_rva + reloc_size > image_size) {
                DBGPRINT("PELOAD64: relocation directory extends beyond image\n");
                VxD_PageFree(image);
                return PE_ERR_RELOC_FAIL;
            }

            offset = 0;
            while (offset < reloc_size) {
                ULONG block_va;
                ULONG block_sz;
                ULONG num_entries;
                const USHORT *entries;
                ULONG j;

                reloc = (const IMAGE_BASE_RELOCATION *)(image + reloc_rva + offset);
                block_va = reloc->VirtualAddress;
                block_sz = reloc->SizeOfBlock;

                if (block_sz == 0) {
                    DBGPRINT("PELOAD64: reloc block size is zero, stopping\n");
                    break;
                }

                if (block_sz < sizeof(IMAGE_BASE_RELOCATION)) {
                    DBGPRINT("PELOAD64: reloc block too small (%lu), stopping\n",
                             block_sz);
                    break;
                }

                num_entries = (block_sz - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(USHORT);
                entries = (const USHORT *)((const UCHAR *)reloc +
                                          sizeof(IMAGE_BASE_RELOCATION));

                for (j = 0; j < num_entries; j++) {
                    USHORT entry = entries[j];
                    USHORT type   = (entry >> 12) & 0x0F;
                    USHORT roff   = entry & 0x0FFF;
                    ULONG  target = block_va + roff;

                    switch (type) {
                    case IMAGE_REL_BASED_ABSOLUTE:
                        /* Padding entry, skip */
                        break;

                    case IMAGE_REL_BASED_HIGHLOW:
                        /* PE32+ images may still contain HIGHLOW entries */
                        if (target + 4 <= image_size) {
                            PULONG patchaddr = (PULONG)(image + target);
                            *patchaddr += delta;
                        } else {
                            DBGPRINT("PELOAD64: HIGHLOW reloc target 0x%08lX "
                                     "out of bounds\n", target);
                        }
                        break;

                    case IMAGE_REL_BASED_DIR64:
                        /*
                         * 8-byte fixup for 64-bit addresses.
                         * Since we're running on 32-bit, the load address
                         * fits in 32 bits. Add delta to the lower ULONG,
                         * then zero the upper ULONG (our address space is
                         * entirely below 4 GB).
                         */
                        if (target + 8 <= image_size) {
                            PULONG patch_lo = (PULONG)(image + target);
                            PULONG patch_hi = (PULONG)(image + target + 4);
                            *patch_lo += delta;
                            *patch_hi = 0;
                        } else {
                            DBGPRINT("PELOAD64: DIR64 reloc target 0x%08lX "
                                     "out of bounds\n", target);
                        }
                        break;

                    default:
                        DBGPRINT("PELOAD64: unknown reloc type %u at offset "
                                 "0x%08lX\n", (unsigned)type, target);
                        break;
                    }
                }

                offset += block_sz;
            }

            DBGPRINT("PELOAD64: relocations complete\n");
        }
    } else {
        DBGPRINT("PELOAD64: image loaded at preferred base, no relocations needed\n");
    }

    /* ---- Resolve imports (PE32+ uses 8-byte thunks) ---- */

    if (nt64->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
        IMAGE_DATA_DIRECTORY import_dir;
        const IMAGE_IMPORT_DESCRIPTOR *imp;
        ULONG import_rva;

        import_dir = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        import_rva = import_dir.VirtualAddress;

        if (import_rva != 0 && import_dir.Size != 0) {
            DBGPRINT("PELOAD64: processing imports at RVA 0x%08lX, size %lu\n",
                     import_rva, import_dir.Size);

            if (import_rva + sizeof(IMAGE_IMPORT_DESCRIPTOR) > image_size) {
                DBGPRINT("PELOAD64: import directory beyond image bounds\n");
                VxD_PageFree(image);
                return PE_ERR_IMPORT_FAIL;
            }

            /* IMAGE_IMPORT_DESCRIPTOR is the same struct for PE32 and PE32+ */
            imp = (const IMAGE_IMPORT_DESCRIPTOR *)(image + import_rva);

            while (imp->u.OriginalFirstThunk != 0 || imp->FirstThunk != 0) {
                const char *dll_name;

                if (imp->Name == 0 || imp->Name >= image_size) {
                    DBGPRINT("PELOAD64: import descriptor has invalid Name RVA "
                             "0x%08lX\n", imp->Name);
                    VxD_PageFree(image);
                    return PE_ERR_IMPORT_FAIL;
                }

                dll_name = (const char *)(image + imp->Name);
                DBGPRINT("PELOAD64: importing from DLL: %s\n", dll_name);

                /*
                 * PE32+ thunks are 8 bytes wide (IMAGE_THUNK_DATA64).
                 * The RVA to IMAGE_IMPORT_BY_NAME lives in the lower
                 * 4 bytes (Lo). The ordinal flag is bit 31 of Hi
                 * (= bit 63 of the full 64-bit value).
                 */
                {
                    ULONG int_rva = imp->u.OriginalFirstThunk;
                    ULONG iat_rva = imp->FirstThunk;
                    const IMAGE_THUNK_DATA64 *name_thunk;
                    IMAGE_THUNK_DATA64 *iat_entry;

                    if (int_rva == 0) int_rva = iat_rva;

                    if (int_rva >= image_size || iat_rva >= image_size) {
                        DBGPRINT("PELOAD64: import thunk RVAs out of bounds "
                                 "(INT=0x%08lX, IAT=0x%08lX)\n",
                                 int_rva, iat_rva);
                        VxD_PageFree(image);
                        return PE_ERR_IMPORT_FAIL;
                    }

                    name_thunk = (const IMAGE_THUNK_DATA64 *)(image + int_rva);
                    iat_entry  = (IMAGE_THUNK_DATA64 *)(image + iat_rva);

                    /* Walk until both Lo and Hi are zero (null terminator) */
                    while (name_thunk->Lo != 0 || name_thunk->Hi != 0) {
                        void *resolved;

                        /* Check ordinal flag (bit 63 = bit 31 of Hi) */
                        if (name_thunk->Hi & IMAGE_ORDINAL_FLAG64_HI) {
                            ULONG ordinal = name_thunk->Lo & 0xFFFF;
                            DBGPRINT("PELOAD64: ERROR: ordinal import #%lu "
                                     "not supported\n", ordinal);
                            VxD_PageFree(image);
                            return PE_ERR_IMPORT_FAIL;
                        }

                        /* Named import: RVA is in Lo */
                        {
                            ULONG hint_rva = name_thunk->Lo;
                            const IMAGE_IMPORT_BY_NAME *hint_name;
                            const char *func_name;

                            if (hint_rva >= image_size) {
                                DBGPRINT("PELOAD64: import name RVA 0x%08lX "
                                         "out of bounds\n", hint_rva);
                                VxD_PageFree(image);
                                return PE_ERR_IMPORT_FAIL;
                            }

                            hint_name = (const IMAGE_IMPORT_BY_NAME *)(image + hint_rva);
                            func_name = hint_name->Name;

                            DBGPRINT("PELOAD64:   resolving: %s!%s (hint %u)\n",
                                     dll_name, func_name,
                                     (unsigned)hint_name->Hint);

                            resolved = resolve_dll_import(
                                dll_name, func_name, func_table);

                            if (!resolved) {
                                DBGPRINT("PELOAD64: UNRESOLVED: ");
                                DBGPRINT(dll_name);
                                DBGPRINT("!");
                                DBGPRINT(func_name);
                                DBGPRINT("\r\n");
                                VxD_PageFree(image);
                                return PE_ERR_IMPORT_FAIL;
                            }

                            DBGPRINT("PELOAD64: resolved ");
                            DBGPRINT(func_name);
                            DBGPRINT("\r\n");

                            /*
                             * Write resolved 32-bit address into the
                             * 8-byte IAT slot: lower 4 bytes get the
                             * address, upper 4 bytes zeroed.
                             */
                            iat_entry->Lo = (ULONG)resolved;
                            iat_entry->Hi = 0;
                        }

                        name_thunk++;
                        iat_entry++;
                    }
                }

                imp++;
            }

            DBGPRINT("PELOAD64: all imports resolved\n");
        } else {
            DBGPRINT("PELOAD64: no import directory present\n");
        }
    } else {
        DBGPRINT("PELOAD64: no import data directory entries\n");
    }

    /* ---- Compute entry point ---- */

    if (nt64->OptionalHeader.AddressOfEntryPoint == 0) {
        DBGPRINT("PELOAD64: WARNING: AddressOfEntryPoint is zero\n");
        VxD_PageFree(image);
        return PE_ERR_NO_ENTRY;
    }

    *out_entry = (void *)(image + nt64->OptionalHeader.AddressOfEntryPoint);
    *out_base  = (void *)image;

    DBGPRINT("PELOAD64: load complete\n");
    DBGPRINT("PELOAD64:   image base  = 0x%08lX\n", (ULONG)image);
    DBGPRINT("PELOAD64:   image size  = 0x%08lX (%lu bytes)\n", image_size, image_size);
    DBGPRINT("PELOAD64:   entry point = 0x%08lX\n", (ULONG)*out_entry);
    DBGPRINT("PELOAD64:   NOTE: 64-bit code cannot execute on 32-bit Win98\n");

    /*
     * TODO: pe_parse_exports() currently walks IMAGE_NT_HEADERS (PE32)
     * to find the DataDirectory. The DataDirectory offset differs in
     * PE32+ due to the larger optional header, so pe_parse_exports()
     * cannot be used on PE32+ images without a PE32+ variant.
     */

    return PE_OK;
}

/* ================================================================
 * pe_unload_image - Free a previously loaded PE image
 *
 * Also removes the image from the loaded image registry if present.
 * ================================================================ */

void pe_unload_image(void *image_base)
{
    if (image_base) {
        int i;

        DBGPRINT("PELOAD: unloading image at 0x%08lX\n", (ULONG)image_base);

        /* Remove from loaded image registry if present */
        for (i = 0; i < loaded_image_count; i++) {
            if (loaded_images[i].base == image_base) {
                DBGPRINT("PELOAD: removing '%s' from loaded image registry\n",
                         loaded_images[i].name ? loaded_images[i].name : "(null)");

                /* Shift remaining entries down */
                if (i < loaded_image_count - 1) {
                    ULONG move_size =
                        (ULONG)(loaded_image_count - 1 - i) * sizeof(LOADED_IMAGE);
                    pe_memcpy(&loaded_images[i], &loaded_images[i + 1], move_size);
                }
                loaded_image_count--;
                break;
            }
        }

        VxD_PageFree(image_base);
    }
}
