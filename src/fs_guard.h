/* Fullscreen guard: decides what the engine is told about the swap chain's
 * fullscreen state. Pure C, no OS calls.
 *
 * Background. Once per frame the DX11 renderer asks the swap chain whether it
 * is still fullscreen and, when the answer differs from its own flag, adopts
 * the answer: it calls SetFullscreenState with it and overwrites its
 * Fullscreen setting. That is how Alt+Enter is supported. But Windows also
 * takes a swap chain out of exclusive fullscreen when the window loses focus
 * (Alt+Tab, the Windows key, a notification stealing focus). The engine treats
 * that like a user toggle, so the game comes back as a window and stays one.
 *
 * The guard tells the two cases apart by whether the game window was the
 * foreground window when fullscreen went away. A loss without focus is hidden
 * from the engine (it keeps hearing "fullscreen") and fullscreen is requested
 * again once the window is back in the foreground.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_FS_GUARD_H
#define FC3PP_FS_GUARD_H

/* Restore attempts are spaced out; after this many failures the guard gives
 * up and tells the engine the truth (the game then continues windowed, as it
 * would without the patch). */
#define FS_GUARD_RETRY_MS 250ul
#define FS_GUARD_MAX_ATTEMPTS 20u

typedef enum fs_event {
    FS_EV_NONE = 0,
    FS_EV_LOST,          /* fullscreen was taken away while the window was not in the foreground */
    FS_EV_RESTORED,      /* fullscreen is back after such a loss */
    FS_EV_USER_WINDOWED, /* fullscreen ended while the window had focus: a real toggle */
    FS_EV_GAVE_UP        /* restoring kept failing */
} fs_event;

typedef struct fs_guard {
    int was_fullscreen;            /* the game has been fullscreen and still means to be */
    int lost;                      /* currently hiding a focus-loss drop from the engine */
    unsigned attempts;             /* restore attempts since the drop */
    unsigned long last_attempt_ms; /* time of the latest attempt */
} fs_guard;

typedef struct fs_decision {
    int report_fullscreen; /* what the engine is told */
    int restore;           /* non-zero: ask the swap chain for fullscreen now */
    fs_event event;        /* state change worth logging */
} fs_decision;

void fs_guard_reset(fs_guard *g);

/* One poll. real_fullscreen: the swap chain's actual state. active: the game
 * window is the foreground window. now_ms: a monotonic millisecond clock
 * (wrap-around is handled). */
fs_decision fs_guard_step(fs_guard *g, int real_fullscreen, int active, unsigned long now_ms);

/* Call after acting on fs_decision.restore, whatever the outcome. Success is
 * observed through the next step's real_fullscreen. */
void fs_guard_attempted(fs_guard *g, unsigned long now_ms);

#endif /* FC3PP_FS_GUARD_H */
