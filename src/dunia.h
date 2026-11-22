/* Reading the game's data archives (Dunia 2 ".fat" index + ".dat" data) and
 * texture files (".xbt"), as far as the texture guard needs them. Pure C.
 *
 * Everything here was derived from the files themselves:
 *
 *   .fat   "2TAF", u32 version (9), ..., u32 entry count at 14h, then 20-byte
 *          entries sorted by name hash:
 *              u32 hash high, u32 hash low,
 *              u32 (content size << 2) | scheme     scheme 0 = stored, 1 = LZO1X
 *              u32 d, u32 e                         offset = (d << 2) | (e >> 30)
 *                                                   stored size = e & 3FFFFFFFh
 *          For stored entries the content size field is 0.
 *   names  are not stored; an entry is found by the CRC-64 (polynomial 1Bh,
 *          reflected, no inversion) of its lower-case path.
 *   .xbt   "TBX\0", u32 version, u32 header size, u32, u32 flags, 12-byte
 *          source id, then (only in a base texture) the path of the file that
 *          holds its top mip level, then a DDS image. A base texture and its
 *          top level file carry the same source id.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_DUNIA_H
#define FC3PP_DUNIA_H

#include <stddef.h>

#include "tex_rule.h"

/* ---- archive index ------------------------------------------------------------- */

#define DN_SCHEME_STORED 0u
#define DN_SCHEME_LZO 1u

typedef struct dn_entry {
    unsigned long long hash;
    unsigned long long offset; /* in the .dat file */
    unsigned long stored;      /* bytes in the .dat file */
    unsigned long size;        /* bytes of content */
    unsigned scheme;
} dn_entry;

typedef struct dn_fat {
    const unsigned char *data; /* the whole .fat file */
    unsigned long count;
    int sorted; /* entries ascend by hash (true for every archive seen) */
} dn_fat;

/* Checks the header of a .fat file held in memory. Returns 0 on success. */
int dn_fat_open(dn_fat *fat, const unsigned char *data, size_t len);

void dn_fat_entry(const dn_fat *fat, unsigned long index, dn_entry *out);

/* Finds an entry by name hash. Returns 1 and fills *out, or 0. */
int dn_fat_find(const dn_fat *fat, unsigned long long hash, dn_entry *out);

unsigned long long dn_name_hash(const char *path);

/* ---- LZO1X ------------------------------------------------------------------------
 * Decompresses as much as fits: stops when dst is full, when src ends (callers
 * usually hand in only the beginning of an entry) or at the end marker.
 * Returns the number of bytes written, or -1 when the data is not LZO1X. */
long dn_lzo1x(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_cap);

/* ---- textures ------------------------------------------------------------------- */

#define DN_XBT_FIXED_HEADER 0x20u
#define DN_XBT_ID_OFFSET 0x14u
#define DN_XBT_ID_SIZE 12u
#define DN_XBT_MAX_PATH 256u

typedef struct dn_xbt {
    unsigned header_size;             /* the DDS image starts here */
    char top_level[DN_XBT_MAX_PATH];  /* "" when the texture has no separate top level */
    unsigned char id[DN_XBT_ID_SIZE];
    tex_dds dds;
} dn_xbt;

/* Reads the head of a texture file (needs the header and the DDS header that
 * follows it). Returns 0 on success, -1 when this is not such a file. */
int dn_xbt_parse(const unsigned char *data, size_t len, dn_xbt *out);

/* A value that identifies one top-level file by what the engine has in its
 * hands when it is about to use it: fields of the texture header, the image's
 * dimensions, format and size, and the beginning of its pixel data.
 * xbt: the file's first DN_XBT_FIXED_HEADER bytes; dds: the DDS image;
 * dds_size: its full size in the file; dds_avail: how much of it is readable. */
#define DN_IDENTITY_PIXELS 256u
unsigned long long dn_top_identity(const unsigned char *xbt, const unsigned char *dds, size_t dds_size,
                                   size_t dds_avail);

#endif /* FC3PP_DUNIA_H */
