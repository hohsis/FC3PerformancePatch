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
__declspec(dllimport) int fx_dxgi_present(void);
__declspec(dllimport) int fx_audio_probe(void);
__declspec(dllimport) void *fx_tex_create(unsigned index, unsigned width, unsigned height, unsigned levels,
                                          unsigned dxgi_format);
__declspec(dllimport) int fx_tex_add(void *texture, const void *dds, unsigned size);
__declspec(dllimport) int fx_tex_add_file(void *texture, const void *xbt, unsigned size);
__declspec(dllimport) int fx_tex_drop(void *texture);
__declspec(dllimport) void fx_tex_destroy(void *texture);
__declspec(dllimport) void fx_tex_fail_next_add(void *texture, int fail);
__declspec(dllimport) void fx_tex_state(void *texture, unsigned out[6]);
__declspec(dllimport) void *fx_top_create(unsigned index, unsigned name_low, unsigned name_high, int link);
__declspec(dllimport) int fx_top_load(void *resource, const void *file, unsigned size);
__declspec(dllimport) void fx_top_unload(void *resource);
__declspec(dllimport) void fx_top_state(unsigned index, long out[5]);
__declspec(dllimport) unsigned long fx_pad_read(unsigned index, void *state);
__declspec(dllimport) int fx_input_open(void);
__declspec(dllimport) long fx_input_state(unsigned device, void *data, unsigned size);
__declspec(dllimport) long fx_input_events(unsigned device, void *data, unsigned object_size,
                                           unsigned long *count, unsigned flags);
__declspec(dllimport) unsigned char *fx_ui_manager(int create);
__declspec(dllimport) long fx_read_file(int wide, const wchar_t *path, char *out, unsigned cap);
__declspec(dllimport) int fx_write_file(const wchar_t *path, const char *data, unsigned size);
__declspec(dllimport) int fx_append_file(const wchar_t *path, const char *data, unsigned size);

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

/* Does the system itself say that some cores are faster than others (hybrid
 * x86 processors on Windows)? Then the patch does not rank them itself. */
static int system_reports_speed_classes(void)
{
    typedef BOOL(WINAPI * glpi_ex_fn)(int, void *, DWORD *);
    glpi_ex_fn glpi_ex = (glpi_ex_fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                            "GetLogicalProcessorInformationEx");
    static unsigned char buf[16384];
    DWORD len = sizeof(buf), off = 0;
    int first = -1;

    if (!glpi_ex || !glpi_ex(0 /* RelationProcessorCore */, buf, &len)) {
        return 0;
    }
    while (off + 12 <= len) {
        DWORD size = *(DWORD *)(void *)(buf + off + 4);
        int eff = buf[off + 9]; /* PROCESSOR_RELATIONSHIP.EfficiencyClass */
        if (size < 12) {
            break;
        }
        if (first < 0) {
            first = eff;
        } else if (eff != first) {
            return 1;
        }
        off += size;
    }
    return 0;
}

/* A processor with faster and slower cores, as in ARM handhelds. The patch is
 * pointed at a stand-in for the host's processor files (run_runtime.py writes
 * them) which say that every logical processor from `first_fast` on is faster
 * than the ones below. The engine pins a worker to every core it is given but
 * not its main and render threads, so two of the fastest cores must be kept
 * out of what the engine is given - and still be usable by a thread that asks
 * for "anywhere". */
static void scenario_fast_cores(const real_machine *m, int max_cores, unsigned first_fast)
{
    unsigned v = fx_nproc(), i, k, fast = 0, spare = 0, expected;
    unsigned long spared_mask = 0, selected = 0, vfull;

    if (system_reports_speed_classes()) {
        printf("  this machine reports faster and slower cores itself; the stand-in files are not used\n");
        return;
    }
    for (i = 0; i < m->cores; ++i) {
        unsigned lowest = 0;
        while (!((m->core_mask[i] >> lowest) & 1)) {
            ++lowest;
        }
        if (lowest >= first_fast) {
            ++fast;
        }
    }
    if (fast > 0 && fast < m->cores && m->cores > 2) {
        spare = fast < 2 ? fast : 2;
        if (spare > m->cores - 2) {
            spare = m->cores - 2;
        }
    }
    /* the fast cores with the lowest numbers are the ones kept free */
    for (i = 0, k = 0; i < m->cores && k < spare; ++i) {
        unsigned lowest = 0;
        while (!((m->core_mask[i] >> lowest) & 1)) {
            ++lowest;
        }
        if (lowest >= first_fast) {
            spared_mask |= m->core_mask[i];
            ++k;
        }
    }
    expected = m->cores - spare;
    if (max_cores > 0 && (unsigned)max_cores < expected) {
        expected = (unsigned)max_cores;
    }
    printf("  machine: %u cores, %u of them \"fast\" (logical processors from %u on); %u kept free (mask 0x%lX); "
           "engine sees %u\n",
           m->cores, fast, first_fast, spare, spared_mask, v);
    CHECK(v == expected, "engine sees %u processors, expected %u", v, expected);
    vfull = v >= 32 ? 0xFFFFFFFFul : (1ul << v) - 1;
    for (k = 0; k < v; ++k) {
        unsigned long real;
        CHECK(fx_set_thread_affinity(1ul << k) != 0, "SetThreadAffinityMask(virtual %u) failed", k);
        real = real_thread_affinity(m);
        CHECK(popcount(real) == 1, "virtual processor %u maps to real mask 0x%lX (not exactly one)", k, real);
        CHECK((real & spared_mask) == 0, "virtual processor %u is on a core that was to be kept free (0x%lX)", k, real);
        selected |= real;
    }
    /* "Anywhere" includes the cores that were kept free: that is what they are for. */
    CHECK(fx_set_thread_affinity(vfull) != 0, "SetThreadAffinityMask(all virtual) failed");
    CHECK(real_thread_affinity(m) == (selected | spared_mask), "thread affinity 0x%lX after 'all', expected 0x%lX",
          real_thread_affinity(m), selected | spared_mask);
    CHECK(real_process_affinity() == m->process_mask, "Mode=engine must not restrict the process (0x%lX)",
          real_process_affinity());
    SetThreadAffinityMask(GetCurrentThread(), m->process_mask);
    CHECK(fx_glpi_cores() == v, "GetLogicalProcessorInformation reports %u cores to the engine, expected %u",
          fx_glpi_cores(), v);
}

/* The thread report. Two busy threads for a few seconds: this one, which the
 * "game" pins to its first processor, and one that nobody places. The patch
 * looks at the threads once a second here (ThreadReportSeconds) and writes
 * what it found when the process exits; run_runtime.py reads that. */
static DWORD WINAPI busy_until(LPVOID tick)
{
    volatile unsigned long spin = 0;
    while ((LONG)(GetTickCount() - (DWORD)(ULONG_PTR)tick) < 0) {
        ++spin;
    }
    return spin ? 0 : 1;
}

static void scenario_threads(const real_machine *m)
{
    DWORD until = GetTickCount() + 2700;
    HANDLE other = CreateThread(NULL, 0, busy_until, (LPVOID)(ULONG_PTR)until, 0, NULL);

    CHECK(other != NULL, "could not start a second thread");
    CHECK(fx_set_thread_affinity(1) != 0, "SetThreadAffinityMask(virtual 0) failed");
    {
        /* busy, and now and then reading the gamepad as the game's input thread does */
        unsigned char pad[32];
        unsigned long spin = 0;
        while ((LONG)(GetTickCount() - until) < 0) {
            if ((++spin & 0x3FFF) == 0) {
                fx_pad_read(0, pad);
            }
        }
    }
    if (other) {
        WaitForSingleObject(other, 5000);
        CloseHandle(other);
    }
    SetThreadAffinityMask(GetCurrentThread(), m->process_mask);
    printf("  two threads were busy for 2.7 s, one of them pinned by the engine\n");
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

/* ---- the in-game menu's place in the frame ------------------------------------------
 * The fake swap chain has no Direct3D device behind it, so nothing can be
 * drawn; what must hold is that the game's frames go through untouched, with
 * the menu closed, and with a menu that wants to open but cannot. */
static void scenario_menu(void)
{
    typedef long (*presents_fn)(void);
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    presents_fn presents = dxgi ? (presents_fn)(void *)GetProcAddress(dxgi, "fxdxgi_presents") : NULL;
    int i;

    CHECK(presents != NULL, "the loaded dxgi.dll is not the test fixture");
    if (!presents) {
        return;
    }
    CHECK(fx_dxgi_open((void *)(ULONG_PTR)0x00FC3001) == 0, "the engine fixture could not create its swap chain");
    for (i = 0; i < 5; ++i) {
        CHECK(fx_dxgi_present() == 0, "Present failed (frame %d)", i);
    }
    CHECK(presents() == 5, "%ld of 5 frames reached the swap chain", presents());
    /* a second swap chain (window resized) is watched as well */
    CHECK(fx_dxgi_open((void *)(ULONG_PTR)0x00FC3001) == 0, "could not create a second swap chain");
    CHECK(fx_dxgi_present() == 0 && presents() == 6, "the second swap chain does not present");
}

/* ---- gamepad, keyboard and mouse ---------------------------------------------------------
 * The engine fixture reads a gamepad (fake xinput1_3.dll) and DirectInput
 * devices (fake dinput8.dll) the way the game does. What the patch may do in
 * between: switch the game's input mode by writing one byte of its UI manager,
 * and hold the input back while the settings menu is open. */

typedef struct pad_state {
    DWORD packet;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
} pad_state;

typedef struct input_event {
    DWORD offset, data, time, sequence;
    UINT_PTR app;
} input_event;

typedef void (*fxxi_set_fn)(int connected, unsigned buttons, int right_trigger, int lx);
typedef void (*fxdi_key_fn)(unsigned scan_code, int down);
typedef void (*fxdi_move_fn)(long dx, long dy);
typedef void (*fxdi_button_fn)(unsigned button, int down);
typedef void (*fxdi_fill_fn)(unsigned char value);
typedef void (*fxdi_reset_fn)(void);

static fxxi_set_fn pad_set;
static fxdi_key_fn key_set;
static fxdi_move_fn mouse_move;
static fxdi_button_fn mouse_button;
static fxdi_fill_fn other_fill;
static fxdi_reset_fn devices_reset;

#define MODE_IN_EFFECT 0x30
#define MODE_ASKED_FOR 0x31
#define MODE_PENDING 0x32
#define PAD_A 0x1000u
#define KEY_W 0x11u
#define SETTLE_MS 550 /* longer than the patch waits for the device last used to come to rest */

static int input_fixtures(void)
{
    HMODULE xi = GetModuleHandleA("xinput1_3.dll"), di = GetModuleHandleA("dinput8.dll");

    pad_set = xi ? (fxxi_set_fn)(void *)GetProcAddress(xi, "fxxi_set") : NULL;
    key_set = di ? (fxdi_key_fn)(void *)GetProcAddress(di, "fxdi_key") : NULL;
    mouse_move = di ? (fxdi_move_fn)(void *)GetProcAddress(di, "fxdi_mouse_move") : NULL;
    mouse_button = di ? (fxdi_button_fn)(void *)GetProcAddress(di, "fxdi_mouse_button") : NULL;
    other_fill = di ? (fxdi_fill_fn)(void *)GetProcAddress(di, "fxdi_other_fill") : NULL;
    devices_reset = di ? (fxdi_reset_fn)(void *)GetProcAddress(di, "fxdi_reset") : NULL;
    CHECK(pad_set != NULL, "the loaded xinput1_3.dll is not the test fixture");
    CHECK(key_set && mouse_move && mouse_button && other_fill && devices_reset,
          "the loaded dinput8.dll is not the test fixture");
    return pad_set && key_set && mouse_move && mouse_button && other_fill && devices_reset;
}

/* One frame's worth of reading, as the game does it. Returns the gamepad
 * buttons the engine saw. */
static unsigned read_pad(void)
{
    pad_state st;
    memset(&st, 0xEE, sizeof(st));
    if (fx_pad_read(0, &st) != ERROR_SUCCESS) {
        return 0xFFFFFFFFu;
    }
    return st.buttons;
}

static int key_seen(unsigned scan_code)
{
    unsigned char keys[256];
    memset(keys, 0xEE, sizeof(keys));
    return fx_input_state(0, keys, sizeof(keys)) == 0 && keys[scan_code] == 0x80;
}

/* switching: 1 = the patch is to follow the device in use, 0 = it is not.
 * menu: 1 = the in-game menu exists (its opening combination is L3+R3). */
static void scenario_input(int switching, int menu)
{
    unsigned char *mgr = fx_ui_manager(0);
    unsigned char before[64];
    unsigned char mouse[16];
    input_event events[4];
    pad_state st;
    unsigned long count;
    LONG axis[3];
    unsigned i, changed = 0;
    unsigned char expect;

    if (!input_fixtures()) {
        return;
    }
    CHECK(mgr != NULL, "the engine fixture's input-mode code does not point at its manager variable");
    if (!mgr) {
        return;
    }
    CHECK(fx_input_open() == 0, "the engine fixture could not create its input devices");

    /* The two buttons that open the menu when held together belong to the menu
     * from the moment both are down; each alone is the game's. */
    pad_set(1, 0x0040, 0, 0);
    CHECK(read_pad() == 0x0040, "the engine does not see the left thumbstick button alone");
    pad_set(1, 0x00C0 | PAD_A, 0, 0);
    CHECK(read_pad() == (menu ? PAD_A : (0x00C0 | PAD_A)),
          "both thumbstick buttons held: the engine sees buttons 0x%04X", read_pad());
    pad_set(1, 0x0080, 0, 0);
    CHECK(read_pad() == 0x0080, "the engine does not see the right thumbstick button alone");
    pad_set(1, 0, 0, 0);
    read_pad();

    /* The game has no UI manager yet (early start-up): input flows, nothing is written. */
    pad_set(1, PAD_A, 0, 0);
    CHECK(read_pad() == PAD_A, "the engine does not see the gamepad button");
    key_set(KEY_W, 1);
    CHECK(key_seen(KEY_W), "the engine does not see the key");
    key_set(KEY_W, 0);
    CHECK(!key_seen(KEY_W), "the engine does not see the key released");
    CHECK(mgr[MODE_ASKED_FOR] == 0 && mgr[MODE_PENDING] == 0, "the input mode was written before the game had one");

    /* Now it has one, in keyboard/mouse mode. The player picks up the gamepad. */
    fx_ui_manager(1);
    memcpy(before, mgr, sizeof(before));
    Sleep(SETTLE_MS);
    CHECK(read_pad() == PAD_A, "the engine does not see the gamepad button");
    expect = switching ? 1 : 0;
    CHECK(mgr[MODE_ASKED_FOR] == expect, "gamepad in use: mode asked for is %u, expected %u", mgr[MODE_ASKED_FOR], expect);
    for (i = 0; i < sizeof(before); ++i) {
        changed += i != MODE_ASKED_FOR && mgr[i] != before[i];
    }
    CHECK(changed == 0, "%u other bytes of the game's object were changed", changed);
    CHECK(mgr[MODE_IN_EFFECT] == 0 && mgr[MODE_PENDING] == 0, "more than the mode asked for was written");
    /* a pad that is not connected stays not connected */
    memset(&st, 0xEE, sizeof(st));
    CHECK(fx_pad_read(1, &st) == ERROR_DEVICE_NOT_CONNECTED && st.buttons == 0xEEEE,
          "a gamepad that is not connected was reported differently");

    /* A key while the pad is still held: the pad keeps the mode. */
    key_set(KEY_W, 1);
    CHECK(key_seen(KEY_W), "the engine does not see the key");
    CHECK(mgr[MODE_ASKED_FOR] == expect, "a key pressed while the gamepad is in use switched the mode");
    /* The pad is put down; the key is still held a moment later. */
    pad_set(1, 0, 0, 3000); /* a stick slightly off centre is a pad at rest */
    CHECK(read_pad() == 0, "the engine does not see the gamepad at rest");
    Sleep(SETTLE_MS);
    CHECK(read_pad() == 0 && key_seen(KEY_W), "input is not flowing");
    CHECK(mgr[MODE_ASKED_FOR] == 0, "keyboard in use: the mode asked for is still %u", mgr[MODE_ASKED_FOR]);
    key_set(KEY_W, 0);
    CHECK(!key_seen(KEY_W), "the engine does not see the key released");

    /* Trigger and stick count as use of the pad, too. */
    Sleep(SETTLE_MS);
    pad_set(1, 0, 200, 0);
    CHECK(read_pad() == 0, "the trigger changed the buttons");
    CHECK(mgr[MODE_ASKED_FOR] == expect, "trigger pulled: mode asked for is %u", mgr[MODE_ASKED_FOR]);
    pad_set(1, 0, 0, 0);
    read_pad();

    /* The mouse: lying on the desk it jitters by a count; a hand moves it. */
    Sleep(SETTLE_MS);
    mouse_move(1, 0);
    memset(mouse, 0xEE, sizeof(mouse));
    CHECK(fx_input_state(1, mouse, sizeof(mouse)) == 0, "the mouse could not be read");
    memcpy(axis, mouse, sizeof(axis));
    CHECK(axis[0] == 1 && axis[1] == 0 && mouse[12] == 0, "the engine does not see the mouse movement");
    CHECK(mgr[MODE_ASKED_FOR] == expect, "a mouse jitter of one count switched the mode");
    mouse_move(40, -25);
    CHECK(fx_input_state(1, mouse, sizeof(mouse)) == 0, "the mouse could not be read");
    memcpy(axis, mouse, sizeof(axis));
    CHECK(axis[0] == 40 && axis[1] == -25, "the engine does not see the mouse movement");
    CHECK(mgr[MODE_ASKED_FOR] == 0, "mouse in use: the mode asked for is still %u", mgr[MODE_ASKED_FOR]);

    /* The same through buffered events, which is the other way to read a device. */
    Sleep(SETTLE_MS);
    pad_set(1, PAD_A, 0, 0);
    read_pad();
    pad_set(1, 0, 0, 0);
    read_pad();
    CHECK(mgr[MODE_ASKED_FOR] == expect, "gamepad in use again: mode asked for is %u", mgr[MODE_ASKED_FOR]);
    devices_reset();
    Sleep(SETTLE_MS);
    mouse_button(0, 1);
    count = 4;
    memset(events, 0xEE, sizeof(events));
    CHECK(fx_input_events(1, events, sizeof(events[0]), &count, 0) == 0 && count == 1 && events[0].offset == 12 &&
              events[0].data == 0x80,
          "the engine does not see the buffered mouse click (%lu events)", count);
    CHECK(mgr[MODE_ASKED_FOR] == 0, "mouse click: the mode asked for is still %u", mgr[MODE_ASKED_FOR]);
    mouse_button(0, 0);
    count = 4;
    fx_input_events(1, events, sizeof(events[0]), &count, 0);

    Sleep(SETTLE_MS);
    pad_set(1, PAD_A, 0, 0);
    read_pad();
    pad_set(1, 0, 0, 0);
    read_pad();
    Sleep(SETTLE_MS);
    key_set(KEY_W, 1);
    count = 0; /* asking how many there are, without a buffer, is not "reading a key" */
    CHECK(fx_input_events(0, NULL, sizeof(events[0]), &count, 1) == 0, "peeking at the keyboard failed");
    count = 4;
    CHECK(fx_input_events(0, events, sizeof(events[0]), &count, 0) == 0 && count == 1 && events[0].offset == KEY_W &&
              events[0].data == 0x80,
          "the engine does not see the buffered key press (%lu events)", count);
    CHECK(mgr[MODE_ASKED_FOR] == 0, "key press: the mode asked for is still %u", mgr[MODE_ASKED_FOR]);
    key_set(KEY_W, 0);
    devices_reset();

    /* A device that is neither keyboard nor mouse is none of the patch's business. */
    Sleep(SETTLE_MS);
    pad_set(1, PAD_A, 0, 0);
    read_pad();
    pad_set(1, 0, 0, 0);
    read_pad();
    Sleep(SETTLE_MS);
    other_fill(0x80);
    memset(before, 0, sizeof(before));
    CHECK(fx_input_state(2, before, 64) == 0 && before[0] == 0x80 && before[63] == 0x80,
          "another input device was not read unchanged");
    CHECK(mgr[MODE_ASKED_FOR] == expect, "another input device switched the mode");

    /* The player sets the mode in the game's options menu: that is where the patch starts from. */
    mgr[MODE_ASKED_FOR] = 0;
    mgr[MODE_PENDING] = 1;
    read_pad();
    CHECK(mgr[MODE_ASKED_FOR] == 0 && mgr[MODE_PENDING] == 1, "a gamepad at rest changed the mode");
}

/* The settings menu is open (it opens at start in this scenario): the game
 * must see a gamepad at rest, no keys and no mouse, until the menu is gone.
 * The fake swap chain cannot be drawn on, so the first frame closes the menu
 * for good - and the input must flow again. */
static void scenario_input_held(void)
{
    unsigned char *mgr = fx_ui_manager(1);
    unsigned char keys[256], mouse[16], other[64];
    input_event events[4];
    pad_state st;
    unsigned long count;

    if (!input_fixtures() || !mgr) {
        CHECK(mgr != NULL, "the engine fixture has no UI manager");
        return;
    }
    CHECK(fx_input_open() == 0, "the engine fixture could not create its input devices");
    pad_set(1, PAD_A, 255, 30000);
    key_set(KEY_W, 1);
    mouse_move(50, 50);
    mouse_button(1, 1);
    other_fill(0x80);

    memset(&st, 0xEE, sizeof(st));
    CHECK(fx_pad_read(0, &st) == ERROR_SUCCESS, "the gamepad disappeared while the menu is open");
    CHECK(st.buttons == 0 && st.right_trigger == 0 && st.lx == 0 && st.packet != 0 && st.packet != 0xEEEEEEEEul,
          "the game sees the gamepad while the menu is open");
    memset(keys, 0xEE, sizeof(keys));
    CHECK(fx_input_state(0, keys, sizeof(keys)) == 0 && keys[KEY_W] == 0 && keys[0] == 0 && keys[255] == 0,
          "the game sees the keyboard while the menu is open");
    memset(mouse, 0xEE, sizeof(mouse));
    CHECK(fx_input_state(1, mouse, sizeof(mouse)) == 0 && mouse[0] == 0 && mouse[4] == 0 && mouse[13] == 0,
          "the game sees the mouse while the menu is open");
    count = 4;
    CHECK(fx_input_events(0, events, sizeof(events[0]), &count, 0) == 0 && count == 0,
          "the game sees buffered key presses while the menu is open");
    count = 4;
    CHECK(fx_input_events(1, events, sizeof(events[0]), &count, 0) == 0 && count == 0,
          "the game sees buffered mouse events while the menu is open");
    /* a controller's axes rest at mid-range: zeroing them would be a full deflection */
    memset(other, 0, sizeof(other));
    CHECK(fx_input_state(2, other, sizeof(other)) == 0 && other[0] == 0x80 && other[63] == 0x80,
          "another input device was changed while the menu is open");
    CHECK(mgr[MODE_ASKED_FOR] == 0, "input meant for the menu switched the game's input mode");

    /* The first frame: nothing to draw on, the menu closes for good. */
    CHECK(fx_dxgi_open((void *)(ULONG_PTR)0x00FC3001) == 0, "the engine fixture could not create its swap chain");
    CHECK(fx_dxgi_present() == 0, "Present failed");
    memset(&st, 0, sizeof(st));
    CHECK(fx_pad_read(0, &st) == ERROR_SUCCESS && st.buttons == PAD_A && st.right_trigger == 255 && st.lx == 30000,
          "the gamepad stays held back after the menu has gone");
    CHECK(key_seen(KEY_W), "the keyboard stays held back after the menu has gone");
    mouse_move(7, 0);
    count = 4;
    CHECK(fx_input_events(1, events, sizeof(events[0]), &count, 0) == 0 && count == 1,
          "the mouse stays held back after the menu has gone");
}

/* ---- draw distance: the profile the game gets to read ---------------------------------------
 * The test driver has put a GamerProfile.xml next to the host and the default
 * render configuration into the archives of the data folder. */

static long disk_read(const wchar_t *path, char *out, unsigned cap)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD got = 0;

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (!ReadFile(h, out, cap, &got, NULL)) {
        got = (DWORD)-1;
    }
    CloseHandle(h);
    return (long)got;
}

static int contains(const char *text, long len, const char *needle)
{
    size_t n = strlen(needle);
    long i;
    for (i = 0; i + (long)n <= len; ++i) {
        if (memcmp(text + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static unsigned temporary_copies(void)
{
    wchar_t pattern[MAX_PATH + 64];
    WIN32_FIND_DATAW found;
    HANDLE h;
    unsigned n = 0;
    DWORD len = GetTempPathW(MAX_PATH, pattern);

    if (!len || len > MAX_PATH) {
        return 0;
    }
    _snwprintf(pattern + len, 60, L"fc3pp-profile-%lu-*.xml", (unsigned long)GetCurrentProcessId());
    h = FindFirstFileW(pattern, &found);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            ++n;
        } while (FindNextFileW(h, &found));
        FindClose(h);
    }
    return n;
}

/* applied: 1 = a preset is selected and the archives hold a render
 * configuration; 0 = the game must get exactly its file. */
static void scenario_draw_distance(int applied)
{
    static char disk[8192], wide[16384], narrow[16384], other[8192];
    static const char rewritten[] = "<GamerProfile>\r\n\t<RenderProfile ResolutionX=\"1280\" />\r\n</GamerProfile>\r\n";
    long disk_len = disk_read(L"GamerProfile.xml", disk, sizeof(disk));
    long n, m;

    CHECK(disk_len > 100, "the test profile is missing");
    if (disk_len <= 100) {
        return;
    }
    n = fx_read_file(1, L"GamerProfile.xml", wide, sizeof(wide));
    m = fx_read_file(0, L".\\GamerProfile.xml", narrow, sizeof(narrow));
    CHECK(n > 0 && m == n && memcmp(wide, narrow, (size_t)n) == 0,
          "the two ways of opening the profile give different text (%ld and %ld bytes)", n, m);
    if (applied) {
        CHECK(n > disk_len, "the profile the game read has nothing added (%ld bytes, on disk %ld)", n, disk_len);
        CHECK(contains(wide, n, "<Geometry>") && contains(wide, n, "<Shadow>") && contains(wide, n, "<Terrain>"),
              "the profile the game read lacks the preset's sections");
        CHECK(contains(wide, n, "id=\"ultrahigh\" LodScale=\"0.35\" KillLodScale=\"0.35\""),
              "the level-of-detail scales were not multiplied by 0.7");
        CHECK(contains(wide, n, "SunShadowRange0=\"4.5\" SunShadowRange1=\"18\" SunShadowRange2=\"90\""),
              "the shadow ranges were not multiplied by 1.5");
        CHECK(contains(wide, n, "</Terrain>\r\n</RenderProfile>\r\n\t<NetworkProfile />\r\n</GamerProfile>\r\n"),
              "the sections are not at the end of the render profile");
        CHECK(contains(wide, n, "<CustomQuality>") && memcmp(wide, disk, 120) == 0,
              "what was in the profile is not all there any more");
    } else {
        CHECK(n == disk_len && memcmp(wide, disk, (size_t)n) == 0, "the game did not get its profile unchanged");
    }
    /* nothing of it is on disk, and no copy is left behind */
    m = disk_read(L"GamerProfile.xml", other, sizeof(other));
    CHECK(m == disk_len && memcmp(other, disk, (size_t)m) == 0, "the profile on disk was changed");
    /* the first time a preset is applied, the profile is copied next to itself */
    m = disk_read(L"GamerProfile.xml.fc3pp.bak", other, sizeof(other));
    if (applied) {
        CHECK(m == disk_len && memcmp(other, disk, (size_t)m) == 0, "no copy of the profile was kept");
    } else {
        CHECK(m == -1, "a copy of the profile was made although no preset was applied");
    }
    CHECK(temporary_copies() == 0, "%u temporary copies of the profile were left behind", temporary_copies());

    /* other files, also with similar names, are the game's own business */
    n = fx_read_file(1, L"MyGamerProfile.xml", wide, sizeof(wide));
    m = disk_read(L"MyGamerProfile.xml", other, sizeof(other));
    CHECK(n > 0 && n == m && memcmp(wide, other, (size_t)n) == 0, "another file was not read as it is");
    CHECK(fx_read_file(1, L"no-such-folder\\GamerProfile.xml", wide, sizeof(wide)) == -1 &&
              fx_read_file(0, L"no-such-folder\\GamerProfile.xml", wide, sizeof(wide)) == -1,
          "a profile that does not exist could be opened");

    /* The game saves its profile: that goes to the real file, as written. */
    CHECK(fx_write_file(L"GamerProfile.xml", rewritten, sizeof(rewritten) - 1) == 0, "the profile could not be saved");
    m = disk_read(L"GamerProfile.xml", other, sizeof(other));
    CHECK(m == (long)sizeof(rewritten) - 1 && memcmp(other, rewritten, (size_t)m) == 0,
          "the saved profile is not what the game wrote");
    /* an open that appends goes to the real file, too */
    CHECK(fx_append_file(L"GamerProfile.xml", "\r\n", 2) == 0, "the profile could not be appended to");
    m = disk_read(L"GamerProfile.xml", other, sizeof(other));
    CHECK(m == (long)sizeof(rewritten) + 1 && memcmp(other, rewritten, sizeof(rewritten) - 1) == 0,
          "appending did not reach the profile on disk");
    /* and reading it again starts from what is on disk now */
    n = fx_read_file(1, L"GamerProfile.xml", wide, sizeof(wide));
    if (applied) {
        CHECK(n > m && contains(wide, n, "<RenderProfile ResolutionX=\"1280\" ><Geometry>") &&
                  contains(wide, n, "</Terrain>\r\n</RenderProfile>\r\n</GamerProfile>\r\n\r\n"),
              "the saved profile was not read back with the preset");
        /* the copy is the first profile, not the rewritten one */
        m = disk_read(L"GamerProfile.xml.fc3pp.bak", other, sizeof(other));
        CHECK(m == disk_len && memcmp(other, disk, (size_t)m) == 0, "the copy of the profile was overwritten");
    } else {
        CHECK(n == m && memcmp(wide, other, (size_t)n) == 0, "the saved profile was not read back as it is");
    }
    CHECK(temporary_copies() == 0, "%u temporary copies of the profile were left behind", temporary_copies());
}

/* ---- top level files that several textures name ---------------------------------------
 * The test driver has built archives in which two textures of the "mod" name
 * the top level file tex\boar_d_mip0.xbt, and left that file (shared_top.xbt)
 * and an ordinary one (fresh_top.xbt) next to the host. The engine fixture's
 * top level resources follow their link to a texture as the game's do: one
 * whose texture is gone crashes the process unless the patch keeps the engine
 * away from it. */

static unsigned long long file_name_hash(const char *path)
{
    unsigned long long crc = 0;
    int bit;

    for (; *path; ++path) {
        unsigned char c = (unsigned char)*path;
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c + 32);
        }
        crc ^= c;
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xD800000000000000ull : crc >> 1;
        }
    }
    return crc;
}

enum { TOP_LOADS, TOP_UNLOADS, TOP_ADDED, TOP_COUNTER, TOP_SIZE };

/* handled: 1 = the patch keeps shared files away from the engine. */
static void scenario_shared_tops(int handled)
{
    static unsigned char shared_file[4096], fresh_file[4096];
    unsigned long long shared_name = file_name_hash("tex\\boar_d_mip0.xbt");
    unsigned long long fresh_name = file_name_hash("tex\\rock_d_mip0.xbt");
    unsigned shared_size = read_local_file("shared_top.xbt", shared_file, sizeof(shared_file));
    unsigned fresh_size = read_local_file("fresh_top.xbt", fresh_file, sizeof(fresh_file));
    void *top;
    long st[5];

    CHECK(shared_size > 160 && fresh_size > 160, "the test driver did not provide the top level files");
    if (shared_size <= 160 || fresh_size <= 160) {
        return;
    }
    Sleep(300); /* the patch reads the archives in the background */

    /* An ordinary top level file: loaded and unloaded by the engine as always. */
    top = fx_top_create(0, (unsigned)fresh_name, (unsigned)(fresh_name >> 32), 0);
    CHECK(fx_top_load(top, fresh_file, (unsigned)fresh_size) == 0, "an ordinary top level file was not loaded");
    fx_top_state(0, st);
    CHECK(st[TOP_LOADS] == 1 && st[TOP_ADDED] == 1, "an ordinary top level did not reach the engine");
    fx_top_unload(top);
    fx_top_state(0, st);
    CHECK(st[TOP_UNLOADS] == 1 && st[TOP_COUNTER] == 99 && st[TOP_SIZE] == 0,
          "an ordinary top level was not unloaded by the engine (%ld unloads, counter %ld)", st[TOP_UNLOADS],
          st[TOP_COUNTER]);
    if (!handled) {
        return; /* without the patch the rest is the crash */
    }

    /* A file two textures name, recognised by its name. The texture it is
     * linked to has been destroyed: following the link is the crash. */
    top = fx_top_create(1, (unsigned)shared_name, (unsigned)(shared_name >> 32), 1);
    CHECK(fx_top_load(top, shared_file, (unsigned)shared_size) == 1, "a shared top level file was loaded");
    fx_top_unload(top);
    fx_top_state(1, st);
    CHECK(st[TOP_LOADS] == 0 && st[TOP_UNLOADS] == 0 && st[TOP_SIZE] == 4096,
          "the engine's methods ran for a shared top level file (%ld loads, %ld unloads)", st[TOP_LOADS],
          st[TOP_UNLOADS]);
    /* unloading without a load before (the engine releases what it never used) */
    top = fx_top_create(1, (unsigned)shared_name, (unsigned)(shared_name >> 32), 1);
    fx_top_unload(top);
    fx_top_state(1, st);
    CHECK(st[TOP_UNLOADS] == 0, "a shared top level file that was never loaded reached the engine's Unload");

    /* The same file under a name the patch does not know: recognised by its content. */
    top = fx_top_create(2, 0x11111111u, 0x22222222u, 1);
    CHECK(fx_top_load(top, shared_file, (unsigned)shared_size) == 1, "a shared top level file was loaded (by content)");
    fx_top_unload(top);
    fx_top_state(2, st);
    CHECK(st[TOP_LOADS] == 0 && st[TOP_UNLOADS] == 0, "the engine's methods ran for a shared file known by content");
    /* The engine reuses the address for another resource: that one is ordinary again. */
    top = fx_top_create(2, 0x11111111u, 0x22222222u, 0);
    CHECK(fx_top_load(top, shared_file, (unsigned)shared_size) == 1, "the shared file was loaded the second time");
    top = fx_top_create(2, 0x33333333u, 0x44444444u, 0);
    CHECK(fx_top_load(top, fresh_file, (unsigned)fresh_size) == 0, "an ordinary file at a reused address was refused");
    fx_top_unload(top);
    fx_top_state(2, st);
    CHECK(st[TOP_LOADS] == 1 && st[TOP_UNLOADS] == 1 && st[TOP_COUNTER] == 99,
          "an ordinary file at a reused address did not reach the engine (%ld loads, %ld unloads)", st[TOP_LOADS],
          st[TOP_UNLOADS]);

    /* The last line of defence: an ordinary file whose texture, or whose
     * texture's record, is gone when it is unloaded. */
    top = fx_top_create(3, (unsigned)fresh_name, (unsigned)(fresh_name >> 32), 2);
    fx_top_unload(top);
    fx_top_state(3, st);
    CHECK(st[TOP_UNLOADS] == 0 && st[TOP_SIZE] == 0, "unloading for a texture whose record is gone: %ld unloads, size %ld",
          st[TOP_UNLOADS], st[TOP_SIZE]);
    top = fx_top_create(3, (unsigned)fresh_name, (unsigned)(fresh_name >> 32), 1);
    fx_top_unload(top);
    fx_top_state(3, st);
    CHECK(st[TOP_UNLOADS] == 0 && st[TOP_SIZE] == 0, "unloading for a texture that is gone: %ld unloads, size %ld",
          st[TOP_UNLOADS], st[TOP_SIZE]);
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
    } else if (strcmp(scenario, "threads") == 0) {
        scenario_threads(&machine);
    } else if (strcmp(scenario, "fastcores") == 0) {
        scenario_fast_cores(&machine, max_cores, argc > 3 ? (unsigned)atoi(argv[3]) : 0);
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
    } else if (strcmp(scenario, "menu") == 0 || strcmp(scenario, "menu-open") == 0 ||
               strcmp(scenario, "menu-off") == 0) {
        scenario_menu();
    } else if (strcmp(scenario, "input") == 0) {
        scenario_input(1, 1);
    } else if (strcmp(scenario, "input-off") == 0) {
        scenario_input(0, 1);
    } else if (strcmp(scenario, "input-nomenu") == 0) {
        scenario_input(0, 0);
    } else if (strcmp(scenario, "input-switch-nomenu") == 0) {
        scenario_input(1, 0);
    } else if (strcmp(scenario, "input-held") == 0) {
        scenario_input_held();
    } else if (strcmp(scenario, "drawdistance") == 0) {
        scenario_draw_distance(1);
    } else if (strcmp(scenario, "drawdistance-off") == 0) {
        scenario_draw_distance(0);
    } else if (strcmp(scenario, "shared-tops") == 0) {
        scenario_shared_tops(1);
    } else if (strcmp(scenario, "shared-tops-off") == 0) {
        scenario_shared_tops(0);
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
