# Multi-Port-Driver Shim Framework

## Architecture

NT port/miniport lifecycle (all classes):
1. Miniport DriverEntry calls PortXxxInitialize()
2. Port driver calls HwFindAdapter (or equivalent)
3. Port driver calls HwInitialize
4. Port driver calls HwStartIo (or equivalent) per I/O
5. Miniport calls PortXxxNotification on completion

## Core Structures

```c
typedef struct { const char *name; void *func; } IMPORT_FUNC_ENTRY;

typedef struct _PORT_DRIVER_SHIM {
    const char              *dll_name;   /* "scsiport.sys", "videoprt.sys" */
    const IMPORT_FUNC_ENTRY *func_table; /* NULL-terminated */
    int                      func_count;
    int  (*bridge_init)(void *context);
    int  (*bridge_io)(void *request);
    void (*bridge_cleanup)(void);
} PORT_DRIVER_SHIM;

#define MAX_PORT_SHIMS 8
static PORT_DRIVER_SHIM *shim_registry[MAX_PORT_SHIMS];
static int shim_count = 0;

int register_port_driver(PORT_DRIVER_SHIM *shim) {
    if (shim_count >= MAX_PORT_SHIMS) return -1;
    shim_registry[shim_count++] = shim;
    return 0;
}

PORT_DRIVER_SHIM *find_port_driver(const char *dll_name) {
    int i;
    for (i = 0; i < shim_count; i++)
        if (strcmp_nocase(dll_name, shim_registry[i]->dll_name) == 0)
            return shim_registry[i];
    return (PORT_DRIVER_SHIM *)0;
}
```

## Multi-DLL PE Loader

Replace PELOAD.C lines 663-695 (SCSIPORT-only check):

```c
{
    const IMPORT_FUNC_ENTRY *resolve_table = func_table;
    PORT_DRIVER_SHIM *shim = find_port_driver(dll_name);
    if (shim) {
        resolve_table = shim->func_table;
    } else {
        LOADED_IMAGE *dep = find_loaded_image(dll_name);
        if (dep) resolve_table = dep->exports;
        else { VxD_PageFree(image); return PE_ERR_IMPORT_FAIL; }
    }
    /* resolve imports against resolve_table */
}
```

For driver stacks (pciidex.sys + pciide.sys + atapi.sys):

```c
typedef struct _LOADED_IMAGE {
    const char        *name;
    void              *base;
    IMPORT_FUNC_ENTRY  exports[64];
    int                n_exports;
} LOADED_IMAGE;

#define MAX_LOADED_IMAGES 8
static LOADED_IMAGE loaded_images[MAX_LOADED_IMAGES];
static int loaded_image_count = 0;

LOADED_IMAGE *find_loaded_image(const char *name) {
    int i;
    for (i = 0; i < loaded_image_count; i++)
        if (strcmp_nocase(name, loaded_images[i].name) == 0)
            return &loaded_images[i];
    return (LOADED_IMAGE *)0;
}
```

Load order: parse import tables, build dependency graph, load leaves first. ntoskrnl.exe and hal.dll register as PORT_DRIVER_SHIM pseudo-shims so the same resolution path handles kernel/HAL imports without special-casing.

## Per-Class Shim Specifications

### VideoPort (videoprt.sys) -> Win98 mini-VDD (or DDI/DIB as tractable alternative)

**Exports:** VideoPortInitialize, VideoPortGetDeviceBase, VideoPortFreeDeviceBase, VideoPortMapMemory, VideoPortUnmapMemory, VideoPortReadPortUchar/Ushort/Ulong, VideoPortWritePortUchar/Ushort/Ulong, VideoPortSetRegistryParameters, VideoPortGetRegistryParameters, VideoPortInt10, VideoPortStartTimer, VideoPortAllocatePool, VideoPortGetAccessRanges

**Examples:** vga.sys, ati.sys, nv4_mini.sys, s3mini.sys
**~3000 lines.** VideoPortInt10 requires V86 thunking via VMM.

### NDIS (ndis.sys) -> Win98 NDIS 4.0 (ndis.vxd)

**Exports:** NdisInitializeWrapper, NdisMRegisterMiniport, NdisMSetAttributesEx, NdisMRegisterIoPortRange, NdisMRegisterInterrupt, NdisMDeregisterInterrupt, NdisAllocateMemory, NdisFreeMemory, NdisMAllocateSharedMemory, NdisMIndicateReceivePacket, NdisMSendComplete, NdisReadPciSlotInformation, NdisWritePciSlotInformation, NdisMMapIoSpace, NdisMIndicateStatus

**Examples:** ne2000.sys, e1000325.sys, rtl8139.sys
**~4000 lines.** Bulk is NDIS_PACKET descriptor translation (5.x to 4.0).

### PortCls (portcls.sys) -> Win98 MMSYSTEM wave/mixer

**Exports:** PcInitializeAdapterDriver, PcAddAdapterDevice, PcNewPort, PcNewMiniport, PcNewResourceList, PcRegisterSubdevice, PcCompleteIrp, PcNewIrpStreamPhysical, PcRegisterPhysicalConnection, PcNewServiceGroup

**Examples:** ac97.sys, ichaud.sys, es1370.sys
**~5000 lines.** Full KS framework internally. WaveCyclic DMA model must map to MMSYSTEM callbacks.

### StorPort (storport.sys) -> Win98 IOS (same as ScsiPort)

**Exports:** StorPortInitialize, StorPortGetDeviceBase, StorPortFreeDeviceBase, StorPortNotification, StorPortReadPortUchar/Ushort/Ulong, StorPortWritePortUchar/Ushort/Ulong, StorPortGetPhysicalAddress, StorPortGetUncachedExtension, StorPortInitializeDpc, StorPortIssueDpc, StorPortBusy, StorPortReady, StorPortSynchronizeAccess, StorPortGetScatterGatherList

**Examples:** ahci.sys (SATA), usbstor.sys
**~3500 lines.** ScsiPort shim reuses directly. New: DPC queue (trivial in non-preemptive ring 0) + scatter-gather.

## Priority

| # | Class | Score | Rationale |
|---|-------|-------|-----------|
| 1 | VideoPort | High/Med/3K | #1 retro need. V86 thunk is hard but contained. |
| 2 | NDIS | High/High/4K | Win98 NDIS 4.0 exists; shim is version translation. |
| 3 | PortCls | Med/Low/5K | KS is heavy. Many retro cards have native Win9x drivers. |
| 4 | StorPort | Low/High/3.5K | ScsiPort covers storage. Few need AHCI on Win98. |

## Registration Example

```c
static PORT_DRIVER_SHIM scsiport_shim = {
    "scsiport.sys", scsiport_funcs, 37,
    scsiport_bridge_init, scsiport_bridge_io, scsiport_bridge_cleanup
};
static PORT_DRIVER_SHIM videoprt_shim = {
    "videoprt.sys", videoprt_funcs, 15,
    videoprt_bridge_init, videoprt_bridge_io, videoprt_bridge_cleanup
};

register_port_driver(&scsiport_shim);
register_port_driver(&videoprt_shim);
register_port_driver(&ntoskrnl_shim); /* pseudo-shim */
register_port_driver(&hal_shim);      /* pseudo-shim */
```

Miniport loading becomes DLL-agnostic: pe_load_image searches registered shims and loaded images, no hardcoded DLL checks.
