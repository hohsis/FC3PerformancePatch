/* Unit tests: the settings menu's behaviour and its layout.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "menu_draw.h"
#include "menu_model.h"
#include "test.h"

enum { ID_INFO = 1, ID_SWITCH, ID_CHOICE, ID_NUMBER, ID_ACTION, ID_LAST };

static const char *const CHOICES[] = {"low", "medium", "high"};

static mm_item g_items[8];
static mm_menu g_menu;

static void build(void)
{
    static const mm_item items[] = {
        {MM_HEADING, 0, "First", NULL, 0, 0, 0, 0, NULL, 0, 0, "", 0},
        {MM_INFO, ID_INFO, "Version", NULL, 0, 0, 0, 0, NULL, 0, 0, "1.2.3", 0},
        {MM_TOGGLE, ID_SWITCH, "Switch", "A switch.", 0, 0, 1, 1, NULL, 0, MM_RESTART, "", 0},
        {MM_CHOICE, ID_CHOICE, "Level", "A choice.", 1, 0, 2, 1, CHOICES, 3, 0, "", 0},
        {MM_HEADING, 0, "Second", NULL, 0, 0, 0, 0, NULL, 0, 0, "", 0},
        {MM_NUMBER, ID_NUMBER, "Cores", "A number.", 6, 0, 15, 1, NULL, 0, 0, "", 0},
        {MM_ACTION, ID_ACTION, "Close", "", 0, 0, 0, 0, NULL, 0, 0, "", 0},
        {MM_TOGGLE, ID_LAST, "Last", "The last one.", 1, 0, 1, 1, NULL, 0, 0, "", 0},
    };
    memcpy(g_items, items, sizeof(items));
    memset(&g_menu, 0, sizeof(g_menu));
    g_menu.title = "Test";
    g_menu.items = g_items;
    g_menu.count = 8;
}

static void test_navigation(void)
{
    build();
    CHECK(mm_handle(&g_menu, MM_IN_DOWN) == MM_NO_CHANGE); /* closed: nothing happens */
    CHECK_EQ_U(g_menu.selected, 0);

    mm_open(&g_menu, 1);
    CHECK(g_menu.open == 1);
    CHECK_EQ_U(g_menu.selected, 2); /* the first thing that can be changed */
    mm_handle(&g_menu, MM_IN_DOWN);
    CHECK_EQ_U(g_menu.selected, 3);
    mm_handle(&g_menu, MM_IN_DOWN); /* over the heading */
    CHECK_EQ_U(g_menu.selected, 5);
    mm_handle(&g_menu, MM_IN_DOWN);
    mm_handle(&g_menu, MM_IN_DOWN);
    CHECK_EQ_U(g_menu.selected, 7);
    mm_handle(&g_menu, MM_IN_DOWN); /* wraps, over heading and info */
    CHECK_EQ_U(g_menu.selected, 2);
    mm_handle(&g_menu, MM_IN_UP);
    CHECK_EQ_U(g_menu.selected, 7);
    mm_handle(&g_menu, MM_IN_UP);
    mm_handle(&g_menu, MM_IN_UP);
    mm_handle(&g_menu, MM_IN_UP); /* over the heading, upwards */
    CHECK_EQ_U(g_menu.selected, 3);

    CHECK(mm_handle(&g_menu, MM_IN_BACK) == MM_NO_CHANGE);
    CHECK(g_menu.open == 0);
    /* reopening keeps the place */
    mm_open(&g_menu, 1);
    CHECK_EQ_U(g_menu.selected, 3);

    CHECK(mm_selectable(&g_items[2]) && !mm_selectable(&g_items[0]) && !mm_selectable(&g_items[1]));
    CHECK(mm_find(&g_menu, ID_NUMBER) == &g_items[5]);
    CHECK(mm_find(&g_menu, 99) == NULL);
    CHECK(mm_find(&g_menu, 0) == NULL); /* headings are not found by the id they share */
}

static void test_values(void)
{
    char text[32];

    build();
    mm_open(&g_menu, 1);

    /* switch */
    CHECK_STR(mm_value_text(&g_items[2], text, sizeof(text)), "Off");
    CHECK(mm_handle(&g_menu, MM_IN_ACCEPT) == ID_SWITCH);
    CHECK(g_items[2].value == 1 && (g_items[2].flags & MM_CHANGED) && (g_items[2].flags & MM_RESTART));
    CHECK_STR(mm_value_text(&g_items[2], text, sizeof(text)), "On");
    CHECK(mm_handle(&g_menu, MM_IN_LEFT) == ID_SWITCH && g_items[2].value == 0);
    CHECK(mm_handle(&g_menu, MM_IN_RIGHT) == ID_SWITCH && g_items[2].value == 1);

    /* a setting that needs a restart knows which value is in use */
    CHECK(mm_pending(&g_items[2]));
    CHECK_STR(mm_applied_text(&g_items[2], text, sizeof(text)), "Off");
    mm_handle(&g_menu, MM_IN_ACCEPT); /* put back: nothing is waiting, though it was touched */
    CHECK(!mm_pending(&g_items[2]) && (g_items[2].flags & MM_CHANGED));
    mm_set(&g_items[2], 1); /* started with it on */
    CHECK(!mm_pending(&g_items[2]) && !(g_items[2].flags & MM_CHANGED) && g_items[2].value == 1);
    CHECK_STR(mm_applied_text(&g_items[2], text, sizeof(text)), "On");
    mm_handle(&g_menu, MM_IN_ACCEPT);
    CHECK(mm_pending(&g_items[2]) && g_items[2].value == 0);
    mm_handle(&g_menu, MM_IN_ACCEPT);
    /* one that applies at once never waits */
    mm_set(&g_items[3], 1);
    g_items[3].value = 2;
    CHECK(!mm_pending(&g_items[3]));
    CHECK_STR(mm_applied_text(&g_items[3], text, sizeof(text)), "medium");
    g_items[3].value = 1;
    CHECK(!mm_pending(&g_items[0]) && !mm_pending(&g_items[1]));

    /* choice: left and right step, accept goes on, both wrap */
    mm_handle(&g_menu, MM_IN_DOWN);
    CHECK_STR(mm_value_text(&g_items[3], text, sizeof(text)), "medium");
    CHECK(mm_handle(&g_menu, MM_IN_RIGHT) == ID_CHOICE && g_items[3].value == 2);
    CHECK(mm_handle(&g_menu, MM_IN_RIGHT) == ID_CHOICE && g_items[3].value == 0);
    CHECK(mm_handle(&g_menu, MM_IN_LEFT) == ID_CHOICE && g_items[3].value == 2);
    CHECK(mm_handle(&g_menu, MM_IN_ACCEPT) == ID_CHOICE && g_items[3].value == 0);
    CHECK_STR(mm_value_text(&g_items[3], text, sizeof(text)), "low");

    /* number: clamped, accept does nothing */
    mm_handle(&g_menu, MM_IN_DOWN);
    CHECK(mm_handle(&g_menu, MM_IN_ACCEPT) == MM_NO_CHANGE && g_items[5].value == 6);
    CHECK(mm_handle(&g_menu, MM_IN_RIGHT) == ID_NUMBER && g_items[5].value == 7);
    g_items[5].value = 15;
    CHECK(mm_handle(&g_menu, MM_IN_RIGHT) == MM_NO_CHANGE && g_items[5].value == 15);
    g_items[5].value = 0;
    CHECK(mm_handle(&g_menu, MM_IN_LEFT) == MM_NO_CHANGE && g_items[5].value == 0);
    CHECK_STR(mm_value_text(&g_items[5], text, sizeof(text)), "0");

    /* action: only accept triggers it */
    mm_handle(&g_menu, MM_IN_DOWN);
    CHECK(mm_handle(&g_menu, MM_IN_RIGHT) == MM_NO_CHANGE);
    CHECK(mm_handle(&g_menu, MM_IN_ACCEPT) == ID_ACTION);

    CHECK_STR(mm_value_text(&g_items[1], text, sizeof(text)), "1.2.3");
    CHECK_STR(mm_value_text(&g_items[0], text, sizeof(text)), "");
}

static void test_scrolling(void)
{
    build();
    mm_open(&g_menu, 1);
    mm_scroll_into_view(&g_menu, 8); /* everything fits */
    CHECK_EQ_U(g_menu.top, 0);

    g_menu.selected = 7;
    mm_scroll_into_view(&g_menu, 4);
    CHECK_EQ_U(g_menu.top, 4); /* the last four rows */
    g_menu.selected = 5;
    mm_scroll_into_view(&g_menu, 4);
    CHECK_EQ_U(g_menu.top, 4); /* still visible, with its heading */
    g_menu.selected = 3;
    mm_scroll_into_view(&g_menu, 4);
    CHECK_EQ_U(g_menu.top, 2);
    g_menu.selected = 2;
    mm_scroll_into_view(&g_menu, 4);
    CHECK_EQ_U(g_menu.top, 1); /* one row above the selection stays in view */
    mm_scroll_into_view(&g_menu, 0);
    CHECK_EQ_U(g_menu.top, 0);
}

static void test_repeat_and_chord(void)
{
    mm_repeat r;
    mm_chord c;
    unsigned long t = 0xFFFFFF00ul; /* close to the tick counter's wrap */
    int fired = 0, i;

    memset(&r, 0, sizeof(r));
    CHECK(mm_repeat_step(&r, 0, t) == 0);
    CHECK(mm_repeat_step(&r, 1, t) == 1); /* at once */
    CHECK(mm_repeat_step(&r, 1, t + 100) == 0);
    CHECK(mm_repeat_step(&r, 1, t + MM_REPEAT_DELAY_MS - 1) == 0);
    CHECK(mm_repeat_step(&r, 1, t + MM_REPEAT_DELAY_MS) == 1); /* after the pause, across the wrap */
    CHECK(mm_repeat_step(&r, 1, t + MM_REPEAT_DELAY_MS + 10) == 0);
    CHECK(mm_repeat_step(&r, 1, t + MM_REPEAT_DELAY_MS + MM_REPEAT_RATE_MS) == 1); /* then steadily */
    CHECK(mm_repeat_step(&r, 0, t + 2000) == 0);
    CHECK(mm_repeat_step(&r, 1, t + 2001) == 1); /* a new press */

    memset(&c, 0, sizeof(c));
    CHECK(mm_chord_step(&c, 1, 0) == 0); /* already held when we first look: must be released first */
    CHECK(mm_chord_step(&c, 1, 5000) == 0);
    CHECK(mm_chord_step(&c, 0, 5001) == 0);
    CHECK(mm_chord_step(&c, 1, 6000) == 0);
    CHECK(mm_chord_step(&c, 1, 6000 + MM_CHORD_HOLD_MS - 1) == 0);
    CHECK(mm_chord_step(&c, 1, 6000 + MM_CHORD_HOLD_MS) == 1); /* held long enough */
    for (i = 0; i < 20; ++i) {
        fired += mm_chord_step(&c, 1, 7000 + 100ul * (unsigned long)i);
    }
    CHECK(fired == 0); /* once per hold */
    CHECK(mm_chord_step(&c, 0, 10000) == 0);
    CHECK(mm_chord_step(&c, 1, 10001) == 0);
    CHECK(mm_chord_step(&c, 0, 10100) == 0); /* a short press does nothing */
    CHECK(mm_chord_step(&c, 1, 10200) == 0);
    CHECK(mm_chord_step(&c, 1, 10200 + MM_CHORD_HOLD_MS) == 1);
}

static void test_key_names(void)
{
    CHECK_EQ_U(mm_key_from_name("F8"), 0x77);
    CHECK_EQ_U(mm_key_from_name("f1"), 0x70);
    CHECK_EQ_U(mm_key_from_name("F12"), 0x7B);
    CHECK_EQ_U(mm_key_from_name("F13"), 0);
    CHECK_EQ_U(mm_key_from_name("F0"), 0);
    CHECK_EQ_U(mm_key_from_name("F1x"), 0);
    CHECK_EQ_U(mm_key_from_name("Home"), 0x24);
    CHECK_EQ_U(mm_key_from_name("insert"), 0x2D);
    CHECK_EQ_U(mm_key_from_name("SCROLLLOCK"), 0x91);
    CHECK_EQ_U(mm_key_from_name("Hom"), 0);
    CHECK_EQ_U(mm_key_from_name(""), 0);
    CHECK_EQ_U(mm_key_from_name(NULL), 0);
}

/* ---- layout ------------------------------------------------------------------------- */

static md_font g_font;
static md_vertex g_vertices[6000];

/* Every glyph 10 wide in a cell of 14 x 20. */
static void make_font(void)
{
    int i;
    for (i = 0; i < MD_GLYPHS; ++i) {
        g_font.glyph[i].u0 = 0.1f;
        g_font.glyph[i].v0 = 0.1f;
        g_font.glyph[i].u1 = 0.2f;
        g_font.glyph[i].v1 = 0.2f;
        g_font.glyph[i].w = 14;
        g_font.glyph[i].h = 20;
        g_font.glyph[i].x_offset = -2;
        g_font.glyph[i].advance = 10;
    }
    g_font.line_height = 20;
    g_font.solid_u = g_font.solid_v = 0.01f;
}

static md_list list(unsigned cap)
{
    md_list l;
    l.v = g_vertices;
    l.count = 0;
    l.cap = cap;
    l.dropped = 0;
    return l;
}

static unsigned g_wrap_lines;
static size_t g_wrap_len[4];

static void wrap_line(void *ctx, const char *line, size_t len, unsigned index)
{
    (void)ctx;
    (void)line;
    if (index < 4) {
        g_wrap_len[index] = len;
    }
    ++g_wrap_lines;
}

static void test_text(void)
{
    md_list l = list(6000);
    float end;

    make_font();
    CHECK(md_text_width(&g_font, "abc") == 30);
    CHECK(md_text_width(&g_font, "") == 0);
    CHECK(md_text_width(&g_font, NULL) == 0);

    end = md_text(&l, &g_font, 100, 50, "ab cd", 0xFFFFFFFFul, 0);
    CHECK(end == 150);
    CHECK_EQ_U(l.count, 4 * 6); /* the space draws nothing */
    CHECK(l.v[0].x == 98 && l.v[0].y == 50); /* the cell starts left of the pen */
    CHECK(l.v[4].x == 112 && l.v[4].y == 70);
    CHECK(l.v[6].x == 108);                   /* second glyph, 10 further */
    CHECK(l.v[0].u == 0.1f && l.v[4].v == 0.2f);

    /* too long: cut, with three dots, never wider than allowed */
    l = list(6000);
    end = md_text(&l, &g_font, 0, 0, "abcdefghij", 0xFFFFFFFFul, 65);
    CHECK(end <= 65);
    CHECK_EQ_U(l.count, 6 * 6); /* three letters and three dots */
    /* characters outside the font become question marks instead of garbage */
    l = list(6000);
    md_text(&l, &g_font, 0, 0, "\x01\xE9", 0xFFFFFFFFul, 0);
    CHECK_EQ_U(l.count, 2 * 6);

    md_rect(&l, &g_font, 5, 6, 7, 8, 0x80000000ul);
    CHECK(l.v[12].x == 5 && l.v[12].y == 6 && l.v[16].x == 12 && l.v[16].y == 14);
    CHECK(l.v[12].u == 0.01f && l.v[16].u == 0.01f && l.v[12].color == 0x80000000ul);
    md_rect(&l, &g_font, 0, 0, 0, 5, 0); /* nothing to draw */
    CHECK_EQ_U(l.count, 3 * 6);

    /* a full list counts what it could not take */
    l = list(12);
    md_text(&l, &g_font, 0, 0, "abcd", 0xFFFFFFFFul, 0);
    CHECK_EQ_U(l.count, 12);
    CHECK_EQ_U(l.dropped, 2);

    /* wrapping at spaces: 10 px per character, 100 px lines */
    g_wrap_lines = 0;
    CHECK_EQ_U(md_wrap(&g_font, "aaaa bbbb cccc dddd", 100, 4, wrap_line, NULL), 2);
    CHECK_EQ_U(g_wrap_len[0], 9); /* "aaaa bbbb" */
    CHECK_EQ_U(g_wrap_len[1], 9);
    CHECK_EQ_U(md_wrap(&g_font, "aaaa bbbb cccc dddd", 100, 1, wrap_line, NULL), 1); /* no more lines than allowed */
    CHECK_EQ_U(md_wrap(&g_font, "short", 100, 4, wrap_line, NULL), 1);
    CHECK_EQ_U(md_wrap(&g_font, "", 100, 4, wrap_line, NULL), 0);
    CHECK_EQ_U(md_wrap(&g_font, "aaaaaaaaaaaaaaaaaaaaaaaaa", 100, 4, NULL, NULL), 3); /* one long word is cut */
}

/* How many vertices a line of text takes. */
static unsigned drawn(const char *text)
{
    md_list l = list(2000);
    md_text(&l, &g_font, 0, 0, text, 0xFFFFFFFFul, 0);
    return l.count;
}

static void test_menu_layout(void)
{
    md_list l = list(6000);
    const float w = 1920, h = 1080;
    unsigned i, rows;
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;

    make_font();
    build();

    md_menu(&l, &g_font, &g_menu, w, h, "hints"); /* closed: nothing */
    CHECK_EQ_U(l.count, 0);

    mm_open(&g_menu, 1);
    rows = md_visible_rows(&g_font, &g_menu, h);
    CHECK_EQ_U(rows, 8); /* everything fits on a 1080p screen */
    mm_scroll_into_view(&g_menu, rows);
    md_menu(&l, &g_font, &g_menu, w, h, "hints");
    CHECK(l.count > 100 * 6 / 6 && l.count % 6 == 0);
    CHECK_EQ_U(l.dropped, 0);
    for (i = 0; i < l.count; ++i) {
        if (l.v[i].x < min_x) {
            min_x = l.v[i].x;
        }
        if (l.v[i].x > max_x) {
            max_x = l.v[i].x;
        }
        if (l.v[i].y < min_y) {
            min_y = l.v[i].y;
        }
        if (l.v[i].y > max_y) {
            max_y = l.v[i].y;
        }
    }
    CHECK(min_x >= 0 && max_x <= w && min_y >= 0 && max_y <= h); /* all on screen */
    CHECK(max_x - min_x <= 30 * 20 + 1);                           /* the panel: 30 lines wide */
    /* the first rectangle is the panel itself and everything else lies inside it */
    CHECK(l.v[0].x == min_x && l.v[0].y == min_y && l.v[4].x == max_x && l.v[4].y == max_y);

    /* a small screen: fewer rows, still on screen */
    l = list(6000);
    rows = md_visible_rows(&g_font, &g_menu, 300);
    CHECK(rows >= 1 && rows < 8);
    g_menu.selected = 7;
    mm_scroll_into_view(&g_menu, rows);
    md_menu(&l, &g_font, &g_menu, 400, 300, "hints");
    for (i = 0; i < l.count; ++i) {
        CHECK(l.v[i].x >= 0 && l.v[i].x <= 400 && l.v[i].y >= -1 && l.v[i].y <= 301);
        if (g_failures) {
            break;
        }
    }
    CHECK(l.count > 0 && l.dropped == 0);

    /* a setting waiting for a restart says so: in its row and in the line under the help text */
    {
        unsigned plain, waiting, at_once;

        build();
        mm_open(&g_menu, 1); /* on the switch, which needs a restart */
        l = list(6000);
        md_menu(&l, &g_font, &g_menu, w, h, "hints");
        plain = l.count;
        mm_handle(&g_menu, MM_IN_ACCEPT);
        l = list(6000);
        md_menu(&l, &g_font, &g_menu, w, h, "hints");
        waiting = l.count;
        /* the row says so, and the note under the help text names the value in use */
        CHECK_EQ_U(waiting, plain - drawn("Off") + drawn("On (restart)") -
                                drawn("A change here applies after a restart of the game.") +
                                drawn("In use now: Off. Restart the game to change it."));
        CHECK(waiting != plain);
        mm_handle(&g_menu, MM_IN_ACCEPT); /* back to the value in use */
        l = list(6000);
        md_menu(&l, &g_font, &g_menu, w, h, "hints");
        CHECK_EQ_U(l.count, plain);
        mm_handle(&g_menu, MM_IN_DOWN); /* a choice that applies at once: changing it adds no note */
        l = list(6000);
        md_menu(&l, &g_font, &g_menu, w, h, "hints");
        at_once = l.count;
        mm_handle(&g_menu, MM_IN_LEFT); /* "medium" -> "low" and nothing else */
        l = list(6000);
        md_menu(&l, &g_font, &g_menu, w, h, "hints");
        CHECK_EQ_U(l.count, at_once - drawn("medium") + drawn("low"));
    }

    /* no font yet, or no menu: draws nothing rather than crashing */
    l = list(6000);
    md_menu(&l, NULL, &g_menu, w, h, "hints");
    md_menu(&l, &g_font, NULL, w, h, "hints");
    CHECK_EQ_U(l.count, 0);
}

static void test_pad_combinations(void)
{
    unsigned mask = 0xDEAD;

    CHECK(mm_pad_combo_from_name("L3+R3", &mask) == 1);
    CHECK_EQ_U(mask, MM_PAD_L3 | MM_PAD_R3);
    CHECK(mm_pad_combo_from_name("back + start", &mask) == 1);
    CHECK_EQ_U(mask, MM_PAD_BACK | MM_PAD_START);
    CHECK(mm_pad_combo_from_name("  Select+Menu ", &mask) == 1); /* other names for the same two */
    CHECK_EQ_U(mask, MM_PAD_BACK | MM_PAD_START);
    CHECK(mm_pad_combo_from_name("LB+RB+Y", &mask) == 1);
    CHECK_EQ_U(mask, MM_PAD_LB | MM_PAD_RB | MM_PAD_Y);
    CHECK(mm_pad_combo_from_name("View+Down", &mask) == 1);
    CHECK_EQ_U(mask, MM_PAD_BACK | MM_PAD_DOWN);
    /* the values are the ones XInput reports */
    CHECK_EQ_U(MM_PAD_L3 | MM_PAD_R3, 0x00C0);
    CHECK_EQ_U(MM_PAD_A | MM_PAD_B | MM_PAD_X | MM_PAD_Y, 0xF000);

    /* switched off */
    mask = 0xDEAD;
    CHECK(mm_pad_combo_from_name("None", &mask) == 1);
    CHECK_EQ_U(mask, 0);
    mask = 0xDEAD;
    CHECK(mm_pad_combo_from_name(" off", &mask) == 1 && mask == 0);

    /* not understood: the caller keeps what it had */
    mask = 0xDEAD;
    CHECK(mm_pad_combo_from_name("L3", &mask) == 0);       /* one button would fire in play */
    CHECK(mm_pad_combo_from_name("L3+L3", &mask) == 0);    /* still one button */
    CHECK(mm_pad_combo_from_name("L3+", &mask) == 0);
    CHECK(mm_pad_combo_from_name("+R3", &mask) == 0);
    CHECK(mm_pad_combo_from_name("L3 R3", &mask) == 0);
    CHECK(mm_pad_combo_from_name("L3+Banana", &mask) == 0);
    CHECK(mm_pad_combo_from_name("LeftShoulderButton+R3", &mask) == 0);
    CHECK(mm_pad_combo_from_name("", &mask) == 0);
    CHECK(mm_pad_combo_from_name(NULL, &mask) == 0);
    CHECK(mm_pad_combo_from_name("L3+R3", NULL) == 0);
    CHECK_EQ_U(mask, 0xDEAD);
}

void suite_menu(void)
{
    RUN(test_navigation);
    RUN(test_values);
    RUN(test_scrolling);
    RUN(test_repeat_and_chord);
    RUN(test_key_names);
    RUN(test_pad_combinations);
    RUN(test_text);
    RUN(test_menu_layout);
}
