/*
 * PELOAD.C - PE Image Loader for NT Kernel-Mode Drivers on Win9x
 *
 * Part of the NTMINI project: running NT4 SCSI miniport drivers on Win98.
 *
 * Loads a .sys PE image from a memory buffer into ring 0 memory,
 * processes relocations, resolves imports against a provided
 * ScsiPort function table, and returns the DriverEntry address.
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
#define IMAGE_DIRECTORY_ENTRY_IMPORT    1
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5

/* Relocation types */
#define IMAGE_REL_BASED_ABSOLUTE    0
#define IMAGE_REL_BASED_HIGH        1
#define IMAGE_REL_BASED_LOW         2
#define IMAGE_REL_BASED_HIGHLOW     3
#define IMAGE_REL_BASED_HIGHADJ     4

/* Import thunk flag */
#define IMAGE_ORDINAL_FLAG32        0x80000000UL

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

#pragma pack(pop)

/* ================================================================
 * Import function table entry (provided by caller)
 * ================================================================ */

typedef struct {
    const char *name;
    void       *func;
} IMPORT_FUNC_ENTRY;

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
 * Helper: resolve an imported function name against the func table
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
 * pe_load_image - Load a PE image from a memory buffer
 *
 * pe_data:     pointer to the entire .sys file contents in memory
 * pe_size:     size of the file in bytes
 * func_table:  array of {name, func_ptr} pairs, NULL-name terminated
 * out_entry:   receives the entry point (DriverEntry) address
 * out_base:    receives the loaded image base address
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

    /* Verify i386 machine type */
    if (nt->FileHeader.Machine != 0x014C) {
        DBGPRINT("PELOAD: not an i386 image (Machine=0x%04X)\n",
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
                const IMAGE_THUNK_DATA32 *name_thunk;
                PULONG iat_entry;

                /* Validate DLL name RVA */
                if (imp->Name == 0 || imp->Name >= image_size) {
                    DBGPRINT("PELOAD: import descriptor has invalid Name RVA 0x%08lX\n",
                             imp->Name);
                    VxD_PageFree(image);
                    return PE_ERR_IMPORT_FAIL;
                }

                dll_name = (const char *)(image + imp->Name);
                DBGPRINT("PELOAD: importing from DLL: %s\n", dll_name);

                /* Only SCSIPORT.SYS imports are supported */
                if (strcmp_nocase(dll_name, "SCSIPORT.SYS") != 0 &&
                    strcmp_nocase(dll_name, "scsiport.sys") != 0) {
                    /* Try a more lenient check: just look for "scsiport" */
                    int is_scsiport = 0;
                    {
                        const char *p = dll_name;
                        /* Simple check: does the name contain "scsiport" (case insensitive)? */
                        ULONG namelen = pe_strlen(dll_name);
                        if (namelen >= 8) {
                            ULONG k;
                            for (k = 0; k <= namelen - 8; k++) {
                                if (strcmp_nocase(p + k, "scsiport") == 0 ||
                                    (((p[k]   | 0x20) == 's') &&
                                     ((p[k+1] | 0x20) == 'c') &&
                                     ((p[k+2] | 0x20) == 's') &&
                                     ((p[k+3] | 0x20) == 'i') &&
                                     ((p[k+4] | 0x20) == 'p') &&
                                     ((p[k+5] | 0x20) == 'o') &&
                                     ((p[k+6] | 0x20) == 'r') &&
                                     ((p[k+7] | 0x20) == 't'))) {
                                    is_scsiport = 1;
                                    break;
                                }
                            }
                        }
                    }
                    if (!is_scsiport) {
                        DBGPRINT("PELOAD: ERROR: unsupported import DLL: %s\n", dll_name);
                        DBGPRINT("PELOAD: only SCSIPORT.SYS imports are supported\n");
                        VxD_PageFree(image);
                        return PE_ERR_IMPORT_FAIL;
                    }
                }

                /*
                 * Use OriginalFirstThunk (INT) for names if available,
                 * otherwise fall back to FirstThunk.
                 */
                {
                    ULONG int_rva = imp->u.OriginalFirstThunk;
                    ULONG iat_rva = imp->FirstThunk;

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

                            DBGPRINT("PELOAD:   resolving: %s (hint %u)\n",
                                     func_name, (unsigned)hint_name->Hint);

                            resolved = resolve_import(func_name, func_table);
                            if (!resolved) {
                                DBGPRINT("PELOAD: ERROR: unresolved import: %s\n",
                                         func_name);
                                VxD_PageFree(image);
                                return PE_ERR_IMPORT_FAIL;
                            }

                            DBGPRINT("PELOAD:   resolved %s -> 0x%08lX\n",
                                     func_name, (ULONG)resolved);

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
 * Multi-DLL export table for NT5 import resolution
 * ================================================================ */

typedef struct {
    const char *dll_name;
    const IMPORT_FUNC_ENTRY *func_table;
    ULONG func_count;
} DLL_EXPORT_TABLE;

/* ================================================================
 * Stub function infrastructure
 *
 * Pre-allocated array of stub slots. Each slot, when called, logs
 * the function name it was assigned to and returns 0. Since we
 * cannot generate code at runtime in Watcom C, we use a fixed
 * dispatch table with a shared handler.
 * ================================================================ */

#define PE_MAX_STUBS 32

static const char *pe_stub_names[PE_MAX_STUBS];
static ULONG pe_stub_count = 0;

/*
 * pe_stub_dispatch - called by stub trampolines with a slot index.
 * Logs the stub name and returns 0.
 */
static ULONG pe_stub_dispatch(ULONG slot_index)
{
    if (slot_index < PE_MAX_STUBS && pe_stub_names[slot_index]) {
        DBGPRINT("NTK: STUB called for %s\n", pe_stub_names[slot_index]);
    } else {
        DBGPRINT("NTK: STUB called for unknown slot %lu\n", slot_index);
    }
    return 0;
}

/*
 * Individual stub entry points. Each calls pe_stub_dispatch with its
 * slot index. Watcom C requires these to be real functions.
 */
static ULONG pe_stub_0(void)  { return pe_stub_dispatch(0);  }
static ULONG pe_stub_1(void)  { return pe_stub_dispatch(1);  }
static ULONG pe_stub_2(void)  { return pe_stub_dispatch(2);  }
static ULONG pe_stub_3(void)  { return pe_stub_dispatch(3);  }
static ULONG pe_stub_4(void)  { return pe_stub_dispatch(4);  }
static ULONG pe_stub_5(void)  { return pe_stub_dispatch(5);  }
static ULONG pe_stub_6(void)  { return pe_stub_dispatch(6);  }
static ULONG pe_stub_7(void)  { return pe_stub_dispatch(7);  }
static ULONG pe_stub_8(void)  { return pe_stub_dispatch(8);  }
static ULONG pe_stub_9(void)  { return pe_stub_dispatch(9);  }
static ULONG pe_stub_10(void) { return pe_stub_dispatch(10); }
static ULONG pe_stub_11(void) { return pe_stub_dispatch(11); }
static ULONG pe_stub_12(void) { return pe_stub_dispatch(12); }
static ULONG pe_stub_13(void) { return pe_stub_dispatch(13); }
static ULONG pe_stub_14(void) { return pe_stub_dispatch(14); }
static ULONG pe_stub_15(void) { return pe_stub_dispatch(15); }
static ULONG pe_stub_16(void) { return pe_stub_dispatch(16); }
static ULONG pe_stub_17(void) { return pe_stub_dispatch(17); }
static ULONG pe_stub_18(void) { return pe_stub_dispatch(18); }
static ULONG pe_stub_19(void) { return pe_stub_dispatch(19); }
static ULONG pe_stub_20(void) { return pe_stub_dispatch(20); }
static ULONG pe_stub_21(void) { return pe_stub_dispatch(21); }
static ULONG pe_stub_22(void) { return pe_stub_dispatch(22); }
static ULONG pe_stub_23(void) { return pe_stub_dispatch(23); }
static ULONG pe_stub_24(void) { return pe_stub_dispatch(24); }
static ULONG pe_stub_25(void) { return pe_stub_dispatch(25); }
static ULONG pe_stub_26(void) { return pe_stub_dispatch(26); }
static ULONG pe_stub_27(void) { return pe_stub_dispatch(27); }
static ULONG pe_stub_28(void) { return pe_stub_dispatch(28); }
static ULONG pe_stub_29(void) { return pe_stub_dispatch(29); }
static ULONG pe_stub_30(void) { return pe_stub_dispatch(30); }
static ULONG pe_stub_31(void) { return pe_stub_dispatch(31); }

typedef ULONG (*PE_STUB_FUNC)(void);

static const PE_STUB_FUNC pe_stub_table[PE_MAX_STUBS] = {
    pe_stub_0,  pe_stub_1,  pe_stub_2,  pe_stub_3,
    pe_stub_4,  pe_stub_5,  pe_stub_6,  pe_stub_7,
    pe_stub_8,  pe_stub_9,  pe_stub_10, pe_stub_11,
    pe_stub_12, pe_stub_13, pe_stub_14, pe_stub_15,
    pe_stub_16, pe_stub_17, pe_stub_18, pe_stub_19,
    pe_stub_20, pe_stub_21, pe_stub_22, pe_stub_23,
    pe_stub_24, pe_stub_25, pe_stub_26, pe_stub_27,
    pe_stub_28, pe_stub_29, pe_stub_30, pe_stub_31
};

/* ================================================================
 * pe_make_stub - Allocate a stub slot for an unresolved function
 *
 * Returns a function pointer that, when called, logs the name
 * and returns 0. Returns NULL if all stub slots are exhausted.
 * ================================================================ */

static void *pe_make_stub(const char *func_name)
{
    ULONG slot;

    if (pe_stub_count >= PE_MAX_STUBS) {
        DBGPRINT("PELOAD: WARNING: stub slots exhausted, cannot stub %s\n",
                 func_name);
        return (void *)0;
    }

    slot = pe_stub_count++;
    pe_stub_names[slot] = func_name;
    DBGPRINT("PELOAD: created stub slot %lu for %s\n", slot, func_name);
    return (void *)pe_stub_table[slot];
}

/* ================================================================
 * Helper: find a DLL export table by name (case-insensitive)
 * ================================================================ */

static const DLL_EXPORT_TABLE *find_dll_table(
    const char *dll_name,
    const DLL_EXPORT_TABLE *dll_tables,
    ULONG dll_count)
{
    ULONG i;

    for (i = 0; i < dll_count; i++) {
        if (strcmp_nocase(dll_name, dll_tables[i].dll_name) == 0) {
            return &dll_tables[i];
        }
    }
    return (const DLL_EXPORT_TABLE *)0;
}

/* ================================================================
 * Helper: resolve an import against a specific function table,
 * with count-based bounds (no NULL terminator required)
 * ================================================================ */

static void *resolve_import_counted(
    const char *name,
    const IMPORT_FUNC_ENTRY *table,
    ULONG count)
{
    ULONG i;

    if (!table || !name) return (void *)0;

    for (i = 0; i < count; i++) {
        if (table[i].name && strcmp_nocase(name, table[i].name) == 0) {
            return table[i].func;
        }
    }
    return (void *)0;
}

/* ================================================================
 * pe_load_image_multi - Load a PE image with multi-DLL import
 *                       resolution for NT5 support
 *
 * pe_data:      pointer to the entire .sys file contents in memory
 * pe_size:      size of the file in bytes
 * dll_tables:   array of DLL_EXPORT_TABLE entries
 * dll_count:    number of entries in dll_tables
 * out_entry:    receives the entry point (DriverEntry) address
 * out_base:     receives the loaded image base address
 *
 * Returns: 0 on success, negative error code on failure
 * ================================================================ */

int pe_load_image_multi(
    const void *pe_data,
    unsigned long pe_size,
    const DLL_EXPORT_TABLE *dll_tables,
    ULONG dll_count,
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

    if (nt->FileHeader.Machine != 0x014C) {
        DBGPRINT("PELOAD: not an i386 image (Machine=0x%04X)\n",
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
    DBGPRINT("PELOAD: allocating %lu pages (%lu bytes) for image\n",
             num_pages, num_pages * PAGESIZE);

    image = (PUCHAR)VxD_PageAllocate(num_pages, PAGEFIXED);
    if (!image) {
        DBGPRINT("PELOAD: VxD_PageAllocate failed\n");
        return PE_ERR_ALLOC_FAIL;
    }

    DBGPRINT("PELOAD: image allocated at 0x%08lX\n", (ULONG)image);

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

        if (vaddr + vsize > image_size) {
            ULONG actual_end = vaddr + (vsize > raw_size ? vsize : raw_size);
            if (vaddr >= image_size) {
                DBGPRINT("PELOAD: section %.8s VA beyond image bounds\n",
                         sec[i].Name);
                VxD_PageFree(image);
                return PE_ERR_SECTION_OOB;
            }
            if (vsize > image_size - vaddr) {
                vsize = image_size - vaddr;
            }
        }

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
                        if (target + 2 <= image_size && j + 1 < num_entries) {
                            PUSHORT patchaddr = (PUSHORT)(image + target);
                            USHORT low_part = entries[j + 1];
                            ULONG val = ((ULONG)*patchaddr << 16) + (ULONG)low_part;
                            val += delta;
                            val += 0x8000;
                            *patchaddr = (USHORT)(val >> 16);
                            j++;
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

    /* ---- Resolve imports (multi-DLL) ---- */

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

            while (imp->u.OriginalFirstThunk != 0 || imp->FirstThunk != 0) {
                const char *dll_name;
                const DLL_EXPORT_TABLE *dll_tab;

                if (imp->Name == 0 || imp->Name >= image_size) {
                    DBGPRINT("PELOAD: import descriptor has invalid Name RVA 0x%08lX\n",
                             imp->Name);
                    VxD_PageFree(image);
                    return PE_ERR_IMPORT_FAIL;
                }

                dll_name = (const char *)(image + imp->Name);
                DBGPRINT("PELOAD: importing from DLL: %s\n", dll_name);

                /* Find the matching DLL export table */
                dll_tab = find_dll_table(dll_name, dll_tables, dll_count);

                if (!dll_tab) {
                    DBGPRINT("PELOAD: WARNING: no export table for DLL: %s (skipping)\n",
                             dll_name);
                    imp++;
                    continue;
                }

                /* Resolve all imports from this DLL */
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
                            DBGPRINT("PELOAD: WARNING: ordinal import #%lu from %s, stubbing\n",
                                     ordinal, dll_name);
                            resolved = pe_make_stub("ordinal_import");
                            if (!resolved) resolved = (void *)0;
                            *iat_entry = (ULONG)resolved;
                            name_thunk++;
                            iat_entry++;
                            continue;
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

                            DBGPRINT("PELOAD:   resolving: %s (hint %u)\n",
                                     func_name, (unsigned)hint_name->Hint);

                            resolved = resolve_import_counted(
                                func_name, dll_tab->func_table, dll_tab->func_count);

                            if (!resolved) {
                                DBGPRINT("PELOAD: WARNING: unresolved import %s from %s, stubbing\n",
                                         func_name, dll_name);
                                resolved = pe_make_stub(func_name);
                                if (!resolved) {
                                    DBGPRINT("PELOAD: ERROR: stub allocation failed for %s\n",
                                             func_name);
                                    VxD_PageFree(image);
                                    return PE_ERR_IMPORT_FAIL;
                                }
                            }

                            DBGPRINT("PELOAD:   resolved %s -> 0x%08lX\n",
                                     func_name, (ULONG)resolved);

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

    DBGPRINT("PELOAD: load complete (multi-DLL)\n");
    DBGPRINT("PELOAD:   image base  = 0x%08lX\n", (ULONG)image);
    DBGPRINT("PELOAD:   image size  = 0x%08lX (%lu bytes)\n", image_size, image_size);
    DBGPRINT("PELOAD:   entry point = 0x%08lX\n", (ULONG)*out_entry);
    DBGPRINT("PELOAD:   stubs used  = %lu / %lu\n", pe_stub_count, (ULONG)PE_MAX_STUBS);

    return PE_OK;
}

/* ================================================================
 * pe_unload_image - Free a previously loaded PE image
 * ================================================================ */

void pe_unload_image(void *image_base)
{
    if (image_base) {
        DBGPRINT("PELOAD: unloading image at 0x%08lX\n", (ULONG)image_base);
        VxD_PageFree(image_base);
    }
}
