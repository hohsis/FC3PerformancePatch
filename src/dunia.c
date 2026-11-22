/* Reading the game's data archives and texture files. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dunia.h"

#include <string.h>

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

/* ---- archive index ------------------------------------------------------------- */

#define FAT_HEADER 0x18u
#define FAT_ENTRY 20u
#define FAT_VERSION 9ul

static unsigned long long entry_hash(const dn_fat *fat, unsigned long index)
{
    const unsigned char *p = fat->data + FAT_HEADER + (size_t)index * FAT_ENTRY;
    return ((unsigned long long)rd32(p) << 32) | rd32(p + 4);
}

int dn_fat_open(dn_fat *fat, const unsigned char *data, size_t len)
{
    unsigned long count, i;
    unsigned long long previous = 0;

    memset(fat, 0, sizeof(*fat));
    if (!data || len < FAT_HEADER || memcmp(data, "2TAF", 4) != 0 || rd32(data + 4) != FAT_VERSION) {
        return -1;
    }
    count = rd32(data + 0x14);
    if (count > (len - FAT_HEADER) / FAT_ENTRY) {
        return -1;
    }
    fat->data = data;
    fat->count = count;
    fat->sorted = 1;
    for (i = 0; i < count; ++i) {
        unsigned long long h = entry_hash(fat, i);
        if (i && h < previous) {
            fat->sorted = 0;
            break;
        }
        previous = h;
    }
    return 0;
}

void dn_fat_entry(const dn_fat *fat, unsigned long index, dn_entry *out)
{
    const unsigned char *p = fat->data + FAT_HEADER + (size_t)index * FAT_ENTRY;
    unsigned long c = rd32(p + 8), d = rd32(p + 12), e = rd32(p + 16);

    out->hash = ((unsigned long long)rd32(p) << 32) | rd32(p + 4);
    out->scheme = (unsigned)(c & 3);
    out->offset = ((unsigned long long)d << 2) | (e >> 30);
    out->stored = e & 0x3FFFFFFFul;
    out->size = out->scheme == DN_SCHEME_STORED ? out->stored : (c >> 2);
}

int dn_fat_find(const dn_fat *fat, unsigned long long hash, dn_entry *out)
{
    unsigned long lo = 0, hi = fat->count;

    if (!fat->data) {
        return 0;
    }
    if (!fat->sorted) {
        for (lo = 0; lo < fat->count; ++lo) {
            if (entry_hash(fat, lo) == hash) {
                dn_fat_entry(fat, lo, out);
                return 1;
            }
        }
        return 0;
    }
    while (lo < hi) {
        unsigned long mid = lo + (hi - lo) / 2;
        unsigned long long h = entry_hash(fat, mid);
        if (h == hash) {
            dn_fat_entry(fat, mid, out);
            return 1;
        }
        if (h < hash) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

unsigned long long dn_name_hash(const char *path)
{
    unsigned long long crc = 0;
    int bit;

    for (; path && *path; ++path) {
        unsigned char c = (unsigned char)*path;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + ('a' - 'A'));
        }
        crc ^= c;
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xD800000000000000ull : crc >> 1;
        }
    }
    return crc;
}

/* ---- LZO1X ------------------------------------------------------------------------
 * The stream is a sequence of instructions. What a small first byte means
 * depends on what came before, hence the state. */
enum { LZO_START, LZO_AFTER_LITERALS, LZO_AFTER_SHORT_RUN };

long dn_lzo1x(const unsigned char *src, size_t n, unsigned char *dst, size_t cap)
{
    size_t ip = 0, op = 0;
    int state = LZO_START;

    if (!src || !dst || !n) {
        return src && dst ? 0 : -1;
    }
/* Literal bytes; leaves the function when either buffer runs out. */
#define COPY_LITERALS(count)                                          \
    do {                                                              \
        size_t want_ = (count), k_ = want_;                           \
        if (k_ > n - ip) {                                            \
            k_ = n - ip;                                              \
        }                                                             \
        if (k_ > cap - op) {                                          \
            k_ = cap - op;                                            \
        }                                                             \
        memcpy(dst + op, src + ip, k_);                               \
        ip += k_;                                                     \
        op += k_;                                                     \
        if (k_ < want_) {                                             \
            return (long)op;                                          \
        }                                                             \
    } while (0)
/* A length that continues in the following bytes: each zero adds 255. */
#define LONG_LENGTH(t, base)                                          \
    do {                                                              \
        while (ip < n && src[ip] == 0) {                              \
            (t) += 255;                                               \
            ++ip;                                                     \
        }                                                             \
        if (ip >= n) {                                                \
            return (long)op;                                          \
        }                                                             \
        (t) += (base) + src[ip++];                                    \
    } while (0)

    if (src[0] > 17) {
        size_t t = (size_t)src[0] - 17;
        ip = 1;
        COPY_LITERALS(t);
        state = t < 4 ? LZO_AFTER_SHORT_RUN : LZO_AFTER_LITERALS;
    }
    while (ip < n && op < cap) {
        size_t t = src[ip++];
        size_t distance, length, tail;

        if (t < 16) {
            if (state == LZO_START) {
                if (t == 0) {
                    LONG_LENGTH(t, 15);
                }
                COPY_LITERALS(t + 3);
                state = LZO_AFTER_LITERALS;
                continue;
            }
            if (ip >= n) {
                break;
            }
            if (state == LZO_AFTER_LITERALS) {
                distance = 0x801 + (t >> 2) + ((size_t)src[ip++] << 2);
                length = 3;
            } else {
                distance = 1 + (t >> 2) + ((size_t)src[ip++] << 2);
                length = 2;
            }
        } else if (t >= 64) {
            if (ip >= n) {
                break;
            }
            distance = 1 + ((t >> 2) & 7) + ((size_t)src[ip++] << 3);
            length = (t >> 5) + 1;
        } else if (t >= 32) {
            t &= 31;
            if (t == 0) {
                LONG_LENGTH(t, 31);
            }
            if (ip + 2 > n) {
                break;
            }
            distance = 1 + (src[ip] >> 2) + ((size_t)src[ip + 1] << 6);
            ip += 2;
            length = t + 2;
        } else {
            distance = (t & 8) << 11;
            t &= 7;
            if (t == 0) {
                LONG_LENGTH(t, 7);
            }
            if (ip + 2 > n) {
                break;
            }
            distance += (src[ip] >> 2) + ((size_t)src[ip + 1] << 6);
            ip += 2;
            if (distance == 0) {
                break; /* end marker */
            }
            distance += 0x4000;
            length = t + 2;
        }
        if (distance > op) {
            return -1;
        }
        while (length-- && op < cap) {
            dst[op] = dst[op - distance];
            ++op;
        }
        /* The low two bits of the instruction's second-to-last byte: a few
         * literals that follow directly. */
        tail = src[ip - 2] & 3;
        if (tail) {
            COPY_LITERALS(tail);
            state = LZO_AFTER_SHORT_RUN;
        } else {
            state = LZO_START;
        }
    }
    return (long)op;
#undef COPY_LITERALS
#undef LONG_LENGTH
}

/* ---- textures ------------------------------------------------------------------- */

int dn_xbt_parse(const unsigned char *data, size_t len, dn_xbt *out)
{
    unsigned long header;
    size_t i, path_len;

    if (!data || !out || len < DN_XBT_FIXED_HEADER + 4 || memcmp(data, "TBX\0", 4) != 0) {
        return -1;
    }
    header = rd32(data + 8);
    if (header < DN_XBT_FIXED_HEADER + 4 || header > DN_XBT_FIXED_HEADER + DN_XBT_MAX_PATH ||
        header + TEX_DDS_HEADER_SIZE > len) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->header_size = (unsigned)header;
    memcpy(out->id, data + DN_XBT_ID_OFFSET, DN_XBT_ID_SIZE);
    path_len = header - DN_XBT_FIXED_HEADER;
    for (i = 0; i < path_len && i + 1 < DN_XBT_MAX_PATH && data[DN_XBT_FIXED_HEADER + i]; ++i) {
        out->top_level[i] = (char)data[DN_XBT_FIXED_HEADER + i];
    }
    out->top_level[i] = '\0';
    return tex_dds_parse(data + header, len - header, &out->dds);
}

static unsigned long long fnv(unsigned long long h, const unsigned char *p, size_t n)
{
    while (n--) {
        h = (h ^ *p++) * 0x100000001B3ull;
    }
    return h;
}

unsigned long long dn_top_identity(const unsigned char *xbt, const unsigned char *dds, size_t dds_size,
                                   size_t dds_avail)
{
    unsigned long long h = 0xCBF29CE484222325ull;
    unsigned char size_bytes[4];
    size_t pixels, sample;

    if (!xbt || !dds || dds_size < TEX_DDS_HEADER_SIZE || dds_avail < TEX_DDS_HEADER_SIZE) {
        return 0;
    }
    pixels = dds_size - TEX_DDS_HEADER_SIZE;
    sample = pixels < DN_IDENTITY_PIXELS ? pixels : DN_IDENTITY_PIXELS;
    if (dds_avail - TEX_DDS_HEADER_SIZE < sample) {
        return 0; /* not enough of the image in hand to tell */
    }
    size_bytes[0] = (unsigned char)pixels;
    size_bytes[1] = (unsigned char)(pixels >> 8);
    size_bytes[2] = (unsigned char)(pixels >> 16);
    size_bytes[3] = (unsigned char)(pixels >> 24);
    h = fnv(h, xbt + 0x0C, DN_XBT_FIXED_HEADER - 0x0C); /* flags and source id */
    h = fnv(h, dds + 12, 8);                             /* height, width */
    h = fnv(h, dds + 84, 4);                             /* pixel format */
    h = fnv(h, size_bytes, 4);
    h = fnv(h, dds + TEX_DDS_HEADER_SIZE, sample);
    return h ? h : 1; /* 0 means "unknown" */
}
