/* Unit tests: fullscreen guard state machine.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fs_guard.h"
#include "test.h"

#define ACTIVE 1
#define INACTIVE 0
#define FS 1
#define WINDOWED 0

static void test_windowed_game_is_left_alone(void)
{
    fs_guard g;
    fs_decision d;
    int i;

    fs_guard_reset(&g);
    for (i = 0; i < 5; ++i) {
        d = fs_guard_step(&g, WINDOWED, i & 1, 100ul * (unsigned long)i);
        CHECK(d.report_fullscreen == 0);
        CHECK(d.restore == 0);
        CHECK(d.event == FS_EV_NONE);
    }
}

static void test_fullscreen_passes_through(void)
{
    fs_guard g;
    fs_decision d;

    fs_guard_reset(&g);
    d = fs_guard_step(&g, FS, ACTIVE, 0);
    CHECK(d.report_fullscreen == 1 && d.restore == 0 && d.event == FS_EV_NONE);
    d = fs_guard_step(&g, FS, INACTIVE, 16);
    CHECK(d.report_fullscreen == 1 && d.restore == 0 && d.event == FS_EV_NONE);
}

/* The bug this exists for: Alt+Tab away and back. */
static void test_alt_tab_round_trip(void)
{
    fs_guard g;
    fs_decision d;

    fs_guard_reset(&g);
    fs_guard_step(&g, FS, ACTIVE, 0);

    /* Focus goes elsewhere and Windows drops fullscreen. */
    d = fs_guard_step(&g, WINDOWED, INACTIVE, 100);
    CHECK(d.report_fullscreen == 1); /* hidden from the engine */
    CHECK(d.restore == 0);           /* nothing to do while in the background */
    CHECK(d.event == FS_EV_LOST);

    d = fs_guard_step(&g, WINDOWED, INACTIVE, 5000);
    CHECK(d.report_fullscreen == 1 && d.restore == 0 && d.event == FS_EV_NONE);

    /* The user comes back. */
    d = fs_guard_step(&g, WINDOWED, ACTIVE, 9000);
    CHECK(d.report_fullscreen == 1);
    CHECK(d.restore == 1);
    fs_guard_attempted(&g, 9000);

    d = fs_guard_step(&g, FS, ACTIVE, 9016);
    CHECK(d.report_fullscreen == 1 && d.restore == 0);
    CHECK(d.event == FS_EV_RESTORED);

    /* Back to normal: a second round trip works the same way. */
    d = fs_guard_step(&g, WINDOWED, INACTIVE, 20000);
    CHECK(d.event == FS_EV_LOST && d.report_fullscreen == 1);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, 21000);
    CHECK(d.restore == 1);
}

/* Windows itself may put the swap chain back; that needs no request. */
static void test_restored_by_the_system(void)
{
    fs_guard g;
    fs_decision d;

    fs_guard_reset(&g);
    fs_guard_step(&g, FS, ACTIVE, 0);
    fs_guard_step(&g, WINDOWED, INACTIVE, 100);
    d = fs_guard_step(&g, FS, ACTIVE, 200);
    CHECK(d.report_fullscreen == 1 && d.restore == 0 && d.event == FS_EV_RESTORED);
    CHECK(g.lost == 0 && g.attempts == 0);
}

/* Alt+Enter (or the options menu) with the window focused is a real choice. */
static void test_user_toggle_is_respected(void)
{
    fs_guard g;
    fs_decision d;

    fs_guard_reset(&g);
    fs_guard_step(&g, FS, ACTIVE, 0);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, 100);
    CHECK(d.report_fullscreen == 0);
    CHECK(d.restore == 0);
    CHECK(d.event == FS_EV_USER_WINDOWED);

    /* Losing focus afterwards must not bring fullscreen back. */
    d = fs_guard_step(&g, WINDOWED, INACTIVE, 200);
    CHECK(d.report_fullscreen == 0 && d.restore == 0 && d.event == FS_EV_NONE);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, 300);
    CHECK(d.report_fullscreen == 0 && d.restore == 0 && d.event == FS_EV_NONE);

    /* Toggling back to fullscreen re-arms the guard. */
    d = fs_guard_step(&g, FS, ACTIVE, 400);
    CHECK(d.report_fullscreen == 1);
    d = fs_guard_step(&g, WINDOWED, INACTIVE, 500);
    CHECK(d.report_fullscreen == 1 && d.event == FS_EV_LOST);
}

static void test_retries_are_rate_limited(void)
{
    fs_guard g;
    fs_decision d;
    unsigned long t = 1000;

    fs_guard_reset(&g);
    fs_guard_step(&g, FS, ACTIVE, 0);
    fs_guard_step(&g, WINDOWED, INACTIVE, 100);

    d = fs_guard_step(&g, WINDOWED, ACTIVE, t);
    CHECK(d.restore == 1);
    fs_guard_attempted(&g, t);

    /* Polled every frame: no new attempt until the retry interval passed. */
    d = fs_guard_step(&g, WINDOWED, ACTIVE, t + 16);
    CHECK(d.restore == 0 && d.report_fullscreen == 1);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, t + FS_GUARD_RETRY_MS - 1);
    CHECK(d.restore == 0);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, t + FS_GUARD_RETRY_MS);
    CHECK(d.restore == 1);
}

static void test_gives_up_after_repeated_failures(void)
{
    fs_guard g;
    fs_decision d;
    unsigned long t = 0;
    unsigned attempts = 0;
    int guard = 0;

    fs_guard_reset(&g);
    fs_guard_step(&g, FS, ACTIVE, t);
    fs_guard_step(&g, WINDOWED, INACTIVE, t);

    for (;;) {
        t += FS_GUARD_RETRY_MS;
        d = fs_guard_step(&g, WINDOWED, ACTIVE, t);
        if (d.event == FS_EV_GAVE_UP || ++guard > 1000) {
            break;
        }
        CHECK(d.report_fullscreen == 1);
        if (d.restore) {
            fs_guard_attempted(&g, t);
            ++attempts;
        }
    }
    CHECK(d.event == FS_EV_GAVE_UP);
    CHECK(d.report_fullscreen == 0); /* the engine finally hears the truth */
    CHECK(d.restore == 0);
    CHECK_EQ_U(attempts, FS_GUARD_MAX_ATTEMPTS);

    /* ... and keeps hearing it. */
    d = fs_guard_step(&g, WINDOWED, ACTIVE, t + 10000);
    CHECK(d.report_fullscreen == 0 && d.restore == 0 && d.event == FS_EV_NONE);
}

/* Focus flickering while the drop is hidden must not confuse the state. */
static void test_focus_changes_while_lost(void)
{
    fs_guard g;
    fs_decision d;

    fs_guard_reset(&g);
    fs_guard_step(&g, FS, ACTIVE, 0);
    fs_guard_step(&g, WINDOWED, INACTIVE, 10);

    d = fs_guard_step(&g, WINDOWED, ACTIVE, 20);
    CHECK(d.restore == 1);
    fs_guard_attempted(&g, 20);
    d = fs_guard_step(&g, WINDOWED, INACTIVE, 30); /* focus gone again before it took effect */
    CHECK(d.report_fullscreen == 1 && d.restore == 0 && d.event == FS_EV_NONE);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, 30 + FS_GUARD_RETRY_MS);
    CHECK(d.report_fullscreen == 1 && d.restore == 1);
}

static void test_clock_wraparound(void)
{
    fs_guard g;
    fs_decision d;
    unsigned long t = 0xFFFFFFF0ul;

    fs_guard_reset(&g);
    fs_guard_step(&g, FS, ACTIVE, t);
    fs_guard_step(&g, WINDOWED, INACTIVE, t);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, t);
    CHECK(d.restore == 1);
    fs_guard_attempted(&g, t);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, (t + 100) & 0xFFFFFFFFul); /* wrapped, 100 ms later */
    CHECK(d.restore == 0);
    d = fs_guard_step(&g, WINDOWED, ACTIVE, (t + FS_GUARD_RETRY_MS + 5) & 0xFFFFFFFFul);
    CHECK(d.restore == 1);
}

void suite_fs_guard(void)
{
    RUN(test_windowed_game_is_left_alone);
    RUN(test_fullscreen_passes_through);
    RUN(test_alt_tab_round_trip);
    RUN(test_restored_by_the_system);
    RUN(test_user_toggle_is_respected);
    RUN(test_retries_are_rate_limited);
    RUN(test_gives_up_after_repeated_failures);
    RUN(test_focus_changes_while_lost);
    RUN(test_clock_wraparound);
}
