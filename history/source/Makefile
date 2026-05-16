# Makefile for NT5 WDM Driver Backport VxD
# Build environment: Docker container with Open Watcom v2 + NASM
# Usage: make nt5 (or just make)

# Tools
WCC = wcc386
WLINK = wlink
NASM_CMD = nasm
WASM = wasm

# Watcom C flags
# -bt=windows  : target Windows (VxD ring-0)
# -3s          : 386, stack-based calling convention
# -s           : no stack overflow checks
# -zl          : suppress default library info in object
# -d0          : no debug info (use -d1 for line numbers)
# -i=.         : include path
# -w4          : max warnings
# -ze          : enable extensions
CFLAGS = -bt=windows -3s -s -zl -d0 -i=. -w4 -ze -dDEBUG

# NASM flags: OMF output for wlink
NASMFLAGS = -f obj

# Output
NT5_VXD = NT5RAW.VXD
NT4_VXD = V5RAW.VXD
V5SMALL_VXD = V5SMALL.VXD

# ============================================================
# NT5 PATH (WDM compatibility layer for W2K atapi.sys)
# ============================================================

# Assembly source
NT5_ASM_SRC = VXDWRAP_NASM.ASM
NT5_ASM_OBJ = vxdwrap_nasm.obj

# C sources for NT5 path
NT5_C_SRCS = \
	NTKSHIM.C \
	NTKEXPORTS.C \
	IRPMGR.C \
	PNPMGR.C \
	PCIBUS.C \
	WDMBRIDGE.C \
	NT5LOADER.C \
	PELOAD.C \
	IOSBRIDGE.C \
	IRQHOOK.C

# Derive object file names (lowercase .obj)
NT5_C_OBJS = $(NT5_C_SRCS:.C=.obj)

# Linker directive file
NT5_LNK = link_nt5.lnk

# ============================================================
# V5 PATH (static VxD path used by LE fixer + SYSTEM.INI boot)
# ============================================================

V5_ASM_SRC = VXDWRAP.ASM
V5_ASM_OBJ = V5R_wrap.obj
V5_C_MAIN_SRC = NTMINI_V5.C
V5_C_MAIN_OBJ = V5R_c.obj
V5_C_PE_SRC = PELOAD.C
V5_C_PE_OBJ = V5R_pe.obj
V5_LNK = link_v5.lnk

V5S_ASM_OBJ = V5S_wrap.obj
V5S_C_MAIN_OBJ = V5S_c.obj
V5S_C_PE_OBJ = V5S_pe.obj
V5SMALL_LNK = v5small_1obj.lnk

# ============================================================
# V5SCSI PATH (SCSI miniport: sym_hi.sys on LSI53C895A)
# ============================================================

V5SCSI_VXD = V5SCSI.VXD
V5SCSI_C_OBJ = V5SCSI_c.obj
V5SCSI_PE_OBJ = V5SCSI_pe.obj
V5SCSI_LNK = link_v5scsi.lnk
V5SCSI_CFLAGS = $(CFLAGS) -dNTMINI_USE_SCSI=1

# ============================================================
# Targets
# ============================================================

.PHONY: all nt5 v5 v5small v5scsi clean

all: nt5

nt5: $(NT5_VXD)
v5: $(NT4_VXD)
v5small: $(V5SMALL_VXD)
v5scsi: $(V5SCSI_VXD)

# Assemble VXDWRAP
$(NT5_ASM_OBJ): $(NT5_ASM_SRC)
	$(NASM_CMD) $(NASMFLAGS) -o $@ $<

$(V5_ASM_OBJ): $(V5_ASM_SRC)
	$(WASM) -fo=$@ $<

$(V5S_ASM_OBJ): $(V5_ASM_SRC)
	$(WASM) -fo=$@ $<

# Generic rule: compile .C to .obj
%.obj: %.C
	$(WCC) $(CFLAGS) -fo=$@ $<

$(V5_C_MAIN_OBJ): $(V5_C_MAIN_SRC)
	$(WCC) $(CFLAGS) -fo=$@ $<

$(V5_C_PE_OBJ): $(V5_C_PE_SRC)
	$(WCC) $(CFLAGS) -fo=$@ $<

$(V5S_C_MAIN_OBJ): $(V5_C_MAIN_SRC)
	$(WCC) $(CFLAGS) -fo=$@ $<

$(V5S_C_PE_OBJ): $(V5_C_PE_SRC)
	$(WCC) $(CFLAGS) -fo=$@ $<

# Link the VxD
$(NT5_VXD): $(NT5_ASM_OBJ) $(NT5_C_OBJS) $(NT5_LNK)
	$(WLINK) @$(NT5_LNK)

$(NT4_VXD): $(V5_ASM_OBJ) $(V5_C_MAIN_OBJ) $(V5_C_PE_OBJ) $(V5_LNK)
	$(WLINK) @$(V5_LNK)

$(V5SMALL_VXD): $(V5S_ASM_OBJ) $(V5S_C_MAIN_OBJ) $(V5S_C_PE_OBJ) $(V5SMALL_LNK)
	$(WLINK) @$(V5SMALL_LNK)
	cp V5SMALL_1OBJ.VXD $(V5SMALL_VXD)

# V5SCSI: SCSI miniport variant (sym_hi.sys on LSI53C895A)
$(V5SCSI_C_OBJ): $(V5_C_MAIN_SRC)
	$(WCC) $(V5SCSI_CFLAGS) -fo=$@ $<

$(V5SCSI_PE_OBJ): $(V5_C_PE_SRC)
	$(WCC) $(V5SCSI_CFLAGS) -fo=$@ $<

$(V5SCSI_VXD): $(NT5_ASM_OBJ) $(V5SCSI_C_OBJ) $(V5SCSI_PE_OBJ) $(V5SCSI_LNK)
	$(WLINK) @$(V5SCSI_LNK)

# ============================================================
# Convenience: compile individual files for debugging
# ============================================================

.PHONY: ntkshim irpmgr pnpmgr pcibus wdmbridge nt5loader ntkexports peload iosbridge ntmini_v5 irqhook asm

ntkshim:    NTKSHIM.obj
irpmgr:     IRPMGR.obj
pnpmgr:     PNPMGR.obj
pcibus:     PCIBUS.obj
wdmbridge:  WDMBRIDGE.obj
nt5loader:  NT5LOADER.obj
ntkexports: NTKEXPORTS.obj
peload:     PELOAD.obj
iosbridge:  IOSBRIDGE.obj
ntmini_v5:  NTMINI_V5.obj
irqhook:    IRQHOOK.obj
asm:        $(NT5_ASM_OBJ)

# ============================================================
# Clean
# ============================================================

clean:
	rm -f *.obj *.OBJ $(NT5_VXD) $(NT4_VXD) $(V5SMALL_VXD) V5SMALL_1OBJ.VXD *.err *.map
