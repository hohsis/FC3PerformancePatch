/* PE file-buffer helpers. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "pe_file.h"

#define PE_MACHINE_I386 0x014Cu
#define PE_MAGIC_PE32 0x010Bu

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

static void wr16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void wr32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* Validates the headers and returns the offset of the COFF file header
 * (just past "PE\0\0") in *coff. */
static pef_status locate(const unsigned char *buf, size_t len, size_t *coff)
{
    unsigned long e_lfanew;
    size_t opt;

    if (!buf || len < 0x40) {
        return len < 2 || !buf || buf[0] != 'M' || buf[1] != 'Z' ? PEF_NOT_PE : PEF_TRUNCATED;
    }
    if (buf[0] != 'M' || buf[1] != 'Z') {
        return PEF_NOT_PE;
    }
    e_lfanew = rd32(buf + 0x3C);
    if (e_lfanew > len || len - e_lfanew < 4 + 20) {
        return PEF_TRUNCATED;
    }
    if (buf[e_lfanew] != 'P' || buf[e_lfanew + 1] != 'E' || buf[e_lfanew + 2] != 0 || buf[e_lfanew + 3] != 0) {
        return PEF_NOT_PE;
    }
    *coff = (size_t)e_lfanew + 4;
    /* Optional header: we need it up to and including CheckSum (offset 64+4). */
    opt = *coff + 20;
    if (len - opt < 68 || rd16(buf + *coff + 16) < 68) {
        return PEF_TRUNCATED;
    }
    if (rd16(buf + *coff) != PE_MACHINE_I386 || rd16(buf + opt) != PE_MAGIC_PE32) {
        return PEF_NOT_X86;
    }
    return PEF_OK;
}

pef_status pef_get_laa(const unsigned char *buf, size_t len, int *laa)
{
    size_t coff = 0;
    pef_status s = locate(buf, len, &coff);
    if (s != PEF_OK) {
        return s;
    }
    if (laa) {
        *laa = (rd16(buf + coff + 18) & PEF_LARGE_ADDRESS_AWARE) != 0;
    }
    return PEF_OK;
}

pef_status pef_stored_checksum(const unsigned char *buf, size_t len, unsigned long *checksum)
{
    size_t coff = 0;
    pef_status s = locate(buf, len, &coff);
    if (s != PEF_OK) {
        return s;
    }
    if (checksum) {
        *checksum = rd32(buf + coff + 20 + 64);
    }
    return PEF_OK;
}

pef_status pef_compute_checksum(const unsigned char *buf, size_t len, unsigned long *checksum)
{
    size_t coff = 0;
    size_t field;
    size_t i;
    unsigned long sum = 0;
    pef_status s = locate(buf, len, &coff);
    if (s != PEF_OK) {
        return s;
    }
    field = coff + 20 + 64;
    for (i = 0; i + 1 < len; i += 2) {
        unsigned w;
        if (i == field || i == field + 2) {
            w = 0; /* the checksum field itself counts as zero */
        } else {
            w = rd16(buf + i);
        }
        sum += w;
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    if (len & 1) {
        sum += buf[len - 1];
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    sum = (sum & 0xFFFF) + (sum >> 16);
    if (checksum) {
        *checksum = (sum + (unsigned long)len) & 0xFFFFFFFFul;
    }
    return PEF_OK;
}

pef_status pef_set_laa(unsigned char *buf, size_t len, int enable, int *changed)
{
    size_t coff = 0;
    unsigned chars;
    unsigned wanted;
    pef_status s = locate(buf, len, &coff);
    if (changed) {
        *changed = 0;
    }
    if (s != PEF_OK) {
        return s;
    }
    chars = rd16(buf + coff + 18);
    wanted = enable ? (chars | PEF_LARGE_ADDRESS_AWARE) : (chars & ~PEF_LARGE_ADDRESS_AWARE);
    if (wanted == chars) {
        return PEF_OK;
    }
    wr16(buf + coff + 18, wanted);
    if (rd32(buf + coff + 20 + 64) != 0) {
        unsigned long sum = 0;
        pef_compute_checksum(buf, len, &sum);
        wr32(buf + coff + 20 + 64, sum);
    }
    if (changed) {
        *changed = 1;
    }
    return PEF_OK;
}

const char *pef_status_text(pef_status s)
{
    switch (s) {
    case PEF_OK:
        return "ok";
    case PEF_NOT_PE:
        return "not a PE file";
    case PEF_TRUNCATED:
        return "truncated PE headers";
    case PEF_NOT_X86:
        return "not a 32-bit x86 PE image";
    default:
        return "unknown";
    }
}
