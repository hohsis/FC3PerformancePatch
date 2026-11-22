/* COM virtual-table slot hooks.
 *
 * Objects of one COM class share a virtual table, so replacing a slot affects
 * every object of that class in the process. Callers therefore keep one
 * vt_hook array per hooked method, look the original up by the object's table
 * and decide per object whether to act or to pass straight through.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_VTABLE_H
#define FC3PP_VTABLE_H

#define VT_MAX_TABLES 4

typedef struct vt_hook {
    const void *vtbl; /* the table that was patched */
    void *original;   /* the slot's previous content */
} vt_hook;

/* The virtual table of a COM (or COM-like) object. */
#define VT_OF(object) (*(void ***)(object))

/* Returns the function a slot held before it was hooked, or NULL when this
 * table was never hooked through `hooks`. */
void *vt_original(const vt_hook *hooks, const void *vtbl);

/* Replaces vtbl[slot] with replacement and remembers the previous content in
 * hooks (an array of VT_MAX_TABLES entries, zero-initialised). Idempotent.
 * Returns 1 when the slot is hooked afterwards, 0 on failure. Not thread
 * safe: serialise calls that share a hooks array. */
int vt_hook_slot(vt_hook *hooks, void **vtbl, unsigned slot, void *replacement);

#endif /* FC3PP_VTABLE_H */
