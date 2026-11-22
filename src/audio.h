/* Audio: keeps XAudio2 2.7 loaded for the lifetime of the game, plus an
 * optional diagnostic trace of how the engine uses it. The sound itself is
 * never altered.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_AUDIO_H
#define FC3PP_AUDIO_H

#include <windows.h>

#include "config.h"

/* Hooks the engine module's CoCreateInstance import when a feature needs it.
 * Returns the number of imports hooked. */
unsigned audio_install(HMODULE engine, const fc3pp_config *cfg);

/* Writes the trace summary (formats of all source voices seen) to the log. */
void audio_log_stats(void);

#endif /* FC3PP_AUDIO_H */
