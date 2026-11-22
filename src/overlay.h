/* Draws lists of coloured, textured rectangles over the game's picture
 * (Direct3D 11) and provides the font they use.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_OVERLAY_H
#define FC3PP_OVERLAY_H

#include <windows.h>

#include "menu_draw.h"

/* Prepares to draw onto the back buffer of swap_chain (an IDXGISwapChain).
 * On success returns 1 and gives the size of the picture and a font sized for
 * it; the caller then fills a list and calls overlay_draw. Returns 0 when the
 * overlay cannot be used (the reason is logged once). Call from the thread
 * that presents. */
int overlay_begin(void *swap_chain, float *width, float *height, const md_font **font);

/* Size of the text in percent of the normal size (100 to 200). Takes effect
 * with the next overlay_begin. */
void overlay_set_scale(int percent);

/* Draws the list and puts the engine's Direct3D state back as it was. */
void overlay_draw(const md_list *list);

/* Releases everything created for the current device. */
void overlay_release(void);

#endif /* FC3PP_OVERLAY_H */
