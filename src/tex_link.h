/* Top mip level files that more than one texture lays claim to. Pure C.
 *
 * The engine loads a texture's top mip level from a file of its own, and the
 * object it keeps for such a file ("top level resource") has room for exactly
 * one texture it belongs to:
 *
 *     +20h  64-bit name of the file (the hash the archives use)
 *     +30h  the texture it is linked to ("linkTextureResource")
 *     +34h  valid
 *
 * In the game's own data every top level file is named by one texture. A mod
 * that copies texture headers around ends up with several textures naming the
 * same file. They then share one top level resource, whose link points at
 * whichever of them was set up last - and stays that way when that texture is
 * destroyed while another still holds the resource. When the resource is
 * unloaded later it follows the link:
 *
 *     Unload:  if (valid) { t = link; if (t->users < 2 && t->record) Drop(t->record); ... }
 *
 * and Drop writes into a record that was freed long ago. The game crashes
 * there if the memory is gone, and corrupts whatever has taken its place if
 * it is not. Load takes the same path with Add.
 *
 * The two methods sit next to each other in the class's virtual table. This
 * file finds them; texlink.c wraps them so that a top level file with several
 * claimants is neither loaded nor unloaded - its texture stays at the size
 * of its base file, which for all claimants but one is what it should have
 * been anyway.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_TEX_LINK_H
#define FC3PP_TEX_LINK_H

#include <stddef.h>

#define TL_OFFSET_NAME 0x20u    /* of the top level resource: file name hash, low half first */
#define TL_OFFSET_SIZE 0x28u    /*   cleared by Unload */
#define TL_OFFSET_LINK 0x30u    /*   the texture it is linked to */
#define TL_OFFSET_VALID 0x34u
#define TL_TEXTURE_RECORD 0x30u /* of the texture: its render-side record */
#define TL_TEXTURE_USERS 0x48u
#define TL_TEXTURE_BYTES 0x58u  /* how much of a texture object Unload looks at */
#define TL_RECORD_BYTES 0x24u   /* how much of a record Drop writes to */

typedef struct tl_site {
    const char *variant;  /* build variant that matched */
    unsigned long load;   /* bool __thiscall Load(const void *file, unsigned size) */
    unsigned long unload; /* void __thiscall Unload(void); the next slot of the table */
} tl_site;

/* Looks for both methods in a code section mapped at text_va. Returns 1 when
 * exactly one pair was found, 0 when none, -1 when the match is ambiguous. */
int tl_find_methods(const unsigned char *text, size_t size, unsigned long text_va, tl_site *out);

/* Counts the places in a data section mapped at data_va where a pointer to
 * Load is directly followed by a pointer to Unload, and stores the address of
 * the first (the table's Load slot) in *slot_va. */
unsigned tl_find_slots(const unsigned char *data, size_t size, unsigned long data_va, const tl_site *site,
                       unsigned long *slot_va);

#endif /* FC3PP_TEX_LINK_H */
