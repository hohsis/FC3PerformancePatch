/* Unit tests: finding the game's input-mode object, and deciding when to
 * switch between gamepad and keyboard/mouse.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "padmode.h"
#include "test.h"

/* ---- code as the compilers left it ------------------------------------------------ */

/* Far Cry 3: mov cl,[eax+0D0h] / mov eax,[ptr] / mov [eax+31h],cl / mov byte [eax+32h],1 / ret */
static const unsigned char INLINE_CODE[] = {0x8A, 0x88, 0xD0, 0x00, 0x00, 0x00, 0xA1, 0x48, 0x9B, 0x82,
                                            0x12, 0x88, 0x48, 0x31, 0xC6, 0x40, 0x32, 0x01, 0xC3};
/* Blood Dragon: movzx eax,byte [ecx+0D0h] / mov ecx,[ptr] / push eax / call setter */
static const unsigned char CALL_CODE[] = {0x0F, 0xB6, 0x81, 0xD0, 0x00, 0x00, 0x00, 0x8B, 0x0D, 0x80,
                                          0x15, 0x87, 0x11, 0x50, 0xE8, 0x00, 0x00, 0x00, 0x00};
static const unsigned char SETTER_CODE[] = {0x55, 0x8B, 0xEC, 0x8A, 0x45, 0x08, 0x88, 0x41, 0x31,
                                            0xC6, 0x41, 0x32, 0x01, 0x5D, 0xC2, 0x04, 0x00};

static unsigned char g_text[1024];

static void fill(void)
{
    memset(g_text, 0xCC, sizeof(g_text));
}

static void put_call(size_t at, size_t target)
{
    long rel = (long)target - (long)(at + sizeof(CALL_CODE));
    memcpy(g_text + at, CALL_CODE, sizeof(CALL_CODE));
    g_text[at + 15] = (unsigned char)rel;
    g_text[at + 16] = (unsigned char)(rel >> 8);
    g_text[at + 17] = (unsigned char)(rel >> 16);
    g_text[at + 18] = (unsigned char)(rel >> 24);
}

static void test_find_inline(void)
{
    unsigned long pointer = 0;
    const char *variant = NULL;

    fill();
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, &variant) == 0);

    memcpy(g_text + 100, INLINE_CODE, sizeof(INLINE_CODE));
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, &variant) == 1);
    CHECK_EQ_U(pointer, 0x12829B48ul);
    CHECK_STR(variant, "release");

    /* the same pointer used in a second copy of the code is still one answer */
    memcpy(g_text + 400, INLINE_CODE, sizeof(INLINE_CODE));
    pointer = 0;
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == 1);
    CHECK_EQ_U(pointer, 0x12829B48ul);

    /* two different pointers: refuse to guess */
    g_text[400 + 7] ^= 0x10;
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == -1);

    /* at the very end of the section */
    fill();
    memcpy(g_text + sizeof(g_text) - sizeof(INLINE_CODE), INLINE_CODE, sizeof(INLINE_CODE));
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == 1);

    /* another offset in the object is another piece of code */
    fill();
    memcpy(g_text + 100, INLINE_CODE, sizeof(INLINE_CODE));
    g_text[100 + 13] = 0x30;
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == 0);

    CHECK(pm_find_manager(NULL, 10, 0, &pointer, NULL) == 0);
    CHECK(pm_find_manager(g_text, 0, 0, &pointer, NULL) == 0);
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0, NULL, NULL) == 0);
}

static void test_find_call(void)
{
    unsigned long pointer = 0;
    const char *variant = NULL;

    /* the call leads to the setter, behind it ... */
    fill();
    memcpy(g_text + 600, SETTER_CODE, sizeof(SETTER_CODE));
    put_call(200, 600);
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, &variant) == 1);
    CHECK_EQ_U(pointer, 0x11871580ul);
    CHECK_STR(variant, "release-size");

    /* ... or before it */
    fill();
    memcpy(g_text + 40, SETTER_CODE, sizeof(SETTER_CODE));
    put_call(700, 40);
    pointer = 0;
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == 1);
    CHECK_EQ_U(pointer, 0x11871580ul);

    /* a call to something else does not count */
    fill();
    memcpy(g_text + 600, SETTER_CODE, sizeof(SETTER_CODE));
    put_call(200, 601);
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == 0);

    /* nor one that leaves the section */
    fill();
    put_call(200, 600);
    g_text[200 + 17] = 0x40; /* far beyond the end */
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == 0);
    put_call(200, 0);
    g_text[200 + 15] = 0x00;
    g_text[200 + 16] = 0x00;
    g_text[200 + 17] = 0x00;
    g_text[200 + 18] = 0x80; /* far before the start */
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == 0);
    put_call(200, sizeof(g_text) - 4); /* the setter would run past the end */
    CHECK(pm_find_manager(g_text, sizeof(g_text), 0x10001000ul, &pointer, NULL) == 0);
}

/* ---- deciding ------------------------------------------------------------------------ */

static void test_gamepad_in_use(void)
{
    CHECK(!pm_gamepad_active(0, 0, 0, 0, 0, 0, 0));
    CHECK(pm_gamepad_active(0x1000, 0, 0, 0, 0, 0, 0));
    CHECK(!pm_gamepad_active(0, PM_TRIGGER_THRESHOLD, PM_TRIGGER_THRESHOLD, 0, 0, 0, 0));
    CHECK(pm_gamepad_active(0, PM_TRIGGER_THRESHOLD + 1, 0, 0, 0, 0, 0));
    CHECK(pm_gamepad_active(0, 0, 255, 0, 0, 0, 0));
    /* a worn stick that rests off centre is not "in use" */
    CHECK(!pm_gamepad_active(0, 0, 0, 6000, -7000, 4000, -PM_STICK_THRESHOLD));
    CHECK(pm_gamepad_active(0, 0, 0, PM_STICK_THRESHOLD + 1, 0, 0, 0));
    CHECK(pm_gamepad_active(0, 0, 0, 0, -32768, 0, 0));
    CHECK(pm_gamepad_active(0, 0, 0, 0, 0, 0, 32767));
    CHECK(pm_gamepad_active(0, 0, 0, 0, 0, -20000, 0));
}

static void test_switching(void)
{
    pm_state s;
    unsigned long t = 1000;

    pm_reset(&s);
    CHECK(s.mode == PM_UNKNOWN);
    CHECK(pm_note_gamepad(&s, 0, t) == PM_UNKNOWN);  /* a pad at rest says nothing */
    CHECK(pm_note_keyboard(&s, 0, t) == PM_UNKNOWN);
    CHECK(pm_note_gamepad(&s, 1, t) == PM_GAMEPAD);  /* the first thing touched decides at once */
    CHECK(pm_note_gamepad(&s, 1, t + 16) == PM_UNKNOWN); /* already there */
    CHECK_EQ_U(s.switches, 1);

    /* a key while the pad is still in use: the pad keeps the mode */
    CHECK(pm_note_keyboard(&s, 1, t + 32) == PM_UNKNOWN);
    CHECK(pm_note_gamepad(&s, 1, t + 300) == PM_UNKNOWN);
    CHECK(pm_note_keyboard(&s, 1, t + 300 + PM_SETTLE_MS - 1) == PM_UNKNOWN);
    /* the pad has been quiet for long enough */
    CHECK(pm_note_keyboard(&s, 1, t + 300 + PM_SETTLE_MS) == PM_KEYBOARD_MOUSE);
    CHECK(s.mode == PM_KEYBOARD_MOUSE);
    CHECK_EQ_U(s.switches, 2);

    /* and back, with the same patience */
    t += 5000;
    CHECK(pm_note_keyboard(&s, 1, t) == PM_UNKNOWN);
    CHECK(pm_note_gamepad(&s, 1, t + 100) == PM_UNKNOWN);
    CHECK(pm_note_gamepad(&s, 1, t + PM_SETTLE_MS) == PM_GAMEPAD);
    CHECK_EQ_U(s.switches, 3);

    /* the mode was changed elsewhere (the game's options menu): follow it */
    s.mode = PM_KEYBOARD_MOUSE;
    CHECK(pm_note_gamepad(&s, 1, t + 10000) == PM_GAMEPAD);
}

static void test_mouse(void)
{
    pm_state s;
    unsigned long t = 50000;
    int i;

    pm_reset(&s);
    CHECK(pm_note_gamepad(&s, 1, t) == PM_GAMEPAD);
    t += 2000;

    /* a mouse on the desk: a count now and then never adds up */
    for (i = 0; i < 40; ++i) {
        CHECK(pm_note_mouse(&s, i & 1 ? 1 : -1, 0, t + (unsigned long)i * 100) == PM_UNKNOWN);
    }
    t += 10000;
    /* a hand on it: enough travel within the window */
    CHECK(pm_note_mouse(&s, 5, 0, t) == PM_UNKNOWN);
    CHECK(pm_note_mouse(&s, -5, 0, t + 16) == PM_UNKNOWN);
    CHECK(pm_note_mouse(&s, 5, 0, t + 32) == PM_KEYBOARD_MOUSE);
    CHECK(s.mode == PM_KEYBOARD_MOUSE);

    /* a button needs no travel */
    pm_reset(&s);
    CHECK(pm_note_gamepad(&s, 1, t) == PM_GAMEPAD);
    CHECK(pm_note_mouse(&s, 0, 1, t + PM_SETTLE_MS) == PM_KEYBOARD_MOUSE);

    /* a nudge as the hand leaves for the pad does not switch back */
    t += 5000;
    CHECK(pm_note_gamepad(&s, 1, t) == PM_GAMEPAD);
    CHECK(pm_note_mouse(&s, 200, 0, t + 50) == PM_UNKNOWN);
    CHECK(s.mode == PM_GAMEPAD);
}

static void test_clock_wraps(void)
{
    pm_state s;
    unsigned long t = 0xFFFFFF00ul; /* GetTickCount shortly before it wraps */

    pm_reset(&s);
    CHECK(pm_note_gamepad(&s, 1, t) == PM_GAMEPAD);
    CHECK(pm_note_keyboard(&s, 1, t + 0x80) == PM_UNKNOWN);
    CHECK(pm_note_keyboard(&s, 1, (t + 0x100 + PM_SETTLE_MS) & 0xFFFFFFFFul) == PM_KEYBOARD_MOUSE);
}

void suite_padmode(void)
{
    RUN(test_find_inline);
    RUN(test_find_call);
    RUN(test_gamepad_in_use);
    RUN(test_switching);
    RUN(test_mouse);
    RUN(test_clock_wraps);
}
