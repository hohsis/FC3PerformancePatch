/* Which threads of the game use the processor, and where they run.
 *
 * A thread of the patch's own looks at the process's threads every half
 * minute (their processor time, their affinity, the module they started in)
 * and keeps two results: the whole session so far, and the half minute in
 * which the process was busiest. Both are written to the log when the game
 * exits; a measurement over a chosen time can be asked for as well (the
 * in-game menu does). Hooks that the game's own threads pass through say which
 * thread presents the picture and which reads the input, and on which
 * processor they were at that moment.
 *
 * The reason for it: the engine pins its workers and leaves its main and
 * render threads to the system. Whether those then run on the fastest cores of
 * a processor whose cores differ (ARM handhelds) can only be seen on the
 * device, and the log is what comes back from there.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_THREADS_H
#define FC3PP_THREADS_H

#include <windows.h>

/* Call once, on the process's first thread. period_seconds: time between two
 * looks (0 = the default, 30). Does nothing when not enabled. */
void threads_start(int enabled, unsigned period_seconds);

/* The game asked for a thread to run on these real processors; all = it asked
 * for "all processors". */
void threads_note_affinity(DWORD thread_id, unsigned long real_mask, int all);

/* Called on a thread of the game, from a hook it passes through regularly. */
typedef enum threads_place { THREADS_AT_PRESENT = 0, THREADS_AT_INPUT = 1, THREADS_PLACES = 2 } threads_place;
void threads_note_here(threads_place place);

/* Measures for this many seconds from now and writes the result to the log.
 * Returns 0 when that is not possible (not started, or one is running). */
int threads_measure(unsigned seconds);

/* Seconds left of a running measurement; 0 when none is running. */
unsigned threads_measuring(void);

/* Writes both results to the log. Takes no locks. */
void threads_log_stats(void);

#endif /* FC3PP_THREADS_H */
