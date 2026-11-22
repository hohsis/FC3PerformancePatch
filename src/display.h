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

/* Hooks the engine module's DXGI factory import so that the swap chains it
 * creates can be watched (see fs_guard.h). Returns the number of imports
 * hooked; 0 for the DX9 engine module, which does not use DXGI. */
unsigned display_install_hooks(HMODULE engine);

#endif /* FC3PP_DISPLAY_H */
