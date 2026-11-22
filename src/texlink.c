/* Top mip level files that more than one texture names: the part that touches
 * the game.
 *
 * Two methods of the engine's "top level resource" class are wrapped in its
 * virtual table (tex_link.h):
 *
 *   Load(file, size)   For a file on the list of shared ones (found by the
 *                      archive scan, tex_scan.h) the engine's method is not
 *                      called; the wrapper answers "not added", which is what
 *                      the method itself answers when top levels are switched
 *                      off, so its callers know the case.
 *   Unload()           Not called for such a file either: there is nothing to
 *                      take back, and the link it would follow is exactly
 *                      what cannot be trusted.
 *
 * A file is recognised by the name the resource carries and, independently,
 * by its content when it is loaded; resources refused by content are
 * remembered until they are unloaded or loaded again.
 *
 * Unload gets one more check for all other files: if the record it is about
 * to write to is not there any more (no memory at that address), the write
 * is left out instead of crashing the game. That cannot happen with the
 * game's own data; it is the last line of defence against a case the list
 * does not cover, and it says so in the log.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "texlink.h"

#include <string.h>

#include "log.h"
#include "pe_image.h"
#include "tex_link.h"
#include "tex_stale.h"

#define MAX_SECTIONS 32
#define MAX_REMEMBERED 512

/* __thiscall methods, written as __fastcall with a spare register argument. */
typedef unsigned char(__fastcall *load_fn)(void *self, void *unused, const void *file, unsigned size);
typedef void(__fastcall *unload_fn)(void *self, void *unused);

static load_fn g_load;
static unload_fn g_unload;
static void **g_slots; /* the table's Load slot; Unload is the next */

static volatile LONG g_refused, g_by_name, g_by_content, g_skipped_unloads, g_gone, g_logged, g_gone_logged;

/* Resources whose file was refused because of its content. */
static void *g_remembered[MAX_REMEMBERED];
static SRWLOCK g_lock = SRWLOCK_INIT;

static void remember(void *self, int on)
{
    unsigned i, free_slot = MAX_REMEMBERED;

    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < MAX_REMEMBERED; ++i) {
        if (g_remembered[i] == self) {
            if (!on) {
                g_remembered[i] = NULL;
            }
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }
        if (!g_remembered[i] && free_slot == MAX_REMEMBERED) {
            free_slot = i;
        }
    }
    if (on && free_slot < MAX_REMEMBERED) {
        g_remembered[free_slot] = self;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* Is self remembered? Forgets it. */
static int take_remembered(void *self)
{
    unsigned i;
    int found = 0;

    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < MAX_REMEMBERED; ++i) {
        if (g_remembered[i] == self) {
            g_remembered[i] = NULL;
            found = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

static int shared_by_name(const void *self)
{
    unsigned long lo, hi;

    memcpy(&lo, (const unsigned char *)self + TL_OFFSET_NAME, 4);
    memcpy(&hi, (const unsigned char *)self + TL_OFFSET_NAME + 4, 4);
    return tex_stale_shared_name(((unsigned long long)hi << 32) | lo);
}

static void say_once(void)
{
    if (InterlockedExchange(&g_logged, 1) == 0) {
        LOGI("textures: left a top mip level file unloaded because more than one texture names it (the game would "
             "link it to one of them and crash or corrupt memory when that texture goes away); further ones are "
             "counted");
    }
}

static unsigned char __fastcall hook_load(void *self, void *unused, const void *file, unsigned size)
{
    int by_name = shared_by_name(self);
    int by_content = !by_name && tex_stale_shared_file(file, size);

    if (by_name || by_content) {
        InterlockedIncrement(&g_refused);
        InterlockedIncrement(by_name ? &g_by_name : &g_by_content);
        if (by_content) {
            remember(self, 1);
        }
        say_once();
        return 1; /* "not added" */
    }
    remember(self, 0); /* the address of a resource that has gone may be used again */
    return g_load(self, unused, file, size);
}

/* Is there memory at p that can be read (and written)? */
static int present(const void *p, size_t len, int writable)
{
    MEMORY_BASIC_INFORMATION info;
    const DWORD rw = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    const DWORD ro = PAGE_READONLY | PAGE_EXECUTE_READ;

    if (!p || VirtualQuery(p, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(info.Protect & (writable ? rw : rw | ro))) {
        return 0;
    }
    return (size_t)((const unsigned char *)info.BaseAddress + info.RegionSize - (const unsigned char *)p) >= len;
}

static void __fastcall hook_unload(void *self, void *unused)
{
    unsigned char *res = (unsigned char *)self;
    int remembered = take_remembered(self);

    if (remembered || shared_by_name(self)) {
        InterlockedIncrement(&g_skipped_unloads);
        return;
    }
    /* What the engine's method is about to do: follow the link to the texture
     * and write to that texture's record. */
    if (res[TL_OFFSET_VALID]) {
        const unsigned char *texture;
        memcpy(&texture, res + TL_OFFSET_LINK, sizeof(texture));
        if (!present(texture, TL_TEXTURE_BYTES, 0)) {
            texture = NULL;
        } else {
            unsigned long users;
            const unsigned char *record;
            memcpy(&users, texture + TL_TEXTURE_USERS, 4);
            memcpy(&record, texture + TL_TEXTURE_RECORD, sizeof(record));
            if (users >= 2 || !record || present(record, TL_RECORD_BYTES, 1)) {
                g_unload(self, unused);
                return;
            }
        }
        /* The texture or its record is gone. Do what is left of Unload. */
        memset(res + TL_OFFSET_SIZE, 0, 4);
        InterlockedIncrement(&g_gone);
        if (InterlockedExchange(&g_gone_logged, 1) == 0) {
            LOGE("textures: a top mip level was about to be taken from a texture that no longer exists (%s); "
                 "left out instead of crashing. Please report this with the log",
                 texture ? "its record is gone" : "the texture is gone");
        }
        return;
    }
    g_unload(self, unused);
}

static int is_code(const pei_section *s)
{
    return s->size && (s->characteristics & IMAGE_SCN_MEM_EXECUTE);
}

static int is_data(const pei_section *s)
{
    return s->size && (s->characteristics & IMAGE_SCN_MEM_READ) &&
           !(s->characteristics & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_DISCARDABLE));
}

int texlink_install(HMODULE engine, const char *name)
{
    pei_section sec[MAX_SECTIONS];
    tl_site site;
    unsigned nsec, i, slots = 0;
    unsigned long slot_va = 0;
    int found = 0;
    void **table;
    void *hook;

    nsec = pei_sections(engine, sec, MAX_SECTIONS);
    if (nsec > MAX_SECTIONS) {
        nsec = MAX_SECTIONS;
    }
    /* Already installed in this module? (Its table then holds the wrappers,
     * so the search below would not find the original pair again.) */
    for (i = 0; i < nsec && g_slots; ++i) {
        const unsigned char *t = (const unsigned char *)g_slots;
        if (is_data(&sec[i]) && t >= sec[i].start && t + 2 * sizeof(void *) <= sec[i].start + sec[i].size &&
            g_slots[0] == (void *)hook_load) {
            return 1;
        }
    }
    memset(&site, 0, sizeof(site));
    for (i = 0; i < nsec && found == 0; ++i) {
        if (is_code(&sec[i])) {
            found = tl_find_methods(sec[i].start, sec[i].size, (unsigned long)(size_t)sec[i].start, &site);
        }
    }
    if (found == 0) {
        LOGI("%s: shared top mip level files are not handled: the code that loads top mip levels was not "
             "recognised in this build of the game",
             name);
        return 0;
    }
    for (i = 0; i < nsec && found > 0; ++i) {
        if (is_data(&sec[i])) {
            unsigned long first = 0;
            unsigned n = tl_find_slots(sec[i].start, sec[i].size, (unsigned long)(size_t)sec[i].start, &site, &first);
            if (n && !slots) {
                slot_va = first;
            }
            slots += n;
        }
    }
    if (found < 0 || slots != 1) {
        LOGE("%s: shared top mip level files are not handled: the class that loads top mip levels could not be "
             "identified with certainty (%s, %u table candidates)",
             name, found < 0 ? "ambiguous code match" : "code found", slots);
        return 0;
    }
    table = (void **)(size_t)slot_va;
    g_load = (load_fn)table[0];
    g_unload = (unload_fn)table[1];
    /* The table is read-only data, and nothing of the engine has run yet. */
    hook = (void *)hook_unload;
    if (mem_patch(&table[1], &hook, sizeof(hook)) != 0) {
        LOGE("%s: shared top mip level files are not handled: the class table could not be written", name);
        return 0;
    }
    hook = (void *)hook_load;
    if (mem_patch(&table[0], &hook, sizeof(hook)) != 0) {
        mem_patch(&table[1], &g_unload, sizeof(g_unload));
        LOGE("%s: shared top mip level files are not handled: the class table could not be written", name);
        return 0;
    }
    g_slots = table;
    tex_stale_shared_handled();
    tex_stale_start();
    LOGI("%s: top mip level files that more than one texture names will be left unloaded ('%s' build variant)", name,
         site.variant);
    return 1;
}

int texlink_counts(long *refused)
{
    if (refused) {
        *refused = g_refused;
    }
    return g_slots != NULL;
}

void texlink_log_stats(void)
{
    if (!g_slots) {
        return;
    }
    if (g_refused || g_gone) {
        LOGI("textures: %ld loads of top mip level files that several textures name were left out (%ld recognised "
             "by name, %ld by content; %ld unloads to match); %ld unloads for a texture that no longer existed "
             "were left out",
             (long)g_refused, (long)g_by_name, (long)g_by_content, (long)g_skipped_unloads, (long)g_gone);
    }
}
