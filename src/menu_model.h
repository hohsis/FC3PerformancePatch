/* The in-game settings menu: what it holds and how it reacts to input.
 * Pure C, no drawing and no OS calls.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_MENU_MODEL_H
#define FC3PP_MENU_MODEL_H

#include <stddef.h>

typedef enum mm_kind {
    MM_HEADING, /* a section title */
    MM_INFO,    /* a line of text with a value that only the program changes */
    MM_TOGGLE,  /* off / on */
    MM_CHOICE,  /* one of a list of named values */
    MM_NUMBER,  /* an integer between min and max */
    MM_ACTION   /* something to run */
} mm_kind;

#define MM_RESTART 0x1u /* takes effect the next time the game starts */
#define MM_CHANGED 0x2u /* set by the menu when the value was changed in this session */

#define MM_TEXT_MAX 96

typedef struct mm_item {
    mm_kind kind;
    int id;                      /* the caller's own identifier */
    const char *label;
    const char *help;            /* one or two sentences shown for the selected item */
    int value;                   /* toggle: 0/1; choice: index; number */
    int min, max, step;          /* number */
    const char *const *choices;  /* choice */
    unsigned choice_count;
    unsigned flags;
    char text[MM_TEXT_MAX];      /* info: the value shown */
} mm_item;

typedef struct mm_menu {
    const char *title;
    mm_item *items;
    unsigned count;
    unsigned selected; /* index of the selected item */
    unsigned top;      /* first visible item when the list scrolls */
    int open;
} mm_menu;

typedef enum mm_input {
    MM_IN_NONE = 0,
    MM_IN_UP,
    MM_IN_DOWN,
    MM_IN_LEFT,
    MM_IN_RIGHT,
    MM_IN_ACCEPT,
    MM_IN_BACK
} mm_input;

#define MM_NO_CHANGE (-1)

/* Can the cursor rest on this item? */
int mm_selectable(const mm_item *item);

/* Opens the menu with the cursor on the first selectable item, or closes it. */
void mm_open(mm_menu *m, int open);

/* Applies one input. Returns the id of the item whose value changed or whose
 * action was triggered, MM_NO_CHANGE otherwise. MM_IN_BACK closes the menu. */
int mm_handle(mm_menu *m, mm_input in);

/* Keeps the selected item inside a window of `visible` rows. */
void mm_scroll_into_view(mm_menu *m, unsigned visible);

/* The value as shown to the user ("On", "6", "engine", info text). */
const char *mm_value_text(const mm_item *item, char *buf, size_t len);

mm_item *mm_find(mm_menu *m, int id);

/* Parses a key name such as "F8", "Home" or "Insert" (upper or lower case)
 * into a Windows virtual-key code; 0 when the name is not known. */
unsigned mm_key_from_name(const char *name);

/* ---- key repeat ------------------------------------------------------------------
 * A held direction acts once, then again after a pause, then steadily. */

#define MM_REPEAT_DELAY_MS 380ul
#define MM_REPEAT_RATE_MS 85ul

typedef struct mm_repeat {
    int down;
    unsigned long next_ms;
} mm_repeat;

/* Returns 1 when the control should act now. */
int mm_repeat_step(mm_repeat *r, int down, unsigned long now_ms);

/* ---- opening chord -----------------------------------------------------------------
 * A combination that must be held for a moment (so it is not hit by accident)
 * and then released before it can fire again. */

#define MM_CHORD_HOLD_MS 350ul

typedef struct mm_chord {
    int armed;             /* released since it last fired */
    int holding;
    unsigned long since_ms;
} mm_chord;

int mm_chord_step(mm_chord *c, int down, unsigned long now_ms);

#endif /* FC3PP_MENU_MODEL_H */
