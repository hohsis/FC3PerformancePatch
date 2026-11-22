/* Texture guard for the DX11 renderer.
 *
 * What the engine does (derived from the game's own code, FC3_d3d11.dll):
 *
 *   resource layer, when a mip0 file has been read from disk
 *       queue "add top level" for the texture object
 *       levels += 1; width *= 2; height *= 2          <- whether or not it works
 *   resource layer, when the texture is no longer needed up close
 *       queue "drop top level" if levels > 1
 *       levels -= 1; width /= 2; height /= 2
 *
 *   texture object, AddTopLevel(dds, size, ...)
 *       desc = d3dTexture->GetDesc()
 *       new  = CreateTexture2D(2 * desc.Width, 2 * desc.Height, desc.MipLevels + 1)
 *       UpdateSubresource(new, level 0, pixels of the DDS image, its row pitch)
 *       CopySubresourceRegion(new, level n + 1  <-  old, level n)   for all n
 *   texture object, DropTopLevel()
 *       the reverse: a texture of half the size, levels copied down by one
 *
 * AddTopLevel never compares the DDS image with the texture it is building.
 * If the image is smaller than twice the current texture, UpdateSubresource
 * reads past the end of the loaded file data and the game dies with an access
 * violation inside the graphics driver.
 *
 * The guard wraps both methods. A top level that does not fit is not added,
 * and because the resource layer will still ask for a level to be dropped
 * later, that request is swallowed for the same object - otherwise the
 * texture would lose a real level on every load/unload cycle. The same is
 * done when the engine's own AddTopLevel fails (out of memory), which has the
 * same imbalance in the unmodified game.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "texguard.h"

#include <string.h>

#include "log.h"
#include "pe_image.h"
#include "tex_rule.h"

#define SLOT_TEXTURE2D_GET_DESC 10
#define MAX_DETAILED_LINES 8
#define MAX_SECTIONS 32

/* D3D11_TEXTURE2D_DESC, spelled out so no SDK header is needed. */
typedef struct tg_desc {
    UINT width, height, mip_levels, array_size, format;
    UINT sample_count, sample_quality;
    UINT usage, bind_flags, cpu_access, misc_flags;
} tg_desc;

/* The methods are __thiscall (object in ecx, callee pops the stack). A
 * __fastcall function with an unused second register argument has exactly
 * that shape and can be written in C with both compilers. */
typedef unsigned char(__fastcall *add_fn)(void *self, void *unused, const void *dds, unsigned size, void *buffer,
                                          unsigned flag);
typedef unsigned char(__fastcall *drop_fn)(void *self, void *unused);
typedef void *(__fastcall *destroy_fn)(void *self, void *unused, unsigned flags);
typedef void(__stdcall *get_desc_fn)(void *texture, tg_desc *desc);

static add_fn g_add;
static drop_fn g_drop;
static destroy_fn g_destroy;
static void **g_vtbl;
static unsigned g_slot_add;

static tex_skipset g_refused;
static SRWLOCK g_lock = SRWLOCK_INIT;

static volatile LONG g_added;         /* top levels the engine added */
static volatile LONG g_engine_failed; /* the engine's own add failed */
static volatile LONG g_left_out[5];   /* indexed by tex_verdict */
static volatile LONG g_drops_ignored;
static volatile LONG g_table_full;
static volatile LONG g_detailed;

static void *d3d_texture_of(void *self)
{
    return *(void **)((char *)self + TEX_OFFSET_D3D_TEXTURE);
}

static int describe(void *self, tg_desc *desc, void **texture)
{
    void *tex = d3d_texture_of(self);
    if (!tex) {
        return 0;
    }
    memset(desc, 0, sizeof(*desc));
    ((get_desc_fn)(*(void ***)tex)[SLOT_TEXTURE2D_GET_DESC])(tex, desc);
    *texture = tex;
    return 1;
}

/* The object did not get the level the resource layer now believes it has. */
static void remember_refusal(void *self, void *texture)
{
    int stored;

    AcquireSRWLockExclusive(&g_lock);
    stored = tex_skip_note(&g_refused, self, texture);
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored && InterlockedIncrement(&g_table_full) == 1) {
        LOGE("textures: too many textures without their top level to keep track of; "
             "some may lose detail when they are unloaded and reloaded");
    }
}

static void log_left_out(const tg_desc *desc, const tex_dds *seen, tex_verdict verdict)
{
    char have[24], got[24];
    LONG n = InterlockedIncrement(&g_detailed);

    if (n > MAX_DETAILED_LINES) {
        return;
    }
    LOGI("textures: left out a top mip level that does not fit its texture: the texture is %ux%u %s, so the level "
         "must be %ux%u, but the file holds %ux%u %s (%s)%s",
         desc->width, desc->height, tex_format_name(tex_format_from_dxgi(desc->format), have, sizeof(have)),
         desc->width * 2, desc->height * 2, seen->width, seen->height, tex_format_name(seen->format, got, sizeof(got)),
         tex_verdict_text(verdict),
         n == MAX_DETAILED_LINES ? "; further cases are only counted" : "");
}

static unsigned char __fastcall hook_add(void *self, void *unused, const void *dds, unsigned size, void *buffer,
                                         unsigned flag)
{
    tg_desc desc;
    tex_dds seen;
    void *texture = NULL;
    tex_verdict verdict = TEX_UNREADABLE;
    unsigned char ok;

    (void)unused;
    memset(&desc, 0, sizeof(desc));
    memset(&seen, 0, sizeof(seen));
    /* Cube maps and texture arrays never come this way in the game data; if
     * one does, it is the engine's business. */
    if (self && dds && describe(self, &desc, &texture) && desc.array_size == 1) {
        verdict = tex_top_level_verdict(desc.width, desc.height, desc.format, dds, size, &seen);
    }
    if (verdict != TEX_FITS && verdict != TEX_UNREADABLE) {
        InterlockedIncrement(&g_left_out[verdict]);
        log_left_out(&desc, &seen, verdict);
        remember_refusal(self, texture);
        return 0; /* the caller releases the file data either way */
    }
    ok = g_add(self, NULL, dds, size, buffer, flag);
    if (ok) {
        InterlockedIncrement(&g_added);
    } else if (self) {
        if (InterlockedIncrement(&g_engine_failed) == 1) {
            LOGI("textures: the engine could not add a top mip level to a texture (out of memory?); "
                 "the texture keeps the detail it has");
        }
        remember_refusal(self, d3d_texture_of(self));
    }
    return ok;
}

static unsigned char __fastcall hook_drop(void *self, void *unused)
{
    (void)unused;
    if (self && g_refused.used) {
        int refused;
        AcquireSRWLockExclusive(&g_lock);
        refused = tex_skip_take(&g_refused, self, d3d_texture_of(self));
        ReleaseSRWLockExclusive(&g_lock);
        if (refused) {
            InterlockedIncrement(&g_drops_ignored);
            return 0; /* nothing to drop: this object never got that level */
        }
    }
    return g_drop(self, NULL);
}

static void *__fastcall hook_destroy(void *self, void *unused, unsigned flags)
{
    (void)unused;
    if (self && g_refused.used) {
        AcquireSRWLockExclusive(&g_lock);
        tex_skip_forget(&g_refused, self);
        ReleaseSRWLockExclusive(&g_lock);
    }
    return g_destroy(self, NULL, flags);
}

static int is_code(const pei_section *s)
{
    return (s->characteristics & IMAGE_SCN_MEM_EXECUTE) && s->size;
}

static int is_data(const pei_section *s)
{
    return s->size && (s->characteristics & IMAGE_SCN_MEM_READ) &&
           !(s->characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE));
}

/* Non-zero when address lies in a code section; *avail = bytes up to its end. */
static int code_bytes_at(const pei_section *sec, unsigned nsec, const void *address, size_t *avail)
{
    const unsigned char *p = (const unsigned char *)address;
    unsigned i;

    for (i = 0; i < nsec; ++i) {
        if (is_code(&sec[i]) && p >= sec[i].start && p < sec[i].start + sec[i].size) {
            *avail = (size_t)(sec[i].start + sec[i].size - p);
            return 1;
        }
    }
    return 0;
}

int texguard_install(HMODULE engine, const char *name)
{
    pei_section sec[MAX_SECTIONS];
    tex_class_site site;
    unsigned nsec, i, slots = 0, slot_add, slot_drop;
    unsigned long slot_va = 0;
    size_t avail = 0;
    int found = 0;
    void **vtbl;
    void *hook;

    nsec = pei_sections(engine, sec, MAX_SECTIONS);
    if (nsec > MAX_SECTIONS) {
        nsec = MAX_SECTIONS;
    }
    /* Already installed in this module? (Its table then holds the wrappers,
     * so the search below would not find the original pair again.) */
    for (i = 0; i < nsec && g_vtbl; ++i) {
        const unsigned char *t = (const unsigned char *)g_vtbl;
        if (is_data(&sec[i]) && t >= sec[i].start && t + (g_slot_add + 2) * sizeof(void *) <= sec[i].start + sec[i].size &&
            g_vtbl[g_slot_add] == (void *)hook_add) {
            return 1;
        }
    }
    memset(&site, 0, sizeof(site));
    for (i = 0; i < nsec && found == 0; ++i) {
        if (is_code(&sec[i])) {
            found = tex_find_methods(sec[i].start, sec[i].size, (unsigned long)(size_t)sec[i].start, &site);
        }
    }
    if (found == 0) {
        LOGI("%s: texture guard not available: the code that streams top mip levels was not recognised "
             "(only the DX11 renderer is supported)",
             name);
        return 0;
    }
    for (i = 0; i < nsec && found > 0; ++i) {
        if (is_data(&sec[i])) {
            unsigned long first = 0;
            unsigned n = tex_find_slots(sec[i].start, sec[i].size, (unsigned long)(size_t)sec[i].start, &site, &first);
            if (n && !slots) {
                slot_va = first;
            }
            slots += n;
        }
    }
    if (found < 0 || slots != 1) {
        LOGE("%s: texture guard not installed: the texture class could not be identified with certainty "
             "(%s, %u table candidates)",
             name, found < 0 ? "ambiguous code match" : "code found", slots);
        return 0;
    }

    /* The table must start with the class's destructor: that confirms the
     * slot numbers, and the destructor is wrapped as well. */
    vtbl = (void **)(size_t)slot_va - site.add_slot;
    slot_add = site.add_slot;
    slot_drop = site.add_slot + 1;
    if (!code_bytes_at(sec, nsec, vtbl[TEX_SLOT_DESTROY], &avail) ||
        !tex_is_destructor((const unsigned char *)vtbl[TEX_SLOT_DESTROY], avail)) {
        LOGE("%s: texture guard not installed: the texture class table does not start with a destructor "
             "('%s' build variant)",
             name, site.variant);
        return 0;
    }
    g_add = (add_fn)vtbl[slot_add];
    g_drop = (drop_fn)vtbl[slot_drop];
    g_destroy = (destroy_fn)vtbl[TEX_SLOT_DESTROY];
    tex_skip_reset(&g_refused);

    /* The table is read-only data. Nothing of the engine has run yet, so the
     * three slots can be replaced one after the other. */
    hook = (void *)hook_destroy;
    if (mem_patch(&vtbl[TEX_SLOT_DESTROY], &hook, sizeof(hook)) != 0) {
        LOGE("%s: texture guard not installed: the texture class table could not be written", name);
        return 0;
    }
    hook = (void *)hook_drop;
    if (mem_patch(&vtbl[slot_drop], &hook, sizeof(hook)) != 0) {
        mem_patch(&vtbl[TEX_SLOT_DESTROY], &g_destroy, sizeof(g_destroy));
        LOGE("%s: texture guard not installed: the texture class table could not be written", name);
        return 0;
    }
    hook = (void *)hook_add;
    if (mem_patch(&vtbl[slot_add], &hook, sizeof(hook)) != 0) {
        mem_patch(&vtbl[slot_drop], &g_drop, sizeof(g_drop));
        mem_patch(&vtbl[TEX_SLOT_DESTROY], &g_destroy, sizeof(g_destroy));
        LOGE("%s: texture guard not installed: the texture class table could not be written", name);
        return 0;
    }
    g_vtbl = vtbl;
    g_slot_add = slot_add;
    LOGI("%s: texture guard active ('%s' build variant): top mip levels that do not fit their texture are left out "
         "instead of crashing the game",
         name, site.variant);
    return 1;
}

void texguard_log_stats(void)
{
    /* ("small" is a macro in the Windows headers.) */
    LONG n_small = g_left_out[TEX_TOO_SMALL], n_large = g_left_out[TEX_TOO_LARGE];
    LONG n_format = g_left_out[TEX_WRONG_FORMAT];

    if (!g_vtbl) {
        return;
    }
    LOGI("textures: %ld top mip levels added, %ld left out because they did not fit (%ld too small, %ld too large, "
         "%ld in a different pixel format), %ld not added by the engine itself; %ld matching unload requests ignored",
         (long)g_added, (long)(n_small + n_large + n_format), (long)n_small, (long)n_large, (long)n_format,
         (long)g_engine_failed, (long)g_drops_ignored);
}
