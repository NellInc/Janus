/* NTMINI_V1.C - Version 1: VxD shell + debug print test
 *
 * Tests that VxD service calls (INT 20h) work by calling
 * Out_Debug_String via VxD_Debug_Printf.
 *
 * Build with: wcc386 -bt=windows -3s -s -zl -zu -d0 NTMINI_V1.C
 * Link with:  VXDWRAP_V1.ASM
 */

extern void VxD_Debug_Printf(const char *msg);

/* Called by VxD control procedure on Device_Init / Sys_Critical_Init
 * Returns 0 on success (matches convention: jnz = fail)
 */
int _ntmini_init(void)
{
    VxD_Debug_Printf("NTMINI: VxD initialized successfully!\r\n");
    return 0; /* success */
}

/* Called by VxD control procedure on Sys_Dynamic_Device_Exit */
void _ntmini_cleanup(void)
{
    VxD_Debug_Printf("NTMINI: VxD unloaded\r\n");
}
