/* Runtime smoke-test harness.
 *
 * A 32-bit host that statically imports the engine fixture (built as
 * FC3_d3d11.dll or FC3.dll), which in turn imports VERSION.dll - the same
 * load chain as the game. The test driver places the freshly built proxy
 * version.dll and an INI file next to it, then runs
 *
 *     fc3pp_harness <scenario> [max-cores]
 *
 * The harness's own import table is never hooked, so it can compare what the
 * "engine" is told with what the operating system really did.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

__declspec(dllimport) long fx_loop_calls(void);
__declspec(dllimport) unsigned fx_enum_loop(unsigned long system_mask);
__declspec(dllimport) unsigned fx_enum_loop_auto(void);
__declspec(dllimport) unsigned fx_nproc(void);
__declspec(dllimport) int fx_affinity(unsigned long *process_mask, unsigned long *system_mask);
__declspec(dllimport) unsigned long fx_set_thread_affinity(unsigned long mask);
__declspec(dllimport) int fx_set_process_affinity(unsigned long mask);
__declspec(dllimport) unsigned long fx_set_ideal(unsigned long index);
__declspec(dllimport) unsigned fx_glpi_cores(void);
__declspec(dllimport) unsigned long fx_version_size(void);
__declspec(dllimport) void *fx_load_sysdet(void);
__declspec(dllimport) void fx_sleep(unsigned ms, unsigned count);
__declspec(dllimport) int fx_dxgi_open(void *window);
__declspec(dllimport) int fx_dxgi_fullscreen(void);
__declspec(dllimport) int fx_dxgi_set_fullscreen(int fullscreen);
__declspec(dllimport) void *fx_dxgi_chain(void);
__declspec(dllimport) int fx_audio_probe(void);
__declspec(dllimport) void *fx_tex_create(unsigned index, unsigned width, unsigned height, unsigned levels,
                                          unsigned dxgi_format);
__declspec(dllimport) int fx_tex_add(void *texture, const void *dds, unsigned size);
__declspec(dllimport) int fx_tex_add_file(void *texture, const void *xbt, unsigned size);
__declspec(dllimport) int fx_tex_drop(void *texture);
__declspec(dllimport) void fx_tex_destroy(void *texture);
__declspec(dllimport) void fx_tex_fail_next_add(void *texture, int fail);
__declspec(dllimport) void fx_tex_state(void *texture, unsigned out[6]);

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)             \
    do {                             \
        ++g_checks;                  \
        if (!(cond)) {               \
            ++g_failures;            \
            printf("  FAIL: ");      \
            printf(__VA_ARGS__);     \
            printf("\n");            \
        }                            \
    } while (0)

static unsigned popcount(unsigned long v)
{
    unsigned n = 0;
    while (v) {
        v &= v - 1;
        ++n;
    }
    return n;
}

/* ---- what the machine really looks like ------------------------------------ */

typedef struct real_machine {
    unsigned long process_mask;
    unsigned long system_mask;
    unsigned nproc;
    unsigned cores;               /* physical cores usable by this process */
    unsigned long core_mask[64];  /* logical processors of each such core */
} real_machine;

static void query_real(real_machine *m)
{
    static SYSTEM_LOGICAL_PROCESSOR_INFORMATION info[256];
    DWORD_PTR pm = 0, sm = 0;
    SYSTEM_INFO si;
    DWORD length = sizeof(info);
    unsigned i;

    memset(m, 0, sizeof(*m));
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    GetSystemInfo(&si);
    m->process_mask = (unsigned long)pm;
    m->system_mask = (unsigned long)sm;
    m->nproc = si.dwNumberOfProcessors;
    if (GetLogicalProcessorInformation(info, &length)) {
        for (i = 0; i < length / sizeof(info[0]) && m->cores < 64; ++i) {
            if (info[i].Relationship == RelationProcessorCore && (info[i].ProcessorMask & pm)) {
                m->core_mask[m->cores++] = (unsigned long)(info[i].ProcessorMask & pm);
            }
        }
    }
    if (m->cores == 0) {
        for (i = 0; i < 32 && m->cores < 64; ++i) {
            if ((pm >> i) & 1) {
                m->core_mask[m->cores++] = 1ul << i;
            }
        }
    }
}

static int core_of(const real_machine *m, unsigned long cpu_bit)
{
    unsigned i;
    for (i = 0; i < m->cores; ++i) {
        if (m->core_mask[i] & cpu_bit) {
            return (int)i;
        }
    }
    return -1;
}

/* Real affinity of the current thread (set-and-restore, the only way Win32 offers). */
static unsigned long real_thread_affinity(const real_machine *m)
{
    DWORD_PTR previous = SetThreadAffinityMask(GetCurrentThread(), m->process_mask);
    if (previous) {
        SetThreadAffinityMask(GetCurrentThread(), previous);
    }
    return (unsigned long)previous;
}

static unsigned long real_process_affinity(void)
{
    DWORD_PTR pm = 0, sm = 0;
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    return (unsigned long)pm;
}

/* ---- running a loop that may never return ----------------------------------- */

typedef unsigned (*loop_call)(unsigned long arg);
typedef long (*calls_fn)(void);
typedef void (*abort_fn)(int abort);

typedef struct guarded {
    loop_call fn;
    unsigned long arg;
    unsigned result;
} guarded;

static DWORD WINAPI guarded_thread(LPVOID p)
{
    guarded *g = (guarded *)p;
    g->result = g->fn(g->arg);
    return 0;
}

/* 1 = returned, 0 = was spinning without end and had to be stopped. The
 * spinning thread is asked to leave from inside the loop's callback rather
 * than being killed from outside. */
static int run_guarded_ex(loop_call fn, unsigned long arg, calls_fn calls, abort_fn abort, unsigned *result)
{
    static guarded g; /* static: must outlive a thread that is told to exit */
    HANDLE thread;
    DWORD wait;
    int spins;

    g.fn = fn;
    g.arg = arg;
    g.result = 0xDEADBEEF;
    thread = CreateThread(NULL, 0, guarded_thread, &g, 0, NULL);
    if (!thread) {
        return -1;
    }
    wait = WaitForSingleObject(thread, 50);
    for (spins = 0; wait == WAIT_TIMEOUT && spins < 200 && calls() < 200000; ++spins) {
        wait = WaitForSingleObject(thread, 50);
    }
    if (wait == WAIT_TIMEOUT) {
        abort(1);
        wait = WaitForSingleObject(thread, 10000);
        abort(0);
        CloseHandle(thread);
        if (wait == WAIT_TIMEOUT) {
            printf("  FAIL: a spinning test thread could not be stopped\n");
            ExitProcess(3);
        }
        return 0;
    }
    CloseHandle(thread);
    *result = g.result;
    return 1;
}

__declspec(dllimport) void fx_abort_loop(int abort);

static int run_guarded(loop_call fn, unsigned long arg, calls_fn calls, unsigned *result)
{
    return run_guarded_ex(fn, arg, calls, fx_abort_loop, result);
}

static unsigned call_auto(unsigned long unused)
{
    (void)unused;
    return fx_enum_loop_auto();
}

/* ---- the late-loaded module ---------------------------------------------------- */

typedef unsigned (*sd_enum_loop_fn)(unsigned long);
typedef long (*sd_loop_calls_fn)(void);
typedef int (*sd_affinity_fn)(unsigned long *, unsigned long *);
typedef unsigned (*sd_nproc_fn)(void);

static sd_enum_loop_fn sd_enum_loop;
static sd_loop_calls_fn sd_loop_calls;
static sd_affinity_fn sd_affinity;
static sd_nproc_fn sd_nproc;
static abort_fn sd_abort_loop;

static int run_guarded_sd(unsigned *result)
{
    return run_guarded_ex((loop_call)sd_enum_loop, 0xFFFFFFFFul, sd_loop_calls, sd_abort_loop, result);
}

static HMODULE g_sysdet;

static int load_sysdet(void)
{
    HMODULE mod = (HMODULE)fx_load_sysdet(); /* loaded BY the engine fixture, like the game does */
    g_sysdet = mod;
    CHECK(mod != NULL, "the fixture could not load systemdetection.dll");
    if (!mod) {
        return 0;
    }
    sd_enum_loop = (sd_enum_loop_fn)(void *)GetProcAddress(mod, "sd_enum_loop");
    sd_loop_calls = (sd_loop_calls_fn)(void *)GetProcAddress(mod, "sd_loop_calls");
    sd_affinity = (sd_affinity_fn)(void *)GetProcAddress(mod, "sd_affinity");
    sd_nproc = (sd_nproc_fn)(void *)GetProcAddress(mod, "sd_nproc");
    sd_abort_loop = (abort_fn)(void *)GetProcAddress(mod, "sd_abort_loop");
    CHECK(sd_enum_loop && sd_loop_calls && sd_affinity && sd_nproc && sd_abort_loop,
          "systemdetection.dll fixture exports missing");
    return sd_enum_loop && sd_loop_calls && sd_affinity && sd_nproc && sd_abort_loop;
}

/* ---- checks shared by all scenarios ---------------------------------------------- */

static void check_proxy_and_forwarding(void)
{
    static const char *const names[] = {
        "GetFileVersionInfoA",       "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
        "GetFileVersionInfoExW",     "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
        "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",    "GetFileVersionInfoW",
        "VerFindFileA",              "VerFindFileW",               "VerInstallFileA",
        "VerInstallFileW",           "VerLanguageNameA",           "VerLanguageNameW",
        "VerQueryValueA",            "VerQueryValueW",
    };
    wchar_t exe[MAX_PATH], dll[MAX_PATH];
    wchar_t *slash;
    HMODULE proxy = GetModuleHandleW(L"version.dll");
    DWORD handle = 0, size;
    size_t i;

    CHECK(proxy != NULL, "version.dll is not loaded");
    if (!proxy) {
        return;
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    GetModuleFileNameW(proxy, dll, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    slash = wcsrchr(dll, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    CHECK(_wcsicmp(exe, dll) == 0,
          "the loaded version.dll is not the proxy next to the executable "
          "(under Wine this needs WINEDLLOVERRIDES=version=n,b)");

    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        CHECK(GetProcAddress(proxy, names[i]) != NULL, "export %s is missing", names[i]);
    }

    /* Forwarding, through this executable's own import of VERSION.dll. */
    size = GetFileVersionInfoSizeW(L"kernel32.dll", &handle);
    CHECK(size > 0, "GetFileVersionInfoSizeW returned 0 (error %lu)", (unsigned long)GetLastError());
    if (size > 0) {
        void *data = malloc(size);
        VS_FIXEDFILEINFO *fixed = NULL;
        UINT fixed_len = 0;
        CHECK(data && GetFileVersionInfoW(L"kernel32.dll", 0, size, data), "GetFileVersionInfoW failed");
        CHECK(data && VerQueryValueW(data, L"\\", (LPVOID *)&fixed, &fixed_len) && fixed &&
                  fixed->dwSignature == 0xFEEF04BD,
              "VerQueryValueW did not return a valid VS_FIXEDFILEINFO");
        free(data);
    }
    {
        char ansi[64];
        wchar_t wide[64];
        CHECK(GetFileVersionInfoSizeA("kernel32.dll", &handle) > 0, "GetFileVersionInfoSizeA returned 0");
        CHECK(VerLanguageNameA(0x0409, ansi, sizeof(ansi)) > 0, "VerLanguageNameA returned 0");
        CHECK(VerLanguageNameW(0x0409, wide, 64) > 0, "VerLanguageNameW returned 0");
    }
    /* ... and through the engine fixture's import of VERSION.dll. */
    CHECK(fx_version_size() > 0, "version API call from the engine module failed");
}

/* ---- scenarios -------------------------------------------------------------------- */

/* Mode=engine / Mode=process: the engine sees a small virtual processor set. */
static void scenario_virtualized(const real_machine *m, int max_cores, int process_mode)
{
    unsigned expected = m->cores;
    unsigned v = fx_nproc();
    unsigned long vfull;
    unsigned long pm = 0, sm = 0;
    unsigned long selected = 0;
    unsigned long seen_cores = 0;
    unsigned k;
    unsigned result = 0;

    if (max_cores > 0 && (unsigned)max_cores < expected) {
        expected = (unsigned)max_cores;
    }
    if (expected > 30) {
        expected = 30;
    }
    printf("  machine: %u logical processors, %u usable physical cores; engine sees %u\n", m->nproc, m->cores, v);
    CHECK(v == expected, "engine sees %u processors, expected %u", v, expected);
    CHECK(v >= 1 && v <= 30, "engine-visible processor count %u out of range", v);
    vfull = v >= 32 ? 0xFFFFFFFFul : (1ul << v) - 1;

    CHECK(fx_affinity(&pm, &sm), "GetProcessAffinityMask failed inside the engine");
    CHECK(pm == vfull && sm == vfull, "engine affinity masks 0x%lX/0x%lX, expected 0x%lX", pm, sm, vfull);
    CHECK((sm & 0x80000000ul) == 0, "engine-visible system mask reaches bit 31");

    /* Every virtual processor maps to its own real processor on its own core. */
    for (k = 0; k < v; ++k) {
        unsigned long previous = fx_set_thread_affinity(1ul << k);
        unsigned long real = real_thread_affinity(m);
        int core;
        CHECK(previous != 0, "SetThreadAffinityMask(virtual %u) failed", k);
        CHECK(popcount(real) == 1, "virtual processor %u maps to real mask 0x%lX (not exactly one)", k, real);
        CHECK((real & m->process_mask) == real, "virtual processor %u maps outside the process affinity", k);
        CHECK((selected & real) == 0, "virtual processor %u reuses a real processor", k);
        core = core_of(m, real);
        CHECK(core >= 0, "virtual processor %u is on an unknown core", k);
        if (core >= 0) {
            CHECK(!((seen_cores >> core) & 1), "virtual processor %u shares a physical core with another (SMT sibling used)", k);
            seen_cores |= 1ul << core;
        }
        selected |= real;
    }
    /* "All processors" from the engine's point of view = the selected set. */
    CHECK(fx_set_thread_affinity(vfull) != 0, "SetThreadAffinityMask(all virtual) failed");
    CHECK(real_thread_affinity(m) == selected, "thread affinity 0x%lX after 'all', expected 0x%lX",
          real_thread_affinity(m), selected);
    /* A processor the engine was never told about must be refused. */
    if (v < 31) {
        CHECK(fx_set_thread_affinity(1ul << v) == 0, "affinity for a non-existent virtual processor succeeded");
        CHECK(real_thread_affinity(m) == selected, "a refused affinity request still changed the thread");
    }
    SetThreadAffinityMask(GetCurrentThread(), m->process_mask);

    /* Something outside confines the process to other processors after
     * start-up (Android launchers do that once the game's window is up). The
     * engine's requests must keep succeeding with whatever is left. */
    if (v >= 2) {
        unsigned long before = real_process_affinity();
        unsigned long lost, left;
        fx_set_thread_affinity(1ul << 0);
        lost = real_thread_affinity(m); /* the real processor behind virtual processor 0 */
        fx_set_thread_affinity(vfull);
        left = before & ~lost;
        if (lost && left && SetProcessAffinityMask(GetCurrentProcess(), left)) {
            unsigned long now;
            CHECK(fx_set_thread_affinity(1ul << 0) != 0,
                  "an affinity request for a processor the process has lost was failed");
            now = (unsigned long)SetThreadAffinityMask(GetCurrentThread(), left);
            CHECK(now != 0 && (now & lost) == 0, "the thread is on a processor the process has lost (0x%lX)", now);
            CHECK(fx_set_thread_affinity(1ul << 1) != 0, "a request for a processor that is still there failed");
            now = (unsigned long)SetThreadAffinityMask(GetCurrentThread(), left);
            CHECK(popcount(now) == 1 && (now & left) == now,
                  "a processor that is still there was not used exactly (0x%lX)", now);
            CHECK(SetProcessAffinityMask(GetCurrentProcess(), before), "could not restore the process affinity");
            SetThreadAffinityMask(GetCurrentThread(), before);
            printf("  process confined to 0x%lX and back: engine affinity requests kept working\n", left);
        }
    }
    SetThreadAffinityMask(GetCurrentThread(), m->process_mask);

    CHECK(fx_set_ideal(0) != (unsigned long)-1, "SetThreadIdealProcessor failed inside the engine");
    CHECK(fx_glpi_cores() == v, "GetLogicalProcessorInformation reports %u cores to the engine, expected %u",
          fx_glpi_cores(), v);

    /* The engine both pins the process to single processors and resets it to
     * the system mask; neither may get through. */
    {
        unsigned long before = real_process_affinity();
        CHECK(fx_set_process_affinity(1) == 1, "SetProcessAffinityMask reported failure to the engine");
        CHECK(real_process_affinity() == before, "the engine changed the real process affinity (0x%lX -> 0x%lX)",
              before, real_process_affinity());
        if (process_mode) {
            CHECK(before == selected, "process affinity 0x%lX, expected the selected set 0x%lX", before, selected);
        } else {
            CHECK(before == m->process_mask, "Mode=engine must not restrict the process (0x%lX)", before);
        }
    }

    /* The enumeration loop: fed by the hooked mask, and with the worst case. */
    CHECK(run_guarded(call_auto, 0, fx_loop_calls, &result) == 1, "enumeration loop did not return");
    CHECK(result == v, "enumeration loop counted %u processors, expected %u", result, v);
    CHECK(run_guarded((loop_call)fx_enum_loop, 0xFFFFFFFFul, fx_loop_calls, &result) == 1,
          "enumeration loop still hangs with a full 32-bit mask");
    CHECK(result == 32, "enumeration loop counted %u for a full mask, expected 32", result);

    /* The module that is loaded later gets the same treatment. */
    if (load_sysdet()) {
        CHECK(sd_nproc() == v, "late module sees %u processors, expected %u", sd_nproc(), v);
        CHECK(sd_affinity(&pm, &sm) && pm == vfull && sm == vfull, "late module affinity masks 0x%lX/0x%lX", pm, sm);
        CHECK(run_guarded_sd(&result) == 1, "late module: enumeration loop still hangs");
        CHECK(result == 32, "late module: loop counted %u, expected 32", result);

        /* The game unloads this module and loads it again later. The second
         * image is brand new (and usually lands on the same address), so it
         * must be patched and hooked again. */
        CHECK(FreeLibrary(g_sysdet), "could not unload systemdetection.dll");
        CHECK(GetModuleHandleA("systemdetection.dll") == NULL, "systemdetection.dll is still loaded");
        if (load_sysdet()) {
            CHECK(run_guarded_sd(&result) == 1, "reloaded late module: enumeration loop hangs again");
            CHECK(result == 32, "reloaded late module: loop counted %u, expected 32", result);
            CHECK(sd_nproc() == v, "reloaded late module sees %u processors, expected %u", sd_nproc(), v);
        }
    }
    fx_sleep(0, 3);
    fx_sleep(1, 1);
}

/* Mode=off: nothing about the processor view changes, the loop is still fixed. */
static void scenario_off(const real_machine *m, int fixed)
{
    unsigned long pm = 0, sm = 0;
    unsigned result = 0;
    int returned;

    CHECK(fx_nproc() == m->nproc, "Mode=off changed the processor count (%u vs %u)", fx_nproc(), m->nproc);
    CHECK(fx_affinity(&pm, &sm) && pm == m->process_mask && sm == m->system_mask,
          "Mode=off changed the affinity masks");
    CHECK(fx_set_thread_affinity(m->process_mask) != 0, "pass-through SetThreadAffinityMask failed");

    returned = run_guarded((loop_call)fx_enum_loop, 0xFFFFFFFFul, fx_loop_calls, &result);
    if (fixed) {
        CHECK(returned == 1 && result == 32, "FixCoreEnumeration=1: loop %s (%u)",
              returned == 1 ? "returned a wrong count" : "hangs", result);
    } else {
        CHECK(returned == 0, "FixCoreEnumeration=0: the unpatched loop returned; the fixture no longer reproduces the bug");
    }
    if (load_sysdet()) {
        returned = run_guarded_sd(&result);
        if (fixed) {
            CHECK(returned == 1 && result == 32, "late module, FixCoreEnumeration=1: loop %s",
                  returned == 1 ? "returned a wrong count" : "hangs");
        } else {
            CHECK(returned == 0, "late module, FixCoreEnumeration=0: the unpatched loop returned");
        }
    }
}

/* Mode=off + SimulateAffinityMask=0xFFFFFFFF: what a 32-thread machine does. */
static void scenario_simulate(int fixed)
{
    unsigned long pm = 0, sm = 0;
    unsigned result = 0;
    int returned;

    CHECK(fx_affinity(&pm, &sm) && pm == 0xFFFFFFFFul && sm == 0xFFFFFFFFul,
          "simulated affinity mask not reported (0x%lX/0x%lX)", pm, sm);
    CHECK(fx_nproc() == 32, "simulated processor count %u, expected 32", fx_nproc());
    returned = run_guarded(call_auto, 0, fx_loop_calls, &result);
    if (fixed) {
        CHECK(returned == 1 && result == 32, "simulated 32-thread machine: loop %s (%u)",
              returned == 1 ? "returned a wrong count" : "hangs", result);
    } else {
        CHECK(returned == 0, "simulated 32-thread machine without the fix: loop returned (%u)", result);
    }
}

/* Engine module without a recognisable loop (the DX9-named fixture): the
 * affinity-mask clamp is the safety net. */
static void scenario_clamp(const real_machine *m)
{
    unsigned long pm = 0, sm = 0;
    CHECK(fx_enum_loop(1) == 0xFFFFFFFEu, "this scenario needs the fixture without the loop");
    CHECK(fx_affinity(&pm, &sm), "GetProcessAffinityMask failed");
    CHECK(pm == (m->process_mask & 0x7FFFFFFFul) && sm == (m->system_mask & 0x7FFFFFFFul),
          "clamped masks 0x%lX/0x%lX do not match the real ones below bit 31", pm, sm);
    CHECK((sm & 0x80000000ul) == 0, "bit 31 still visible");
    CHECK(fx_nproc() == m->nproc, "the clamp changed the processor count");
}

/* ---- fullscreen guard -------------------------------------------------------------
 * The fake dxgi.dll next to the host plays Windows: it changes the swap
 * chain's fullscreen state behind the engine's back and says which window the
 * swap chain belongs to. A hidden window of our own is never the foreground
 * window ("the game is in the background"); whatever window is in the
 * foreground right now stands for "the game is active". */

typedef void (*dx_force_state_fn)(int);
typedef int (*dx_state_fn)(void);
typedef void (*dx_set_window_fn)(void *);
typedef long (*dx_set_calls_fn)(void);
typedef void (*dx_fail_set_fn)(int);
typedef void *(*dx_foreign_chain_fn)(void);
typedef int (*dx_query_fn)(void *);

static dx_force_state_fn dx_force_state;
static dx_state_fn dx_state;
static dx_set_window_fn dx_set_window;
static dx_set_calls_fn dx_set_calls;
static dx_fail_set_fn dx_fail_set;
static dx_foreign_chain_fn dx_foreign_chain;
static dx_query_fn dx_query;

static int load_fake_dxgi(void)
{
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");

    CHECK(dxgi != NULL, "dxgi.dll is not loaded");
    if (!dxgi) {
        return 0;
    }
    dx_force_state = (dx_force_state_fn)(void *)GetProcAddress(dxgi, "fxdxgi_force_state");
    dx_state = (dx_state_fn)(void *)GetProcAddress(dxgi, "fxdxgi_state");
    dx_set_window = (dx_set_window_fn)(void *)GetProcAddress(dxgi, "fxdxgi_set_window");
    dx_set_calls = (dx_set_calls_fn)(void *)GetProcAddress(dxgi, "fxdxgi_set_calls");
    dx_fail_set = (dx_fail_set_fn)(void *)GetProcAddress(dxgi, "fxdxgi_fail_set");
    dx_foreign_chain = (dx_foreign_chain_fn)(void *)GetProcAddress(dxgi, "fxdxgi_foreign_chain");
    dx_query = (dx_query_fn)(void *)GetProcAddress(dxgi, "fxdxgi_query");
    CHECK(dx_force_state && dx_state && dx_set_window && dx_set_calls && dx_fail_set && dx_foreign_chain && dx_query,
          "the loaded dxgi.dll is not the test fixture (under Wine this needs WINEDLLOVERRIDES=dxgi=n,b)");
    return dx_force_state && dx_state && dx_set_window && dx_set_calls && dx_fail_set && dx_foreign_chain &&
           dx_query;
}

static void game_in_background(HWND hidden)
{
    dx_set_window(hidden);
}

static void game_in_foreground(void)
{
    dx_set_window(GetForegroundWindow());
}

static void scenario_fullscreen(int guard_enabled)
{
    HWND hidden;
    int own_window;
    void *foreign;
    long calls;
    int i;

    if (!load_fake_dxgi()) {
        return;
    }
    hidden = CreateWindowExA(0, "STATIC", "fc3pp test", WS_POPUP, 0, 0, 16, 16, NULL, NULL, NULL, NULL);
    own_window = hidden != NULL;
    if (!hidden) {
        /* No display (headless Wine in CI): windows cannot be created. The
         * guard only ever compares the handle with the foreground window, so
         * any value that is not the foreground window serves. */
        hidden = (HWND)(ULONG_PTR)0x00FC3001;
        printf("  no display: using a made-up window handle for the background case\n");
    }
    CHECK(GetForegroundWindow() != hidden, "the background test window is in the foreground");

    CHECK(fx_dxgi_open(hidden) == 0, "the engine fixture could not create its swap chain");
    CHECK(fx_dxgi_fullscreen() == 0, "a new swap chain must report windowed");

    /* The game enters fullscreen. */
    CHECK(fx_dxgi_set_fullscreen(1) == 0, "SetFullscreenState failed");
    CHECK(fx_dxgi_fullscreen() == 1, "fullscreen not reported after entering it");
    calls = dx_set_calls();

    /* Alt+Tab away: Windows drops fullscreen while the window is in the background. */
    game_in_background(hidden);
    dx_force_state(0);
    if (!guard_enabled) {
        CHECK(fx_dxgi_fullscreen() == 0, "RestoreFullscreenOnFocus=0: the engine must see the real state");
        game_in_foreground();
        CHECK(fx_dxgi_fullscreen() == 0, "RestoreFullscreenOnFocus=0: state changed on its own");
        CHECK(dx_set_calls() == calls, "RestoreFullscreenOnFocus=0: fullscreen was requested anyway");
        if (own_window) {
            DestroyWindow(hidden);
        }
        return;
    }
    for (i = 0; i < 3; ++i) {
        CHECK(fx_dxgi_fullscreen() == 1, "the focus-loss drop reached the engine (poll %d)", i);
    }
    CHECK(dx_state() == 0, "the swap chain should really be windowed at this point");
    CHECK(dx_set_calls() == calls, "fullscreen was requested while the game is in the background");

    /* Alt+Tab back. */
    game_in_foreground();
    CHECK(fx_dxgi_fullscreen() == 1, "engine saw windowed when the game came back");
    CHECK(dx_set_calls() == calls + 1, "expected exactly one request to restore fullscreen, saw %ld",
          dx_set_calls() - calls);
    CHECK(dx_state() == 1, "the swap chain is not fullscreen again");
    CHECK(fx_dxgi_fullscreen() == 1, "fullscreen not reported after the restore");
    CHECK(dx_set_calls() == calls + 1, "fullscreen requested again although it is already restored");

    /* Alt+Enter with the game focused is the user's decision. */
    game_in_foreground();
    dx_force_state(0);
    CHECK(fx_dxgi_fullscreen() == 0, "a toggle with the window focused was hidden from the engine");
    CHECK(fx_dxgi_fullscreen() == 0, "the toggle did not stick");
    game_in_background(hidden);
    CHECK(fx_dxgi_fullscreen() == 0, "windowed game reported as fullscreen in the background");
    game_in_foreground();
    CHECK(fx_dxgi_fullscreen() == 0, "windowed game reported as fullscreen when focused again");
    CHECK(dx_set_calls() == calls + 1, "fullscreen forced on a game the user switched to windowed");

    /* Restoring can fail for a moment (mode change in progress): retried, not hammered. */
    CHECK(fx_dxgi_set_fullscreen(1) == 0, "SetFullscreenState failed");
    CHECK(fx_dxgi_fullscreen() == 1, "fullscreen not reported after re-entering it");
    game_in_background(hidden);
    dx_force_state(0);
    CHECK(fx_dxgi_fullscreen() == 1, "second focus-loss drop reached the engine");
    dx_fail_set(1);
    calls = dx_set_calls();
    game_in_foreground();
    for (i = 0; i < 5; ++i) {
        CHECK(fx_dxgi_fullscreen() == 1, "engine saw windowed while the restore is being retried");
    }
    CHECK(dx_set_calls() == calls + 1, "restore attempts are not rate limited (%ld in a row)", dx_set_calls() - calls);
    CHECK(dx_state() == 0, "the failing request changed the state");
    dx_fail_set(0);
    Sleep(400);
    game_in_foreground();
    CHECK(fx_dxgi_fullscreen() == 1, "engine saw windowed during the retry");
    CHECK(dx_set_calls() == calls + 2, "no retry after the interval (%ld requests)", dx_set_calls() - calls);
    CHECK(dx_state() == 1, "the retry did not restore fullscreen");

    /* A swap chain somebody else created (overlay, capture tool) is none of our business. */
    game_in_background(hidden);
    dx_force_state(0);
    CHECK(fx_dxgi_fullscreen() == 1, "third focus-loss drop reached the engine");
    foreign = dx_foreign_chain();
    CHECK(foreign != NULL && foreign != fx_dxgi_chain(), "could not create a foreign swap chain");
    CHECK(dx_query(foreign) == 0, "a swap chain that is not the game's was given the guarded answer");
    CHECK(fx_dxgi_fullscreen() == 1, "the game's swap chain lost its guard");
    dx_force_state(1);

    /* The renderer recreates its swap chain when the window size changes. */
    CHECK(fx_dxgi_open(hidden) == 0, "could not create a second swap chain");
    CHECK(fx_dxgi_fullscreen() == 0, "a recreated swap chain inherited stale state");
    CHECK(fx_dxgi_set_fullscreen(1) == 0, "SetFullscreenState failed on the second swap chain");
    CHECK(fx_dxgi_fullscreen() == 1, "fullscreen not reported on the second swap chain");
    game_in_background(hidden);
    dx_force_state(0);
    CHECK(fx_dxgi_fullscreen() == 1, "the recreated swap chain is not guarded");
    game_in_foreground();
    CHECK(fx_dxgi_fullscreen() == 1 && dx_state() == 1, "the recreated swap chain was not restored");

    if (own_window) {
        DestroyWindow(hidden);
    }
}

/* ---- texture guard -----------------------------------------------------------------
 * The engine fixture has a texture class with the real layout; its "add top
 * mip level" and "drop top mip level" methods count the calls that reach them
 * and double / halve a fake Direct3D texture. */

#define DXGI_BC1 70u
#define DXGI_BC3 76u

enum { TS_ADDS, TS_DROPS, TS_DESTROYED, TS_WIDTH, TS_HEIGHT, TS_LEVELS };

/* The header of a block-compressed DDS image of the given size. */
static const void *dds_image(unsigned width, unsigned height, const char *fourcc)
{
    static unsigned char h[128];
    memset(h, 0, sizeof(h));
    memcpy(h, "DDS ", 4);
    h[4] = 124;
    memcpy(h + 12, &height, 4);
    memcpy(h + 16, &width, 4);
    h[28] = 1;
    h[76] = 32;
    h[80] = 4;
    memcpy(h + 84, fourcc, 4);
    return h;
}

/* Reads a file next to the executable. Returns its size, 0 if it is not there. */
static unsigned read_local_file(const char *name, unsigned char *buf, unsigned cap)
{
    FILE *f = fopen(name, "rb");
    size_t n;
    if (!f) {
        return 0;
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    return (unsigned)n;
}

/* Top mip level files that belong to a texture a mod has replaced. The test
 * driver builds a data folder with a patch archive (the "mod") and a game
 * archive, and leaves two top level files of the game next to the host:
 * one whose base texture the mod repainted, one whose base it left alone.
 * Both have exactly the right size, so only the archive scan can tell. */
static void scenario_stale_textures(int scan_enabled)
{
    static unsigned char stale[4096], fresh[4096];
    unsigned n_stale = read_local_file("stale_top.xbt", stale, sizeof(stale));
    unsigned n_fresh = read_local_file("fresh_top.xbt", fresh, sizeof(fresh));
    unsigned s[6];
    void *t;
    int cycle;

    CHECK(n_stale > 0x24 + 128 && n_fresh > 0x24 + 128, "the test driver did not provide the texture files");
    if (!n_stale || !n_fresh) {
        return;
    }
    t = fx_tex_create(7, 64, 64, 7, DXGI_BC1);
    if (!scan_enabled) {
        /* SkipReplacedTextureMip0=0: nothing but the size is looked at. */
        CHECK(fx_tex_add_file(t, stale, n_stale) == 1, "scan off: a top level of the right size was refused");
        fx_tex_state(t, s);
        CHECK(s[TS_ADDS] == 1 && s[TS_WIDTH] == 128, "scan off: the engine method was not reached");
        return;
    }
    for (cycle = 0; cycle < 2; ++cycle) {
        CHECK(fx_tex_add_file(t, stale, n_stale) == 0, "the original top level of a replaced texture was accepted");
        fx_tex_state(t, s);
        CHECK(s[TS_ADDS] == 0 && s[TS_WIDTH] == 64, "the original top level of a replaced texture reached the engine");
        fx_tex_drop(t);
        fx_tex_state(t, s);
        CHECK(s[TS_DROPS] == 0 && s[TS_WIDTH] == 64, "the unload that follows was not ignored (cycle %d)", cycle);
    }
    /* The top level of a texture the mod did not touch is added as always. */
    t = fx_tex_create(7, 64, 64, 7, DXGI_BC3);
    CHECK(fx_tex_add_file(t, fresh, n_fresh) == 1, "the top level of an untouched texture was refused");
    fx_tex_state(t, s);
    CHECK(s[TS_ADDS] == 1 && s[TS_WIDTH] == 128, "the top level of an untouched texture did not reach the engine");
}

static void scenario_textures(int guard_enabled, int scan_enabled)
{
    unsigned s[6];
    void *t;
    int cycle;

    /* 1. What the original game data looks like: a 512 texture, a 1024 top level. */
    t = fx_tex_create(0, 512, 512, 10, DXGI_BC1);
    CHECK(t != NULL, "the engine fixture has no texture class");
    if (!t) {
        return;
    }
    CHECK(fx_tex_add(t, dds_image(1024, 1024, "DXT1"), 128) == 1, "a fitting top level was refused");
    fx_tex_state(t, s);
    CHECK(s[TS_ADDS] == 1 && s[TS_WIDTH] == 1024 && s[TS_LEVELS] == 11, "fitting top level: texture is %ux%u, %u levels",
          s[TS_WIDTH], s[TS_HEIGHT], s[TS_LEVELS]);
    CHECK(fx_tex_drop(t) == 1, "dropping a real top level failed");
    fx_tex_state(t, s);
    CHECK(s[TS_DROPS] == 1 && s[TS_WIDTH] == 512 && s[TS_LEVELS] == 10, "after the drop: texture is %ux%u, %u levels",
          s[TS_WIDTH], s[TS_HEIGHT], s[TS_LEVELS]);

    /* 2. A texture mod: the base was replaced by a 2048 texture, the top level
     *    file is still the game's 1024 one. Unguarded, the engine builds a 4096
     *    texture and has Direct3D read 16 times the data that was loaded. */
    t = fx_tex_create(1, 2048, 2048, 11, DXGI_BC1);
    if (!guard_enabled) {
        CHECK(fx_tex_add(t, dds_image(1024, 1024, "DXT1"), 128) == 1, "SkipMismatchedMip0=0: the call was held back");
        fx_tex_state(t, s);
        CHECK(s[TS_ADDS] == 1 && s[TS_WIDTH] == 4096, "SkipMismatchedMip0=0: the engine method was not reached");
        CHECK(fx_tex_drop(t) == 1, "SkipMismatchedMip0=0: the drop was held back");
        fx_tex_state(t, s);
        CHECK(s[TS_DROPS] == 1 && s[TS_WIDTH] == 2048, "SkipMismatchedMip0=0: the drop method was not reached");
        return;
    }
    for (cycle = 0; cycle < 3; ++cycle) {
        CHECK(fx_tex_add(t, dds_image(1024, 1024, "DXT1"), 128) == 0, "a top level that is too small was accepted");
        fx_tex_state(t, s);
        CHECK(s[TS_ADDS] == 0, "a top level that is too small reached the engine (cycle %d)", cycle);
        CHECK(s[TS_WIDTH] == 2048 && s[TS_LEVELS] == 11, "the texture changed although nothing was added");
        /* The resource layer does not know and unloads "the level" later. */
        fx_tex_drop(t);
        fx_tex_state(t, s);
        CHECK(s[TS_DROPS] == 0, "the unload after a refused level reached the engine (cycle %d)", cycle);
        CHECK(s[TS_WIDTH] == 2048 && s[TS_LEVELS] == 11,
              "the texture lost a real level after a refused one (cycle %d): %ux%u", cycle, s[TS_WIDTH], s[TS_HEIGHT]);
    }
    /* Only one unload is swallowed per refused level. */
    CHECK(fx_tex_drop(t) == 1, "a real unload was swallowed");
    fx_tex_state(t, s);
    CHECK(s[TS_DROPS] == 1 && s[TS_WIDTH] == 1024, "a real unload did not reach the engine");

    /* 3. Right size, wrong pixel format: half the bytes per block. */
    t = fx_tex_create(2, 512, 512, 10, DXGI_BC3);
    CHECK(fx_tex_add(t, dds_image(1024, 1024, "DXT1"), 128) == 0, "a top level in another format was accepted");
    fx_tex_state(t, s);
    CHECK(s[TS_ADDS] == 0 && s[TS_WIDTH] == 512, "a top level in another format reached the engine");
    CHECK(fx_tex_add(t, dds_image(1024, 1024, "DXT5"), 128) == 1, "the matching format was refused");
    fx_tex_drop(t); /* the one for the refused level */
    fx_tex_state(t, s);
    CHECK(s[TS_ADDS] == 1 && s[TS_DROPS] == 0 && s[TS_WIDTH] == 1024, "format case: wrong call went through");
    fx_tex_drop(t);
    fx_tex_state(t, s);
    CHECK(s[TS_DROPS] == 1 && s[TS_WIDTH] == 512, "format case: the real unload did not go through");

    /* 4. A shrunken base with the original top level. */
    t = fx_tex_create(3, 256, 256, 9, DXGI_BC1);
    CHECK(fx_tex_add(t, dds_image(1024, 1024, "DXT1"), 128) == 0, "a top level that is too large was accepted");
    fx_tex_state(t, s);
    CHECK(s[TS_ADDS] == 0, "a top level that is too large reached the engine");

    /* 5. The engine's own method fails (out of memory): same imbalance, same cure. */
    t = fx_tex_create(4, 512, 512, 10, DXGI_BC1);
    fx_tex_fail_next_add(t, 1);
    CHECK(fx_tex_add(t, dds_image(1024, 1024, "DXT1"), 128) == 0, "the engine's failure was not passed on");
    fx_tex_state(t, s);
    CHECK(s[TS_ADDS] == 1 && s[TS_WIDTH] == 512, "the engine method was not asked");
    fx_tex_drop(t);
    fx_tex_state(t, s);
    CHECK(s[TS_DROPS] == 0 && s[TS_WIDTH] == 512, "the unload after a failed add reached the engine");

    /* 6. A refused texture is destroyed and another one is created at the same
     *    address: its first unload is a real one. */
    t = fx_tex_create(5, 2048, 2048, 11, DXGI_BC1);
    CHECK(fx_tex_add(t, dds_image(1024, 1024, "DXT1"), 128) == 0, "refusal before destruction");
    fx_tex_destroy(t);
    fx_tex_state(t, s);
    CHECK(s[TS_DESTROYED] == 1, "the destructor was not reached");
    t = fx_tex_create(5, 1024, 1024, 11, DXGI_BC1);
    CHECK(fx_tex_drop(t) == 1, "a new texture at a reused address had its unload swallowed");
    fx_tex_state(t, s);
    CHECK(s[TS_DROPS] == 1 && s[TS_WIDTH] == 512, "reused address: the unload did not reach the engine");

    /* 7. Data that is not a DDS image is the engine's to reject. */
    t = fx_tex_create(6, 512, 512, 10, DXGI_BC1);
    fx_tex_add(t, "not a DDS image, but long enough to look at .........................................."
                  "...............................................................................",
               128);
    fx_tex_state(t, s);
    CHECK(s[TS_ADDS] == 1, "unreadable data was not left to the engine");

    scenario_stale_textures(scan_enabled);
}

int main(int argc, char **argv)
{
    const char *scenario = argc > 1 ? argv[1] : "engine";
    int max_cores = argc > 2 ? atoi(argv[2]) : 6;
    real_machine machine;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("scenario: %s\n", scenario);
    query_real(&machine);
    check_proxy_and_forwarding();

    if (strcmp(scenario, "engine") == 0 || strcmp(scenario, "laa") == 0 || strcmp(scenario, "noini") == 0) {
        scenario_virtualized(&machine, max_cores, 0);
    } else if (strcmp(scenario, "process") == 0) {
        scenario_virtualized(&machine, max_cores, 1);
    } else if (strcmp(scenario, "off") == 0) {
        scenario_off(&machine, 1);
    } else if (strcmp(scenario, "off-nofix") == 0 || strcmp(scenario, "disabled") == 0) {
        scenario_off(&machine, 0);
    } else if (strcmp(scenario, "simulate") == 0) {
        scenario_simulate(1);
    } else if (strcmp(scenario, "simulate-nofix") == 0) {
        scenario_simulate(0);
    } else if (strcmp(scenario, "clamp") == 0) {
        scenario_clamp(&machine);
    } else if (strcmp(scenario, "fullscreen") == 0) {
        scenario_fullscreen(1);
    } else if (strcmp(scenario, "fullscreen-off") == 0) {
        scenario_fullscreen(0);
    } else if (strcmp(scenario, "audio") == 0 || strcmp(scenario, "audio-off") == 0) {
        /* XAudio2 2.7 may not be installed (CI) or have no device to play on;
         * what must hold everywhere is that the patch does not get in the way.
         * The test driver matches the log against the reported stage. */
        int stage = fx_audio_probe();
        printf("audio stage: %d\n", stage);
        CHECK(stage >= 0 && stage <= 63, "XAudio2 probe returned %d", stage);
        if ((stage & 1) && strcmp(scenario, "audio") == 0) {
            CHECK(GetModuleHandleA("XAudio2_7.dll") != NULL,
                  "XAudio2_7.dll was unloaded although the patch is meant to keep it loaded");
        }
    } else if (strcmp(scenario, "textures") == 0) {
        scenario_textures(1, 1);
    } else if (strcmp(scenario, "textures-noscan") == 0) {
        scenario_textures(1, 0);
    } else if (strcmp(scenario, "textures-off") == 0) {
        scenario_textures(0, 0);
    } else if (strcmp(scenario, "forward-only") == 0) {
        /* nothing beyond the shared checks */
    } else {
        printf("unknown scenario\n");
        return 2;
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
