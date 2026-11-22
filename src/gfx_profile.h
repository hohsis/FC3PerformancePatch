/* Applies a draw-distance preset (gfx_preset.h) by handing the game a copy of
 * its profile with the preset's sections added, when it opens the profile to
 * read it. The file on disk is never changed.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_GFX_PROFILE_H
#define FC3PP_GFX_PROFILE_H

#include <windows.h>

/* Appended to the profile's name for the one-time copy of it. */
#define GFX_PROFILE_BACKUP_SUFFIX L".fc3pp.bak"

/* level: a gp_level. Hooks the engine module's file-open imports; returns the
 * number hooked (0 when the preset is "original": nothing is touched). */
unsigned gfx_profile_install(HMODULE engine, int level);

/* The game's default render configuration, read from the archives in a
 * data_win32 folder: the patch archive's if a mod brings one, else the game's
 * own. Returns NULL when it cannot be read; the caller frees it. */
char *gfx_profile_read_default_config(const wchar_t *data_dir, size_t *len);

/* Says in the log what became of the preset. Takes no locks. */
void gfx_profile_log_stats(void);

#endif /* FC3PP_GFX_PROFILE_H */
