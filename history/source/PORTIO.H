/*
 * PORTIO.H - Portable x86 Port I/O for Open Watcom and GCC
 *
 * Provides compiler-independent macros and functions for:
 *   - Byte/word/dword port I/O (IN/OUT)
 *   - Buffer port I/O (REP INSW/OUTSW, REP INSD/OUTSD)
 *   - PCI config space access (ports 0xCF8/0xCFC)
 *   - Interrupt control (CLI/STI, flags save/restore)
 *   - Execution stall (port 0x80 read delay)
 *   - CPU halt (HLT)
 *
 * AUTHOR: Claude Commons & Nell Watson, March 2026
 * LICENSE: MIT License
 */

#ifndef PORTIO_H
#define PORTIO_H

/* ================================================================
 * OPEN WATCOM
 * ================================================================ */

#ifdef __WATCOMC__

#include <conio.h>

/* --- Scalar port I/O (Watcom builtins) --- */

#define PORT_IN_BYTE(port)          ((unsigned char)inp((unsigned short)(port)))
#define PORT_IN_WORD(port)          ((unsigned short)inpw((unsigned short)(port)))
#define PORT_IN_DWORD(port)         ((unsigned long)inpd((unsigned short)(port)))
#define PORT_OUT_BYTE(port, val)    outp((unsigned short)(port), (unsigned char)(val))
#define PORT_OUT_WORD(port, val)    outpw((unsigned short)(port), (unsigned short)(val))
#define PORT_OUT_DWORD(port, val)   outpd((unsigned short)(port), (unsigned long)(val))

/* --- Buffer port I/O (pragma aux for rep insw/outsw/insd/outsd) --- */

void port_read_buffer_ushort(unsigned short port, unsigned short *buf, unsigned long count);
#pragma aux port_read_buffer_ushort = \
    "rep insw" \
    parm [dx] [edi] [ecx] \
    modify [edi ecx];

void port_read_buffer_ulong(unsigned short port, unsigned long *buf, unsigned long count);
#pragma aux port_read_buffer_ulong = \
    "rep insd" \
    parm [dx] [edi] [ecx] \
    modify [edi ecx];

void port_write_buffer_ushort(unsigned short port, unsigned short *buf, unsigned long count);
#pragma aux port_write_buffer_ushort = \
    "rep outsw" \
    parm [dx] [esi] [ecx] \
    modify [esi ecx];

void port_write_buffer_ulong(unsigned short port, unsigned long *buf, unsigned long count);
#pragma aux port_write_buffer_ulong = \
    "rep outsd" \
    parm [dx] [esi] [ecx] \
    modify [esi ecx];

#define PORT_READ_BUFFER_USHORT(port, buf, count) \
    port_read_buffer_ushort((unsigned short)(unsigned long)(port), (buf), (count))
#define PORT_READ_BUFFER_ULONG(port, buf, count) \
    port_read_buffer_ulong((unsigned short)(unsigned long)(port), (buf), (count))
#define PORT_WRITE_BUFFER_USHORT(port, buf, count) \
    port_write_buffer_ushort((unsigned short)(unsigned long)(port), (buf), (count))
#define PORT_WRITE_BUFFER_ULONG(port, buf, count) \
    port_write_buffer_ulong((unsigned short)(unsigned long)(port), (buf), (count))

/* --- PCI config space access --- */

/*
 * PORT_PCI_CONFIG_READ: write address to 0xCF8, read dword from 0xCFC.
 * addr must already be OR'd with 0x80000000 | bus<<16 | dev<<11 | func<<8 | (reg & 0xFC).
 */
unsigned long port_pci_config_read(unsigned long addr);
#pragma aux port_pci_config_read = \
    "mov dx, 0CF8h"  \
    "out dx, eax"    \
    "mov dx, 0CFCh"  \
    "in  eax, dx"    \
    parm [eax] \
    value [eax] \
    modify [edx];

#define PORT_PCI_CONFIG_READ(addr) port_pci_config_read(addr)

/* --- Interrupt control --- */

void port_cli(void);
#pragma aux port_cli = "cli";

void port_sti(void);
#pragma aux port_sti = "sti";

unsigned long port_save_flags_cli(void);
#pragma aux port_save_flags_cli = \
    "pushfd"  \
    "pop eax" \
    "cli"     \
    value [eax];

void port_restore_flags(unsigned long flags);
#pragma aux port_restore_flags = \
    "push eax" \
    "popfd"    \
    parm [eax];

#define PORT_CLI()                  port_cli()
#define PORT_STI()                  port_sti()
#define PORT_SAVE_FLAGS_CLI(var)    do { (var) = port_save_flags_cli(); } while(0)
#define PORT_RESTORE_FLAGS(var)     port_restore_flags(var)

/* --- Stall execution (port 0x80 read, ~1us per iteration) --- */

void port_stall_one(void);
#pragma aux port_stall_one = \
    "in al, 80h" \
    modify [eax];

#define PORT_STALL_ONE()            port_stall_one()

/* --- CPU halt --- */

void port_cli_hlt(void);
#pragma aux port_cli_hlt = \
    "cli" \
    "hlt";

void port_hlt(void);
#pragma aux port_hlt = "hlt";

#define PORT_CLI_HLT()              port_cli_hlt()
#define PORT_HLT()                  port_hlt()


/* ================================================================
 * GCC (i686-elf-gcc or similar)
 * ================================================================ */

#else /* GCC / Clang */

/* --- Scalar port I/O --- */

static __inline__ unsigned char PORT_IN_BYTE(unsigned short port)
{
    unsigned char val;
    __asm__ volatile ("inb %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static __inline__ unsigned short PORT_IN_WORD(unsigned short port)
{
    unsigned short val;
    __asm__ volatile ("inw %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static __inline__ unsigned long PORT_IN_DWORD(unsigned short port)
{
    unsigned long val;
    __asm__ volatile ("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static __inline__ void PORT_OUT_BYTE(unsigned short port, unsigned char val)
{
    __asm__ volatile ("outb %0, %w1" : : "a"(val), "Nd"(port));
}

static __inline__ void PORT_OUT_WORD(unsigned short port, unsigned short val)
{
    __asm__ volatile ("outw %0, %w1" : : "a"(val), "Nd"(port));
}

static __inline__ void PORT_OUT_DWORD(unsigned short port, unsigned long val)
{
    __asm__ volatile ("outl %0, %w1" : : "a"(val), "Nd"(port));
}

/* --- Buffer port I/O --- */

static __inline__ void PORT_READ_BUFFER_USHORT(unsigned short port,
                                                unsigned short *buf,
                                                unsigned long count)
{
    __asm__ volatile ("rep insw"
        : "+D"(buf), "+c"(count)
        : "d"(port)
        : "memory");
}

static __inline__ void PORT_READ_BUFFER_ULONG(unsigned short port,
                                               unsigned long *buf,
                                               unsigned long count)
{
    __asm__ volatile ("rep insl"
        : "+D"(buf), "+c"(count)
        : "d"(port)
        : "memory");
}

static __inline__ void PORT_WRITE_BUFFER_USHORT(unsigned short port,
                                                 unsigned short *buf,
                                                 unsigned long count)
{
    __asm__ volatile ("rep outsw"
        : "+S"(buf), "+c"(count)
        : "d"(port)
        : "memory");
}

static __inline__ void PORT_WRITE_BUFFER_ULONG(unsigned short port,
                                                unsigned long *buf,
                                                unsigned long count)
{
    __asm__ volatile ("rep outsl"
        : "+S"(buf), "+c"(count)
        : "d"(port)
        : "memory");
}

/* --- PCI config space access --- */

static __inline__ unsigned long PORT_PCI_CONFIG_READ(unsigned long addr)
{
    unsigned long val;
    __asm__ volatile (
        "movl %1, %%eax\n\t"
        "movw $0xCF8, %%dx\n\t"
        "outl %%eax, %%dx\n\t"
        "movw $0xCFC, %%dx\n\t"
        "inl  %%dx, %%eax"
        : "=a"(val)
        : "r"(addr)
        : "edx"
    );
    return val;
}

/* --- Interrupt control --- */

static __inline__ void PORT_CLI(void)
{
    __asm__ volatile ("cli");
}

static __inline__ void PORT_STI(void)
{
    __asm__ volatile ("sti");
}

#define PORT_SAVE_FLAGS_CLI(var) \
    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(var))

#define PORT_RESTORE_FLAGS(var) \
    __asm__ volatile ("pushl %0; popfl" : : "r"(var))

/* --- Stall execution --- */

static __inline__ void PORT_STALL_ONE(void)
{
    __asm__ volatile ("inb $0x80, %%al" : : : "eax");
}

/* --- CPU halt --- */

static __inline__ void PORT_CLI_HLT(void)
{
    __asm__ volatile ("cli; hlt");
}

static __inline__ void PORT_HLT(void)
{
    __asm__ volatile ("hlt");
}

#endif /* __WATCOMC__ */

#endif /* PORTIO_H */
