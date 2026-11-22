/* Switches the game between gamepad and keyboard/mouse mode according to what
 * the player touches (padmode.h has the reasoning and the engine's side).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_PADSWITCH_H
#define FC3PP_PADSWITCH_H

#include <windows.h>

/* Finds the engine's UI manager in a loaded engine module. Returns 1 when it
 * was found (switching is possible), 0 otherwise. Says so in the log.
 * assume_focus (a testing aid): act as if the game always had the focus. */
int padswitch_install(HMODULE engine, const char *name, int enabled, int assume_focus);

/* Could the engine be found? */
int padswitch_available(void);

/* Switching can be turned on and off while the game runs. */
void padswitch_enable(int on);
int padswitch_enabled(void);

/* What the input hooks saw the game read. Cheap when switching is off. */
void padswitch_gamepad(unsigned buttons, int left_trigger, int right_trigger, int lx, int ly, int rx, int ry);
void padswitch_keyboard(int any_key_down);
void padswitch_mouse(long travel, int any_button_down);

/* Says in the log how often the mode was switched. Takes no locks. */
void padswitch_log_stats(void);

#endif /* FC3PP_PADSWITCH_H */
