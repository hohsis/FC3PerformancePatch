/* The in-game settings menu: its entries, its input, and keeping the game from
 * reacting to that input while the menu is open.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_MENU_H
#define FC3PP_MENU_H

#include <windows.h>

#include "config.h"

/* Builds the menu from the configuration. ini_path is where changed settings
 * are written. Call once, before the hooks are installed. */
void menu_init(const fc3pp_config *cfg, const wchar_t *ini_path);

/* Text for the read-only lines at the top of the menu. */
typedef enum menu_status { MENU_STATUS_PROCESSOR, MENU_STATUS_MEMORY, MENU_STATUS_COUNT } menu_status;
void menu_set_status(menu_status which, const char *text);

/* The renderer gives the menu nowhere to draw (DirectX 9): switch it off and say so. */
void menu_set_unavailable(void);

/* Hooks the engine module's gamepad and keyboard/mouse input so that the game
 * does not act on what is typed into the menu. Returns the number of imports
 * hooked. */
unsigned menu_install_hooks(HMODULE engine);

/* Called for every frame the game presents, with its swap chain. Reads the
 * keys and the gamepad, and draws the menu while it is open. */
void menu_on_present(void *swap_chain);

int menu_is_open(void);

#endif /* FC3PP_MENU_H */
