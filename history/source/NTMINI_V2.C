/* NTMINI_V2.C - Version 2: Heap allocation + file I/O test
 *
 * Tests:
 *   1. HeapAllocate / HeapFree (VMM heap services)
 *   2. R0_OpenCreateFile / R0_ReadFile / R0_CloseFile (file I/O)
 *   3. Read first 512 bytes of ATAPI.SYS and check for MZ header
 *
 * ALL paths return 0 (success). Boot is never blocked.
 *
 * Build with: wcc386 -bt=windows -3s -s -zl -zu -d0 NTMINI_V2.C
 * Link with:  VXDWRAP_V2.ASM
 */

extern void VxD_Debug_Printf(const char *msg);
extern void *VxD_HeapAllocate(unsigned long size, unsigned long flags);
extern void VxD_HeapFree(void *ptr, unsigned long flags);
extern int VxD_File_Open(const char *filename);
extern int VxD_File_Read(int handle, void *buffer, int count);
extern void VxD_File_Close(int handle);

/* Called by VxD control procedure on Device_Init / Sys_Critical_Init
 * Returns 0 on success (matches convention: jnz = fail)
 */
int _ntmini_init(void)
{
    int handle;
    void *buffer;

    VxD_Debug_Printf("NTMINI V2: Init\r\n");

    /* --- Test 1: Heap allocation --- */
    buffer = VxD_HeapAllocate(32768, 0);
    if (!buffer) {
        VxD_Debug_Printf("NTMINI V2: Heap FAIL\r\n");
        return 0;   /* don't block boot */
    }
    VxD_Debug_Printf("NTMINI V2: Heap OK\r\n");

    /* --- Test 2: File open --- */
    handle = VxD_File_Open("C:\\WINDOWS\\SYSTEM\\ATAPI.SYS");
    if (handle < 0) {
        VxD_Debug_Printf("NTMINI V2: Open FAIL\r\n");
        VxD_HeapFree(buffer, 0);
        return 0;   /* don't block boot */
    }
    VxD_Debug_Printf("NTMINI V2: Open OK\r\n");

    /* --- Test 3: Read and check MZ header --- */
    {
        int n;
        unsigned char *p;
        n = VxD_File_Read(handle, buffer, 512);
        p = (unsigned char *)buffer;
        if (n > 1 && p[0] == 'M' && p[1] == 'Z')
            VxD_Debug_Printf("NTMINI V2: MZ found!\r\n");
        else
            VxD_Debug_Printf("NTMINI V2: Read done, no MZ\r\n");
    }

    VxD_File_Close(handle);
    VxD_HeapFree(buffer, 0);
    VxD_Debug_Printf("NTMINI V2: Done\r\n");
    return 0;   /* success - never block boot */
}

/* Called by VxD control procedure on Sys_Dynamic_Device_Exit */
void _ntmini_cleanup(void)
{
    VxD_Debug_Printf("NTMINI V2: VxD unloaded\r\n");
}
