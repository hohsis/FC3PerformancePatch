/* The in-game settings menu: what it holds and how it reacts to input.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "menu_model.h"

#include <stdio.h>
#include <string.h>

int mm_selectable(const mm_item *item)
{
    return item->kind == MM_TOGGLE || item->kind == MM_CHOICE || item->kind == MM_NUMBER || item->kind == MM_ACTION;
}

/* The next selectable item from `from` in direction dir (+1/-1), wrapping
 * around; `from` itself when there is no other. */
static unsigned neighbour(const mm_menu *m, unsigned from, int dir)
{
    unsigned i = from, steps;

    for (steps = 0; steps < m->count; ++steps) {
        i = (dir > 0) ? (i + 1) % m->count : (i + m->count - 1) % m->count;
        if (mm_selectable(&m->items[i])) {
            return i;
        }
    }
    return from;
}

void mm_open(mm_menu *m, int open)
{
    m->open = open ? 1 : 0;
    if (!open || !m->count) {
        return;
    }
    if (m->selected >= m->count || !mm_selectable(&m->items[m->selected])) {
        m->selected = neighbour(m, m->count - 1, +1);
    }
    m->top = 0;
}

static int change(mm_item *it, int dir, int accept)
{
    int before = it->value;

    switch (it->kind) {
    case MM_TOGGLE:
        it->value = !it->value;
        break;
    case MM_CHOICE:
        if (it->choice_count) {
            int n = (int)it->choice_count;
            it->value = ((it->value + (accept ? 1 : dir)) % n + n) % n;
        }
        break;
    case MM_NUMBER: {
        int step = it->step > 0 ? it->step : 1;
        if (accept) {
            return 0; /* a number has no "press" */
        }
        it->value += dir * step;
        if (it->value < it->min) {
            it->value = it->min;
        }
        if (it->value > it->max) {
            it->value = it->max;
        }
        break;
    }
    case MM_ACTION:
        return accept;
    default:
        return 0;
    }
    if (it->value != before) {
        it->flags |= MM_CHANGED;
        return 1;
    }
    return 0;
}

int mm_handle(mm_menu *m, mm_input in)
{
    mm_item *it;

    if (!m->open || !m->count || in == MM_IN_NONE) {
        return MM_NO_CHANGE;
    }
    if (in == MM_IN_BACK) {
        m->open = 0;
        return MM_NO_CHANGE;
    }
    if (m->selected >= m->count) {
        m->selected = 0;
    }
    if (in == MM_IN_UP || in == MM_IN_DOWN) {
        m->selected = neighbour(m, m->selected, in == MM_IN_DOWN ? +1 : -1);
        return MM_NO_CHANGE;
    }
    it = &m->items[m->selected];
    if (!mm_selectable(it)) {
        return MM_NO_CHANGE;
    }
    if (change(it, in == MM_IN_LEFT ? -1 : +1, in == MM_IN_ACCEPT)) {
        return it->id;
    }
    return MM_NO_CHANGE;
}

void mm_scroll_into_view(mm_menu *m, unsigned visible)
{
    if (!visible || m->count <= visible) {
        m->top = 0;
        return;
    }
    /* Keep a heading above the first visible setting in view where possible. */
    if (m->selected < m->top + 1) {
        m->top = m->selected ? m->selected - 1 : 0;
    }
    if (m->selected >= m->top + visible) {
        m->top = m->selected - visible + 1;
    }
    if (m->top + visible > m->count) {
        m->top = m->count - visible;
    }
}

static const char *value_text(const mm_item *it, int value, char *buf, size_t len)
{
    if (!buf || !len) {
        return "";
    }
    buf[0] = '\0';
    switch (it->kind) {
    case MM_TOGGLE:
        snprintf(buf, len, "%s", value ? "On" : "Off");
        break;
    case MM_CHOICE:
        if (it->choices && value >= 0 && (unsigned)value < it->choice_count) {
            snprintf(buf, len, "%s", it->choices[value]);
        }
        break;
    case MM_NUMBER:
        snprintf(buf, len, "%d", value);
        break;
    case MM_INFO:
        snprintf(buf, len, "%s", it->text);
        break;
    default:
        break;
    }
    buf[len - 1] = '\0';
    return buf;
}

const char *mm_value_text(const mm_item *it, char *buf, size_t len)
{
    return value_text(it, it->value, buf, len);
}

const char *mm_applied_text(const mm_item *it, char *buf, size_t len)
{
    return value_text(it, it->applied, buf, len);
}

void mm_set(mm_item *it, int value)
{
    it->value = it->applied = value;
    it->flags &= ~MM_CHANGED;
}

int mm_pending(const mm_item *it)
{
    return (it->flags & MM_RESTART) && it->kind != MM_INFO && it->kind != MM_HEADING && it->value != it->applied;
}

mm_item *mm_find(mm_menu *m, int id)
{
    unsigned i;
    for (i = 0; i < m->count; ++i) {
        if (m->items[i].id == id && m->items[i].kind != MM_HEADING) {
            return &m->items[i];
        }
    }
    return NULL;
}

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') {
            x = (char)(x + 32);
        }
        if (y >= 'A' && y <= 'Z') {
            y = (char)(y + 32);
        }
        if (x != y) {
            return 0;
        }
    }
    return *a == *b;
}

unsigned mm_key_from_name(const char *name)
{
    /* virtual-key codes, spelled out so this file needs no Windows header */
    static const struct {
        const char *name;
        unsigned vk;
    } names[] = {{"Home", 0x24},       {"End", 0x23},    {"Insert", 0x2D},   {"Delete", 0x2E}, {"Pause", 0x13},
                 {"ScrollLock", 0x91}, {"PageUp", 0x21}, {"PageDown", 0x22}, {"Tab", 0x09}};
    size_t i;

    if (!name || !*name) {
        return 0;
    }
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        unsigned n = (unsigned)(name[1] - '0');
        if (name[2] >= '0' && name[2] <= '9' && !name[3]) {
            n = n * 10 + (unsigned)(name[2] - '0');
        } else if (name[2]) {
            return 0;
        }
        return n >= 1 && n <= 12 ? 0x70 + n - 1 : 0; /* VK_F1 = 0x70 */
    }
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (same_name(name, names[i].name)) {
            return names[i].vk;
        }
    }
    return 0;
}

int mm_pad_combo_from_name(const char *text, unsigned *mask)
{
    static const struct {
        const char *name;
        unsigned bit;
    } names[] = {{"A", MM_PAD_A},         {"B", MM_PAD_B},       {"X", MM_PAD_X},         {"Y", MM_PAD_Y},
                 {"LB", MM_PAD_LB},       {"RB", MM_PAD_RB},     {"L3", MM_PAD_L3},       {"R3", MM_PAD_R3},
                 {"Back", MM_PAD_BACK},   {"Select", MM_PAD_BACK}, {"View", MM_PAD_BACK}, {"Start", MM_PAD_START},
                 {"Menu", MM_PAD_START},  {"Up", MM_PAD_UP},     {"Down", MM_PAD_DOWN},   {"Left", MM_PAD_LEFT},
                 {"Right", MM_PAD_RIGHT}};
    unsigned found = 0, buttons = 0;
    const char *p = text;

    if (!text || !mask) {
        return 0;
    }
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (same_name(p, "none") || same_name(p, "off") || same_name(p, "0")) {
        *mask = 0;
        return 1;
    }
    for (;;) {
        char word[8];
        size_t n = 0, i;
        unsigned bit = 0;

        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        while (*p && *p != '+' && *p != ' ' && *p != '\t') {
            if (n + 1 >= sizeof(word)) {
                return 0;
            }
            word[n++] = *p++;
        }
        word[n] = '\0';
        for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            if (same_name(word, names[i].name)) {
                bit = names[i].bit;
            }
        }
        if (!bit) {
            return 0;
        }
        if (!(found & bit)) {
            found |= bit;
            ++buttons;
        }
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (*p != '+') {
            return 0;
        }
        ++p;
    }
    if (buttons < 2) {
        return 0;
    }
    *mask = found;
    return 1;
}

int mm_repeat_step(mm_repeat *r, int down, unsigned long now_ms)
{
    if (!down) {
        r->down = 0;
        return 0;
    }
    if (!r->down) {
        r->down = 1;
        r->next_ms = now_ms + MM_REPEAT_DELAY_MS;
        return 1;
    }
    /* unsigned difference: correct across the tick counter's wrap */
    if ((long)(now_ms - r->next_ms) >= 0) {
        r->next_ms = now_ms + MM_REPEAT_RATE_MS;
        return 1;
    }
    return 0;
}

int mm_chord_step(mm_chord *c, int down, unsigned long now_ms)
{
    if (!down) {
        c->armed = 1;
        c->holding = 0;
        return 0;
    }
    if (!c->armed) {
        return 0; /* still held from the time it fired */
    }
    if (!c->holding) {
        c->holding = 1;
        c->since_ms = now_ms;
        return 0;
    }
    if (now_ms - c->since_ms >= MM_CHORD_HOLD_MS) {
        c->armed = 0;
        c->holding = 0;
        return 1;
    }
    return 0;
}
