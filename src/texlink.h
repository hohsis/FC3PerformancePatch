/* Keeps the engine away from top mip level files that more than one texture
 * names (tex_link.h has the reasoning and the engine's side).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_TEXLINK_H
#define FC3PP_TEXLINK_H

#include <windows.h>

/* Finds the top level resource class in an engine module and wraps its Load
 * and Unload methods. Returns 1 when installed. Says so in the log. The list
 * of files comes from the archive scan: call tex_stale_start afterwards. */
int texlink_install(HMODULE engine, const char *name);

/* Top level files left unloaded so far; returns 0 when not installed. */
int texlink_counts(long *refused);

/* Writes the totals to the log. Takes no locks. */
void texlink_log_stats(void);

#endif /* FC3PP_TEXLINK_H */
