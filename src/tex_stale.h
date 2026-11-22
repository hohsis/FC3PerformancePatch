/* Runs the archive scan of tex_scan.h on the game's real data folder, in a
 * background thread, and answers "is this top mip level file stale?".
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_TEX_STALE_H
#define FC3PP_TEX_STALE_H

#include <stddef.h>
#include <wchar.h>

#include "tex_scan.h"

/* Reads the archives in data_dir (the game's "data_win32" folder: patch.fat
 * is the override, every other .fat below it the game's own) and runs the
 * scan. Returns the number of identities stored in ids, or -1 without memory.
 * *archives (optional) receives the number of archives read. Not reentrant. */
long tex_stale_scan_folder(const wchar_t *data_dir, unsigned long long *ids, size_t cap, ts_stats *stats,
                           unsigned *archives);

/* Starts scanning "<game>\data_win32" (the folder next to the one the game
 * executable is in). Returns at once; does nothing when called again. */
void tex_stale_start(void);

/* Non-zero when the file the engine holds (buffer: start of the .xbt file,
 * dds: the DDS image inside it, dds_size: its size) is on the list. Waits for
 * the scan to finish the first time, for a few seconds at most. */
int tex_stale_check(const void *buffer, const void *dds, unsigned dds_size);

#endif /* FC3PP_TEX_STALE_H */
