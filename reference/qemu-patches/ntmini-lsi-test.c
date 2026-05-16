/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * NTMINI-specific regression tests for lsi53c895a patches.
 *
 * Covers:
 *   - NTMINI_STIME1_NOGEN: writing STIME1 must not immediately raise
 *     SIST1.GEN (upstream QEMU did this as a FreeBSD hack; SYMC8XX
 *     treats the timer as a real countdown and experiences premature
 *     SRB_STATUS_SELECTION_TIMEOUT).
 *   - NTMINI_SCNTL1_CON: CON bit (0x10) in SCNTL1 is hardware-managed
 *     on real 53C8xx silicon. Software writes to SCNTL1 must not be
 *     able to change the CON bit.
 *
 * Both tests drive the device via MMIO on BAR 1. They share the same
 * QEMU init line as the upstream lsi_dma_reentrancy test, which puts
 * BAR 1 at 0xff100000.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define LSI_MMIO_BASE  0xff100000u

#define REG_SCNTL1     0x01
#define REG_SIST1      0x43
#define REG_STIME1     0x49

#define LSI_SCNTL1_CON 0x10
#define LSI_SIST1_GEN  0x02

static QTestState *ntmini_lsi_init(void)
{
    QTestState *s;

    s = qtest_init("-M q35 -m 512M -nodefaults "
                   "-blockdev driver=null-co,node-name=null0 "
                   "-device lsi53c810 -device scsi-cd,drive=null0");

    /* Enable memory + bus master, then program BAR 1 (MMIO registers). */
    qtest_outl(s, 0xcf8, 0x80000804); /* PCI Command Register */
    qtest_outw(s, 0xcfc, 0x7);        /* Memory + I/O + Bus Master */
    qtest_outl(s, 0xcf8, 0x80000814); /* BAR 1 */
    qtest_outl(s, 0xcfc, LSI_MMIO_BASE);

    return s;
}

/* NTMINI_STIME1_NOGEN regression test */
static void test_ntmini_stime1_nogen(void)
{
    QTestState *s = ntmini_lsi_init();
    uint8_t sist1_before;
    uint8_t sist1_after;

    sist1_before = qtest_readb(s, LSI_MMIO_BASE + REG_SIST1);
    g_assert_cmphex(sist1_before & LSI_SIST1_GEN, ==, 0);

    qtest_writeb(s, LSI_MMIO_BASE + REG_STIME1, 0x0f);

    sist1_after = qtest_readb(s, LSI_MMIO_BASE + REG_SIST1);
    g_assert_cmphex(sist1_after & LSI_SIST1_GEN, ==, 0);

    qtest_quit(s);
}

/* NTMINI_SCNTL1_CON regression test */
static void test_ntmini_scntl1_con_preserved(void)
{
    QTestState *s = ntmini_lsi_init();
    uint8_t scntl1;

    /* At reset CON is 0. Software writes must not be able to set it. */
    qtest_writeb(s, LSI_MMIO_BASE + REG_SCNTL1, 0x10); /* just CON */
    scntl1 = qtest_readb(s, LSI_MMIO_BASE + REG_SCNTL1);
    g_assert_cmphex(scntl1 & LSI_SCNTL1_CON, ==, 0);

    /* A write with many bits set, including CON: other bits land,
     * CON stays 0 (still reflects hardware state). The legal bits
     * AESP=0x04 | IARB=0x02 | EXC=0x80 = 0x86 should survive. */
    qtest_writeb(s, LSI_MMIO_BASE + REG_SCNTL1, 0x96); /* CON|EXC|AESP|IARB */
    scntl1 = qtest_readb(s, LSI_MMIO_BASE + REG_SCNTL1);
    g_assert_cmphex(scntl1 & LSI_SCNTL1_CON, ==, 0);
    g_assert_cmphex(scntl1 & 0x86, ==, 0x86);

    /* Zeroing the register keeps CON at whatever hardware set (still 0). */
    qtest_writeb(s, LSI_MMIO_BASE + REG_SCNTL1, 0x00);
    scntl1 = qtest_readb(s, LSI_MMIO_BASE + REG_SCNTL1);
    g_assert_cmphex(scntl1 & LSI_SCNTL1_CON, ==, 0);

    qtest_quit(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_device("lsi53c810")) {
        return 0;
    }

    qtest_add_func("ntmini/lsi/stime1_nogen", test_ntmini_stime1_nogen);
    qtest_add_func("ntmini/lsi/scntl1_con_preserved",
                   test_ntmini_scntl1_con_preserved);

    return g_test_run();
}
