/* Display compatibility: fullscreen guard for the DX11 renderer and
 * process-wide window settings.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_DISPLAY_H
#define FC3PP_DISPLAY_H

#include <windows.h>

#include "config.h"

/* DPI awareness and window ghosting. Call once, before the game creates its
 * first window. */
void display_apply_process_settings(const fc3pp_config *cfg);

/* Called from the engine's Present, before the frame is shown. */
typedef void (*display_present_fn)(void *swap_chain);

/* Hooks the engine module's DXGI factory import so that the swap chains it
 * creates can be watched: with guard_fullscreen for the fullscreen guard (see
 * fs_guard.h), with on_present to be called for every frame they present.
 * Returns the number of imports hooked; 0 for the DX9 engine module, which
 * does not use DXGI. */
unsigned display_install_hooks(HMODULE engine, int guard_fullscreen, display_present_fn on_present);

#endif /* FC3PP_DISPLAY_H */
