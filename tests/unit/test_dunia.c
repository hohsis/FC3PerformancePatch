/* Unit tests: archive index, LZO1X, texture headers, and the scan for stale
 * top mip level files on archives built in memory.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>

#include "dunia.h"
#include "test.h"
#include "tex_scan.h"

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* ---- name hash ------------------------------------------------------------------- */

static void test_name_hash(void)
{
    /* A real pair from the game's archives. */
    static const char path[] =
        "graphics\\__fc3_graphics\\_common\\_textures\\characters\\npc_textures\\sv_male_head_m.xbt";
    static const char upper[] =
        "GRAPHICS\\__FC3_GRAPHICS\\_COMMON\\_TEXTURES\\CHARACTERS\\NPC_TEXTURES\\SV_MALE_HEAD_M.XBT";

    CHECK(dn_name_hash(path) == 0x00B1EC31284A7460ull);
    CHECK(dn_name_hash(upper) == 0x00B1EC31284A7460ull); /* names are case-insensitive */
    CHECK(dn_name_hash("") == 0);
    CHECK(dn_name_hash(NULL) == 0);
    CHECK(dn_name_hash("a") != dn_name_hash("b"));
}

/* ---- archive index --------------------------------------------------------------- */

static size_t fat_begin(unsigned char *fat, unsigned long count)
{
    memset(fat, 0, 0x18);
    memcpy(fat, "2TAF", 4);
    put32(fat + 4, 9);
    put32(fat + 8, 0x301);
    put32(fat + 0x14, count);
    return 0x18;
}

static void fat_put(unsigned char *fat, unsigned long index, unsigned long long hash, unsigned long long offset,
                    unsigned long stored, unsigned long size, unsigned scheme)
{
    unsigned char *p = fat + 0x18 + index * 20;
    put32(p, (unsigned long)(hash >> 32));
    put32(p + 4, (unsigned long)hash);
    put32(p + 8, scheme == DN_SCHEME_STORED ? 0 : (size << 2) | scheme);
    put32(p + 12, (unsigned long)(offset >> 2));
    put32(p + 16, ((unsigned long)(offset & 3) << 30) | stored);
}

static void test_fat(void)
{
    unsigned char fat[0x18 + 4 * 20];
    dn_fat f;
    dn_entry e;

    fat_begin(fat, 4);
    fat_put(fat, 0, 0x0000000100000002ull, 0, 100, 100, DN_SCHEME_STORED);
    fat_put(fat, 1, 0x0000000100000003ull, 0x100000001ull, 0x3FFFFFFFul, 0, DN_SCHEME_STORED);
    fat_put(fat, 2, 0x7FFFFFFF00000000ull, 4096, 50, 1234, DN_SCHEME_LZO);
    fat_put(fat, 3, 0xFFFFFFFFFFFFFFFFull, 7, 1, 1, DN_SCHEME_STORED);

    CHECK(dn_fat_open(&f, fat, sizeof(fat)) == 0);
    CHECK_EQ_U(f.count, 4);
    CHECK(f.sorted == 1);

    CHECK(dn_fat_find(&f, 0x0000000100000002ull, &e) == 1);
    CHECK(e.offset == 0 && e.stored == 100 && e.size == 100 && e.scheme == DN_SCHEME_STORED);
    /* offsets beyond 4 GB and odd offsets: the low two bits live in the last field */
    CHECK(dn_fat_find(&f, 0x0000000100000003ull, &e) == 1);
    CHECK(e.offset == 0x100000001ull);
    CHECK_EQ_U(e.stored, 0x3FFFFFFFul);
    CHECK(dn_fat_find(&f, 0x7FFFFFFF00000000ull, &e) == 1);
    CHECK(e.offset == 4096 && e.stored == 50 && e.size == 1234 && e.scheme == DN_SCHEME_LZO);
    CHECK(dn_fat_find(&f, 0xFFFFFFFFFFFFFFFFull, &e) == 1 && e.offset == 7);
    CHECK(dn_fat_find(&f, 0x0000000100000004ull, &e) == 0);
    CHECK(dn_fat_find(&f, 0, &e) == 0);

    /* An index that is not sorted is still searched correctly. */
    fat_put(fat, 0, 0x9000000000000000ull, 0, 100, 100, DN_SCHEME_STORED);
    CHECK(dn_fat_open(&f, fat, sizeof(fat)) == 0 && f.sorted == 0);
    CHECK(dn_fat_find(&f, 0x9000000000000000ull, &e) == 1 && e.stored == 100);
    CHECK(dn_fat_find(&f, 0xFFFFFFFFFFFFFFFFull, &e) == 1);
    CHECK(dn_fat_find(&f, 0x0000000100000002ull, &e) == 0);

    /* Things that are not an index this code reads. */
    CHECK(dn_fat_open(&f, fat, sizeof(fat) - 1) == -1); /* cut off */
    CHECK(dn_fat_find(&f, 1, &e) == 0);                 /* ... and then holds nothing */
    put32(fat + 4, 8);
    CHECK(dn_fat_open(&f, fat, sizeof(fat)) == -1); /* other version */
    put32(fat + 4, 9);
    fat[0] = 'X';
    CHECK(dn_fat_open(&f, fat, sizeof(fat)) == -1);
    CHECK(dn_fat_open(&f, NULL, 0) == -1);
    fat_begin(fat, 0);
    CHECK(dn_fat_open(&f, fat, 0x18) == 0 && f.count == 0);
}

/* ---- LZO1X ------------------------------------------------------------------------ */

static void test_lzo(void)
{
    /* "abc", then nine bytes copied from three back, end marker */
    static const unsigned char repeat[] = {0x14, 'a', 'b', 'c', 0x27, 0x08, 0x00, 0x11, 0x00, 0x00};
    /* the same with two literals attached to the match */
    static const unsigned char tail[] = {0x14, 'a', 'b', 'c', 0x27, 0x0A, 0x00, 'X', 'Y', 0x11, 0x00, 0x00};
    /* short match forms after a short literal run */
    static const unsigned char m2[] = {0x14, 'a', 'b', 'c', 0x68, 0x00, 0x11, 0x00, 0x00};
    static const unsigned char m1[] = {0x14, 'a', 'b', 'c', 0x04, 0x00, 0x11, 0x00, 0x00};
    /* a match that points before the start of the output */
    static const unsigned char bad[] = {0x14, 'a', 'b', 'c', 0x68, 0x01, 0x11, 0x00, 0x00};
    static unsigned char packed[1100], out[1100];
    unsigned i;

    memset(out, 0, sizeof(out));
    CHECK(dn_lzo1x(repeat, sizeof(repeat), out, sizeof(out)) == 12);
    CHECK(memcmp(out, "abcabcabcabc", 12) == 0);
    CHECK(dn_lzo1x(tail, sizeof(tail), out, sizeof(out)) == 14);
    CHECK(memcmp(out, "abcabcabcabcXY", 14) == 0);
    CHECK(dn_lzo1x(m2, sizeof(m2), out, sizeof(out)) == 7);
    CHECK(memcmp(out, "abcabca", 7) == 0);
    CHECK(dn_lzo1x(m1, sizeof(m1), out, sizeof(out)) == 5);
    CHECK(memcmp(out, "abcbc", 5) == 0);

    /* Only the beginning is wanted: stops when the output is full ... */
    memset(out, 0, sizeof(out));
    CHECK(dn_lzo1x(repeat, sizeof(repeat), out, 5) == 5);
    CHECK(memcmp(out, "abcab", 5) == 0 && out[5] == 0);
    /* ... or when the input ends, whichever comes first. */
    CHECK(dn_lzo1x(repeat, 3, out, sizeof(out)) == 2);
    CHECK(dn_lzo1x(repeat, 5, out, sizeof(out)) == 3);
    CHECK(dn_lzo1x(repeat, 6, out, sizeof(out)) == 3);

    CHECK(dn_lzo1x(bad, sizeof(bad), out, sizeof(out)) == -1);
    CHECK(dn_lzo1x(NULL, 4, out, sizeof(out)) == -1);
    CHECK(dn_lzo1x(repeat, 0, out, sizeof(out)) == 0);

    /* A long literal run: length 1024 = 3 + 15 + 3 * 255 + 241. */
    packed[0] = 0x00;
    packed[1] = packed[2] = packed[3] = 0x00;
    packed[4] = 0xF1;
    for (i = 0; i < 1024; ++i) {
        packed[5 + i] = (unsigned char)(i * 7 + 3);
    }
    packed[1029] = 0x11;
    packed[1030] = packed[1031] = 0x00;
    memset(out, 0, sizeof(out));
    CHECK(dn_lzo1x(packed, 1032, out, sizeof(out)) == 1024);
    CHECK(memcmp(out, packed + 5, 1024) == 0);
    CHECK(dn_lzo1x(packed, 1032, out, 100) == 100);
    CHECK(dn_lzo1x(packed, 300, out, sizeof(out)) == 295); /* input cut off inside the run */
}

/* ---- texture files ------------------------------------------------------------------ */

#define BC1_BASE 64u

/* Builds a texture file: header, optional top level path, DDS header, pixels.
 * Returns its size. */
static size_t make_xbt(unsigned char *out, const char *top_path, unsigned char id_seed, unsigned width,
                       unsigned height, const char *fourcc, size_t pixels, unsigned char pixel_seed)
{
    size_t header = 0x20 + (top_path ? ((strlen(top_path) + 1 + 3) & ~(size_t)3) : 4);
    unsigned char *dds = out + header;
    size_t i;

    memset(out, 0, header + 128);
    memcpy(out, "TBX\0", 4);
    put32(out + 4, 0x68);
    put32(out + 8, (unsigned long)header);
    put32(out + 0x0C, top_path ? 8 : 2);
    put32(out + 0x10, 0xFFFFFF00ul);
    for (i = 0; i < 12; ++i) {
        out[0x14 + i] = (unsigned char)(id_seed * 16 + i);
    }
    if (top_path) {
        memcpy(out + 0x20, top_path, strlen(top_path));
    }
    memcpy(dds, "DDS ", 4);
    put32(dds + 4, 124);
    put32(dds + 12, height);
    put32(dds + 16, width);
    put32(dds + 28, 1);
    put32(dds + 76, 32);
    put32(dds + 80, 4);
    memcpy(dds + 84, fourcc, 4);
    for (i = 0; i < pixels; ++i) {
        dds[128 + i] = (unsigned char)(i * 13 + pixel_seed);
    }
    return header + 128 + pixels;
}

static void test_xbt(void)
{
    static unsigned char file[2048];
    dn_xbt x;
    size_t n;

    n = make_xbt(file, "tex\\ground_d_mip0.xbt", 5, 256, 128, "DXT5", 600, 1);
    CHECK(dn_xbt_parse(file, n, &x) == 0);
    CHECK_STR(x.top_level, "tex\\ground_d_mip0.xbt");
    CHECK_EQ_U(x.header_size, 0x20 + 24);
    CHECK_EQ_U(x.dds.width, 256);
    CHECK_EQ_U(x.dds.height, 128);
    CHECK(x.dds.format.kind == TEX_KIND_BC3);
    CHECK(x.id[0] == 0x50 && x.id[11] == 0x5B);

    n = make_xbt(file, NULL, 5, 512, 256, "DXT1", 300, 2);
    CHECK(dn_xbt_parse(file, n, &x) == 0);
    CHECK_STR(x.top_level, "");
    CHECK_EQ_U(x.header_size, 0x24);
    CHECK(x.dds.width == 512 && x.dds.format.kind == TEX_KIND_BC1);

    /* The head of the file is enough, as long as the DDS header is in it. */
    CHECK(dn_xbt_parse(file, 0x24 + 128, &x) == 0);
    CHECK(dn_xbt_parse(file, 0x24 + 127, &x) == -1);
    CHECK(dn_xbt_parse(file, 8, &x) == -1);
    file[2] = 'Y';
    CHECK(dn_xbt_parse(file, n, &x) == -1);
    file[2] = 'X';
    put32(file + 8, 0x10); /* header size inside the fixed part */
    CHECK(dn_xbt_parse(file, n, &x) == -1);
    put32(file + 8, 0x1000); /* header size beyond any path */
    CHECK(dn_xbt_parse(file, n, &x) == -1);
    CHECK(dn_xbt_parse(NULL, 100, &x) == -1);
}

static void test_identity(void)
{
    static unsigned char a[1024], b[1024];
    size_t n = make_xbt(a, NULL, 5, 512, 512, "DXT1", 600, 1);
    unsigned long long id = dn_top_identity(a, a + 0x24, n - 0x24, n - 0x24);

    CHECK(id != 0);
    memcpy(b, a, n);
    CHECK(dn_top_identity(b, b + 0x24, n - 0x24, n - 0x24) == id);
    /* the head of the file gives the same answer as the whole file */
    CHECK(dn_top_identity(a, a + 0x24, n - 0x24, 128 + DN_IDENTITY_PIXELS) == id);
    /* ... and too little of it gives none */
    CHECK(dn_top_identity(a, a + 0x24, n - 0x24, 128 + DN_IDENTITY_PIXELS - 1) == 0);
    CHECK(dn_top_identity(a, a + 0x24, 100, 100) == 0);
    CHECK(dn_top_identity(NULL, a + 0x24, n - 0x24, n - 0x24) == 0);

    b[0x14] ^= 1; /* source id */
    CHECK(dn_top_identity(b, b + 0x24, n - 0x24, n - 0x24) != id);
    memcpy(b, a, n);
    b[0x24 + 16] ^= 1; /* width */
    CHECK(dn_top_identity(b, b + 0x24, n - 0x24, n - 0x24) != id);
    memcpy(b, a, n);
    b[0x24 + 87] = '5'; /* format */
    CHECK(dn_top_identity(b, b + 0x24, n - 0x24, n - 0x24) != id);
    memcpy(b, a, n);
    b[0x24 + 128 + 200] ^= 1; /* a pixel near the start */
    CHECK(dn_top_identity(b, b + 0x24, n - 0x24, n - 0x24) != id);
    /* same start, different length */
    CHECK(dn_top_identity(a, a + 0x24, n - 0x24 + 4, n - 0x24) != id);
    /* bytes outside the header and the sample do not matter */
    memcpy(b, a, n);
    b[4] ^= 1;
    b[0x24 + 128 + DN_IDENTITY_PIXELS + 10] ^= 1;
    CHECK(dn_top_identity(b, b + 0x24, n - 0x24, n - 0x24) == id);

    /* an image smaller than the sample is hashed whole */
    n = make_xbt(a, NULL, 5, 4, 4, "DXT1", 8, 1);
    CHECK(dn_top_identity(a, a + 0x24, n - 0x24, n - 0x24) != 0);
}

/* ---- archives in memory ------------------------------------------------------------- */

#define MAX_FILES 16

typedef struct mem_file {
    unsigned long long hash;
    const unsigned char *content;
    size_t size;
    int lzo;
} mem_file;

typedef struct mem_archive {
    mem_file files[MAX_FILES];
    unsigned count;
    unsigned char fat[0x18 + MAX_FILES * 20];
    unsigned char *dat;
    size_t dat_size;
    dn_fat index;
} mem_archive;

static mem_archive g_arch[3];
static unsigned g_reads;

static void arch_add(mem_archive *a, const char *name, const unsigned char *content, size_t size, int lzo)
{
    mem_file *f = &a->files[a->count++];
    f->hash = dn_name_hash(name);
    f->content = content;
    f->size = size;
    f->lzo = lzo;
}

static int by_hash(const void *pa, const void *pb)
{
    const mem_file *a = (const mem_file *)pa, *b = (const mem_file *)pb;
    return a->hash < b->hash ? -1 : a->hash > b->hash;
}

/* Literal-only LZO1X: valid for the decompressor, no compression. */
static size_t lzo_store(unsigned char *out, const unsigned char *in, size_t n)
{
    size_t o = 0;
    if (n <= 238) {
        out[o++] = (unsigned char)(17 + n);
    } else {
        size_t rest = n - 3 - 15;
        out[o++] = 0;
        while (rest > 255) {
            out[o++] = 0;
            rest -= 255;
        }
        out[o++] = (unsigned char)rest;
    }
    memcpy(out + o, in, n);
    o += n;
    out[o++] = 0x11;
    out[o++] = 0;
    out[o++] = 0;
    return o;
}

static void arch_finish(mem_archive *a)
{
    unsigned i;
    size_t offset = 1; /* an odd first offset, to use the split offset field */

    qsort(a->files, a->count, sizeof(a->files[0]), by_hash);
    a->dat = (unsigned char *)calloc(1, 0x10000);
    fat_begin(a->fat, a->count);
    for (i = 0; i < a->count; ++i) {
        mem_file *f = &a->files[i];
        size_t stored;
        if (f->lzo) {
            stored = lzo_store(a->dat + offset, f->content, f->size);
        } else {
            memcpy(a->dat + offset, f->content, f->size);
            stored = f->size;
        }
        fat_put(a->fat, i, f->hash, offset, (unsigned long)stored, (unsigned long)f->size,
                f->lzo ? DN_SCHEME_LZO : DN_SCHEME_STORED);
        offset += stored + 5;
    }
    a->dat_size = offset;
    dn_fat_open(&a->index, a->fat, 0x18 + a->count * 20);
}

static int arch_read(void *ctx, unsigned archive, unsigned long long offset, void *buf, size_t len)
{
    mem_archive *all = (mem_archive *)ctx;
    ++g_reads;
    if (archive >= 3 || offset + len > all[archive].dat_size) {
        return -1;
    }
    memcpy(buf, all[archive].dat + (size_t)offset, len);
    return 0;
}

static unsigned long long engine_identity(const unsigned char *file, size_t size)
{
    /* What the texture guard computes from the file the engine hands it. */
    unsigned header = (unsigned)file[8] | ((unsigned)file[9] << 8);
    return dn_top_identity(file, file + header, size - header, size - header);
}

static void test_scan(void)
{
    static unsigned char ground_game[1024], ground_mod[1024], ground_top[2048];
    static unsigned char rock[1024], rock_top[2048];
    static unsigned char wall_mod[1024], wall_top[1024];
    static unsigned char own[1024], own_top[2048];
    static unsigned char fresh[1024], other_top[2048];
    static unsigned char copy[1024], copy_top[2048];
    static unsigned char gone[1024], note[64];
    size_t n_ground_game, n_ground_mod, n_ground_top, n_rock, n_rock_top, n_wall_mod, n_wall_top;
    size_t n_own, n_own_top, n_fresh, n_other_top, n_copy, n_copy_top, n_gone;
    mem_archive *mod = &g_arch[0], *game = &g_arch[1], *world = &g_arch[2];
    dn_fat fats[3];
    ts_input in;
    ts_stats stats;
    unsigned long long ids[8];
    long n;

    memset(g_arch, 0, sizeof(g_arch));
    memset(note, 'n', sizeof(note));

    /* ground: the mod repaints it (same size, same source id as the original
     * header was copied), the top level stays the game's. The visible bug. */
    n_ground_game = make_xbt(ground_game, "tex\\ground_d_mip0.xbt", 1, BC1_BASE, BC1_BASE, "DXT1", 700, 10);
    n_ground_mod = make_xbt(ground_mod, "tex\\ground_d_mip0.xbt", 1, BC1_BASE, BC1_BASE, "DXT1", 700, 99);
    n_ground_top = make_xbt(ground_top, NULL, 1, 2 * BC1_BASE, 2 * BC1_BASE, "DXT1", 1500, 11);
    /* rock: the mod archive carries the game's own file unchanged. */
    n_rock = make_xbt(rock, "tex\\rock_d_mip0.xbt", 2, BC1_BASE, BC1_BASE, "DXT5", 700, 20);
    n_rock_top = make_xbt(rock_top, NULL, 2, 2 * BC1_BASE, 2 * BC1_BASE, "DXT5", 1500, 21);
    /* wall: replaced by a larger one; the game's top level no longer fits. */
    n_wall_mod = make_xbt(wall_mod, "tex\\wall_d_mip0.xbt", 3, 2 * BC1_BASE, 2 * BC1_BASE, "DXT1", 700, 30);
    n_wall_top = make_xbt(wall_top, NULL, 3, 2 * BC1_BASE, 2 * BC1_BASE, "DXT1", 600, 31);
    /* own: the mod brings both files. */
    n_own = make_xbt(own, "tex\\own_d_mip0.xbt", 4, BC1_BASE, BC1_BASE, "DXT1", 700, 40);
    n_own_top = make_xbt(own_top, NULL, 4, 2 * BC1_BASE, 2 * BC1_BASE, "DXT1", 1500, 41);
    /* fresh: a new file name that borrows the top level of another picture. */
    n_fresh = make_xbt(fresh, "tex\\other_d_mip0.xbt", 5, BC1_BASE, BC1_BASE, "DXT1", 700, 50);
    n_other_top = make_xbt(other_top, NULL, 6, 2 * BC1_BASE, 2 * BC1_BASE, "DXT1", 1500, 61);
    /* copy: a game texture duplicated under a new name, source id intact. */
    n_copy = make_xbt(copy, "tex\\copy_src_d_mip0.xbt", 7, BC1_BASE, BC1_BASE, "DXT1", 700, 70);
    n_copy_top = make_xbt(copy_top, NULL, 7, 2 * BC1_BASE, 2 * BC1_BASE, "DXT1", 1500, 71);
    /* gone: names a top level that exists nowhere. */
    n_gone = make_xbt(gone, "tex\\gone_d_mip0.xbt", 8, BC1_BASE, BC1_BASE, "DXT1", 700, 80);

    arch_add(mod, "tex\\ground_d.xbt", ground_mod, n_ground_mod, 0);
    arch_add(mod, "tex\\rock_d.xbt", rock, n_rock, 0);
    arch_add(mod, "tex\\wall_d.xbt", wall_mod, n_wall_mod, 0);
    arch_add(mod, "tex\\own_d.xbt", own, n_own, 0);
    arch_add(mod, "tex\\own_d_mip0.xbt", own_top, n_own_top, 0);
    arch_add(mod, "tex\\fresh_d.xbt", fresh, n_fresh, 0);
    arch_add(mod, "tex\\copy_d.xbt", copy, n_copy, 0);
    arch_add(mod, "tex\\gone_d.xbt", gone, n_gone, 0);
    arch_add(mod, "scripts\\note.lua", note, sizeof(note), 0);
    arch_finish(mod);

    arch_add(game, "tex\\ground_d.xbt", ground_game, n_ground_game, 0);
    arch_add(game, "tex\\ground_d_mip0.xbt", ground_top, n_ground_top, 0);
    arch_add(game, "tex\\rock_d_mip0.xbt", rock_top, n_rock_top, 0);
    arch_add(game, "tex\\wall_d_mip0.xbt", wall_top, n_wall_top, 0);
    arch_add(game, "tex\\other_d_mip0.xbt", other_top, n_other_top, 0);
    arch_finish(game);

    /* A second archive of the game, compressed: another copy of the ground
     * top level, the original rock base and the copy's top level. */
    arch_add(world, "tex\\ground_d_mip0.xbt", ground_top, n_ground_top, 1);
    arch_add(world, "tex\\rock_d.xbt", rock, n_rock, 1);
    arch_add(world, "tex\\copy_src_d_mip0.xbt", copy_top, n_copy_top, 1);
    arch_finish(world);

    fats[0] = mod->index;
    fats[1] = game->index;
    fats[2] = world->index;
    in.fats = fats;
    in.count = 3;
    in.read = arch_read;
    in.ctx = g_arch;

    memset(ids, 0, sizeof(ids));
    n = tex_scan_stale(&in, ids, 8, &stats);
    CHECK_EQ_U(stats.textures, 8);
    CHECK_EQ_U(stats.linked, 7);
    CHECK_EQ_U(stats.own_top, 1);
    CHECK_EQ_U(stats.missing, 1);
    CHECK_EQ_U(stats.unfit, 1);
    CHECK_EQ_U(stats.unchanged, 2); /* rock (compared with a compressed copy) and copy (same source id) */
    CHECK_EQ_U(stats.stale, 2);     /* ground and fresh */
    CHECK_EQ_U(stats.truncated, 0);
    CHECK(n == 2); /* the two copies of the ground top level are one identity */
    CHECK(n == 2 && ids[0] < ids[1]);

    CHECK(tex_scan_contains(ids, 2, engine_identity(ground_top, n_ground_top)));
    CHECK(tex_scan_contains(ids, 2, engine_identity(other_top, n_other_top)));
    CHECK(!tex_scan_contains(ids, 2, engine_identity(rock_top, n_rock_top)));
    CHECK(!tex_scan_contains(ids, 2, engine_identity(copy_top, n_copy_top)));
    CHECK(!tex_scan_contains(ids, 2, engine_identity(own_top, n_own_top)));
    CHECK(!tex_scan_contains(ids, 2, engine_identity(wall_top, n_wall_top)));
    CHECK(!tex_scan_contains(ids, 2, 0));
    CHECK(!tex_scan_contains(ids, 0, ids[0]));
    CHECK(!tex_scan_contains(NULL, 2, ids[0]));

    /* A list that is too short says so instead of overflowing. */
    n = tex_scan_stale(&in, ids, 1, &stats);
    CHECK(n == 1);
    CHECK_EQ_U(stats.truncated, 1);

    /* A game without a mod: its own patch archive carries a base texture
     * unchanged, and nothing is listed. */
    {
        static mem_archive plain[3];
        plain[0] = *world;
        plain[1] = *game;
        plain[2] = *world;
        fats[0] = world->index;
        in.ctx = plain;
        n = tex_scan_stale(&in, ids, 8, &stats);
        CHECK(n == 0);
        CHECK_EQ_U(stats.linked, 1); /* the rock base, whose top level is in the game */
        CHECK_EQ_U(stats.unchanged, 1);
        CHECK_EQ_U(stats.stale, 0);
        in.ctx = g_arch;
    }

    /* Unreadable data is skipped, not fatal. */
    fats[0] = mod->index;
    mod->dat_size = 10;
    n = tex_scan_stale(&in, ids, 8, &stats);
    CHECK(n == 0);
    CHECK_EQ_U(stats.textures, 0);

    /* No override archive at all. */
    memset(&fats[0], 0, sizeof(fats[0]));
    CHECK(tex_scan_stale(&in, ids, 8, &stats) == 0);
    CHECK(tex_scan_stale(NULL, ids, 8, &stats) == 0);

    free(mod->dat);
    free(game->dat);
    free(world->dat);
}

void suite_dunia(void)
{
    RUN(test_name_hash);
    RUN(test_fat);
    RUN(test_lzo);
    RUN(test_xbt);
    RUN(test_identity);
    RUN(test_scan);
}
