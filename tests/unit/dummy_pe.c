/* Minimal PE32 image builder for tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dummy_pe.h"

#include <string.h>

#include "pe_file.h"

#define E_LFANEW 0x80

static void w16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void w32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

void dummy_pe_build(unsigned char *buf, unsigned characteristics, int with_checksum)
{
    unsigned char *coff = buf + E_LFANEW + 4;
    unsigned char *opt = coff + 20;
    unsigned char *sec = opt + 0xE0;
    size_t i;

    memset(buf, 0, DUMMY_PE_SIZE);
    buf[0] = 'M';
    buf[1] = 'Z';
    w32(buf + 0x3C, E_LFANEW);
    memcpy(buf + E_LFANEW, "PE\0\0", 4);

    w16(coff + 0, 0x014C);          /* Machine: i386 */
    w16(coff + 2, 1);               /* NumberOfSections */
    w32(coff + 4, 0x50000000);      /* TimeDateStamp */
    w16(coff + 16, 0xE0);           /* SizeOfOptionalHeader */
    w16(coff + 18, characteristics);

    w16(opt + 0, 0x010B);           /* PE32 */
    w32(opt + 16, 0x1000);          /* AddressOfEntryPoint */
    w32(opt + 28, 0x00400000);      /* ImageBase */
    w32(opt + 32, 0x1000);          /* SectionAlignment */
    w32(opt + 36, 0x200);           /* FileAlignment */
    w16(opt + 40, 5);               /* MajorOperatingSystemVersion */
    w16(opt + 48, 5);               /* MajorSubsystemVersion */
    w32(opt + 56, 0x2000);          /* SizeOfImage */
    w32(opt + 60, 0x200);           /* SizeOfHeaders */
    w16(opt + 68, 3);               /* Subsystem: console */
    w32(opt + 92, 16);              /* NumberOfRvaAndSizes */

    memcpy(sec, ".text\0\0\0", 8);
    w32(sec + 8, 0x200);            /* VirtualSize */
    w32(sec + 12, 0x1000);          /* VirtualAddress */
    w32(sec + 16, 0x200);           /* SizeOfRawData */
    w32(sec + 20, 0x200);           /* PointerToRawData */
    w32(sec + 36, 0x60000020);      /* code, execute, read */

    /* Some deterministic "code" so the checksum has something to chew on. */
    for (i = 0x200; i < DUMMY_PE_SIZE; ++i) {
        buf[i] = (unsigned char)(i * 7u + 3u);
    }
    if (with_checksum) {
        unsigned long sum = 0;
        pef_compute_checksum(buf, DUMMY_PE_SIZE, &sum);
        w32(opt + 64, sum);
    }
}

unsigned dummy_pe_characteristics(const unsigned char *buf)
{
    const unsigned char *p = buf + E_LFANEW + 4 + 18;
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
