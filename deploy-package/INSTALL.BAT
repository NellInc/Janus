@ECHO OFF
ECHO.
ECHO NT5-9x Driver Backport: NTMINI VxD Installer
ECHO ======================================
ECHO.

REM Copy the driver
COPY NTMINI.VXD C:\WINDOWS\SYSTEM\NTMINI.VXD
IF ERRORLEVEL 1 GOTO COPYERR

ECHO Driver copied to C:\WINDOWS\SYSTEM\NTMINI.VXD

REM Check if already installed in SYSTEM.INI
FIND /I "NTMINI.VXD" C:\WINDOWS\SYSTEM.INI > NUL
IF NOT ERRORLEVEL 1 GOTO ALREADY

REM Verify [386Enh] section exists
FIND /I "[386Enh]" C:\WINDOWS\SYSTEM.INI > NUL
IF ERRORLEVEL 1 GOTO NO386

REM Clear read-only attribute if set
ATTRIB -R C:\WINDOWS\SYSTEM.INI

REM Add device= line to SYSTEM.INI
REM COMMAND.COM cannot insert mid-file, so we append.
REM On stock Win98, [386Enh] is typically the last section,
REM so appending places the line in the correct section.
REM If your SYSTEM.INI has sections after [386Enh], add the
REM line manually per README.txt instructions instead.
ECHO.>> C:\WINDOWS\SYSTEM.INI
ECHO device=C:\WINDOWS\SYSTEM\NTMINI.VXD >> C:\WINDOWS\SYSTEM.INI
IF ERRORLEVEL 1 GOTO WRITERR
ECHO Added device line to SYSTEM.INI.
ECHO NOTE: Verify the line is inside the [386Enh] section.
GOTO DONE

:ALREADY
ECHO SYSTEM.INI already contains NTMINI.VXD entry. Skipping.
GOTO DONE

:NO386
ECHO ERROR: [386Enh] section not found in SYSTEM.INI.
ECHO Please add the following line manually under [386Enh]:
ECHO   device=C:\WINDOWS\SYSTEM\NTMINI.VXD
GOTO END

:COPYERR
ECHO ERROR: Could not copy NTMINI.VXD to C:\WINDOWS\SYSTEM\
ECHO Make sure you are running this from the directory containing NTMINI.VXD.
GOTO END

:WRITERR
ECHO ERROR: Could not write to SYSTEM.INI. Check file permissions.
GOTO END

:DONE
ECHO.
ECHO Installation complete. Please restart Windows for changes to take effect.

:END
ECHO.
