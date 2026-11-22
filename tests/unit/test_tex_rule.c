/* Unit tests: texture top-level rule, refusal bookkeeping, class signatures.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <windows.h>

#include "test.h"
#include "tex_blobs.h"
#include "tex_link.h"
#include "tex_rule.h"

#define DXGI_R8G8B8A8_UNORM 28u
#define DXGI_BC1_TYPELESS 70u
#define DXGI_BC1_UNORM_SRGB 72u
#define DXGI_BC2_UNORM 74u
#define DXGI_BC3_TYPELESS 76u
#define DXGI_BC5_UNORM 83u
#define DXGI_BC7_UNORM 98u

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* A DDS header as texture tools write it. fourcc NULL = uncompressed with `bits`. */
static void make_dds(unsigned char *h, unsigned width, unsigned height, const char *fourcc, unsigned bits)
{
    memset(h, 0, TEX_DDS_HEADER_SIZE + 20);
    memcpy(h, "DDS ", 4);
    put32(h + 4, 124);
    put32(h + 8, 0x000A1007ul);
    put32(h + 12, height);
    put32(h + 16, width);
    put32(h + 28, 1);
    put32(h + 76, 32);
    if (fourcc) {
        put32(h + 80, 4);
        memcpy(h + 84, fourcc, 4);
    } else {
        put32(h + 80, 0x41);
        put32(h + 88, bits);
    }
}

static void test_dxgi_formats(void)
{
    char name[24];

    CHECK(tex_format_from_dxgi(DXGI_BC1_TYPELESS).kind == TEX_KIND_BC1);
    CHECK(tex_format_from_dxgi(DXGI_BC1_UNORM_SRGB).kind == TEX_KIND_BC1);
    CHECK(tex_format_from_dxgi(DXGI_BC2_UNORM).kind == TEX_KIND_BC2);
    CHECK(tex_format_from_dxgi(DXGI_BC3_TYPELESS).kind == TEX_KIND_BC3);
    CHECK(tex_format_from_dxgi(79).kind == TEX_KIND_BC4);
    CHECK(tex_format_from_dxgi(DXGI_BC5_UNORM).kind == TEX_KIND_BC5);
    CHECK(tex_format_from_dxgi(95).kind == TEX_KIND_BC6);
    CHECK(tex_format_from_dxgi(DXGI_BC7_UNORM).kind == TEX_KIND_BC7);
    CHECK(tex_format_from_dxgi(DXGI_R8G8B8A8_UNORM).kind == TEX_KIND_RAW);
    CHECK_EQ_U(tex_format_from_dxgi(DXGI_R8G8B8A8_UNORM).bits, 32);
    CHECK_EQ_U(tex_format_from_dxgi(87).bits, 32); /* B8G8R8A8 */
    CHECK_EQ_U(tex_format_from_dxgi(61).bits, 8);  /* R8 */
    CHECK_EQ_U(tex_format_from_dxgi(10).bits, 64); /* R16G16B16A16_FLOAT */
    CHECK(tex_format_from_dxgi(0).kind == TEX_KIND_UNKNOWN);
    CHECK(tex_format_from_dxgi(69).kind == TEX_KIND_UNKNOWN);
    CHECK(tex_format_from_dxgi(1000).kind == TEX_KIND_UNKNOWN);

    CHECK_STR(tex_format_name(tex_format_from_dxgi(DXGI_BC3_TYPELESS), name, sizeof(name)), "BC3");
    CHECK_STR(tex_format_name(tex_format_from_dxgi(DXGI_R8G8B8A8_UNORM), name, sizeof(name)), "32-bit");
    CHECK_STR(tex_format_name(tex_format_from_dxgi(0), name, sizeof(name)), "unknown format");
}

static void test_dds_headers(void)
{
    unsigned char h[TEX_DDS_HEADER_SIZE + 20];
    tex_dds d;

    make_dds(h, 1024, 512, "DXT1", 0);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0);
    CHECK_EQ_U(d.width, 1024);
    CHECK_EQ_U(d.height, 512);
    CHECK(d.format.kind == TEX_KIND_BC1);

    make_dds(h, 64, 64, "DXT3", 0);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0 && d.format.kind == TEX_KIND_BC2);
    make_dds(h, 64, 64, "DXT5", 0);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0 && d.format.kind == TEX_KIND_BC3);
    make_dds(h, 64, 64, "ATI1", 0);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0 && d.format.kind == TEX_KIND_BC4);
    make_dds(h, 64, 64, "ATI2", 0);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0 && d.format.kind == TEX_KIND_BC5);
    make_dds(h, 64, 64, "WXYZ", 0);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0 && d.format.kind == TEX_KIND_UNKNOWN);

    make_dds(h, 64, 64, NULL, 32);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0 && d.format.kind == TEX_KIND_RAW && d.format.bits == 32);
    /* 24-bit data would be converted on load: not judged by its header. */
    make_dds(h, 64, 64, NULL, 24);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0 && d.format.kind == TEX_KIND_UNKNOWN);

    /* The DX10 extension header carries a DXGI format. */
    make_dds(h, 64, 64, "DX10", 0);
    put32(h + TEX_DDS_HEADER_SIZE, DXGI_BC7_UNORM);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == 0 && d.format.kind == TEX_KIND_BC7);
    CHECK(tex_dds_parse(h, TEX_DDS_HEADER_SIZE, &d) == -1); /* extension header missing */

    /* Things that are not a DDS image. */
    make_dds(h, 64, 64, "DXT1", 0);
    CHECK(tex_dds_parse(h, TEX_DDS_HEADER_SIZE - 1, &d) == -1);
    CHECK(tex_dds_parse(NULL, sizeof(h), &d) == -1);
    h[0] = 'X';
    CHECK(tex_dds_parse(h, sizeof(h), &d) == -1);
    make_dds(h, 64, 64, "DXT1", 0);
    put32(h + 4, 100);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == -1);
    make_dds(h, 0, 64, "DXT1", 0);
    CHECK(tex_dds_parse(h, sizeof(h), &d) == -1);
}

static tex_verdict verdict(unsigned w, unsigned h, unsigned dxgi, unsigned dds_w, unsigned dds_h, const char *fourcc)
{
    unsigned char hdr[TEX_DDS_HEADER_SIZE + 20];
    make_dds(hdr, dds_w, dds_h, fourcc, 32);
    return tex_top_level_verdict(w, h, dxgi, hdr, sizeof(hdr), NULL);
}

static void test_rule(void)
{
    unsigned char hdr[TEX_DDS_HEADER_SIZE + 20];
    tex_dds seen;

    /* What every texture of the original game looks like. */
    CHECK(verdict(512, 512, DXGI_BC1_TYPELESS, 1024, 1024, "DXT1") == TEX_FITS);
    CHECK(verdict(256, 512, DXGI_BC3_TYPELESS, 512, 1024, "DXT5") == TEX_FITS);
    CHECK(verdict(1024, 1024, DXGI_BC5_UNORM, 2048, 2048, "ATI2") == TEX_FITS);
    CHECK(verdict(128, 128, DXGI_R8G8B8A8_UNORM, 256, 256, NULL) == TEX_FITS);

    /* The crash this exists for: a mod's 2048 texture and the game's 1024 top level. */
    CHECK(verdict(2048, 2048, DXGI_BC1_TYPELESS, 1024, 1024, "DXT1") == TEX_TOO_SMALL);
    /* Base replaced by a texture of the size of the top level. */
    CHECK(verdict(1024, 1024, DXGI_BC1_TYPELESS, 1024, 1024, "DXT1") == TEX_TOO_SMALL);
    /* One dimension short is enough to run off the end. */
    CHECK(verdict(512, 512, DXGI_BC1_TYPELESS, 1024, 512, "DXT1") == TEX_TOO_SMALL);
    CHECK(verdict(512, 512, DXGI_BC1_TYPELESS, 4096, 256, "DXT1") == TEX_TOO_SMALL);

    /* A shrunken base with the original top level: no overrun, but a wrong picture. */
    CHECK(verdict(256, 256, DXGI_BC1_TYPELESS, 1024, 1024, "DXT1") == TEX_TOO_LARGE);
    CHECK(verdict(256, 64, DXGI_BC1_TYPELESS, 512, 512, "DXT1") == TEX_TOO_LARGE);

    /* Right size, other format: half or double the bytes per block. */
    CHECK(verdict(512, 512, DXGI_BC3_TYPELESS, 1024, 1024, "DXT1") == TEX_WRONG_FORMAT);
    CHECK(verdict(512, 512, DXGI_BC1_TYPELESS, 1024, 1024, "DXT5") == TEX_WRONG_FORMAT);
    CHECK(verdict(512, 512, DXGI_BC2_UNORM, 1024, 1024, "DXT5") == TEX_WRONG_FORMAT);
    CHECK(verdict(512, 512, DXGI_BC1_TYPELESS, 1024, 1024, NULL) == TEX_WRONG_FORMAT);
    CHECK(verdict(512, 512, 61 /* R8 */, 1024, 1024, NULL) == TEX_WRONG_FORMAT);

    /* A format this code does not know is never a reason to refuse. */
    CHECK(verdict(512, 512, 0, 1024, 1024, "DXT1") == TEX_FITS);
    CHECK(verdict(512, 512, DXGI_BC1_TYPELESS, 1024, 1024, "WXYZ") == TEX_FITS);
    /* ... but the size still is. */
    CHECK(verdict(2048, 2048, 0, 1024, 1024, "WXYZ") == TEX_TOO_SMALL);

    /* Not judged at all. */
    make_dds(hdr, 1024, 1024, "DXT1", 0);
    CHECK(tex_top_level_verdict(512, 512, DXGI_BC1_TYPELESS, hdr, 64, NULL) == TEX_UNREADABLE);
    CHECK(tex_top_level_verdict(512, 512, DXGI_BC1_TYPELESS, NULL, 0, NULL) == TEX_UNREADABLE);
    CHECK(tex_top_level_verdict(0, 512, DXGI_BC1_TYPELESS, hdr, sizeof(hdr), NULL) == TEX_UNREADABLE);
    hdr[1] = 'X';
    CHECK(tex_top_level_verdict(512, 512, DXGI_BC1_TYPELESS, hdr, sizeof(hdr), NULL) == TEX_UNREADABLE);

    make_dds(hdr, 1024, 2048, "DXT5", 0);
    CHECK(tex_top_level_verdict(2048, 2048, DXGI_BC1_TYPELESS, hdr, sizeof(hdr), &seen) == TEX_TOO_SMALL);
    CHECK_EQ_U(seen.width, 1024);
    CHECK_EQ_U(seen.height, 2048);
    CHECK(seen.format.kind == TEX_KIND_BC3);

    CHECK_STR(tex_verdict_text(TEX_TOO_SMALL), "too small");
    CHECK_STR(tex_verdict_text(TEX_WRONG_FORMAT), "different pixel format");
}

/* ---- refusal bookkeeping --------------------------------------------------------- */

static tex_skipset g_set; /* too large for the stack */

static void test_skip_basic(void)
{
    char a[16], b[16], tex1[4], tex2[4];

    tex_skip_reset(&g_set);
    CHECK(tex_skip_take(&g_set, a, tex1) == 0); /* nothing refused: a real drop */

    /* refused once -> the next drop is not real, the one after is */
    CHECK(tex_skip_note(&g_set, a, tex1) == 1);
    CHECK_EQ_U(g_set.used, 1);
    CHECK(tex_skip_take(&g_set, b, tex1) == 0); /* other object */
    CHECK(tex_skip_take(&g_set, a, tex1) == 1);
    CHECK_EQ_U(g_set.used, 0);
    CHECK(tex_skip_take(&g_set, a, tex1) == 0);

    /* refused twice in a row -> two drops are not real */
    tex_skip_note(&g_set, a, tex1);
    tex_skip_note(&g_set, a, tex1);
    CHECK_EQ_U(g_set.used, 1);
    CHECK(tex_skip_take(&g_set, a, tex1) == 1);
    CHECK(tex_skip_take(&g_set, a, tex1) == 1);
    CHECK(tex_skip_take(&g_set, a, tex1) == 0);

    /* destroyed in between: forgotten */
    tex_skip_note(&g_set, a, tex1);
    tex_skip_forget(&g_set, a);
    CHECK_EQ_U(g_set.used, 0);
    CHECK(tex_skip_take(&g_set, a, tex1) == 0);
    tex_skip_forget(&g_set, a); /* harmless */

    /* same address, different texture: a new object lives there now */
    tex_skip_note(&g_set, a, tex1);
    CHECK(tex_skip_take(&g_set, a, tex2) == 0);
    CHECK_EQ_U(g_set.used, 0); /* and the stale entry is gone */
    tex_skip_note(&g_set, a, tex1);
    tex_skip_note(&g_set, a, tex2); /* refused again as the new object */
    CHECK(tex_skip_take(&g_set, a, tex2) == 1);
    CHECK(tex_skip_take(&g_set, a, tex2) == 0);

    CHECK(tex_skip_note(&g_set, NULL, tex1) == 0);
    CHECK(tex_skip_take(&g_set, NULL, tex1) == 0);
}

/* Many objects, removed in an order that forces entries to move. */
static void test_skip_many(void)
{
    enum { N = 3000 };
    static char objects[N][16];
    char tex[4];
    unsigned i, misses = 0, extra = 0;

    tex_skip_reset(&g_set);
    for (i = 0; i < N; ++i) {
        CHECK(tex_skip_note(&g_set, objects[i], tex) == 1);
    }
    CHECK_EQ_U(g_set.used, N);
    for (i = 0; i < N; i += 3) { /* every third goes away */
        tex_skip_forget(&g_set, objects[i]);
    }
    for (i = 0; i < N; ++i) {
        int was_refused = tex_skip_take(&g_set, objects[i], tex);
        if (i % 3 == 0) {
            extra += was_refused != 0;
        } else {
            misses += was_refused != 1;
        }
    }
    CHECK_EQ_U(misses, 0);
    CHECK_EQ_U(extra, 0);
    CHECK_EQ_U(g_set.used, 0);
}

static void test_skip_full(void)
{
    static char objects[TEX_SKIP_CAPACITY][16];
    char tex[4];
    unsigned i, stored = 0;

    tex_skip_reset(&g_set);
    for (i = 0; i < TEX_SKIP_CAPACITY; ++i) {
        stored += tex_skip_note(&g_set, objects[i], tex) != 0;
    }
    /* It stops taking new objects while lookups are still short ... */
    CHECK(stored < TEX_SKIP_CAPACITY);
    CHECK(stored >= TEX_SKIP_CAPACITY / 2);
    CHECK_EQ_U(g_set.used, stored);
    /* ... still counts for the ones it knows ... */
    CHECK(tex_skip_note(&g_set, objects[0], tex) == 1);
    CHECK(tex_skip_take(&g_set, objects[0], tex) == 1);
    CHECK(tex_skip_take(&g_set, objects[0], tex) == 1);
    /* ... and has room again afterwards. */
    CHECK(tex_skip_note(&g_set, objects[TEX_SKIP_CAPACITY - 1], tex) == 1);
}

/* ---- class signatures ---------------------------------------------------------------- */

static const unsigned char ADD_REL[] = {TEX_BLOB_ADD_RELEASE};
static const unsigned char DROP_REL[] = {TEX_BLOB_DROP_RELEASE};
static const unsigned char ADD_SIZE[] = {TEX_BLOB_ADD_SIZE_HEAD};
static const unsigned char DROP_SIZE[] = {TEX_BLOB_DROP_SIZE_HEAD};

#define TEXT_VA 0x10001000ul
#define DATA_VA 0x11F00000ul

static void test_find_methods(void)
{
    static unsigned char text[0x800];
    tex_class_site site;

    CHECK_EQ_U(sizeof(ADD_REL), TEX_BLOB_ADD_RELEASE_SIZE);
    CHECK_EQ_U(sizeof(DROP_REL), TEX_BLOB_DROP_RELEASE_SIZE);

    memset(text, 0xCC, sizeof(text));
    memset(&site, 0, sizeof(site));
    CHECK(tex_find_methods(text, sizeof(text), TEXT_VA, &site) == 0);

    /* the optimised build: drop precedes add, as in the game */
    memcpy(text + 0x100, DROP_REL, sizeof(DROP_REL));
    memcpy(text + 0x200, ADD_REL, sizeof(ADD_REL));
    CHECK(tex_find_methods(text, sizeof(text), TEXT_VA, &site) == 1);
    CHECK_STR(site.variant, "release");
    CHECK_EQ_U(site.add, TEXT_VA + 0x200);
    CHECK_EQ_U(site.drop, TEXT_VA + 0x100);
    CHECK_EQ_U(site.add_slot, 9);

    /* only one of the two: not safe to act on */
    memset(text + 0x100, 0xCC, sizeof(DROP_REL));
    CHECK(tex_find_methods(text, sizeof(text), TEXT_VA, &site) == -1);
    memcpy(text + 0x100, DROP_REL, sizeof(DROP_REL));

    /* a second copy of a method: ambiguous */
    memcpy(text + 0x400, ADD_REL, sizeof(ADD_REL));
    CHECK(tex_find_methods(text, sizeof(text), TEXT_VA, &site) == -1);

    /* the size-optimised build */
    memset(text, 0xCC, sizeof(text));
    memcpy(text + 0x40, ADD_SIZE, sizeof(ADD_SIZE));
    memcpy(text + 0x300, DROP_SIZE, sizeof(DROP_SIZE));
    CHECK(tex_find_methods(text, sizeof(text), TEXT_VA, &site) == 1);
    CHECK_STR(site.variant, "release-size");
    CHECK_EQ_U(site.add, TEXT_VA + 0x40);
    CHECK_EQ_U(site.drop, TEXT_VA + 0x300);
    CHECK_EQ_U(site.add_slot, 8); /* this build's class has one method less */

    /* both builds in one module cannot be right */
    memcpy(text + 0x500, DROP_REL, sizeof(DROP_REL));
    memcpy(text + 0x600, ADD_REL, sizeof(ADD_REL));
    CHECK(tex_find_methods(text, sizeof(text), TEXT_VA, &site) == -1);

    CHECK(tex_find_methods(NULL, 0, TEXT_VA, &site) == 0);
}

static void test_find_slots(void)
{
    static unsigned char data[0x200];
    tex_class_site site;
    unsigned long slot = 0;

    memset(data, 0, sizeof(data));
    site.variant = "release";
    site.add = TEXT_VA + 0x200;
    site.drop = TEXT_VA + 0x100;
    site.add_slot = 9;
    CHECK_EQ_U(tex_find_slots(data, sizeof(data), DATA_VA, &site, &slot), 0);

    /* a table at 0x40: add in its slot, drop in the next */
    put32(data + 0x40 + 4 * site.add_slot, site.add);
    put32(data + 0x40 + 4 * (site.add_slot + 1), site.drop);
    CHECK_EQ_U(tex_find_slots(data, sizeof(data), DATA_VA, &site, &slot), 1);
    CHECK_EQ_U(slot, DATA_VA + 0x40 + 4 * site.add_slot);
    CHECK_EQ_U(slot - 4 * site.add_slot, DATA_VA + 0x40); /* the table itself */

    /* add without drop behind it is something else */
    put32(data + 0x100, site.add);
    put32(data + 0x104, site.add);
    CHECK_EQ_U(tex_find_slots(data, sizeof(data), DATA_VA, &site, &slot), 1);

    /* a derived class would have a second table with the same pair */
    put32(data + 0x180, site.add);
    put32(data + 0x184, site.drop);
    slot = 0;
    CHECK_EQ_U(tex_find_slots(data, sizeof(data), DATA_VA, &site, &slot), 2);
    CHECK_EQ_U(slot, DATA_VA + 0x40 + 4 * site.add_slot); /* the first one */

    /* pointers are aligned: a pair at an odd address is not a table */
    memset(data, 0, sizeof(data));
    put32(data + 0x42, site.add);
    put32(data + 0x46, site.drop);
    CHECK_EQ_U(tex_find_slots(data, sizeof(data), DATA_VA, &site, &slot), 0);
    /* ... unless the section itself starts at an odd address */
    CHECK_EQ_U(tex_find_slots(data, sizeof(data), DATA_VA + 2, &site, &slot), 1);
    CHECK_EQ_U(slot, DATA_VA + 2 + 0x42);

    /* a pair that would run past the end of the section is not read */
    memset(data, 0, sizeof(data));
    put32(data + sizeof(data) - 4, site.add);
    CHECK_EQ_U(tex_find_slots(data, sizeof(data), DATA_VA, &site, &slot), 0);
}

static void test_destructor(void)
{
    static const unsigned char destroy[] = {TEX_BLOB_DESTROY};
    /* as in Far Cry 3 and in Blood Dragon (only the call target differs) */
    static const unsigned char fc3[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0xC5, 0x9C, 0x2F,
                                        0x01, 0xF6, 0x45, 0x08, 0x01, 0x74, 0x41, 0x85, 0xF6};
    static const unsigned char bd[] = {0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xE8, 0x58, 0xE5, 0xFF,
                                       0xFF, 0xF6, 0x45, 0x08, 0x01, 0x74, 0x07, 0x56, 0xE8};

    CHECK_EQ_U(sizeof(destroy), TEX_BLOB_DESTROY_SIZE);
    CHECK(tex_is_destructor(destroy, sizeof(destroy)));
    CHECK(tex_is_destructor(fc3, sizeof(fc3)));
    CHECK(tex_is_destructor(bd, sizeof(bd)));
    CHECK(!tex_is_destructor(fc3, 10));               /* cut off */
    CHECK(!tex_is_destructor(fc3 + 1, sizeof(fc3) - 1)); /* must start here */
    CHECK(!tex_is_destructor(ADD_REL, sizeof(ADD_REL)));
    CHECK(!tex_is_destructor(DROP_REL, sizeof(DROP_REL)));
    CHECK(!tex_is_destructor(NULL, 64));
}

/* ---- the top level resource class ---------------------------------------------------- */

static void test_top_level_methods(void)
{
    static const unsigned char load[] = {TOP_BLOB_LOAD};
    static const unsigned char unload[] = {TOP_BLOB_UNLOAD};
    static unsigned char text[2048], data[256];
    tl_site site;
    unsigned long slot = 0;

    CHECK_EQ_U(sizeof(load), TOP_BLOB_LOAD_SIZE);
    CHECK_EQ_U(sizeof(unload), TOP_BLOB_UNLOAD_SIZE);

    memset(text, 0xCC, sizeof(text));
    memset(&site, 0, sizeof(site));
    CHECK(tl_find_methods(text, sizeof(text), 0x10001000ul, &site) == 0);
    memcpy(text + 0x100, unload, sizeof(unload));
    CHECK(tl_find_methods(text, sizeof(text), 0x10001000ul, &site) == -1); /* one without the other */
    memcpy(text + 0x400, load, sizeof(load));
    CHECK(tl_find_methods(text, sizeof(text), 0x10001000ul, &site) == 1);
    CHECK_EQ_U(site.load, 0x10001400ul);
    CHECK_EQ_U(site.unload, 0x10001100ul);
    CHECK_STR(site.variant, "release");

    /* the jump distances are not part of what is recognised */
    text[0x100 + 0x0C] = 0x3F;
    text[0x100 + 0x16] = 0x2D;
    text[0x100 + 0x1E] = 0x24;
    CHECK(tl_find_methods(text, sizeof(text), 0x10001000ul, &site) == 1);
    /* another offset in the object is another method */
    text[0x100 + 6] = 0x38;
    CHECK(tl_find_methods(text, sizeof(text), 0x10001000ul, &site) == -1);
    text[0x100 + 6] = 0x34;
    /* two candidates: refuse to guess */
    memcpy(text + 0x600, load, sizeof(load));
    CHECK(tl_find_methods(text, sizeof(text), 0x10001000ul, &site) == -1);
    memset(text + 0x600, 0xCC, sizeof(load));
    CHECK(tl_find_methods(text, sizeof(text), 0x10001000ul, &site) == 1);
    CHECK(tl_find_methods(NULL, 16, 0, &site) == 0);
    CHECK(tl_find_methods(text, sizeof(text), 0, NULL) == 0);

    /* the class's table: Load directly followed by Unload */
    memset(data, 0, sizeof(data));
    CHECK_EQ_U(tl_find_slots(data, sizeof(data), 0x10200000ul, &site, &slot), 0);
    data[0x68] = 0x00, data[0x69] = 0x14, data[0x6A] = 0x00, data[0x6B] = 0x10; /* Load   */
    data[0x6C] = 0x00, data[0x6D] = 0x11, data[0x6E] = 0x00, data[0x6F] = 0x10; /* Unload */
    CHECK_EQ_U(tl_find_slots(data, sizeof(data), 0x10200000ul, &site, &slot), 1);
    CHECK_EQ_U(slot, 0x10200068ul);
    /* the two the other way round, or not on a pointer boundary, are something else */
    data[0x80] = 0x00, data[0x81] = 0x11, data[0x82] = 0x00, data[0x83] = 0x10;
    data[0x84] = 0x00, data[0x85] = 0x14, data[0x86] = 0x00, data[0x87] = 0x10;
    CHECK_EQ_U(tl_find_slots(data, sizeof(data), 0x10200000ul, &site, &slot), 1);
    CHECK_EQ_U(tl_find_slots(data + 1, sizeof(data) - 1, 0x10200001ul, &site, &slot), 1);
    CHECK_EQ_U(slot, 0x10200068ul);
    /* a second table holding the pair makes it two */
    memcpy(data + 0xA0, data + 0x68, 8);
    CHECK_EQ_U(tl_find_slots(data, sizeof(data), 0x10200000ul, &site, &slot), 2);
    CHECK_EQ_U(slot, 0x10200068ul);
    CHECK_EQ_U(tl_find_slots(NULL, 8, 0, &site, &slot), 0);
}

/* The stand-ins behave like the methods they stand in for: this is what the
 * runtime tests and the fixture rely on. */
typedef unsigned char(__fastcall *top_load_fn)(void *self, void *unused, const void *file, unsigned size);
typedef void(__fastcall *top_unload_fn)(void *self, void *unused);

static void test_top_level_blobs(void)
{
    static const unsigned char load[] = {TOP_BLOB_LOAD};
    static const unsigned char unload[] = {TOP_BLOB_UNLOAD};
    unsigned char *code = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    unsigned long resource[16], texture[24], record[12];

    CHECK(code != NULL);
    if (!code) {
        return;
    }
    memcpy(code, load, sizeof(load));
    memcpy(code + 128, unload, sizeof(unload));
    memset(resource, 0, sizeof(resource));
    memset(texture, 0, sizeof(texture));
    memset(record, 0, sizeof(record));
    resource[0x30 / 4] = (unsigned long)(size_t)texture;
    resource[0x34 / 4] = 1;
    resource[0x28 / 4] = 77;
    texture[0x30 / 4] = (unsigned long)(size_t)record;
    texture[0x48 / 4] = 1;
    record[0x20 / 4] = 5;

    CHECK(((top_load_fn)(void *)code)(resource, NULL, "file", 4) == 0);
    CHECK_EQ_U(resource[1], 1);
    CHECK_EQ_U(texture[0x28 / 4], 1);
    ((top_unload_fn)(void *)(code + 128))(resource, NULL);
    CHECK_EQ_U(resource[2], 1);
    CHECK_EQ_U(record[0x20 / 4], 4);
    CHECK_EQ_U(resource[0x28 / 4], 0);
    /* two users: the record is left alone */
    texture[0x48 / 4] = 2;
    ((top_unload_fn)(void *)(code + 128))(resource, NULL);
    CHECK_EQ_U(resource[2], 2);
    CHECK_EQ_U(record[0x20 / 4], 4);
    /* not valid: nothing at all */
    resource[0x34 / 4] = 0;
    resource[0x30 / 4] = 0;
    ((top_unload_fn)(void *)(code + 128))(resource, NULL);
    CHECK_EQ_U(resource[2], 2);
    VirtualFree(code, 0, MEM_RELEASE);
}

void suite_tex_rule(void)
{
    RUN(test_destructor);
    RUN(test_dxgi_formats);
    RUN(test_dds_headers);
    RUN(test_rule);
    RUN(test_skip_basic);
    RUN(test_skip_many);
    RUN(test_skip_full);
    RUN(test_find_methods);
    RUN(test_find_slots);
    RUN(test_top_level_methods);
    RUN(test_top_level_blobs);
}
