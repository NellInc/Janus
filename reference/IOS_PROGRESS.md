# IOS Integration Progress - 2026-03-23

## BREAKTHROUGHS THIS SESSION

### 1. DRP Struct Packing (ROOT CAUSE of months of failure)
The DRP struct must be **packed** (`#pragma pack(push,1)`). Our C struct had padding
after `DRP_revision` (offset +0x24) that shifted every subsequent field by 3-8 bytes.

**Verified against ESDI_506.PDR hex dump:**
- `DRP_feature_code` at +0x25 (packed), not +0x28 (padded)
- `DRP_reg_result` at +0x2C (packed), not +0x30 (padded)
- Total size: 0x38 (56 bytes), not 0x40 (64 bytes)

Fix: `#pragma pack(push,1)` around the DRP typedef. Result: **DRP_reg_result = REMAIN_RESIDENT!**

### 2. Correct LGN Value
ESDI_506 uses `DRP_LGN = 0x00400000` (= `1 << 0x16` = `DRP_ESDI_PD`).
Previous attempts used 0, 0x16 (bit number not mask!), 0x80000 — all wrong.

### 3. ILB Acquisition via VMM DDB Chain Walk
IOS does NOT provide ILB for late registration (after IOS init phase).
Solution: walk the VMM VxD DDB chain via `VMM_Get_DDB(0x0010)`, follow
`DDB_Reference_Data` of each loaded VxD, check for DRP eyecatcher "XXXXXXXX",
and steal the ILB from a driver that registered during IOS init.

Found ILB from APIX (SCSI API layer): `ILB = 0xC1443670`, `ILB_service_rtn = 0xC003CA8C`.

### 4. ISP_CREATE_DCB via ILB_service_rtn
Called `ILB_service_rtn` with ISP packet in EDX + pushed on stack.
`ISP_CREATE_DCB` (func=1) succeeded: got DCB at `0xCAED3500`.

### 5. ISP_INSERT_CALLDOWN + ISP_ASSOCIATE_DCB
Both succeeded with result=0:
- Our IOR handler is in the DCB calldown chain
- DCB associated with drive D:

## CURRENT STATE
- IOS registration: REMAIN_RESIDENT ✓
- ILB: Acquired from APIX driver ✓
- DCB: Created by IOS, CD-ROM type ✓
- Calldown: Installed ✓
- Drive letter: D: associated ✓
- IOR handling: NOT YET IMPLEMENTED (stub only)
- Win98 boots to desktop ✓

## NEXT STEPS
1. **IOR handler**: Translate IOS I/O requests (IOPs/IORs) to miniport SRBs
2. **INQUIRY response**: Return CD-ROM device type/vendor info
3. **READ handling**: Translate IOR sector reads to SCSI READ(10) via HwStartIo
4. **CDFS detection**: Win98 should detect ISO 9660 filesystem on the CD-ROM

## KEY CALLING CONVENTIONS
- ILB_service_rtn: EDX = ISP packet ptr, also push on stack, CALL [addr]
- ISP packets: PACKED (no padding). USHORT func at +0, USHORT result at +2.
- AEP header: 12 bytes effective (9 packed + 3 IOS padding). Extended fields at +0x0C.
