# NT5-9x Driver Backport

A reverse engineering project that backports Windows NT5 (2000/XP) miniport drivers
to run on Windows 9x and NT4. The core achievement is a VxD (Virtual Device Driver)
wrapper that presents the correct LE (Linear Executable) binary structure so the
Windows 98 SE loader accepts it.

This makes it possible to use modern storage controller drivers on legacy Windows
installations, bridging the gap between the NT5 miniport model and the older IOS/VxD
driver stack that Windows 9x expects.

Developed for the Vogons retro-computing community (vogons.org).

## Project Structure

```
src/              Build, deployment, and QEMU launch scripts
diagnostics/      LE header analysis, VxD disassembly, DDB and fixup inspection
binaries/         Reference VxD binaries (V5SMALL.VXD input, V5REAL.VXD output)
deploy-package/   Ready to ship: prebuilt NTMINI.VXD, INSTALL.BAT, user README
reference/        Design docs (IOS_PROGRESS.md, BUILD.TXT), DDK headers (inc/)
history/source/   C/ASM source evolution from V1 through V5, linker scripts
```

## Prerequisites

- **Python 3** with the `capstone` disassembly library (`pip install capstone`)
- **QEMU** for emulated testing
- **Windows 98 SE disk image** (FAT32 formatted)
- **Open Watcom** toolchain targeting the Win98 DDK

## Quick Start

Set the path to your Win98 SE disk image:

```
export WIN98_IMG=/path/to/win98.img
```

1. Build the corrected VxD from the source binary:

```
python src/build_sysini_fixed.py
```

This reads `binaries/V5SMALL.VXD` and produces a corrected LE binary.

2. Deploy to the disk image (choose one):

```
python src/deploy_to_iosubsys.py    # Deploys to WINDOWS\SYSTEM\IOSUBSYS
python src/deploy_sysini.py         # Deploys to WINDOWS\SYSTEM, patches SYSTEM.INI
```

Both scripts auto-detect FAT32 geometry from the MBR and BPB, traverse the
directory tree, and allocate clusters for the new file.

3. Verify and boot in QEMU:

```
python src/verify_and_launch.py
```

Debug output is captured via `-debugcon` and `-serial` channels.

## How It Works

Windows 98 SE loads port drivers as VxDs, which use the LE (Linear Executable)
binary format. NT5 miniport drivers are PE (Portable Executable) binaries with a
completely different structure. The wrapper bridges these two worlds.

The LE correction pipeline fixes several header fields that the Win98 loader
validates strictly:

- **data_pages_off**: Must be file-absolute, not section-relative. The loader uses
  this offset directly to seek into the file.
- **Loader and fixup section layout**: The fixup section must immediately follow the
  loader section with no gaps. The section sizes and offsets must be self-consistent.
- **Import tables**: Module name references and entry point ordinals must match what
  the IOS subsystem exports.
- **Extended LE fields** at offsets LE+0xB8, LE+0xBC, and LE+0xC0: These control
  debug information pointers and must be zeroed or pointed at valid structures.

The reference driver used for comparison during development was `ESDI_506.PDR`, the
standard IDE/ATAPI port driver shipped with Windows 98 SE.

## Status

This is a proof of concept. The current build wraps the NEC ATAPI miniport as
`ntmini.vxd` and has been tested successfully in QEMU. The Win98 SE loader accepts
the corrected LE binary, loads the VxD, and calls its entry point.

Real hardware validation has not yet been performed.

## Unimplemented Areas

The following subsystems remain unfinished, as documented in BUILD.TXT:

- **Memory allocation**: The wrapper does not yet implement the VMM page allocation
  calls that a full VxD port driver needs for DMA buffers and adapter structures.
- **PE loading**: The embedded NT5 miniport PE image is not yet parsed or relocated
  at runtime. Headers are read but sections are not mapped.
- **IOS registration**: The wrapper does not register with the IOS (I/O Supervisor)
  subsystem as a port driver. It loads but does not claim any devices.
- **IOR to SRB translation**: I/O Request Packets (IORs) from the IOS layer must be
  translated into SCSI Request Blocks (SRBs) that the NT5 miniport expects. This
  translation layer is stubbed but not functional.
- **Interrupt hooking**: Hardware interrupt virtualization through VPICD is not
  implemented. The miniport's ISR cannot be called.

## License

MIT License. See [LICENSE](LICENSE).

## Acknowledgments

This project was developed with and for the Vogons retro-computing community
(vogons.org), whose collective knowledge of legacy PC hardware and operating systems
made it possible.
