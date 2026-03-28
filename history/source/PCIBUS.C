/*
 * PCIBUS.C - PCI Bus Enumeration and Configuration for IDE Controllers
 *
 * Scans the PCI bus for IDE controllers, provides PCI config space
 * read/write via ports 0xCF8/0xCFC, creates PDOs (Physical Device
 * Objects) for discovered controllers, and implements the
 * BUS_INTERFACE_STANDARD that pciidex.sys uses to access PCI config.
 *
 * The PCI config access pattern is derived from sp_GetBusData in
 * NTMINI_V5.C, extended to support arbitrary offsets and write access.
 *
 * AUTHOR:  Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#include "W9XDDK.H"
#include "PORTIO.H"
#include "NTKSHIM.H"
#include "PCIBUS.H"

/* ================================================================
 * Forward Declarations
 * ================================================================ */

static void pci_zero_mem(PVOID dst, ULONG size);
static void pci_copy_mem(PVOID dst, PVOID src, ULONG size);

/* Bus interface callbacks (static, wired into BUS_INTERFACE_STANDARD) */
static ULONG NTAPI pci_bus_get_data(PVOID Context, ULONG DataType,
                                     PVOID Buffer, ULONG Offset,
                                     ULONG Length);
static ULONG NTAPI pci_bus_set_data(PVOID Context, ULONG DataType,
                                     PVOID Buffer, ULONG Offset,
                                     ULONG Length);
static VOID NTAPI pci_interface_ref(PVOID Context);
static VOID NTAPI pci_interface_deref(PVOID Context);


/* ================================================================
 * PART 1: PCI CONFIGURATION SPACE ACCESS
 *
 * x86 PCI config access uses the CONFIG_ADDRESS (0xCF8) and
 * CONFIG_DATA (0xCFC) mechanism defined in the PCI specification.
 *
 * CONFIG_ADDRESS format (32 bits):
 *   Bit 31:     Enable (must be 1)
 *   Bits 23-16: Bus number (0-255)
 *   Bits 15-11: Device number (0-31)
 *   Bits 10-8:  Function number (0-7)
 *   Bits 7-2:   Register offset (dword-aligned)
 *   Bits 1-0:   Must be 0
 *
 * CONFIG_DATA (0xCFC): Read/write the selected dword.
 * ================================================================ */

/*
 * pci_read_config - Read PCI configuration space
 *
 * Parameters:
 *   Bus     - PCI bus number (typically 0)
 *   DevFunc - Device number in bits 0-4, function in bits 5-7
 *             (matches NT SlotNumber encoding from sp_GetBusData)
 *   Offset  - Byte offset into config space to start reading
 *   Buffer  - Destination buffer
 *   Length  - Number of bytes to read
 *
 * Returns: Number of bytes actually read, or 0 on error
 */
ULONG pci_read_config(ULONG Bus, ULONG DevFunc, ULONG Offset,
                       PVOID Buffer, ULONG Length)
{
    ULONG devNum, funcNum, regOff, cfgAddr;
    ULONG i;
    UCHAR *buf = (UCHAR *)Buffer;

    if (Length == 0 || !Buffer) {
        return 0;
    }

    devNum  = DevFunc & 0x1F;
    funcNum = (DevFunc >> 5) & 0x07;

    /* Read PCI config space 4 bytes at a time, extract requested bytes.
     * This is the same pattern used in sp_GetBusData (NTMINI_V5.C). */
    for (i = 0; i < Length; i += 4) {
        regOff  = (Offset + i) & 0xFC;
        cfgAddr = 0x80000000 | (Bus << 16) |
                  (devNum << 11) | (funcNum << 8) | regOff;
        PORT_OUT_DWORD(0xCF8, cfgAddr);
        {
            ULONG val = PORT_IN_DWORD(0xCFC);
            ULONG byteOff = (Offset + i) & 0x03;
            ULONG j;
            for (j = 0; j < 4 && (i + j) < Length; j++) {
                buf[i + j] = (UCHAR)(val >> ((byteOff + j) * 8));
            }
        }
    }

    /* Check if device exists: vendor 0xFFFF means empty slot */
    if (Offset == 0 && Length >= 2 && buf[0] == 0xFF && buf[1] == 0xFF) {
        return 0;
    }

    return Length;
}


/*
 * pci_write_config - Write PCI configuration space
 *
 * Parameters:
 *   Bus     - PCI bus number
 *   DevFunc - Device number in bits 0-4, function in bits 5-7
 *   Offset  - Byte offset into config space to start writing
 *   Buffer  - Source buffer
 *   Length  - Number of bytes to write
 *
 * Returns: Number of bytes actually written, or 0 on error
 *
 * Writing PCI config space requires a read-modify-write cycle for
 * sub-dword writes to preserve the other bytes in the dword.
 */
ULONG pci_write_config(ULONG Bus, ULONG DevFunc, ULONG Offset,
                        PVOID Buffer, ULONG Length)
{
    ULONG devNum, funcNum, regOff, cfgAddr;
    ULONG i;
    UCHAR *buf = (UCHAR *)Buffer;

    if (Length == 0 || !Buffer) {
        return 0;
    }

    devNum  = DevFunc & 0x1F;
    funcNum = (DevFunc >> 5) & 0x07;

    for (i = 0; i < Length; i += 4) {
        ULONG val;
        ULONG byteOff = (Offset + i) & 0x03;
        ULONG remaining = Length - i;
        ULONG j;

        regOff  = (Offset + i) & 0xFC;
        cfgAddr = 0x80000000 | (Bus << 16) |
                  (devNum << 11) | (funcNum << 8) | regOff;

        if (remaining < 4 || byteOff != 0) {
            /* Sub-dword write: read-modify-write */
            PORT_OUT_DWORD(0xCF8, cfgAddr);
            val = PORT_IN_DWORD(0xCFC);

            for (j = 0; j < 4 && (i + j) < Length; j++) {
                ULONG shift = (byteOff + j) * 8;
                val &= ~(0xFF << shift);
                val |= ((ULONG)buf[i + j]) << shift;
            }
        } else {
            /* Full dword write */
            val = (ULONG)buf[i] |
                  ((ULONG)buf[i + 1] << 8) |
                  ((ULONG)buf[i + 2] << 16) |
                  ((ULONG)buf[i + 3] << 24);
        }

        PORT_OUT_DWORD(0xCF8, cfgAddr);
        PORT_OUT_DWORD(0xCFC, val);
    }

    return Length;
}


/* ================================================================
 * PART 2: PCI BUS SCAN FOR IDE CONTROLLERS
 *
 * Walks bus 0, all 32 device slots, all 8 functions. For each
 * device that responds (vendor != 0xFFFF) and has class 0x01
 * subclass 0x01 (IDE controller), we record the full config header.
 *
 * We only scan bus 0 because IDE controllers are always on the
 * root bus in the PC systems this project targets (Win98-era PCs).
 * ================================================================ */

ULONG pci_scan_for_ide(PCI_IDE_DEVICE results[], ULONG max_results)
{
    ULONG count = 0;
    ULONG dev, func;
    UCHAR config_buf[64]; /* First 64 bytes of PCI config header */

    VxD_Debug_Printf("PCI: Scanning bus 0 for IDE controllers\n");

    for (dev = 0; dev < 32; dev++) {
        for (func = 0; func < 8; func++) {
            ULONG devFunc;
            ULONG bytesRead;
            UCHAR baseClass, subClass;
            PPCI_CONFIG_SPACE cfg;

            if (count >= max_results) {
                goto done;
            }

            /* Encode device/function the same way NT does */
            devFunc = (dev & 0x1F) | ((func & 0x07) << 5);

            /* Read first 64 bytes of config space */
            bytesRead = pci_read_config(0, devFunc, 0,
                                         config_buf, 64);
            if (bytesRead == 0) {
                /* No device at this slot. If function 0 is empty,
                 * skip remaining functions (single-function device). */
                if (func == 0) {
                    break;
                }
                continue;
            }

            cfg = (PPCI_CONFIG_SPACE)config_buf;

            /* Check for multi-function device (bit 7 of HeaderType).
             * If function 0 is not multi-function, skip functions 1-7. */
            if (func == 0 && !(cfg->HeaderType & 0x80)) {
                /* Single-function device: check this one, skip rest */
                baseClass = cfg->BaseClass;
                subClass  = cfg->SubClass;

                if (baseClass == PCI_CLASS_MASS_STORAGE &&
                    subClass  == PCI_SUBCLASS_IDE) {
                    PPCI_IDE_DEVICE entry = &results[count];
                    ULONG bar;

                    pci_zero_mem(entry, sizeof(PCI_IDE_DEVICE));
                    entry->Bus          = 0;
                    entry->Device       = dev;
                    entry->Function     = func;
                    entry->VendorId     = cfg->VendorId;
                    entry->DeviceId     = cfg->DeviceId;
                    entry->ProgIf       = cfg->ProgIf;
                    entry->InterruptLine = cfg->InterruptLine;
                    entry->InterruptPin  = cfg->InterruptPin;

                    for (bar = 0; bar < 6; bar++) {
                        entry->BAR[bar] = cfg->BAR[bar];
                    }

                    pci_copy_mem(&entry->FullConfig, cfg,
                                 sizeof(PCI_CONFIG_SPACE));
                    entry->Pdo = NULL;

                    VxD_Debug_Printf(
                        "PCI: IDE controller at %lu:%lu.%lu "
                        "vendor=%04x device=%04x progif=%02x irq=%d\n",
                        (ULONG)0, dev, func,
                        (ULONG)cfg->VendorId, (ULONG)cfg->DeviceId,
                        (ULONG)cfg->ProgIf, (ULONG)cfg->InterruptLine);

                    count++;
                }

                /* Skip to next device (already handled func 0) */
                break;
            }

            /* Multi-function device or func > 0 */
            baseClass = cfg->BaseClass;
            subClass  = cfg->SubClass;

            if (baseClass == PCI_CLASS_MASS_STORAGE &&
                subClass  == PCI_SUBCLASS_IDE) {
                PPCI_IDE_DEVICE entry = &results[count];
                ULONG bar;

                pci_zero_mem(entry, sizeof(PCI_IDE_DEVICE));
                entry->Bus          = 0;
                entry->Device       = dev;
                entry->Function     = func;
                entry->VendorId     = cfg->VendorId;
                entry->DeviceId     = cfg->DeviceId;
                entry->ProgIf       = cfg->ProgIf;
                entry->InterruptLine = cfg->InterruptLine;
                entry->InterruptPin  = cfg->InterruptPin;

                for (bar = 0; bar < 6; bar++) {
                    entry->BAR[bar] = cfg->BAR[bar];
                }

                pci_copy_mem(&entry->FullConfig, cfg,
                             sizeof(PCI_CONFIG_SPACE));
                entry->Pdo = NULL;

                VxD_Debug_Printf(
                    "PCI: IDE controller at %lu:%lu.%lu "
                    "vendor=%04x device=%04x progif=%02x irq=%d\n",
                    (ULONG)0, dev, func,
                    (ULONG)cfg->VendorId, (ULONG)cfg->DeviceId,
                    (ULONG)cfg->ProgIf, (ULONG)cfg->InterruptLine);

                count++;
            }
        }
    }

done:
    VxD_Debug_Printf("PCI: Scan complete, %lu IDE controller(s) found\n",
                     count);
    return count;
}


/* ================================================================
 * PART 3: PDO CREATION
 *
 * A Physical Device Object (PDO) represents the actual PCI device.
 * The bus driver (pciidex.sys in NT5) expects a PDO to exist before
 * it can call AddDevice. We create a minimal PDO with our PCI device
 * info stored in the DeviceExtension.
 *
 * On real NT, the PCI bus driver creates PDOs during enumeration.
 * We simulate that here because Win9x has no PCI bus driver.
 * ================================================================ */

/*
 * pci_create_pdo - Create a DEVICE_OBJECT (PDO) for an IDE controller
 *
 * Allocates a DEVICE_OBJECT via IoCreateDevice with a DeviceExtension
 * pointing to the PCI_IDE_DEVICE info. This PDO is what pciidex.sys
 * will receive in its AddDevice call.
 */
PDEVICE_OBJECT pci_create_pdo(PPCI_IDE_DEVICE pci_device)
{
    PDEVICE_OBJECT pdo;
    NTSTATUS status;

    VxD_Debug_Printf("PCI: Creating PDO for %lu:%lu.%lu\n",
                     pci_device->Bus, pci_device->Device,
                     pci_device->Function);

    /* IoCreateDevice allocates the DEVICE_OBJECT + extension.
     * We use FILE_DEVICE_CONTROLLER as the device type since this
     * represents an IDE controller, not a disk or CD-ROM directly.
     *
     * DriverObject is NULL because the PDO is not owned by any
     * particular WDM driver yet. The bus driver (pciidex) will
     * attach its FDO on top of this. */
    status = IoCreateDevice(
        NULL,                               /* DriverObject (bus owns it) */
        sizeof(PCI_IDE_DEVICE),             /* DeviceExtensionSize */
        NULL,                               /* DeviceName (unnamed) */
        FILE_DEVICE_CONTROLLER,             /* DeviceType */
        0,                                  /* DeviceCharacteristics */
        FALSE,                              /* Exclusive */
        &pdo);

    if (!NT_SUCCESS(status) || !pdo) {
        VxD_Debug_Printf("PCI: IoCreateDevice failed, status=%08lx\n",
                         (ULONG)status);
        return NULL;
    }

    /* Copy PCI device info into the DeviceExtension.
     * Drivers querying the PDO's extension will find the PCI config. */
    if (pdo->DeviceExtension) {
        pci_copy_mem(pdo->DeviceExtension, pci_device,
                     sizeof(PCI_IDE_DEVICE));
    }

    /* Store the PDO pointer back in our scan result */
    pci_device->Pdo = pdo;

    VxD_Debug_Printf("PCI: PDO created at %08lx for %04x:%04x\n",
                     (ULONG)pdo,
                     (ULONG)pci_device->VendorId,
                     (ULONG)pci_device->DeviceId);

    return pdo;
}


/* ================================================================
 * PART 4: BUS INTERFACE
 *
 * NT5 drivers (especially pciidex.sys and atapi.sys) obtain PCI
 * config space access by sending IRP_MN_QUERY_INTERFACE with
 * GUID_BUS_INTERFACE_STANDARD. The bus driver returns a
 * BUS_INTERFACE_STANDARD structure with GetBusData/SetBusData
 * function pointers.
 *
 * Our GetBusData/SetBusData callbacks read/write PCI config space
 * using direct port I/O, with the bus/device/function encoded in
 * the Context pointer (which points to our PCI_IDE_DEVICE).
 * ================================================================ */

/*
 * pci_bus_get_data - BUS_INTERFACE_STANDARD GetBusData callback
 *
 * Context is a pointer to PCI_IDE_DEVICE, from which we extract
 * bus/device/function to form the PCI config address.
 */
static ULONG NTAPI pci_bus_get_data(PVOID Context, ULONG DataType,
                                     PVOID Buffer, ULONG Offset,
                                     ULONG Length)
{
    PPCI_IDE_DEVICE dev = (PPCI_IDE_DEVICE)Context;
    ULONG devFunc;

    if (!dev || !Buffer || Length == 0) {
        return 0;
    }

    /* DataType 4 = PCIConfiguration (same check as sp_GetBusData) */
    if (DataType != 4) {
        return 0;
    }

    devFunc = (dev->Device & 0x1F) | ((dev->Function & 0x07) << 5);
    return pci_read_config(dev->Bus, devFunc, Offset, Buffer, Length);
}


/*
 * pci_bus_set_data - BUS_INTERFACE_STANDARD SetBusData callback
 */
static ULONG NTAPI pci_bus_set_data(PVOID Context, ULONG DataType,
                                     PVOID Buffer, ULONG Offset,
                                     ULONG Length)
{
    PPCI_IDE_DEVICE dev = (PPCI_IDE_DEVICE)Context;
    ULONG devFunc;

    if (!dev || !Buffer || Length == 0) {
        return 0;
    }

    if (DataType != 4) {
        return 0;
    }

    devFunc = (dev->Device & 0x1F) | ((dev->Function & 0x07) << 5);
    return pci_write_config(dev->Bus, devFunc, Offset, Buffer, Length);
}


/*
 * pci_interface_ref / pci_interface_deref - Reference counting stubs
 *
 * In our shim environment, the PCI device info is statically allocated
 * for the lifetime of the VxD, so reference counting is a no-op.
 */
static VOID NTAPI pci_interface_ref(PVOID Context)
{
    /* No-op: device lifetime matches VxD lifetime */
}

static VOID NTAPI pci_interface_deref(PVOID Context)
{
    /* No-op */
}


/*
 * pci_get_bus_interface - Fill BUS_INTERFACE_STANDARD for a PDO
 *
 * Called when the PnP manager (our shim) processes IRP_MN_QUERY_INTERFACE.
 * The Context pointer in the interface will be the PCI_IDE_DEVICE stored
 * in the PDO's DeviceExtension.
 */
NTSTATUS pci_get_bus_interface(PDEVICE_OBJECT Pdo,
                                PBUS_INTERFACE_STANDARD Interface)
{
    PPCI_IDE_DEVICE dev;

    if (!Pdo || !Interface) {
        return STATUS_INVALID_PARAMETER;
    }

    dev = (PPCI_IDE_DEVICE)Pdo->DeviceExtension;
    if (!dev) {
        VxD_Debug_Printf("PCI: PDO has no DeviceExtension\n");
        return STATUS_INVALID_PARAMETER;
    }

    Interface->Size                 = sizeof(BUS_INTERFACE_STANDARD);
    Interface->Version              = 1;
    Interface->Context              = dev;
    Interface->InterfaceReference   = pci_interface_ref;
    Interface->InterfaceDereference = pci_interface_deref;
    Interface->GetBusData           = pci_bus_get_data;
    Interface->SetBusData           = pci_bus_set_data;

    VxD_Debug_Printf("PCI: Bus interface filled for %04x:%04x\n",
                     (ULONG)dev->VendorId, (ULONG)dev->DeviceId);

    return STATUS_SUCCESS;
}


/* ================================================================
 * PART 5: UTILITY FUNCTIONS
 * ================================================================ */

static void pci_zero_mem(PVOID dst, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = 0;
    }
}

static void pci_copy_mem(PVOID dst, PVOID src, ULONG size)
{
    PUCHAR d = (PUCHAR)dst;
    PUCHAR s = (PUCHAR)src;
    ULONG i;
    for (i = 0; i < size; i++) {
        d[i] = s[i];
    }
}
