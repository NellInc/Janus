NT5-9x Driver Backport: NTMINI VxD
===================================

This driver backports NT5 miniport drivers to Windows 9x. Current proof
of concept: NEC ATAPI controller support on Windows 98 SE.

INSTALLATION
------------
1. Copy NTMINI.VXD to C:\WINDOWS\SYSTEM\

2. Edit C:\WINDOWS\SYSTEM.INI
   Find the [386Enh] section and add this line:
   device=C:\WINDOWS\SYSTEM\NTMINI.VXD

3. Restart Windows

VERIFICATION
------------
After reboot, check Device Manager for the DVD/CD-ROM drive.
The driver registers during the Init_Complete phase of boot.
DEVICEINIT showing as "failed" in BOOTLOG.TXT is normal and
expected behavior (the driver defers IOS registration to
Init_Complete by design).

REMOVAL
-------
1. Delete C:\WINDOWS\SYSTEM\NTMINI.VXD
2. Remove the device=C:\WINDOWS\SYSTEM\NTMINI.VXD line from SYSTEM.INI
3. Restart Windows

TECHNICAL NOTES
---------------
- This is a repacked NEC ATAPI miniport wrapper VxD
- Contains an embedded NT SCSI miniport PE driver
- Loads via SYSTEM.INI device= directive (not IOSUBSYS)
- 682 internal fixup records, 8 pages, single merged object
- Built with corrected LE/VLE header for Win98 VMM compatibility
