/* Finds "stale" top mip level files (see tex_scan.h). Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tex_scan.h"

#include <stdlib.h>
#include <string.h>

/* Enough for the texture header, the longest top level path and the DDS
 * header, plus the pixels that go into an identity. */
#define HEAD_BYTES 0x300u
#define MAX_FITTING 8

typedef struct scan_buffers {
    unsigned char *a;       /* TS_COMPARE_BYTES */
    unsigned char *b;       /* TS_COMPARE_BYTES */
    unsigned char *packed;  /* compressed input for one prefix */
    size_t packed_cap;
} scan_buffers;

/* The first `want` bytes of an entry's content. Returns how many are in out,
 * or -1 when the entry cannot be read. */
static long entry_prefix(const ts_input *in, unsigned archive, const dn_entry *e, size_t want, unsigned char *out,
                         scan_buffers *buf)
{
    if (e->scheme == DN_SCHEME_STORED) {
        size_t n = e->stored < want ? e->stored : want;
        if (n && in->read(in->ctx, archive, e->offset, out, n) != 0) {
            return -1;
        }
        return (long)n;
    }
    if (e->scheme == DN_SCHEME_LZO) {
        /* LZO never expands the beginning of a stream by more than a few
         * bytes per hundred, so this much input yields `want` bytes. */
        size_t n = want + want / 8 + 256;
        if (n > e->stored) {
            n = e->stored;
        }
        if (n > buf->packed_cap || (n && in->read(in->ctx, archive, e->offset, buf->packed, n) != 0)) {
            return -1;
        }
        return dn_lzo1x(buf->packed, n, out, want);
    }
    return -1;
}

static int same_format(tex_format a, tex_format b)
{
    return a.kind == b.kind && (a.kind != TEX_KIND_RAW || a.bits == b.bits);
}

static int compare_ids(const void *pa, const void *pb)
{
    unsigned long long a = *(const unsigned long long *)pa, b = *(const unsigned long long *)pb;
    return a < b ? -1 : a > b;
}

long tex_scan_stale(const ts_input *in, unsigned long long *ids, size_t cap, ts_stats *stats)
{
    ts_stats local;
    scan_buffers buf;
    unsigned long i;
    size_t count = 0, kept = 0, k;

    if (!stats) {
        stats = &local;
    }
    memset(stats, 0, sizeof(*stats));
    if (!in || !in->fats || in->count < 1 || !in->read || !in->fats[0].data) {
        return 0;
    }
    buf.packed_cap = TS_COMPARE_BYTES + TS_COMPARE_BYTES / 8 + 256;
    buf.a = (unsigned char *)malloc(TS_COMPARE_BYTES);
    buf.b = (unsigned char *)malloc(TS_COMPARE_BYTES);
    buf.packed = (unsigned char *)malloc(buf.packed_cap);
    if (!buf.a || !buf.b || !buf.packed) {
        free(buf.a);
        free(buf.b);
        free(buf.packed);
        return -1;
    }

    for (i = 0; i < in->fats[0].count; ++i) {
        dn_entry base, other;
        dn_xbt bx, tx;
        unsigned long long top_hash, fitting[MAX_FITTING];
        unsigned a, n_fitting = 0;
        int top_seen = 0, id_differs = 0, any_copy = 0, identical = 0, have_mine = 0;
        long n, mine = 0;

        dn_fat_entry(&in->fats[0], i, &base);
        n = entry_prefix(in, 0, &base, HEAD_BYTES, buf.a, &buf);
        if (n < 0 || dn_xbt_parse(buf.a, (size_t)n, &bx) != 0) {
            continue;
        }
        stats->textures++;
        if (!bx.top_level[0]) {
            continue;
        }
        stats->linked++;
        top_hash = dn_name_hash(bx.top_level);
        if (dn_fat_find(&in->fats[0], top_hash, &other)) {
            stats->own_top++; /* the mod's own pair; the size rule still applies */
            continue;
        }

        /* Every copy of the top level in the game's archives that would be accepted. */
        for (a = 1; a < in->count; ++a) {
            unsigned long long id;
            if (!dn_fat_find(&in->fats[a], top_hash, &other)) {
                continue;
            }
            top_seen = 1;
            n = entry_prefix(in, a, &other, HEAD_BYTES, buf.b, &buf);
            if (n < 0 || dn_xbt_parse(buf.b, (size_t)n, &tx) != 0 || tx.top_level[0]) {
                continue;
            }
            if (tx.dds.width != bx.dds.width * 2 || tx.dds.height != bx.dds.height * 2 ||
                !same_format(tx.dds.format, bx.dds.format) || other.size < tx.header_size) {
                continue;
            }
            id = dn_top_identity(buf.b, buf.b + tx.header_size, other.size - tx.header_size,
                                 (size_t)n - tx.header_size);
            if (!id) {
                continue;
            }
            if (memcmp(tx.id, bx.id, DN_XBT_ID_SIZE) != 0) {
                id_differs = 1;
            }
            for (k = 0; k < n_fitting && fitting[k] != id; ++k) {
            }
            if (k == n_fitting && n_fitting < MAX_FITTING) {
                fitting[n_fitting++] = id;
            }
        }
        if (!n_fitting) {
            if (top_seen) {
                stats->unfit++;
            } else {
                stats->missing++;
            }
            continue;
        }

        /* Is the override's base still the picture the top level was made for? */
        for (a = 1; a < in->count && !identical; ++a) {
            if (!dn_fat_find(&in->fats[a], base.hash, &other)) {
                continue;
            }
            any_copy = 1;
            if (other.size != base.size) {
                continue;
            }
            if (!have_mine) {
                mine = entry_prefix(in, 0, &base, TS_COMPARE_BYTES, buf.a, &buf);
                have_mine = 1;
            }
            n = entry_prefix(in, a, &other, TS_COMPARE_BYTES, buf.b, &buf);
            if (mine > 0 && n == mine && memcmp(buf.a, buf.b, (size_t)n) == 0) {
                identical = 1;
            }
        }
        /* A file under a new name has no original to compare with; there the
         * source id tells whether it was made from the same picture. */
        if (any_copy ? identical : !id_differs) {
            stats->unchanged++;
            continue;
        }
        stats->stale++;
        for (k = 0; k < n_fitting; ++k) {
            if (count < cap) {
                ids[count++] = fitting[k];
            } else {
                stats->truncated++;
            }
        }
    }
    free(buf.a);
    free(buf.b);
    free(buf.packed);

    if (count > 1) {
        qsort(ids, count, sizeof(ids[0]), compare_ids);
    }
    for (k = 0; k < count; ++k) {
        if (!kept || ids[kept - 1] != ids[k]) {
            ids[kept++] = ids[k];
        }
    }
    return (long)kept;
}

int tex_scan_contains(const unsigned long long *ids, size_t count, unsigned long long id)
{
    size_t lo = 0, hi = count;

    if (!ids || !id) {
        return 0;
    }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ids[mid] == id) {
            return 1;
        }
        if (ids[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}
