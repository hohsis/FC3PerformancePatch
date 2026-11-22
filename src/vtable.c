/* COM virtual-table slot hooks.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "vtable.h"

#include <stddef.h>

#include "pe_image.h"

void *vt_original(const vt_hook *hooks, const void *vtbl)
{
    unsigned i;
    for (i = 0; i < VT_MAX_TABLES; ++i) {
        if (hooks[i].vtbl == vtbl) {
            return hooks[i].original;
        }
    }
    return NULL;
}

int vt_hook_slot(vt_hook *hooks, void **vtbl, unsigned slot, void *replacement)
{
    unsigned i;

    if (vtbl[slot] == replacement) {
        return vt_original(hooks, vtbl) != NULL;
    }
    for (i = 0; i < VT_MAX_TABLES; ++i) {
        if (!hooks[i].vtbl) {
            void *original = vtbl[slot];
            /* Virtual tables live in read-only data. */
            if (mem_patch(&vtbl[slot], &replacement, sizeof(replacement)) != 0) {
                return 0;
            }
            hooks[i].original = original;
            hooks[i].vtbl = vtbl;
            return 1;
        }
    }
    return 0; /* more implementations of one interface than anyone has seen in one process */
}
