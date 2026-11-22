/* Fullscreen guard state machine. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "fs_guard.h"

#include <string.h>

void fs_guard_reset(fs_guard *g)
{
    memset(g, 0, sizeof(*g));
}

static fs_decision decide(int report, int restore, fs_event event)
{
    fs_decision d;
    d.report_fullscreen = report;
    d.restore = restore;
    d.event = event;
    return d;
}

fs_decision fs_guard_step(fs_guard *g, int real_fullscreen, int active, unsigned long now_ms)
{
    fs_event event = FS_EV_NONE;
    int restore = 0;

    if (real_fullscreen) {
        if (g->lost) {
            event = FS_EV_RESTORED;
        }
        g->was_fullscreen = 1;
        g->lost = 0;
        g->attempts = 0;
        return decide(1, 0, event);
    }
    if (!g->was_fullscreen) {
        return decide(0, 0, FS_EV_NONE); /* windowed all along */
    }
    if (!g->lost) {
        if (active) {
            /* The window has focus and fullscreen ended anyway: Alt+Enter or
             * the game's own options menu. Not ours to undo. */
            g->was_fullscreen = 0;
            return decide(0, 0, FS_EV_USER_WINDOWED);
        }
        g->lost = 1;
        g->attempts = 0;
        event = FS_EV_LOST;
    }
    if (active) {
        if (g->attempts >= FS_GUARD_MAX_ATTEMPTS) {
            g->lost = 0;
            g->was_fullscreen = 0;
            return decide(0, 0, FS_EV_GAVE_UP);
        }
        /* Unsigned subtraction keeps this correct across a clock wrap. */
        if (g->attempts == 0 || now_ms - g->last_attempt_ms >= FS_GUARD_RETRY_MS) {
            restore = 1;
        }
    }
    return decide(1, restore, event);
}

void fs_guard_attempted(fs_guard *g, unsigned long now_ms)
{
    ++g->attempts;
    g->last_attempt_ms = now_ms;
}
