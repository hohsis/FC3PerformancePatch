/* Engine-facing CPU layer: import-table hooks for the game's own modules.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cpu_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ini.h"
#include "log.h"
#include "pe_image.h"

/* ---- state -------------------------------------------------------------- */

static shim_kind g_kind = SHIM_NONE;
static cpu_selection g_sel;
static unsigned char g_vcore[FC3PP_MAX_CPUS]; /* dense core id per virtual processor */
static unsigned g_vcore_count;
static unsigned long g_vfull;
static unsigned long g_sim_mask;
static DWORD_PTR g_real_system_mask;
static int g_trace;
static LONG g_trace_budget = 400;
static int g_sleep_yield;
static LONG g_sleep_zero, g_sleep_one, g_sleep_other;
static LONG64 g_sleep_one_ticks;
static shim_module_loaded_fn g_loaded_cb;
static LONG g_confined_logged;

typedef struct fc3_group_affinity {
    ULONG_PTR Mask;
    WORD Group;
    WORD Reserved[3];
} fc3_group_affinity;

typedef BOOL(WINAPI *glpi_ex_fn)(int relationship, void *buffer, PDWORD length);
typedef BOOL(WINAPI *glpi_fn)(void *buffer, PDWORD length);
typedef BOOL(WINAPI *get_thread_group_affinity_fn)(HANDLE thread, fc3_group_affinity *affinity);

/* Real kernel32 entry points (never the hooked import slots). */
static VOID(WINAPI *r_GetSystemInfo)(LPSYSTEM_INFO);
static BOOL(WINAPI *r_GetProcessAffinityMask)(HANDLE, PDWORD_PTR, PDWORD_PTR);
static BOOL(WINAPI *r_SetProcessAffinityMask)(HANDLE, DWORD_PTR);
static DWORD_PTR(WINAPI *r_SetThreadAffinityMask)(HANDLE, DWORD_PTR);
static DWORD(WINAPI *r_SetThreadIdealProcessor)(HANDLE, DWORD);
static FARPROC(WINAPI *r_GetProcAddress)(HMODULE, LPCSTR);
static HMODULE(WINAPI *r_LoadLibraryA)(LPCSTR);
static HMODULE(WINAPI *r_LoadLibraryW)(LPCWSTR);
static HMODULE(WINAPI *r_LoadLibraryExA)(LPCSTR, HANDLE, DWORD);
static HMODULE(WINAPI *r_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
static VOID(WINAPI *r_Sleep)(DWORD);

static void resolve_real(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    if (r_GetSystemInfo || !k32) {
        return;
    }
#define RESOLVE(name)                               \
    do {                                            \
        FARPROC p_ = GetProcAddress(k32, #name);    \
        memcpy(&r_##name, &p_, sizeof(p_));         \
    } while (0)
    RESOLVE(GetSystemInfo);
    RESOLVE(GetProcessAffinityMask);
    RESOLVE(SetProcessAffinityMask);
    RESOLVE(SetThreadAffinityMask);
    RESOLVE(SetThreadIdealProcessor);
    RESOLVE(GetProcAddress);
    RESOLVE(LoadLibraryA);
    RESOLVE(LoadLibraryW);
    RESOLVE(LoadLibraryExA);
    RESOLVE(LoadLibraryExW);
    RESOLVE(Sleep);
#undef RESOLVE
}

static int trace_on(void)
{
    return g_trace && log_level() >= LOG_DEBUG && InterlockedDecrement(&g_trace_budget) >= 0;
}

static int is_current_process(HANDLE h)
{
    return h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId();
}

/* ---- topology query ------------------------------------------------------ */

/* Reads a small decimal number from a file of the host system. Under Wine the
 * host's file system is reachable through the "\\?\unix" prefix and, in the
 * usual set-up, as drive Z:. Returns 0 when the file cannot be read. */
static char g_host_root[200];

void shim_set_host_root(const char *folder)
{
    if (folder && strlen(folder) < sizeof(g_host_root)) {
        strcpy(g_host_root, folder);
    }
}

static unsigned long read_host_number(const char *unix_path)
{
    const char *prefixes[2] = {"\\\\?\\unix", "Z:"};
    unsigned p, count = 2;

    if (g_host_root[0]) {
        prefixes[0] = g_host_root;
        count = 1;
    }
    for (p = 0; p < count; ++p) {
        char path[320], text[32];
        HANDLE h;
        DWORD got = 0;
        size_t i;

        if (snprintf(path, sizeof(path), "%s%s", prefixes[p], unix_path) >= (int)sizeof(path)) {
            continue;
        }
        for (i = 0; path[i]; ++i) {
            if (path[i] == '/') {
                path[i] = '\\';
            }
        }
        h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            continue;
        }
        if (ReadFile(h, text, sizeof(text) - 1, &got, NULL) && got) {
            unsigned long v = 0;
            text[got] = '\0';
            for (i = 0; text[i] >= '0' && text[i] <= '9' && v < 100000000ul; ++i) {
                v = v * 10 + (unsigned long)(text[i] - '0');
            }
            CloseHandle(h);
            return v;
        }
        CloseHandle(h);
    }
    return 0;
}

/* Wine fills in EfficiencyClass only for Intel hybrid processors. On ARM
 * processors with fast and slow cores (phones and handhelds that run x86
 * games through FEX) every core is reported as class 0, and the slow cores
 * usually come first - exactly the ones "the first N cores" would pick. The
 * Linux kernel publishes each core's relative capacity; use it to tell them
 * apart. */
static void rank_cores_from_host(cpu_topology *topo)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    unsigned long capacity[FC3PP_MAX_CPUS];
    unsigned i, classes;

    if (!g_host_root[0] && (!ntdll || !GetProcAddress(ntdll, "wine_get_version"))) {
        return; /* Windows reports the classes itself */
    }
    memset(capacity, 0, sizeof(capacity));
    for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
        if (topo->cpu[i].present) {
            char path[96];
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpu_capacity", i);
            capacity[i] = read_host_number(path);
            if (!capacity[i]) {
                return; /* not an ARM host, or the host's files are out of reach */
            }
        }
    }
    classes = cpu_topology_rank_by_capacity(topo, capacity);
    if (classes) {
        LOGI("cpu: the host reports cores of %u different speeds (cpu_capacity); faster cores are preferred", classes);
    }
}

int shim_query_topology(cpu_topology *topo, unsigned long *process_mask, unsigned long *system_mask)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    DWORD_PTR pm = 0, sm = 0;
    unsigned group = 0;
    glpi_ex_fn glpi_ex;
    glpi_fn glpi;
    get_thread_group_affinity_fn gtga;

    cpu_topology_init(topo);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) || !pm) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        pm = sm = si.dwActiveProcessorMask ? si.dwActiveProcessorMask : 1;
    }
    if (process_mask) {
        *process_mask = (unsigned long)pm;
    }
    if (system_mask) {
        *system_mask = (unsigned long)sm;
    }
    if (!k32) {
        cpu_topology_flat(topo, (unsigned long)sm);
        return 0;
    }

    gtga = (get_thread_group_affinity_fn)(void *)GetProcAddress(k32, "GetThreadGroupAffinity");
    if (gtga) {
        fc3_group_affinity ga;
        memset(&ga, 0, sizeof(ga));
        if (gtga(GetCurrentThread(), &ga)) {
            group = ga.Group;
        }
    }

    glpi_ex = (glpi_ex_fn)(void *)GetProcAddress(k32, "GetLogicalProcessorInformationEx");
    if (glpi_ex) {
        DWORD len = 0;
        glpi_ex(0 /* RelationProcessorCore */, NULL, &len);
        if (len && len < (1u << 20)) {
            unsigned char *buf = (unsigned char *)malloc(len);
            if (buf) {
                if (glpi_ex(0, buf, &len)) {
                    cpu_topology_from_glpi_ex(topo, buf, len, group);
                }
                free(buf);
            }
        }
    }
    if (topo->core_count == 0) {
        glpi = (glpi_fn)(void *)GetProcAddress(k32, "GetLogicalProcessorInformation");
        if (glpi) {
            DWORD len = 0;
            glpi(NULL, &len);
            if (len && len < (1u << 20)) {
                unsigned char *buf = (unsigned char *)malloc(len);
                if (buf) {
                    if (glpi(buf, &len)) {
                        cpu_topology_from_glpi(topo, buf, len);
                    }
                    free(buf);
                }
            }
        }
    }
    rank_cores_from_host(topo);
    /* No usable topology, or one that does not overlap the processors this
     * process may run on: fall back to "every logical processor is a core". */
    {
        unsigned i;
        int overlap = 0;
        for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
            if (topo->cpu[i].present && (((unsigned long)pm >> i) & 1ul)) {
                overlap = 1;
                break;
            }
        }
        if (!overlap) {
            cpu_topology_flat(topo, (unsigned long)pm);
        }
    }
    return 0;
}

/* ---- configuration -------------------------------------------------------- */

void shim_configure(shim_kind kind, const cpu_topology *topo, const cpu_selection *sel, const fc3pp_config *cfg)
{
    unsigned i, j;

    resolve_real();
    g_kind = kind;
    g_trace = cfg ? cfg->trace_cpu_api : 0;
    g_sleep_yield = cfg ? cfg->sleep_zero_yield : 0;
    g_sim_mask = cfg ? cfg->simulate_affinity_mask : 0;
    if (r_GetProcessAffinityMask) {
        DWORD_PTR pm = 0, sm = 0;
        if (r_GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) {
            g_real_system_mask = sm;
        }
    }
    memset(&g_sel, 0, sizeof(g_sel));
    memset(g_vcore, 0, sizeof(g_vcore));
    g_vcore_count = 0;
    g_vfull = 0;
    if (kind != SHIM_VIRTUALIZE || !sel || !topo) {
        return;
    }
    g_sel = *sel;
    g_vfull = cpu_virtual_full_mask(sel);
    /* Dense virtual core ids: processors that share a real core share an id. */
    for (i = 0; i < g_sel.count; ++i) {
        unsigned real_core = topo->cpu[g_sel.real[i]].core;
        for (j = 0; j < i; ++j) {
            if (topo->cpu[g_sel.real[j]].core == real_core) {
                break;
            }
        }
        g_vcore[i] = (j < i) ? g_vcore[j] : (unsigned char)g_vcore_count++;
    }
}

shim_kind shim_current_kind(void)
{
    return g_kind;
}

/* ---- CPU hooks ------------------------------------------------------------ */

static VOID WINAPI hk_GetSystemInfo(LPSYSTEM_INFO si)
{
    r_GetSystemInfo(si);
    if (!si) {
        return;
    }
    if (g_kind == SHIM_VIRTUALIZE) {
        si->dwNumberOfProcessors = g_sel.count;
        si->dwActiveProcessorMask = g_vfull;
    } else if (g_kind == SHIM_SIMULATE && g_sim_mask) {
        si->dwNumberOfProcessors = cpu_popcount(g_sim_mask);
        si->dwActiveProcessorMask = g_sim_mask;
    }
    if (trace_on()) {
        LOGD("trace: GetSystemInfo -> %lu processors", (unsigned long)si->dwNumberOfProcessors);
    }
}

static BOOL WINAPI hk_GetProcessAffinityMask(HANDLE process, PDWORD_PTR process_mask, PDWORD_PTR system_mask)
{
    DWORD_PTR pm = 0, sm = 0;
    if (!r_GetProcessAffinityMask(process, &pm, &sm)) {
        return FALSE;
    }
    if (is_current_process(process)) {
        if (g_kind == SHIM_VIRTUALIZE) {
            pm = sm = g_vfull;
        } else if (g_kind == SHIM_CLAMP) {
            /* Keep bit 31 clear so "mask <= systemAffinity" loops terminate. */
            pm &= 0x7FFFFFFFul;
            sm &= 0x7FFFFFFFul;
        } else if (g_kind == SHIM_SIMULATE && g_sim_mask) {
            pm = sm = g_sim_mask;
        }
    }
    if (process_mask) {
        *process_mask = pm;
    }
    if (system_mask) {
        *system_mask = sm;
    }
    if (trace_on()) {
        LOGD("trace: GetProcessAffinityMask -> process=0x%08lX system=0x%08lX", (unsigned long)pm,
             (unsigned long)sm);
    }
    return TRUE;
}

static BOOL WINAPI hk_SetProcessAffinityMask(HANDLE process, DWORD_PTR mask)
{
    if (g_kind == SHIM_VIRTUALIZE && is_current_process(process)) {
        /* The engine both pins the whole process to single processors while it
         * probes CPUID and later resets the process affinity to the system
         * mask. Process affinity is owned by this patch; report success. */
        if (trace_on()) {
            LOGD("trace: SetProcessAffinityMask(0x%08lX) ignored (tid %lu)", (unsigned long)mask,
                 (unsigned long)GetCurrentThreadId());
        }
        return TRUE;
    }
    if (g_kind == SHIM_SIMULATE && g_real_system_mask) {
        /* The simulated machine has processors this one lacks; behave as if
         * they existed by ignoring them instead of failing the whole call. */
        mask &= g_real_system_mask;
        if (!mask) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }
    if (trace_on()) {
        LOGD("trace: SetProcessAffinityMask(0x%08lX) passed through (tid %lu)", (unsigned long)mask,
             (unsigned long)GetCurrentThreadId());
    }
    return r_SetProcessAffinityMask(process, mask);
}

static DWORD_PTR WINAPI hk_SetThreadAffinityMask(HANDLE thread, DWORD_PTR mask)
{
    DWORD_PTR previous;
    unsigned long real_mask;
    unsigned long virtual_previous;

    if (g_kind != SHIM_VIRTUALIZE) {
        if (g_kind == SHIM_SIMULATE && g_real_system_mask) {
            mask &= g_real_system_mask; /* processors that only exist in the simulation */
            if (!mask) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return 0;
            }
        }
        previous = r_SetThreadAffinityMask(thread, mask);
        if (trace_on()) {
            LOGD("trace: SetThreadAffinityMask(0x%08lX) -> 0x%08lX (tid %lu)", (unsigned long)mask,
                 (unsigned long)previous, (unsigned long)GetCurrentThreadId());
        }
        return previous;
    }
    real_mask = cpu_v2r_mask(&g_sel, (unsigned long)mask & g_vfull);
    if (((unsigned long)mask & g_vfull) == g_vfull) {
        /* "Anywhere": that includes the cores kept free of pinned workers. */
        real_mask |= g_sel.spared_mask;
    }
    if (!real_mask) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    previous = r_SetThreadAffinityMask(thread, real_mask);
    if (!previous) {
        /* The processors chosen at start-up may no longer be available: some
         * launchers (Android ones in particular) confine the process to other
         * processors from outside once its window appears. The engine only
         * wants its threads spread out; give it what is still possible
         * rather than a failure it does not expect. */
        DWORD_PTR pm = 0, sm = 0;
        if (r_GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm) && pm &&
            ((unsigned long)pm & real_mask) != real_mask) {
            unsigned long possible = real_mask & (unsigned long)pm;
            previous = r_SetThreadAffinityMask(thread, possible ? possible : pm);
            if (previous && InterlockedExchange(&g_confined_logged, 1) == 0) {
                LOGI("cpu: the process was confined to other processors after start-up (affinity now 0x%08lX); "
                     "engine threads are placed on what is left of the selection",
                     (unsigned long)pm);
            }
        }
    }
    if (trace_on()) {
        LOGD("trace: SetThreadAffinityMask(virtual 0x%08lX -> real 0x%08lX) -> 0x%08lX (thread %lu, called by tid %lu)",
             (unsigned long)mask, real_mask, (unsigned long)previous, (unsigned long)GetThreadId(thread),
             (unsigned long)GetCurrentThreadId());
    }
    if (!previous) {
        return 0;
    }
    virtual_previous = cpu_r2v_mask(&g_sel, (unsigned long)previous);
    if (!virtual_previous || ((unsigned long)previous & ~g_sel.real_mask)) {
        virtual_previous = g_vfull; /* the thread was not confined to the selection */
    }
    return virtual_previous;
}

static DWORD WINAPI hk_SetThreadIdealProcessor(HANDLE thread, DWORD ideal)
{
    DWORD previous;
    unsigned i;

    if (g_kind != SHIM_VIRTUALIZE || g_sel.count == 0) {
        return r_SetThreadIdealProcessor(thread, ideal);
    }
    if (ideal != MAXIMUM_PROCESSORS) {
        ideal = g_sel.real[ideal % g_sel.count];
    }
    previous = r_SetThreadIdealProcessor(thread, ideal);
    if (trace_on()) {
        LOGD("trace: SetThreadIdealProcessor(real %lu) -> %ld (thread %lu, called by tid %lu)", (unsigned long)ideal,
             (long)previous, (unsigned long)GetThreadId(thread), (unsigned long)GetCurrentThreadId());
    }
    if (previous == (DWORD)-1) {
        return previous;
    }
    for (i = 0; i < g_sel.count; ++i) {
        if (g_sel.real[i] == previous) {
            return i;
        }
    }
    return 0;
}

/* Synthesised GetLogicalProcessorInformation: the Havok thread pool resolves
 * this dynamically and counts RelationProcessorCore records. */
static BOOL WINAPI hk_GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer, PDWORD length)
{
    DWORD needed = (DWORD)((g_vcore_count + 2) * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
    unsigned core, i;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION out = buffer;

    if (!length) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!buffer || *length < needed) {
        *length = needed;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memset(buffer, 0, needed);
    for (core = 0; core < g_vcore_count; ++core, ++out) {
        ULONG_PTR mask = 0;
        for (i = 0; i < g_sel.count; ++i) {
            if (g_vcore[i] == core) {
                mask |= (ULONG_PTR)1 << i;
            }
        }
        out->ProcessorMask = mask;
        out->Relationship = RelationProcessorCore;
        out->ProcessorCore.Flags = (mask & (mask - 1)) ? 1 : 0; /* LTP_PC_SMT when shared */
    }
    out->ProcessorMask = g_vfull;
    out->Relationship = RelationNumaNode;
    out->NumaNode.NodeNumber = 0;
    ++out;
    out->ProcessorMask = g_vfull;
    out->Relationship = RelationProcessorPackage;
    *length = needed;
    if (trace_on()) {
        LOGD("trace: GetLogicalProcessorInformation -> %u cores", g_vcore_count);
    }
    return TRUE;
}

static FARPROC WINAPI hk_GetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC real = r_GetProcAddress(module, name);
    if (real && g_kind == SHIM_VIRTUALIZE && ((ULONG_PTR)name >> 16) != 0 &&
        strcmp(name, "GetLogicalProcessorInformation") == 0) {
        return (FARPROC)(void *)hk_GetLogicalProcessorInformation;
    }
    return real;
}

unsigned shim_install_cpu_hooks(HMODULE mod)
{
    static const char k32[] = "KERNEL32.dll";
    unsigned n = 0;

    resolve_real();
    if (g_kind == SHIM_NONE || !r_GetProcessAffinityMask) {
        return 0;
    }
    n += pei_hook_import(mod, k32, "GetProcessAffinityMask", (void *)hk_GetProcessAffinityMask, NULL) == 1;
    if (g_kind == SHIM_CLAMP) {
        return n;
    }
    n += pei_hook_import(mod, k32, "GetSystemInfo", (void *)hk_GetSystemInfo, NULL) == 1;
    n += pei_hook_import(mod, k32, "SetThreadAffinityMask", (void *)hk_SetThreadAffinityMask, NULL) == 1;
    n += pei_hook_import(mod, k32, "SetProcessAffinityMask", (void *)hk_SetProcessAffinityMask, NULL) == 1;
    if (g_kind == SHIM_VIRTUALIZE) {
        n += pei_hook_import(mod, k32, "SetThreadIdealProcessor", (void *)hk_SetThreadIdealProcessor, NULL) == 1;
        n += pei_hook_import(mod, k32, "GetProcAddress", (void *)hk_GetProcAddress, NULL) == 1;
    }
    return n;
}

/* ---- loader hooks ---------------------------------------------------------- */

static void notify_loaded(HMODULE mod)
{
    if (mod && g_loaded_cb) {
        DWORD saved = GetLastError();
        g_loaded_cb(mod);
        SetLastError(saved);
    }
}

static HMODULE WINAPI hk_LoadLibraryA(LPCSTR name)
{
    HMODULE mod = r_LoadLibraryA(name);
    notify_loaded(mod);
    return mod;
}

static HMODULE WINAPI hk_LoadLibraryW(LPCWSTR name)
{
    HMODULE mod = r_LoadLibraryW(name);
    notify_loaded(mod);
    return mod;
}

static HMODULE WINAPI hk_LoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags)
{
    HMODULE mod = r_LoadLibraryExA(name, file, flags);
    if (!(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | DONT_RESOLVE_DLL_REFERENCES))) {
        notify_loaded(mod);
    }
    return mod;
}

static HMODULE WINAPI hk_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    HMODULE mod = r_LoadLibraryExW(name, file, flags);
    if (!(flags & (LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE | DONT_RESOLVE_DLL_REFERENCES))) {
        notify_loaded(mod);
    }
    return mod;
}

unsigned shim_install_loader_hooks(HMODULE mod, shim_module_loaded_fn callback)
{
    static const char k32[] = "KERNEL32.dll";
    unsigned n = 0;

    resolve_real();
    if (!r_LoadLibraryA) {
        return 0;
    }
    g_loaded_cb = callback;
    n += pei_hook_import(mod, k32, "LoadLibraryA", (void *)hk_LoadLibraryA, NULL) == 1;
    n += pei_hook_import(mod, k32, "LoadLibraryW", (void *)hk_LoadLibraryW, NULL) == 1;
    n += pei_hook_import(mod, k32, "LoadLibraryExA", (void *)hk_LoadLibraryExA, NULL) == 1;
    n += pei_hook_import(mod, k32, "LoadLibraryExW", (void *)hk_LoadLibraryExW, NULL) == 1;
    return n;
}

/* ---- Sleep hook ------------------------------------------------------------- */

static VOID WINAPI hk_Sleep(DWORD ms)
{
    if (ms == 0) {
        InterlockedIncrement(&g_sleep_zero);
        if (g_sleep_yield) {
            /* Sleep(0) only yields to threads of equal priority that are ready
             * on the same processor; SwitchToThread yields to any ready
             * thread, which is what a polite spin-wait wants. */
            if (!SwitchToThread()) {
                YieldProcessor();
            }
            return;
        }
    } else if (ms == 1) {
        InterlockedIncrement(&g_sleep_one);
        if (log_level() >= LOG_DEBUG) {
            /* Measure how long "1 ms" really takes: it shows directly whether
             * the process runs with a coarse (15.6 ms) system timer. */
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            r_Sleep(1);
            QueryPerformanceCounter(&t1);
            InterlockedExchangeAdd64(&g_sleep_one_ticks, t1.QuadPart - t0.QuadPart);
            return;
        }
    } else {
        InterlockedIncrement(&g_sleep_other);
    }
    r_Sleep(ms);
}

unsigned shim_install_sleep_hook(HMODULE mod)
{
    resolve_real();
    if (!r_Sleep) {
        return 0;
    }
    return pei_hook_import(mod, "KERNEL32.dll", "Sleep", (void *)hk_Sleep, NULL) == 1;
}

void shim_log_stats(void)
{
    LARGE_INTEGER freq;
    double average_ms = 0.0;

    if (g_sleep_one > 0 && QueryPerformanceFrequency(&freq) && freq.QuadPart > 0) {
        average_ms = (double)g_sleep_one_ticks * 1000.0 / (double)freq.QuadPart / (double)g_sleep_one;
    }
    LOGD("Sleep calls from the engine: Sleep(0)=%ld Sleep(1)=%ld longer=%ld; a Sleep(1) took %.2f ms on average",
         (long)g_sleep_zero, (long)g_sleep_one, (long)g_sleep_other, average_ms);
}
