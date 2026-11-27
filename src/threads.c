/* Which threads of the game use the processor, and where they run.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "threads.h"

#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>

#include "log.h"

#define MAX_THREADS 384  /* threads looked at per pass */
#define MAX_KNOWN 512    /* threads whose module is remembered */
#define MAX_PLACED 96    /* threads the game placed */
#define REPORT_ROWS 12   /* threads listed per result */
#define MAX_CPUS 64
#define DEFAULT_PERIOD_S 30u
#define MODULE_CHARS 24

typedef LONG(WINAPI *nt_query_thread_fn)(HANDLE, int, void *, ULONG, ULONG *);

typedef struct thread_basic_info { /* THREAD_BASIC_INFORMATION, 32-bit layout */
    LONG exit_status;
    void *teb;
    void *process_id, *thread_id;
    ULONG_PTR affinity;
    LONG priority, base_priority;
} thread_basic_info;

typedef struct seen_thread {
    DWORD tid;
    unsigned long long cpu; /* processor time so far, in 100 ns units */
    unsigned long affinity;
} seen_thread;

typedef struct pass {
    DWORD tick;
    unsigned count;
    unsigned long long total;
    seen_thread t[MAX_THREADS];
} pass;

typedef struct row {
    DWORD tid;
    unsigned permille; /* of one processor */
    unsigned long affinity;
} row;

typedef struct result {
    unsigned valid;
    unsigned seconds;        /* length of the time it covers */
    unsigned permille_total; /* the whole process, of one processor */
    unsigned threads;        /* threads that existed at its end */
    unsigned rows;
    row r[REPORT_ROWS];
} result;

typedef struct published {
    result session, busiest;
} published;

/* A thread the game's own code passes through one of the patch's hooks with. */
typedef struct noted {
    volatile LONG tid;
    volatile LONG calls;
    volatile LONG on_cpu[MAX_CPUS];
} noted;

typedef struct placed {
    volatile LONG tid;
    volatile LONG mask;
    volatile LONG all;
} placed;

typedef struct known {
    DWORD tid;
    char module[MODULE_CHARS];
} known;

static int g_enabled;
static DWORD g_first_tid, g_own_tid;
static DWORD g_start_tick;
static unsigned g_period_ms;
static HANDLE g_wake;
static nt_query_thread_fn g_nt_query_thread;

static published g_published[2];
static volatile LONG g_current = -1; /* index into g_published; -1 = nothing yet */

static noted g_noted[THREADS_PLACES];
static placed g_placed[MAX_PLACED];
static known g_known[MAX_KNOWN]; /* filled by the sampling thread, read at exit */
static volatile LONG g_known_count;

static volatile LONG g_measure_seconds; /* asked for */
static volatile LONG g_measure_until;   /* tick at which the running one ends; 0 = none */

/* ---- what the game's threads tell us ------------------------------------------------ */

void threads_note_affinity(DWORD tid, unsigned long real_mask, int all)
{
    unsigned i;

    if (!g_enabled || !tid) {
        return;
    }
    for (i = 0; i < MAX_PLACED; ++i) {
        LONG have = g_placed[i].tid;
        if (have == 0) {
            /* a free entry: take it, unless another thread just did */
            have = InterlockedCompareExchange(&g_placed[i].tid, (LONG)tid, 0);
            if (have == 0) {
                have = (LONG)tid;
            }
        }
        if (have == (LONG)tid) {
            InterlockedExchange(&g_placed[i].mask, (LONG)real_mask);
            InterlockedExchange(&g_placed[i].all, all ? 1 : 0);
            return;
        }
    }
}

void threads_note_here(threads_place place)
{
    noted *n;
    DWORD cpu;

    if (!g_enabled || place >= THREADS_PLACES) {
        return;
    }
    n = &g_noted[place];
    if (n->tid != (LONG)GetCurrentThreadId()) {
        /* The thread that does this changed (or this is the first time): start again. */
        if (InterlockedExchange(&n->tid, (LONG)GetCurrentThreadId()) != (LONG)GetCurrentThreadId()) {
            unsigned i;
            for (i = 0; i < MAX_CPUS; ++i) {
                InterlockedExchange(&n->on_cpu[i], 0);
            }
            InterlockedExchange(&n->calls, 0);
        }
    }
    cpu = GetCurrentProcessorNumber();
    if (cpu < MAX_CPUS) {
        InterlockedIncrement(&n->on_cpu[cpu]);
    }
    InterlockedIncrement(&n->calls);
}

/* ---- looking at the threads --------------------------------------------------------- */

static void remember_module(DWORD tid, HANDLE thread)
{
    LONG n = g_known_count;
    LONG i;
    void *start = NULL;
    HMODULE mod = NULL;
    char path[MAX_PATH];
    const char *name = "";

    for (i = 0; i < n; ++i) {
        if (g_known[i].tid == tid) {
            return;
        }
    }
    if (n >= MAX_KNOWN) {
        return;
    }
    if (g_nt_query_thread && g_nt_query_thread(thread, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start),
                                                NULL) == 0 &&
        start &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)start, &mod) &&
        mod && GetModuleFileNameA(mod, path, sizeof(path))) {
        path[sizeof(path) - 1] = '\0';
        name = log_basename(path); /* the file's name only, never its folder */
    }
    g_known[n].tid = tid;
    strncpy(g_known[n].module, name, MODULE_CHARS - 1);
    g_known[n].module[MODULE_CHARS - 1] = '\0';
    InterlockedExchange(&g_known_count, n + 1); /* the entry is complete before it counts */
}

static unsigned long long filetime_units(const FILETIME *t)
{
    return ((unsigned long long)t->dwHighDateTime << 32) | t->dwLowDateTime;
}

static void look(pass *p)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry;
    DWORD pid = GetCurrentProcessId();

    p->tick = GetTickCount();
    p->count = 0;
    p->total = 0;
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            HANDLE thread;
            FILETIME created, exited, kernel, user;

            if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == g_own_tid || p->count >= MAX_THREADS) {
                continue;
            }
            thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread) {
                continue;
            }
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
                seen_thread *t = &p->t[p->count++];
                thread_basic_info info;

                t->tid = entry.th32ThreadID;
                t->cpu = filetime_units(&kernel) + filetime_units(&user);
                t->affinity = 0;
                memset(&info, 0, sizeof(info));
                if (g_nt_query_thread &&
                    g_nt_query_thread(thread, 0 /* ThreadBasicInformation */, &info, sizeof(info), NULL) == 0) {
                    t->affinity = (unsigned long)info.affinity;
                }
                p->total += t->cpu;
                remember_module(t->tid, thread);
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

/* The busiest threads between two passes (before may be NULL: since the
 * process started). */
static void compare(const pass *before, const pass *after, DWORD since_tick, result *out)
{
    DWORD ms = after->tick - (before ? before->tick : since_tick);
    unsigned long long total = 0;
    unsigned i, j, k;

    memset(out, 0, sizeof(*out));
    if (!ms || !after->count) {
        return;
    }
    for (i = 0; i < after->count; ++i) {
        unsigned long long used = after->t[i].cpu;
        unsigned permille;

        if (before) {
            for (j = 0; j < before->count && before->t[j].tid != after->t[i].tid; ++j) {
            }
            if (j < before->count) {
                used = used > before->t[j].cpu ? used - before->t[j].cpu : 0;
            }
        }
        total += used;
        permille = (unsigned)(used / 10 / ms); /* 100 ns units -> thousandths of the time */
        /* keep the REPORT_ROWS busiest, in order */
        for (k = out->rows; k > 0 && out->r[k - 1].permille < permille; --k) {
            if (k < REPORT_ROWS) {
                out->r[k] = out->r[k - 1];
            }
        }
        if (k < REPORT_ROWS) {
            out->r[k].tid = after->t[i].tid;
            out->r[k].permille = permille;
            out->r[k].affinity = after->t[i].affinity;
            if (out->rows < REPORT_ROWS) {
                ++out->rows;
            }
        }
    }
    out->seconds = (ms + 500) / 1000;
    out->permille_total = (unsigned)(total / 10 / ms);
    out->threads = after->count;
    out->valid = 1;
}

/* ---- writing a result --------------------------------------------------------------- */

static const char *module_of(DWORD tid)
{
    LONG n = g_known_count, i;

    for (i = 0; i < n; ++i) {
        if (g_known[i].tid == tid) {
            return g_known[i].module;
        }
    }
    return "";
}

/* " seen on processors 6 (80 %), 7 (20 %)" for a thread that passes a hook. */
static void where_seen(DWORD tid, char *out, size_t cap)
{
    unsigned place;

    out[0] = '\0';
    for (place = 0; place < THREADS_PLACES; ++place) {
        const noted *n = &g_noted[place];
        LONG calls = n->calls;
        size_t pos = 0;
        unsigned shown = 0;
        LONG done[4] = {-1, -1, -1, -1};

        if (n->tid != (LONG)tid || calls <= 0) {
            continue;
        }
        pos += (size_t)snprintf(out + pos, cap - pos, "; seen on processor");
        while (shown < 4 && pos + 24 < cap) {
            LONG best = -1, best_count = 0;
            unsigned cpu, k;
            for (cpu = 0; cpu < MAX_CPUS; ++cpu) {
                int taken = 0;
                for (k = 0; k < shown; ++k) {
                    taken |= done[k] == (LONG)cpu;
                }
                if (!taken && n->on_cpu[cpu] > best_count) {
                    best = (LONG)cpu;
                    best_count = n->on_cpu[cpu];
                }
            }
            if (best < 0 || (shown && best_count * 100 / calls < 3)) {
                break;
            }
            pos += (size_t)snprintf(out + pos, cap - pos, "%s %ld (%ld %%)", shown ? "," : "", (long)best,
                                    (long)(best_count * 100 / calls));
            done[shown++] = best;
        }
        return;
    }
}

static void write_result(const char *title, const result *res)
{
    unsigned i;

    if (!res->valid) {
        return;
    }
    LOGI("threads: %s (%u s): the process used %u.%02u processors, %u threads; the busiest:", title, res->seconds,
         res->permille_total / 1000, res->permille_total % 1000 / 10, res->threads);
    for (i = 0; i < res->rows; ++i) {
        const row *r = &res->r[i];
        char what[96], place[72], seen[96];
        const char *module = module_of(r->tid);
        unsigned k;

        if (r->permille < 5 && i >= 1) {
            break; /* idle threads say nothing */
        }
        {
            int first = r->tid == g_first_tid;
            int presents = g_noted[THREADS_AT_PRESENT].tid == (LONG)r->tid;
            int input = g_noted[THREADS_AT_INPUT].tid == (LONG)r->tid;

            if (first || presents || input) {
                snprintf(what, sizeof(what), "%s%s%s%s%s", first ? "the process's first thread" : "",
                         first && (presents || input) ? ", " : "", presents ? "presents the picture" : "",
                         presents && input ? ", " : "", input ? "reads the input" : "");
            } else {
                snprintf(what, sizeof(what), "started in %s", module[0] ? module : "an unknown module");
            }
        }
        snprintf(place, sizeof(place), "not placed by the game (may run on 0x%08lX)", r->affinity);
        for (k = 0; k < MAX_PLACED; ++k) {
            if (g_placed[k].tid == (LONG)r->tid) {
                if (g_placed[k].all) {
                    snprintf(place, sizeof(place), "set to all processors by the game (0x%08lX)", r->affinity);
                } else {
                    snprintf(place, sizeof(place), "pinned by the game (0x%08lX)", (unsigned long)g_placed[k].mask);
                }
                break;
            }
        }
        where_seen(r->tid, seen, sizeof(seen));
        LOGI("threads:   %3u.%u %% of a processor - thread %lu, %s, %s%s", r->permille / 10, r->permille % 10,
             (unsigned long)r->tid, what, place, seen);
    }
}

void threads_log_stats(void)
{
    LONG current = g_current;

    if (!g_enabled || current < 0) {
        return;
    }
    write_result("the busiest stretch of the session", &g_published[current].busiest);
    write_result("the whole session", &g_published[current].session);
}

/* ---- the sampling thread ------------------------------------------------------------ */

static pass g_pass[2], g_measure_start;

static DWORD WINAPI sampler(LPVOID unused)
{
    unsigned turn = 0;
    int have_previous = 0;
    result busiest;

    (void)unused;
    g_own_tid = GetCurrentThreadId();
    memset(&busiest, 0, sizeof(busiest));
    while (g_enabled) {
        pass *now = &g_pass[turn & 1], *before = &g_pass[(turn + 1) & 1];
        LONG wanted, next;
        published *out;
        result interval;

        WaitForSingleObject(g_wake, g_period_ms);
        wanted = InterlockedExchange(&g_measure_seconds, 0);
        if (wanted > 0) {
            /* A measurement on request: look now, and again when the time is up. */
            static pass end;
            result res;

            look(&g_measure_start);
            InterlockedExchange(&g_measure_until, (LONG)(GetTickCount() + (DWORD)wanted * 1000));
            Sleep((DWORD)wanted * 1000);
            look(&end);
            compare(&g_measure_start, &end, 0, &res);
            InterlockedExchange(&g_measure_until, 0);
            write_result("measurement asked for in the menu", &res);
            continue;
        }
        look(now);
        if (have_previous) {
            compare(before, now, 0, &interval);
            if (interval.valid && interval.permille_total >= busiest.permille_total) {
                busiest = interval;
            }
        }
        next = g_current == 0 ? 1 : 0;
        out = &g_published[next];
        compare(NULL, now, g_start_tick, &out->session);
        out->busiest = busiest;
        InterlockedExchange(&g_current, next);
        have_previous = 1;
        ++turn;
    }
    return 0;
}

void threads_start(int enabled, unsigned period_seconds)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    HANDLE thread;

    if (!enabled || g_enabled) {
        return;
    }
    g_first_tid = GetCurrentThreadId();
    g_start_tick = GetTickCount();
    g_period_ms = (period_seconds ? period_seconds : DEFAULT_PERIOD_S) * 1000u;
    if (ntdll) {
        g_nt_query_thread = (nt_query_thread_fn)(void *)GetProcAddress(ntdll, "NtQueryInformationThread");
    }
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_wake) {
        return;
    }
    g_enabled = 1;
    thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    if (!thread) {
        g_enabled = 0;
        return;
    }
    SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(thread);
}

int threads_measure(unsigned seconds)
{
    if (!g_enabled || !seconds || g_measure_until || g_measure_seconds) {
        return 0;
    }
    InterlockedExchange(&g_measure_seconds, (LONG)seconds);
    SetEvent(g_wake);
    return 1;
}

unsigned threads_measuring(void)
{
    LONG until = g_measure_until;
    LONG left;

    if (g_measure_seconds) {
        return (unsigned)g_measure_seconds;
    }
    if (!until) {
        return 0;
    }
    left = until - (LONG)GetTickCount();
    return left > 0 ? (unsigned)(left + 999) / 1000 : 1;
}
