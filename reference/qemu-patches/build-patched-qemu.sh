#!/bin/bash
# Build a patched QEMU 10.2.2 for the NTMINI SCSI project.
#
# Applies lsi-ntmini-full.patch to hw/scsi/lsi53c895a.c. This patch contains:
#   1. lsi_bar1_self_check — route SCRIPTS MOVE MEMORY to own MMIO BAR through
#      register handlers instead of pci_dma_write (which misses device regs).
#   2. NTMINI_PA_FIXUP — translate VxD-reported wrong PAs (VA - 0xC0000000)
#      through guest page tables via cpu_get_phys_page_debug(), 64-entry cache.
#      Translated reads/writes go through address_space_memory.
#   3. NTMINI_SCNTL1_CON — preserve the hardware-managed CON bit on software
#      writes to SCNTL1 (real hardware: read-only status). Fixes SYMC8XX
#      clearing the nexus after a successful SELECT.
#   4. NTMINI_STIME1_NOGEN — do NOT raise LSI_SIST1_GEN immediately when
#      software programs STIME1. Fixes premature SRB_STATUS_SELECTION_TIMEOUT.
#
# Usage: ./build-patched-qemu.sh
# Result: /tmp/qemu-patched/qemu-10.2.2/build/qemu-system-i386

set -euo pipefail

QEMU_VERSION="10.2.2"
WORK_DIR="/tmp/qemu-patched"
PATCH_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH_FILE="${PATCH_DIR}/lsi-ntmini-full.patch"
QTEST_SRC="${PATCH_DIR}/ntmini-lsi-test.c"

echo "=== Building patched QEMU ${QEMU_VERSION} ==="
echo "Work dir:   ${WORK_DIR}"
echo "Patch file: ${PATCH_FILE}"

if [ ! -f "${PATCH_FILE}" ]; then
    echo "ERROR: patch file not found: ${PATCH_FILE}"
    exit 1
fi

# Download source if not already present
if [ ! -d "${WORK_DIR}/qemu-${QEMU_VERSION}" ]; then
    mkdir -p "${WORK_DIR}"
    cd "${WORK_DIR}"
    echo "Downloading QEMU ${QEMU_VERSION} source..."
    curl -L -o "qemu-${QEMU_VERSION}.tar.xz" \
        "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz"
    echo "Extracting..."
    tar xf "qemu-${QEMU_VERSION}.tar.xz"
fi

cd "${WORK_DIR}"

# Apply the full patch unless already applied
LSI_SRC="qemu-${QEMU_VERSION}/hw/scsi/lsi53c895a.c"
if grep -q "NTMINI_PA_FIXUP" "${LSI_SRC}" 2>/dev/null; then
    echo "Patch already applied."
else
    echo "Applying lsi-ntmini-full.patch..."
    patch -p1 < "${PATCH_FILE}"
fi

# Install the NTMINI qtest (copy source, register in meson.build)
QTEST_DST="qemu-${QEMU_VERSION}/tests/qtest/ntmini-lsi-test.c"
MESON_FILE="qemu-${QEMU_VERSION}/tests/qtest/meson.build"
if [ -f "${QTEST_SRC}" ]; then
    cp "${QTEST_SRC}" "${QTEST_DST}"
    if ! grep -q "ntmini-lsi-test" "${MESON_FILE}"; then
        echo "Registering ntmini-lsi-test in tests/qtest/meson.build..."
        python3 - "${MESON_FILE}" <<'PYEOF'
import sys
path = sys.argv[1]
with open(path) as f:
    src = f.read()
needle = ("(config_all_devices.has_key('CONFIG_LSI_SCSI_PCI') ? "
          "['fuzz-lsi53c895a-test'] : []) +     \\\n")
add = ("  (config_all_devices.has_key('CONFIG_LSI_SCSI_PCI') ? "
       "['ntmini-lsi-test'] : []) +           \\\n")
if needle not in src:
    print("ERROR: could not locate fuzz-lsi53c895a-test line in meson.build")
    sys.exit(1)
src = src.replace(needle, needle + add, 1)
with open(path, "w") as f:
    f.write(src)
print("ntmini-lsi-test registered.")
PYEOF
    fi
fi

# Configure for i386 only (minimal build)
cd "qemu-${QEMU_VERSION}"
mkdir -p build
cd build

if [ ! -f config-host.mak ]; then
    echo "Configuring QEMU (i386 target only)..."
    ../configure \
        --target-list=i386-softmmu \
        --disable-docs \
        --disable-guest-agent \
        --disable-tools \
        --disable-user \
        --enable-slirp
fi

echo "Building qemu-system-i386..."
if command -v ninja >/dev/null 2>&1 && [ -f build.ninja ]; then
    ninja qemu-system-i386
    echo "Building NTMINI qtest..."
    ninja tests/qtest/ntmini-lsi-test || echo "  (qtest build failed, continuing)"
else
    make -j$(sysctl -n hw.ncpu 2>/dev/null || nproc) qemu-system-i386
fi

echo ""
echo "=== Build complete ==="
echo "Patched binary: ${WORK_DIR}/qemu-${QEMU_VERSION}/build/qemu-system-i386"
echo ""
echo "Run NTMINI regression tests:"
echo "  cd ${WORK_DIR}/qemu-${QEMU_VERSION}/build"
echo "  QTEST_QEMU_BINARY=./qemu-system-i386 ./tests/qtest/ntmini-lsi-test"
