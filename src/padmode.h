/* Switching between gamepad and keyboard/mouse while playing. Pure C.
 *
 * The game treats the two as exclusive modes chosen in the options menu
 * ("Use gamepad"). Internally its UI manager keeps three bytes:
 *
 *     +30h  the mode in effect            (1 = gamepad)
 *     +31h  the mode asked for
 *     +32h  "a change was asked for"
 *
 * and re-evaluates them every frame: when "asked for AND a pad is connected"
 * differs from the mode in effect, it switches - button prompts, input
 * handling and a global flag that 50-odd functions read. The options menu
 * does nothing more than write +31h and +32h. This module finds that object
 * in an engine module and decides, from what the player touches, which mode
 * to ask for.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_PADMODE_H
#define FC3PP_PADMODE_H

#include <stddef.h>

/* ---- locating the UI manager ----------------------------------------------------- */

#define PM_OFFSET_REQUESTED 0x31u
#define PM_OFFSET_PENDING 0x32u

/* Looks in a code section mapped at text_va for the code that copies the
 * profile's "use gamepad" setting to the UI manager, and returns the address
 * of the variable that points to the UI manager. Returns 1 when exactly one
 * place was found, 0 when none, -1 when the match is ambiguous. *variant
 * (optional) receives the name of the build variant. */
int pm_find_manager(const unsigned char *text, size_t size, unsigned long text_va, unsigned long *pointer_va,
                    const char **variant);

/* ---- deciding ----------------------------------------------------------------------- */

#define PM_STICK_THRESHOLD 12000 /* of 32767: well outside a worn stick's rest position */
#define PM_TRIGGER_THRESHOLD 40  /* of 255 */
#define PM_MOUSE_THRESHOLD 12    /* counts of movement within the window below */
#define PM_MOUSE_WINDOW_MS 250ul
#define PM_SETTLE_MS 400ul       /* after a switch, the other device must stay quiet this long to switch back */

typedef enum pm_mode { PM_UNKNOWN = -1, PM_KEYBOARD_MOUSE = 0, PM_GAMEPAD = 1 } pm_mode;

typedef struct pm_state {
    pm_mode mode;                /* what was asked for last */
    unsigned long switched_ms;
    long mouse_travel;           /* accumulated since mouse_since_ms */
    unsigned long mouse_since_ms;
    unsigned switches;
} pm_state;

void pm_reset(pm_state *s);

/* Is a gamepad state "in use"? buttons: XInput button bits; sticks -32768..32767. */
int pm_gamepad_active(unsigned buttons, int left_trigger, int right_trigger, int lx, int ly, int rx, int ry);

/* Feed what was just seen. Each returns the mode to ask the game for, or
 * PM_UNKNOWN when nothing should change. */
pm_mode pm_note_gamepad(pm_state *s, int active, unsigned long now_ms);
pm_mode pm_note_keyboard(pm_state *s, int any_key_down, unsigned long now_ms);
pm_mode pm_note_mouse(pm_state *s, long travel, int any_button_down, unsigned long now_ms);

#endif /* FC3PP_PADMODE_H */
