/* Top mip level files that more than one texture lays claim to. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "tex_link.h"

#include "sigscan.h"

/* Far Cry 3 (FC3.dll and FC3_d3d11.dll):
 *
 * Load:
 *     55                 push ebp
 *     8B EC              mov  ebp, esp
 *     83 EC 2C           sub  esp, 2Ch
 *     56 57              push esi / push edi
 *     8B 7D 0C           mov  edi, [ebp+0Ch]      ; size
 *     8B F1              mov  esi, ecx            ; this
 *     8B 4D 08           mov  ecx, [ebp+8]        ; file
 *     57                 push edi
 *     8D 45 D4           lea  eax, [ebp-2Ch]      ; -> parse the texture header
 *
 * Unload:
 *     55 8B EC 51        push ebp / mov ebp, esp / push ecx
 *     80 79 34 00        cmp  byte [ecx+34h], 0   ; valid?
 *     89 4D FC           mov  [ebp-4], ecx
 *     74 rel8            je   out
 *     53                 push ebx
 *     8B 59 30           mov  ebx, [ecx+30h]      ; the linked texture
 *     83 7B 48 01        cmp  dword [ebx+48h], 1
 *     77 rel8            ja   skip
 *     56                 push esi
 *     8B 73 30           mov  esi, [ebx+30h]      ; its record
 *     85 F6              test esi, esi
 *     74 rel8            je   ...                 ; else Drop(record)
 */
static const struct {
    const char *name;
    const char *load;
    const char *unload;
} VARIANTS[] = {
    {"release", "55 8B EC 83 EC 2C 56 57 8B 7D 0C 8B F1 8B 4D 08 57 8D 45 D4",
     "55 8B EC 51 80 79 34 00 89 4D FC 74 ?? 53 8B 59 30 83 7B 48 01 77 ?? 56 8B 73 30 85 F6 74"},
};

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

int tl_find_methods(const unsigned char *text, size_t size, unsigned long text_va, tl_site *out)
{
    size_t v;
    int found = 0;

    if (!text || !size || !out) {
        return 0;
    }
    for (v = 0; v < sizeof(VARIANTS) / sizeof(VARIANTS[0]); ++v) {
        sig_pattern load, unload;
        const unsigned char *load_at[2], *unload_at[2];
        size_t n_load, n_unload;

        if (sig_parse(&load, VARIANTS[v].load) != 0 || sig_parse(&unload, VARIANTS[v].unload) != 0) {
            continue;
        }
        n_load = sig_find_all(text, size, &load, load_at, 2);
        n_unload = sig_find_all(text, size, &unload, unload_at, 2);
        if (!n_load && !n_unload) {
            continue;
        }
        if (n_load != 1 || n_unload != 1 || found) {
            return -1;
        }
        out->variant = VARIANTS[v].name;
        out->load = text_va + (unsigned long)(load_at[0] - text);
        out->unload = text_va + (unsigned long)(unload_at[0] - text);
        found = 1;
    }
    return found;
}

unsigned tl_find_slots(const unsigned char *data, size_t size, unsigned long data_va, const tl_site *site,
                       unsigned long *slot_va)
{
    size_t off = (size_t)((4 - (data_va & 3)) & 3); /* pointers are aligned */
    unsigned count = 0;

    if (!data || !site) {
        return 0;
    }
    for (; off + 8 <= size; off += 4) {
        if (rd32(data + off) == site->load && rd32(data + off + 4) == site->unload) {
            if (!count && slot_va) {
                *slot_va = data_va + (unsigned long)off;
            }
            ++count;
        }
    }
    return count;
}
