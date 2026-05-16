NT5-9x Driver Backport: NTMINI VxD
===================================

Backports Windows 2000/XP storage drivers to Windows 98 SE. Wraps the
NT5 WDM atapi.sys miniport inside a Win9x VxD, bridging NT's ScsiPort
model into Win98's IOS (I/O Supervisor) layer.

Validated capabilities:
  - ATA hard disk read/write via direct IDE port I/O
  - ATAPI CD-ROM/DVD read via NT miniport SCSI path
  - IOS calldown chain integration (splices above ESDI_506.PDR)

Developed for the Vogons retro-computing community (vogons.org).

INSTALLATION
------------
1. Copy NTMINI.VXD to C:\WINDOWS\SYSTEM\

2. Edit C:\WINDOWS\SYSTEM.INI
   Find the [386Enh] section and add this line:
   device=C:\WINDOWS\SYSTEM\NTMINI.VXD

3. Restart Windows

IMPORTANT: On a fresh Windows 98 SE install, complete the "Add New
Hardware Wizard" BEFORE installing this driver. The driver must not
be present during the first-boot hardware detection phase.

VERIFICATION
------------
After reboot, the driver loads during Init_Complete and registers
with IOS. Check BOOTLOG.TXT for "NTMINI" entries. DEVICEINIT
showing as "failed" is normal (IOS registration is deferred to
Init_Complete by design).

For IDE hard disks on the secondary channel, files on D: should
be accessible through Windows Explorer and command-line tools.

REMOVAL
-------
1. Remove the device= line from SYSTEM.INI
2. Delete C:\WINDOWS\SYSTEM\NTMINI.VXD
3. Restart Windows

TECHNICAL NOTES
---------------
- Contains embedded W2K atapi.sys PE image (NT5 ATAPI miniport)
- Loads via SYSTEM.INI device= directive (not IOSUBSYS PDR)
- Skips IOS_Register to avoid boot hang; uses direct ILB splice
- Non-BOTTOM calldown position (above ESDI_506) with passthrough
- ATA direct I/O for HDD: ports 0x1F0 (primary), 0x170 (secondary)
- SCSI CDB translation for CD-ROM: IOR to READ(10)/WRITE(10)
- 2406 internal fixup records, 40 pages, single merged LE object
- Built with Open Watcom + NASM, corrected LE header for Win98 VMM
