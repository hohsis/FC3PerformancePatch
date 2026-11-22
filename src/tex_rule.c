/* Texture streaming: the "top mip level" rule, its bookkeeping and the
 * signatures that locate the engine's texture class. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tex_rule.h"

#include <stdio.h>
#include <string.h>

#include "sigscan.h"

/* ---- pixel formats ---------------------------------------------------------- */

static tex_format make_format(tex_kind kind, unsigned bits)
{
    tex_format f;
    f.kind = kind;
    f.bits = bits;
    return f;
}

tex_format tex_format_from_dxgi(unsigned v)
{
    /* DXGI_FORMAT values; every family is typeless, unorm, srgb/snorm ... */
    if (v >= 70 && v <= 72) {
        return make_format(TEX_KIND_BC1, 0);
    }
    if (v >= 73 && v <= 75) {
        return make_format(TEX_KIND_BC2, 0);
    }
    if (v >= 76 && v <= 78) {
        return make_format(TEX_KIND_BC3, 0);
    }
    if (v >= 79 && v <= 81) {
        return make_format(TEX_KIND_BC4, 0);
    }
    if (v >= 82 && v <= 84) {
        return make_format(TEX_KIND_BC5, 0);
    }
    if (v >= 94 && v <= 96) {
        return make_format(TEX_KIND_BC6, 0);
    }
    if (v >= 97 && v <= 99) {
        return make_format(TEX_KIND_BC7, 0);
    }
    if (v >= 1 && v <= 4) { /* R32G32B32A32 */
        return make_format(TEX_KIND_RAW, 128);
    }
    if (v >= 9 && v <= 14) { /* R16G16B16A16 */
        return make_format(TEX_KIND_RAW, 64);
    }
    if ((v >= 23 && v <= 47) || (v >= 87 && v <= 93)) {
        /* R10G10B10A2, R11G11B10, R8G8B8A8, R16G16, R32, D24S8; B8G8R8A8/X8 */
        return make_format(TEX_KIND_RAW, 32);
    }
    if ((v >= 48 && v <= 59) || v == 85 || v == 86) { /* R8G8, R16; B5G6R5, B5G5R5A1 */
        return make_format(TEX_KIND_RAW, 16);
    }
    if (v >= 60 && v <= 65) { /* R8, A8 */
        return make_format(TEX_KIND_RAW, 8);
    }
    return make_format(TEX_KIND_UNKNOWN, 0);
}

const char *tex_format_name(tex_format f, char *buf, size_t len)
{
    static const char *const names[] = {"unknown format", "BC1", "BC2", "BC3", "BC4", "BC5", "BC6", "BC7"};
    if (!buf || !len) {
        return "";
    }
    if (f.kind == TEX_KIND_RAW) {
        snprintf(buf, len, "%u-bit", f.bits);
    } else {
        snprintf(buf, len, "%s", names[f.kind <= TEX_KIND_BC7 ? f.kind : 0]);
    }
    buf[len - 1] = '\0';
    return buf;
}

static int same_format(tex_format a, tex_format b)
{
    return a.kind == b.kind && (a.kind != TEX_KIND_RAW || a.bits == b.bits);
}

/* ---- DDS images --------------------------------------------------------------- */

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

#define FOURCC(a, b, c, d) \
    ((unsigned long)(a) | ((unsigned long)(b) << 8) | ((unsigned long)(c) << 16) | ((unsigned long)(d) << 24))
#define DDPF_FOURCC 0x4ul

static tex_format format_from_fourcc(unsigned long cc)
{
    switch (cc) {
    case FOURCC('D', 'X', 'T', '1'):
        return make_format(TEX_KIND_BC1, 0);
    case FOURCC('D', 'X', 'T', '2'):
    case FOURCC('D', 'X', 'T', '3'):
        return make_format(TEX_KIND_BC2, 0);
    case FOURCC('D', 'X', 'T', '4'):
    case FOURCC('D', 'X', 'T', '5'):
        return make_format(TEX_KIND_BC3, 0);
    case FOURCC('A', 'T', 'I', '1'):
    case FOURCC('B', 'C', '4', 'U'):
    case FOURCC('B', 'C', '4', 'S'):
        return make_format(TEX_KIND_BC4, 0);
    case FOURCC('A', 'T', 'I', '2'):
    case FOURCC('B', 'C', '5', 'U'):
    case FOURCC('B', 'C', '5', 'S'):
        return make_format(TEX_KIND_BC5, 0);
    default:
        return make_format(TEX_KIND_UNKNOWN, 0);
    }
}

int tex_dds_parse(const void *data, size_t size, tex_dds *out)
{
    const unsigned char *p = (const unsigned char *)data;
    unsigned long pf_flags, fourcc, bits;

    if (!p || !out || size < TEX_DDS_HEADER_SIZE) {
        return -1;
    }
    /* "DDS ", then a 124-byte header whose pixel-format block is 32 bytes. */
    if (rd32(p) != FOURCC('D', 'D', 'S', ' ') || rd32(p + 4) != 124 || rd32(p + 76) != 32) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->height = (unsigned)rd32(p + 12);
    out->width = (unsigned)rd32(p + 16);
    out->mips = (unsigned)rd32(p + 28);
    if (!out->mips) {
        out->mips = 1;
    }
    if (!out->width || !out->height) {
        return -1;
    }
    pf_flags = rd32(p + 80);
    fourcc = rd32(p + 84);
    bits = rd32(p + 88);
    if (pf_flags & DDPF_FOURCC) {
        if (fourcc == FOURCC('D', 'X', '1', '0')) {
            if (size < TEX_DDS_HEADER_SIZE + 20) {
                return -1;
            }
            out->format = tex_format_from_dxgi((unsigned)rd32(p + TEX_DDS_HEADER_SIZE));
        } else {
            out->format = format_from_fourcc(fourcc);
        }
    } else if (bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128) {
        /* 24-bit images have no Direct3D 11 counterpart and would be converted
         * on load: not something to judge by the header alone. */
        out->format = make_format(TEX_KIND_RAW, (unsigned)bits);
    }
    return 0;
}

/* ---- the rule ------------------------------------------------------------------- */

tex_verdict tex_top_level_verdict(unsigned width, unsigned height, unsigned dxgi_format, const void *dds, size_t size,
                                  tex_dds *seen)
{
    tex_dds local;
    tex_format have;

    if (!seen) {
        seen = &local;
    }
    memset(seen, 0, sizeof(*seen));
    if (!width || !height || width > 0x8000u || height > 0x8000u || tex_dds_parse(dds, size, seen) != 0) {
        return TEX_UNREADABLE;
    }
    if (seen->width != width * 2 || seen->height != height * 2) {
        /* Direct3D copies as many rows and columns as the new level has. */
        return (seen->width < width * 2 || seen->height < height * 2) ? TEX_TOO_SMALL : TEX_TOO_LARGE;
    }
    have = tex_format_from_dxgi(dxgi_format);
    if (have.kind != TEX_KIND_UNKNOWN && seen->format.kind != TEX_KIND_UNKNOWN && !same_format(have, seen->format)) {
        return TEX_WRONG_FORMAT;
    }
    return TEX_FITS;
}

const char *tex_verdict_text(tex_verdict v)
{
    switch (v) {
    case TEX_FITS:
        return "fits";
    case TEX_TOO_SMALL:
        return "too small";
    case TEX_TOO_LARGE:
        return "too large";
    case TEX_WRONG_FORMAT:
        return "different pixel format";
    case TEX_STALE:
        return "made for the texture a mod replaced";
    default:
        return "not readable";
    }
}

/* ---- objects that were refused a level ------------------------------------------- */

#define SKIP_MASK (TEX_SKIP_CAPACITY - 1u)
#define SKIP_LIMIT (TEX_SKIP_CAPACITY / 4u * 3u)

static unsigned skip_home(const void *key)
{
    /* Objects are at least 16-byte aligned; Knuth's multiplicative hash. */
    unsigned long v = (unsigned long)((size_t)key >> 4);
    return (unsigned)((v * 2654435761ul) >> 7) & SKIP_MASK;
}

/* Index of key, or of the free slot where it would go. */
static unsigned skip_find(const tex_skipset *s, const void *key)
{
    unsigned i = skip_home(key);
    while (s->e[i].key && s->e[i].key != key) {
        i = (i + 1) & SKIP_MASK;
    }
    return i;
}

static void skip_remove(tex_skipset *s, unsigned i)
{
    unsigned j = i;
    for (;;) {
        unsigned home;
        j = (j + 1) & SKIP_MASK;
        if (!s->e[j].key) {
            break;
        }
        home = skip_home(s->e[j].key);
        /* An entry may stay where it is if its home lies in (i, j]. */
        if (i <= j ? (i < home && home <= j) : (i < home || home <= j)) {
            continue;
        }
        s->e[i] = s->e[j];
        i = j;
    }
    s->e[i].key = NULL;
    s->e[i].tag = NULL;
    s->e[i].count = 0;
    s->used--;
}

void tex_skip_reset(tex_skipset *s)
{
    memset(s, 0, sizeof(*s));
}

int tex_skip_note(tex_skipset *s, const void *key, const void *tag)
{
    unsigned i;

    if (!key) {
        return 0;
    }
    i = skip_find(s, key);
    if (s->e[i].key) {
        if (s->e[i].tag == tag) {
            s->e[i].count++;
        } else {
            s->e[i].tag = tag; /* the address was reused without us noticing */
            s->e[i].count = 1;
        }
        return 1;
    }
    if (s->used >= SKIP_LIMIT) {
        return 0;
    }
    s->e[i].key = key;
    s->e[i].tag = tag;
    s->e[i].count = 1;
    s->used++;
    return 1;
}

int tex_skip_take(tex_skipset *s, const void *key, const void *tag)
{
    unsigned i;

    if (!key || !s->used) {
        return 0;
    }
    i = skip_find(s, key);
    if (!s->e[i].key) {
        return 0;
    }
    if (s->e[i].tag != tag) {
        skip_remove(s, i); /* stale: a different object lives here now */
        return 0;
    }
    if (--s->e[i].count == 0) {
        skip_remove(s, i);
    }
    return 1;
}

void tex_skip_forget(tex_skipset *s, const void *key)
{
    unsigned i;

    if (!key || !s->used) {
        return;
    }
    i = skip_find(s, key);
    if (s->e[i].key) {
        skip_remove(s, i);
    }
}

/* ---- locating the texture class ------------------------------------------------------
 * Both methods start by asking the Direct3D texture at [this+1Ch] for its
 * description (virtual call through offset 28h, ID3D11Texture2D::GetDesc).
 *
 * "release" (Far Cry 3, FC3_d3d11.dll), AddTopLevel:
 *     55                    push ebp
 *     8B EC                 mov  ebp, esp
 *     83 E4 F0              and  esp, -16
 *     81 EC imm32           sub  esp, imm32
 *     53 56                 push ebx / push esi
 *     8B D9                 mov  ebx, ecx              ; this
 *     8B 43 1C              mov  eax, [ebx+1Ch]        ; Direct3D texture
 *     8B 08                 mov  ecx, [eax]
 *     57                    push edi
 *     8D 94 24 disp32       lea  edx, [esp+disp32]     ; description
 *     52 50                 push edx / push eax
 *     8B 41 28              mov  eax, [ecx+28h]
 *     FF D0                 call eax                   ; GetDesc
 *     8B 8C 24 disp32       mov  ecx, [esp+disp32]     ; width
 *     66 0F 6F 84 24 ...    movdqa xmm0, [esp+disp32]
 * DropTopLevel:
 *     55 8B EC 83 EC imm8 53 57 8B F9 8B 47 1C 8B 08 8D 55 disp8 52 50 8B 41 28 FF D0
 *     BB 01 00 00 00        mov  ebx, 1
 *     39 5D disp8           cmp  [ebp+disp8], ebx      ; only one level left?
 *     75 08 5F 32 C0 5B 8B E5 5D C3                    ; then return false
 *     8B 45 disp8 83 F8 08  mov eax, [ebp+disp8] / cmp eax, 8
 *
 * "release-size" (Blood Dragon, FC3_d3d11.dll): the same two methods from a
 * size-optimised build (call through memory, rep movsd for the description).
 */
static const char REL_ADD[] = "55 8B EC 83 E4 F0 81 EC ?? ?? 00 00 53 56 8B D9 8B 43 1C 8B 08 57 8D 94 24 ?? ?? 00 00 "
                              "52 50 8B 41 28 FF D0 8B 8C 24 ?? ?? 00 00 66 0F 6F 84 24";
static const char REL_DROP[] = "55 8B EC 83 EC ?? 53 57 8B F9 8B 47 1C 8B 08 8D 55 ?? 52 50 8B 41 28 FF D0 "
                               "BB 01 00 00 00 39 5D ?? 75 08 5F 32 C0 5B 8B E5 5D C3 8B 45 ?? 83 F8 08";
static const char SIZE_ADD[] = "55 8B EC 81 EC ?? ?? 00 00 53 56 8B D9 8B 43 1C 8B 08 57 8D 55 ?? 52 50 89 5D FC "
                               "FF 51 28 8B 45 ?? 6A 0B 03 C0 59 8D 75 ?? 8D 7D ?? F3 A5";
static const char SIZE_DROP[] = "55 8B EC 83 EC ?? 53 56 57 8B D9 8B 43 1C 8B 08 8D 55 ?? 52 50 FF 51 28 "
                                "33 D2 42 39 55 ?? 0F 84 ?? ?? ?? ?? 8B 45 ?? 83 F8 08";

/* The destructor every class table starts with, identical in both builds:
 *     55 8B EC              push ebp / mov ebp, esp
 *     56                    push esi
 *     8B F1                 mov  esi, ecx
 *     E8 rel32              call ~Texture
 *     F6 45 08 01           test byte [ebp+8], 1
 *     74 ..                 jz   (do not free)
 */
static const char DESTRUCTOR[] = "55 8B EC 56 8B F1 E8 ?? ?? ?? ?? F6 45 08 01 74";

typedef struct tex_variant {
    const char *name;
    const char *add;
    const char *drop;
    unsigned add_slot;
} tex_variant;

static const tex_variant TEX_VARIANTS[] = {
    {"release", REL_ADD, REL_DROP, 9},
    {"release-size", SIZE_ADD, SIZE_DROP, 8},
};

int tex_find_methods(const unsigned char *text, size_t size, unsigned long text_va, tex_class_site *out)
{
    size_t v;
    int found = 0;

    if (!text || !size || !out) {
        return 0;
    }
    for (v = 0; v < sizeof(TEX_VARIANTS) / sizeof(TEX_VARIANTS[0]); ++v) {
        sig_pattern add, drop;
        const unsigned char *add_at[2], *drop_at[2];
        size_t n_add, n_drop;

        if (sig_parse(&add, TEX_VARIANTS[v].add) != 0 || sig_parse(&drop, TEX_VARIANTS[v].drop) != 0) {
            continue;
        }
        n_add = sig_find_all(text, size, &add, add_at, 2);
        n_drop = sig_find_all(text, size, &drop, drop_at, 2);
        if (!n_add && !n_drop) {
            continue;
        }
        if (n_add != 1 || n_drop != 1 || found) {
            return -1;
        }
        out->variant = TEX_VARIANTS[v].name;
        out->add = text_va + (unsigned long)(add_at[0] - text);
        out->drop = text_va + (unsigned long)(drop_at[0] - text);
        out->add_slot = TEX_VARIANTS[v].add_slot;
        found = 1;
    }
    return found;
}

int tex_is_destructor(const unsigned char *code, size_t size)
{
    sig_pattern sig;

    if (!code || sig_parse(&sig, DESTRUCTOR) != 0 || size < sig.len) {
        return 0;
    }
    return sig_find(code, sig.len, &sig) == code;
}

unsigned tex_find_slots(const unsigned char *data, size_t size, unsigned long data_va, const tex_class_site *site,
                        unsigned long *slot_va)
{
    size_t off = (size_t)((4 - (data_va & 3)) & 3); /* pointers are aligned */
    unsigned count = 0;

    if (!data || !site) {
        return 0;
    }
    for (; off + 8 <= size; off += 4) {
        if (rd32(data + off) == site->add && rd32(data + off + 4) == site->drop) {
            if (!count && slot_va) {
                *slot_va = data_va + (unsigned long)off;
            }
            ++count;
        }
    }
    return count;
}
