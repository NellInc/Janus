/* NTMINI_STUB.C - Minimal stub for debugging VxD loading
 *
 * Replace NTMINI.C + IOSBRIDGE.C + NTKRNL.C + PELOAD.C + IRQHOOK.C
 * with just this file to test if the VxD shell loads correctly.
 *
 * Build with: wcc386 -bt=windows -3s -s -zl -zu -d0 NTMINI_STUB.C
 * Link with:  VXDSTUB_NASM.ASM (minimal wrapper)
 */

/* Called by VxD control procedure on Device_Init
 * Returns 0 on success (matches original convention: jnz = fail)
 */
int _ntmini_init(void)
{
    /* Do nothing - just prove the VxD loads without crashing */
    return 0; /* success */
}

/* Called by VxD control procedure on Sys_Dynamic_Device_Exit */
void _ntmini_cleanup(void)
{
    /* Nothing to clean up */
}
