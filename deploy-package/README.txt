NTMINI - Win98 CD-ROM File System Driver
=========================================

This VxD driver restores CD-ROM file access on Windows 98 systems
where the standard IOS/CDFS driver chain is broken. This is common
on systems with NEC ATAPI controllers and certain hardware configs
where Windows 98's built-in CD-ROM drivers fail to initialize.

The driver registers as an IFSMgr File System Driver, mounts D:,
and serves file I/O using direct ATAPI commands and ISO 9660
filesystem parsing, bypassing the broken IOS chain entirely.


INSTALLATION
------------
1. Copy NTMINI.VXD to C:\WINDOWS\SYSTEM\

2. Edit C:\WINDOWS\SYSTEM.INI
   Find the [386Enh] section and add this line:
   device=C:\WINDOWS\SYSTEM\NTMINI.VXD

3. Restart Windows

Or simply run INSTALL.BAT from this directory.


VERIFICATION
------------
After reboot, D: should be accessible. Try opening a file on D:
from a DOS prompt or Explorer.

Note: DEVICEINIT showing as "failed" in BOOTLOG.TXT is normal.
The driver defers initialization to Init_Complete by design.


REMOVAL
-------
1. Delete C:\WINDOWS\SYSTEM\NTMINI.VXD
2. Remove the device= line from SYSTEM.INI
3. Restart Windows


LIMITATIONS
-----------
- Read-only access (appropriate for CD-ROM)
- Root directory files only (no subdirectories)
- Hardcoded to D: drive
- Best with sequential open-read-close file access patterns


TECHNICAL NOTES
---------------
- Registers with IFSMgr via RegisterMount as a local FSD
- Entry table provides Open, Read, Close, and other FSD functions
- Direct ATAPI PACKET commands bypass the IOS layer
- ISO 9660 primary volume descriptor and root directory parsing
- Built with Open Watcom C 2.0 and NASM assembler


SOURCE CODE
-----------
Full source code and build instructions are available in the
project repository. Released under the MIT License.


CREDITS
-------
Built with assistance from Claude (Anthropic).
