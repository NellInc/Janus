# Makefile for NT5 WDM Driver Backport VxD
# Build environment: Docker container with Open Watcom v2 + NASM
# Usage: make nt5 (or just make)

# Tools
WCC = wcc386
WLINK = wlink
NASM_CMD = nasm

# Watcom C flags
# -bt=windows  : target Windows (VxD ring-0)
# -3s          : 386, stack-based calling convention
# -s           : no stack overflow checks
# -zl          : suppress default library info in object
# -d0          : no debug info (use -d1 for line numbers)
# -i=.         : include path
# -w4          : max warnings
# -ze          : enable extensions
CFLAGS = -bt=windows -3s -s -zl -d0 -i=. -w4 -ze

# NASM flags: OMF output for wlink
NASMFLAGS = -f obj

# Output
NT5_VXD = NT5RAW.VXD
NT4_VXD = V5RAW.VXD

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
# Targets
# ============================================================

.PHONY: all nt5 clean

all: nt5

nt5: $(NT5_VXD)

# Assemble VXDWRAP
$(NT5_ASM_OBJ): $(NT5_ASM_SRC)
	$(NASM_CMD) $(NASMFLAGS) -o $@ $<

# Generic rule: compile .C to .obj
%.obj: %.C
	$(WCC) $(CFLAGS) -fo=$@ $<

# Link the VxD
$(NT5_VXD): $(NT5_ASM_OBJ) $(NT5_C_OBJS) $(NT5_LNK)
	$(WLINK) @$(NT5_LNK)

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
	rm -f *.obj *.OBJ $(NT5_VXD) *.err *.map
