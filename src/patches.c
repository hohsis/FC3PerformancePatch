/* Engine code patches located by signature.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "patches.h"

#include <string.h>

#include "sigscan.h"

#define MAX_SITES 8
#define MAX_BACK_JUMP 0x1000

static long rd_rel32(const unsigned char *p)
{
    unsigned long v = (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
                      ((unsigned long)p[3] << 24);
    return (long)(int)v;
}

static void wr_rel32(unsigned char *p, long v)
{
    unsigned long u = (unsigned long)v;
    p[0] = (unsigned char)(u & 0xFF);
    p[1] = (unsigned char)((u >> 8) & 0xFF);
    p[2] = (unsigned char)((u >> 16) & 0xFF);
    p[3] = (unsigned char)((u >> 24) & 0xFF);
}

/* Returns the loop head if the backward branch target is plausible. */
static const unsigned char *loop_head(const unsigned char *text, size_t size, const unsigned char *next_insn,
                                      long rel)
{
    const unsigned char *target = next_insn + rel;
    if (rel >= 0 || rel < -MAX_BACK_JUMP) {
        return NULL;
    }
    if (target < text || target + 4 > text + size) {
        return NULL;
    }
    return target;
}

/* ---------------------------------------------------------------------------
 * Variant "release": optimised MSVC build (FC3.dll / FC3_d3d11.dll)
 *
 *   +0  41                 inc  ecx
 *   +1  BE 01 00 00 00     mov  esi, 1
 *   +6  D3 E6              shl  esi, cl
 *   +8  89 4D dI           mov  [ebp+dI], ecx        ; i
 *   +11 85 F6              test esi, esi
 *   +13 0F 85 rel32        jnz  head                 ; head: cmp esi,[ebp+..] / ja exit
 *
 * A known third-party fix replaces the jnz with "EB 04" (leaving the rel32
 * bytes in place), which makes the loop run exactly once. That form is also
 * recognised and upgraded.
 *
 * Replacement (same 19 bytes):
 *
 *   +0  41                 inc  ecx
 *   +1  89 4D dI           mov  [ebp+dI], ecx
 *   +4  33 F6              xor  esi, esi
 *   +6  0F AB CE           bts  esi, ecx             ; esi = 1 << (ecx & 31)
 *   +9  83 F9 20           cmp  ecx, 32
 *   +12 0F 82 rel32'       jb   head                 ; stop after bit 31
 *   +18 90                 nop
 * ------------------------------------------------------------------------- */
#define REL_LEN 19
static const char REL_SIG[] = "41 BE 01 00 00 00 D3 E6 89 4D ?? 85 F6 ?? ?? ?? ?? ?? ??";
static const char REL_DONE[] = "41 89 4D ?? 33 F6 0F AB CE 83 F9 20 0F 82 ?? ?? ?? ?? 90";

static int rel_build(const unsigned char *text, size_t size, const unsigned char *site, unsigned char *out)
{
    const unsigned char *head;
    long rel;
    int is_jnz = site[13] == 0x0F && site[14] == 0x85;
    int is_foreign = site[13] == 0xEB && site[14] == 0x04;

    if (!is_jnz && !is_foreign) {
        return -1;
    }
    rel = rd_rel32(site + 15);
    head = loop_head(text, size, site + REL_LEN, rel);
    if (!head || head[0] != 0x3B || head[1] != 0x75) { /* cmp esi, [ebp+disp8] */
        return -1;
    }
    out[0] = 0x41;
    out[1] = 0x89;
    out[2] = 0x4D;
    out[3] = site[10];
    out[4] = 0x33;
    out[5] = 0xF6;
    out[6] = 0x0F;
    out[7] = 0xAB;
    out[8] = 0xCE;
    out[9] = 0x83;
    out[10] = 0xF9;
    out[11] = 0x20;
    out[12] = 0x0F;
    out[13] = 0x82;
    wr_rel32(out + 14, rel + 1); /* the branch now ends one byte earlier */
    out[18] = 0x90;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Variant "debug": unoptimised MSVC build (systemdetection.dll)
 *
 *   +0  8B 45 dI           mov  eax, [ebp+dI]        ; i
 *   +3  83 C0 01           add  eax, 1
 *   +6  89 45 dI           mov  [ebp+dI], eax
 *   +9  B8 01 00 00 00     mov  eax, 1
 *   +14 8B 4D dI           mov  ecx, [ebp+dI]
 *   +17 D3 E0              shl  eax, cl
 *   +19 89 45 dM           mov  [ebp+dM], eax        ; mask
 *   +22 E9 rel32           jmp  head                 ; head: cmp dword [ebp+dM], 0 / je exit
 *
 * Replacement (same 27 bytes): the mask becomes 0 once i reaches 32, which the
 * existing "mask != 0" test at the loop head turns into a normal exit.
 *
 *   +0  8B 4D dI           mov  ecx, [ebp+dI]
 *   +3  41                 inc  ecx
 *   +4  89 4D dI           mov  [ebp+dI], ecx
 *   +7  33 C0              xor  eax, eax
 *   +9  83 F9 20           cmp  ecx, 32
 *   +12 73 03              jae  +3
 *   +14 0F AB C8           bts  eax, ecx
 *   +17 89 45 dM           mov  [ebp+dM], eax
 *   +20 E9 rel32'          jmp  head
 *   +25 90 90              nop; nop
 * ------------------------------------------------------------------------- */
#define DBG_LEN 27
static const char DBG_SIG[] =
    "8B 45 ?? 83 C0 01 89 45 ?? B8 01 00 00 00 8B 4D ?? D3 E0 89 45 ?? E9 ?? ?? ?? ??";
static const char DBG_DONE[] =
    "8B 4D ?? 41 89 4D ?? 33 C0 83 F9 20 73 03 0F AB C8 89 45 ?? E9 ?? ?? ?? ?? 90 90";

static int dbg_build(const unsigned char *text, size_t size, const unsigned char *site, unsigned char *out)
{
    const unsigned char *head;
    long rel;
    unsigned char di = site[2];
    unsigned char dm = site[21];

    if (site[8] != di || site[16] != di) {
        return -1;
    }
    rel = rd_rel32(site + 23);
    if (rel == 0) {
        return 1; /* third-party fix: "jmp +0" drops out of the loop after one pass */
    }
    head = loop_head(text, size, site + DBG_LEN, rel);
    /* cmp dword [ebp+dM], 0 */
    if (!head || head[0] != 0x83 || head[1] != 0x7D || head[2] != dm || head[3] != 0x00) {
        return -1;
    }
    out[0] = 0x8B;
    out[1] = 0x4D;
    out[2] = di;
    out[3] = 0x41;
    out[4] = 0x89;
    out[5] = 0x4D;
    out[6] = di;
    out[7] = 0x33;
    out[8] = 0xC0;
    out[9] = 0x83;
    out[10] = 0xF9;
    out[11] = 0x20;
    out[12] = 0x73;
    out[13] = 0x03;
    out[14] = 0x0F;
    out[15] = 0xAB;
    out[16] = 0xC8;
    out[17] = 0x89;
    out[18] = 0x45;
    out[19] = dm;
    out[20] = 0xE9;
    wr_rel32(out + 21, rel + 2); /* the jump now ends two bytes earlier */
    out[25] = 0x90;
    out[26] = 0x90;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Variant "release-disp32": optimised build with 32-bit stack displacements
 * (Blood Dragon systemdetection.dll)
 *
 *   +0  41                 inc  ecx
 *   +1  BE 01 00 00 00     mov  esi, 1
 *   +6  D3 E6              shl  esi, cl
 *   +8  89 8D d32          mov  [ebp+d32], ecx       ; i
 *   +14 85 F6              test esi, esi
 *   +16 0F 85 rel32        jnz  head
 *
 * Replacement (same 22 bytes):
 *
 *   +0  41                 inc  ecx
 *   +1  89 8D d32          mov  [ebp+d32], ecx
 *   +7  33 F6              xor  esi, esi
 *   +9  0F AB CE           bts  esi, ecx
 *   +12 83 F9 20           cmp  ecx, 32
 *   +15 0F 82 rel32'       jb   head
 *   +21 90                 nop
 * ------------------------------------------------------------------------- */
#define R32_LEN 22
static const char R32_SIG[] = "41 BE 01 00 00 00 D3 E6 89 8D ?? ?? ?? ?? 85 F6 0F 85 ?? ?? ?? ??";
static const char R32_DONE[] = "41 89 8D ?? ?? ?? ?? 33 F6 0F AB CE 83 F9 20 0F 82 ?? ?? ?? ?? 90";

static int is_cmp_esi_stack(const unsigned char *head)
{
    /* cmp esi, [ebp+disp8] or cmp esi, [ebp+disp32] */
    return head[0] == 0x3B && (head[1] == 0x75 || head[1] == 0xB5);
}

static int r32_build(const unsigned char *text, size_t size, const unsigned char *site, unsigned char *out)
{
    long rel = rd_rel32(site + 18);
    const unsigned char *head = loop_head(text, size, site + R32_LEN, rel);

    if (!head || !is_cmp_esi_stack(head)) {
        return -1;
    }
    out[0] = 0x41;
    out[1] = 0x89;
    out[2] = 0x8D;
    memcpy(out + 3, site + 10, 4);
    out[7] = 0x33;
    out[8] = 0xF6;
    out[9] = 0x0F;
    out[10] = 0xAB;
    out[11] = 0xCE;
    out[12] = 0x83;
    out[13] = 0xF9;
    out[14] = 0x20;
    out[15] = 0x0F;
    out[16] = 0x82;
    wr_rel32(out + 17, rel + 1);
    out[21] = 0x90;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Variant "release-ebx": size-optimised build that keeps the constant 1 in
 * ebx (Blood Dragon FC3.dll / FC3_d3d11.dll)
 *
 *   +0  FF 45 dI           inc  dword [ebp+dI]       ; ++i
 *   +3  8B 4D dI           mov  ecx, [ebp+dI]
 *   +6  8B F3              mov  esi, ebx             ; ebx == 1
 *   +8  D3 E6              shl  esi, cl
 *   +10 85 F6              test esi, esi
 *   +12 0F 85 rel32        jnz  head
 *
 * i is at least 1 here, so "1 << i" can only equal 1 again when the shift
 * count has wrapped (i == 32). Comparing against ebx instead of zero is
 * therefore exactly the missing termination test, in the same two bytes:
 *
 *   +10 3B F3              cmp  esi, ebx
 * ------------------------------------------------------------------------- */
#define EBX_LEN 18
static const char EBX_SIG[] = "FF 45 ?? 8B 4D ?? 8B F3 D3 E6 85 F6 0F 85 ?? ?? ?? ??";
static const char EBX_DONE[] = "FF 45 ?? 8B 4D ?? 8B F3 D3 E6 3B F3 0F 85 ?? ?? ?? ??";

static int ebx_build(const unsigned char *text, size_t size, const unsigned char *site, unsigned char *out)
{
    long rel = rd_rel32(site + 14);
    const unsigned char *head = loop_head(text, size, site + EBX_LEN, rel);

    if (site[2] != site[5] || !head || !is_cmp_esi_stack(head)) {
        return -1;
    }
    memcpy(out, site, EBX_LEN);
    out[10] = 0x3B;
    out[11] = 0xF3;
    return 0;
}

typedef int (*build_fn)(const unsigned char *text, size_t size, const unsigned char *site, unsigned char *out);

typedef struct variant {
    const char *name;
    const char *sig;
    const char *done_sig;
    size_t len;
    build_fn build; /* 0 = ok, 1 = third-party fix present, -1 = reject */
} variant;

static const variant VARIANTS[] = {
    {"release", REL_SIG, REL_DONE, REL_LEN, rel_build},
    {"debug", DBG_SIG, DBG_DONE, DBG_LEN, dbg_build},
    {"release-disp32", R32_SIG, R32_DONE, R32_LEN, r32_build},
    {"release-ebx", EBX_SIG, EBX_DONE, EBX_LEN, ebx_build},
};

unsigned patch_enum_loop(unsigned char *text, size_t size, patch_write_fn write, void *ctx, patch_report *report)
{
    patch_report local;
    size_t v;

    if (!report) {
        report = &local;
    }
    memset(report, 0, sizeof(*report));
    if (!text || !size) {
        return 0;
    }

    for (v = 0; v < sizeof(VARIANTS) / sizeof(VARIANTS[0]); ++v) {
        const variant *var = &VARIANTS[v];
        sig_pattern sig;
        const unsigned char *sites[MAX_SITES];
        size_t n, i;

        if (sig_parse(&sig, var->done_sig) == 0) {
            n = sig_find_all(text, size, &sig, sites, MAX_SITES);
            if (n) {
                report->already += (unsigned)n;
                report->variant = var->name;
                report->site = sites[0];
            }
        }
        if (sig_parse(&sig, var->sig) != 0) {
            continue;
        }
        n = sig_find_all(text, size, &sig, sites, MAX_SITES);
        if (n > MAX_SITES) {
            /* Far more hits than the one or two real loops: the signature is
             * not selective in this binary. Refuse to touch anything. */
            report->rejected += (unsigned)n;
            continue;
        }
        for (i = 0; i < n; ++i) {
            unsigned char repl[SIG_MAX_LEN];
            int rc = var->build(text, size, sites[i], repl);
            if (rc < 0) {
                report->rejected++;
                continue;
            }
            report->variant = var->name;
            report->site = sites[i];
            if (rc > 0) {
                report->foreign++;
                continue;
            }
            if (write) {
                if (write(ctx, (unsigned char *)sites[i], repl, var->len) != 0) {
                    report->rejected++;
                    continue;
                }
            }
            report->applied++;
        }
    }
    return report->applied + report->already;
}
