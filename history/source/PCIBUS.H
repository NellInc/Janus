/*
 * PCIBUS.H - PCI Bus Enumeration and Configuration for IDE Controllers
 *
 * Provides PCI config space access, IDE controller scanning, PDO creation,
 * and BUS_INTERFACE_STANDARD for the NT5 driver stack. The NT5 pciidex.sys
 * bus driver queries this interface to read PCI config space.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#ifndef PCIBUS_H
#define PCIBUS_H

#include "NTKSHIM.H"

/* ================================================================
 * PCI Configuration Space Structure
 *
 * Standard Type 0 PCI header (256 bytes). We define only the fields
 * needed for IDE controller identification and resource discovery.
 * Layout matches PCI Local Bus Specification 2.2.
 * ================================================================ */

typedef struct _PCI_CONFIG_SPACE {
    USHORT  VendorId;           /* 0x00 */
    USHORT  DeviceId;           /* 0x02 */
    USHORT  Command;            /* 0x04 */
    USHORT  Status;             /* 0x06 */
    UCHAR   RevisionId;         /* 0x08 */
    UCHAR   ProgIf;             /* 0x09: Programming interface */
    UCHAR   SubClass;           /* 0x0A */
    UCHAR   BaseClass;          /* 0x0B */
    UCHAR   CacheLineSize;      /* 0x0C */
    UCHAR   LatencyTimer;       /* 0x0D */
    UCHAR   HeaderType;         /* 0x0E */
    UCHAR   BIST;               /* 0x0F */
    ULONG   BAR[6];             /* 0x10-0x27: Base Address Registers 0-5 */
    ULONG   CardbusCIS;         /* 0x28 */
    USHORT  SubVendorId;        /* 0x2C */
    USHORT  SubSystemId;        /* 0x2E */
    ULONG   ROMBaseAddress;     /* 0x30 */
    UCHAR   CapabilitiesPtr;    /* 0x34 */
    UCHAR   Reserved1[3];       /* 0x35-0x37 */
    ULONG   Reserved2;          /* 0x38 */
    UCHAR   InterruptLine;      /* 0x3C */
    UCHAR   InterruptPin;       /* 0x3D */
    UCHAR   MinGrant;           /* 0x3E */
    UCHAR   MaxLatency;         /* 0x3F */
} PCI_CONFIG_SPACE, *PPCI_CONFIG_SPACE;

/* PCI class codes for IDE controllers */
#define PCI_CLASS_MASS_STORAGE      0x01
#define PCI_SUBCLASS_IDE            0x01

/* ================================================================
 * PCI_SLOT_NUMBER
 *
 * Encodes device and function number for a PCI slot.
 * Matches the NT PCI_SLOT_NUMBER layout.
 * ================================================================ */

typedef struct _PCI_SLOT_NUMBER {
    ULONG   DeviceNumber;       /* 0-31 */
    ULONG   FunctionNumber;     /* 0-7  */
} PCI_SLOT_NUMBER, *PPCI_SLOT_NUMBER;

/* ================================================================
 * PCI Scan Result
 *
 * Returned by pci_scan_for_ide() for each IDE controller found.
 * ================================================================ */

typedef struct _PCI_IDE_DEVICE {
    ULONG           Bus;
    ULONG           Device;
    ULONG           Function;
    USHORT          VendorId;
    USHORT          DeviceId;
    UCHAR           ProgIf;
    UCHAR           InterruptLine;
    UCHAR           InterruptPin;
    UCHAR           Reserved;
    ULONG           BAR[6];
    PCI_CONFIG_SPACE FullConfig;     /* Full 64-byte header cache */
    PVOID           Pdo;            /* Created DEVICE_OBJECT (PDO) */
} PCI_IDE_DEVICE, *PPCI_IDE_DEVICE;

/* Maximum IDE controllers we expect to find */
#define PCI_MAX_IDE_CONTROLLERS     4

/* ================================================================
 * BUS_INTERFACE_STANDARD (Simplified)
 *
 * NT5 drivers query IRP_MN_QUERY_INTERFACE for this structure
 * to read/write PCI config space. We provide GetBusData and
 * SetBusData pointing to our port I/O PCI config accessors.
 * ================================================================ */

/* Function pointer types for the bus interface */
typedef ULONG (NTAPI *PGET_BUS_DATA)(
    PVOID   Context,
    ULONG   DataType,
    PVOID   Buffer,
    ULONG   Offset,
    ULONG   Length
);

typedef ULONG (NTAPI *PSET_BUS_DATA)(
    PVOID   Context,
    ULONG   DataType,
    PVOID   Buffer,
    ULONG   Offset,
    ULONG   Length
);

typedef VOID (NTAPI *PINTERFACE_REFERENCE)(PVOID Context);
typedef VOID (NTAPI *PINTERFACE_DEREFERENCE)(PVOID Context);

typedef struct _BUS_INTERFACE_STANDARD {
    USHORT                  Size;
    USHORT                  Version;
    PVOID                   Context;
    PINTERFACE_REFERENCE    InterfaceReference;
    PINTERFACE_DEREFERENCE  InterfaceDereference;
    PGET_BUS_DATA           GetBusData;
    PSET_BUS_DATA           SetBusData;
} BUS_INTERFACE_STANDARD, *PBUS_INTERFACE_STANDARD;

/* ================================================================
 * Function Prototypes
 * ================================================================ */

/*
 * pci_scan_for_ide - Scan PCI bus 0 for IDE controllers
 *
 * Walks all 32 devices x 8 functions on bus 0, reading class code.
 * For each device with BaseClass=0x01 SubClass=0x01 (IDE controller),
 * records the full PCI config header into results[].
 *
 * Returns: number of IDE controllers found (0 to max_results)
 */
ULONG pci_scan_for_ide(PCI_IDE_DEVICE results[], ULONG max_results);

/*
 * pci_read_config - Read PCI configuration space via ports 0xCF8/0xCFC
 *
 * Pattern matches sp_GetBusData from NTMINI_V5.C.
 */
ULONG pci_read_config(ULONG Bus, ULONG DevFunc, ULONG Offset,
                       PVOID Buffer, ULONG Length);

/*
 * pci_write_config - Write PCI configuration space via ports 0xCF8/0xCFC
 */
ULONG pci_write_config(ULONG Bus, ULONG DevFunc, ULONG Offset,
                        PVOID Buffer, ULONG Length);

/*
 * pci_create_pdo - Create a DEVICE_OBJECT (PDO) for an IDE controller
 *
 * The PDO represents the physical hardware. pciidex.sys attaches
 * its FDO on top of this when AddDevice is called.
 */
PDEVICE_OBJECT pci_create_pdo(PPCI_IDE_DEVICE pci_device);

/*
 * pci_get_bus_interface - Fill BUS_INTERFACE_STANDARD for a PDO
 *
 * Called in response to IRP_MN_QUERY_INTERFACE. Provides GetBusData
 * and SetBusData function pointers that read/write PCI config space
 * for the specific device associated with this PDO.
 */
NTSTATUS pci_get_bus_interface(PDEVICE_OBJECT Pdo,
                                PBUS_INTERFACE_STANDARD Interface);

#endif /* PCIBUS_H */
