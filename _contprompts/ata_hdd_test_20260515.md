---
stepsCompleted: 0
status: PENDING
---

# ATA Hard Disk Test — 2026-05-15

## Goal
Test the ATA hard disk support (commit deea226) with the three-layer IOS fix (commit 64044bd).

## Prerequisites
- Test HDD image created: `/tmp/win98vm/test_hd.img` (10MB FAT16, contains TESTFILE.TXT)
- run_unix_once.sh already has HDD support: attaches as `if=ide,index=3` (secondary slave)
- CD-ROM file I/O fully working (SREAD + WREAD pass)

## Steps
1. Verify the bridge detects MBR at LBA 0 on the HDD
2. Configure NT5LOADER to scan secondary slave for ATA disk
3. Build and test with WREAD targeting HDD file (modify mkpe_wread.py to read from HDD drive letter)
4. Verify 512-byte sector translation works (vs 2048 for CD-ROM)

## Architecture Notes
- `g_nt5_bridge.DeviceType` switches between 0x05 (CD-ROM) and 0x00 (disk)
- IOSBRIDGE.C `aep_config_dcb` accepts DCB_TYPE_DISK since deea226
- `build_srb_from_ior` uses `dev->sector_size` for block count calculation
- HDD needs its own DCB + calldown splice (separate from CD-ROM D:)

## WARNING: HDD Corrupts Win98 Boot
Attaching a raw 10MB FAT16 image at IDE index=3 (secondary slave) causes Win98 to hang at the splash screen. The HDD corrupts Win98's hardware detection state, requiring a full disk restore from `en_win98_se.qcow2`. The test HDD needs proper CHS geometry that Win98's real-mode IDE init expects. Do NOT attach the test HDD until this is resolved.

## Key Question
Can both CD-ROM (D:) and ATA HDD (E:?) work simultaneously, or is the bridge single-device?
