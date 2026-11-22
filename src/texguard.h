/* Texture guard: keeps the DX11 renderer from loading a top mip level that
 * does not fit its texture (see tex_rule.h). This is what makes texture mods
 * crash the game unless mip0 loading is switched off altogether.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_TEXGUARD_H
#define FC3PP_TEXGUARD_H

#include <windows.h>

/* Locates the engine's texture class in a mapped engine module and wraps its
 * "add top level", "drop top level" and destructor methods. name is the
 * module's file name, for the log. With scan_archives the game's data folder
 * is also read in the background to find top levels that belong to textures
 * a mod has replaced (tex_scan.h). Returns 1 when the guard is active, 0 when
 * the class was not recognised (the module is left untouched). */
int texguard_install(HMODULE engine, const char *name, int scan_archives);

/* Writes the counters to the log. Takes no locks. */
void texguard_log_stats(void);

#endif /* FC3PP_TEXGUARD_H */
