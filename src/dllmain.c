/* FC3PerformancePatch - version.dll proxy entry point.
 *
 * Load order: the game executable statically imports FC3.dll / FC3_d3d11.dll,
 * which statically import VERSION.dll. The loader maps every module and binds
 * all imports before it runs any initialisation routine, and it initialises
 * dependencies first. So when this DllMain runs, the engine module is mapped
 * and its import table is bound, but none of its code has executed yet.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "audio.h"
#include "config.h"
#include "cpu_shim.h"
#include "cpu_topology.h"
#include "display.h"
#include "fc3pp.h"
#include "gfx_profile.h"
#include "ini.h"
#include "laa.h"
#include "log.h"
#include "menu.h"
#include "padswitch.h"
#include "patches.h"
#include "pe_image.h"
#include "proxy.h"
#include "tex_stale.h"
#include "texguard.h"
#include "texlink.h"
#include "threads.h"

#define MAX_TRACKED_MODULES 8
#define MAX_INI_SIZE (1u << 20)

/* Modules that contain engine code and get the full treatment at start-up. */
static const char *const ENGINE_MODULES[] = {"FC3_d3d11.dll", "FC3.dll"};
/* Game modules that are loaded later through LoadLibrary. */
static const char *const LATE_MODULES[] = {"systemdetection.dll"};

static fc3pp_config g_cfg;
static int g_active;
static UINT g_timer_period;
static HMODULE g_attached[MAX_TRACKED_MODULES];
static LONG g_attached_count;
static CRITICAL_SECTION g_attach_lock;
static int g_need_clamp; /* a module with an unpatched enumeration loop needs the clamp hook */

static void on_module_loaded(HMODULE mod);

static char *read_text_file(const wchar_t *path, size_t *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, got = 0;
    char *buf = NULL;

    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    size = GetFileSize(h, NULL);
    if (size != INVALID_FILE_SIZE && size <= MAX_INI_SIZE) {
        buf = (char *)malloc((size_t)size + 1);
        if (buf) {
            if (size && (!ReadFile(h, buf, size, &got, NULL) || got != size)) {
                free(buf);
                buf = NULL;
            } else {
                buf[size] = '\0';
                *len = size;
            }
        }
    }
    CloseHandle(h);
    return buf;
}

#if defined(_MSC_VER)
#include <intrin.h>
static void cpuid(unsigned leaf, unsigned regs[4])
{
    int r[4];
    __cpuid(r, (int)leaf);
    memcpy(regs, r, sizeof(r));
}
#else
#include <cpuid.h>
static void cpuid(unsigned leaf, unsigned regs[4])
{
    __cpuid(leaf, regs[0], regs[1], regs[2], regs[3]);
}
#endif

/* FEX (x86 on ARM64) sets the "hypervisor present" bit and answers the
 * hypervisor identification leaf with "FEXIFEXIEMU". */
static int running_under_fex(void)
{
    unsigned r[4];
    char vendor[13];

    cpuid(1, r);
    if (!(r[2] & 0x80000000u)) {
        return 0;
    }
    cpuid(0x40000000u, r);
    memcpy(vendor, &r[1], 4);
    memcpy(vendor + 4, &r[2], 4);
    memcpy(vendor + 8, &r[3], 4);
    vendor[12] = '\0';
    return strncmp(vendor, "FEXIFEXIEMU", 11) == 0;
}

static void log_environment(const wchar_t *host_name)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const char *(__cdecl *wine_get_version)(void) = NULL;
    SYSTEM_INFO si;
    OSVERSIONINFOW os;
    LONG(WINAPI *rtl_get_version)(OSVERSIONINFOW *) = NULL;

    LOGI(FC3PP_NAME " " FC3PP_VERSION " (version.dll proxy)");
    LOGI("host executable: %ls", host_name);
    if (ntdll) {
        FARPROC p = GetProcAddress(ntdll, "wine_get_version");
        memcpy(&wine_get_version, &p, sizeof(p));
        p = GetProcAddress(ntdll, "RtlGetVersion");
        memcpy(&rtl_get_version, &p, sizeof(p));
    }
    memset(&os, 0, sizeof(os));
    os.dwOSVersionInfoSize = sizeof(os);
    if (rtl_get_version && rtl_get_version(&os) == 0) {
        LOGI("operating system: Windows NT %lu.%lu build %lu%s", (unsigned long)os.dwMajorVersion,
             (unsigned long)os.dwMinorVersion, (unsigned long)os.dwBuildNumber,
             wine_get_version ? " (reported by Wine)" : "");
    }
    if (wine_get_version) {
        LOGI("running under Wine/Proton %s", wine_get_version());
    }
    if (running_under_fex()) {
        LOGI("running under the FEX x86 emulator (ARM processor)");
    }
    GetSystemInfo(&si);
    LOGI("logical processors visible to this 32-bit process: %lu", (unsigned long)si.dwNumberOfProcessors);
}

static void apply_laa(const wchar_t *host_path)
{
    laa_result r;

    if (laa_process_is_aware()) {
        LOGI("memory: process is large-address-aware (4 GB address space)");
        menu_set_status(MENU_STATUS_MEMORY, "4 GB address space");
        return;
    }
    menu_set_status(MENU_STATUS_MEMORY, "2 GB (4 GB after a restart)");
    if (!g_cfg.apply_large_address_aware) {
        LOGI("memory: executable is NOT large-address-aware and ApplyLargeAddressAware=0; "
             "the game is limited to 2 GB of address space");
        return;
    }
    r = laa_apply_to_file(host_path);
    switch (r) {
    case LAA_PATCHED:
        LOGI("memory: enabled the Large Address Aware flag on %ls; a backup was kept next to it. "
             "Restart the game for the 4 GB address space to take effect",
             log_basename_w(host_path));
        break;
    case LAA_ALREADY:
        LOGI("memory: %ls already carries the Large Address Aware flag; it takes effect on the next launch",
             log_basename_w(host_path));
        break;
    default:
        LOGE("memory: could not enable Large Address Aware on %ls: %s. "
             "Make the game folder writable, or set ApplyLargeAddressAware=0 to stop trying",
             log_basename_w(host_path), laa_result_text(r));
        break;
    }
}

static void apply_timer_resolution(void)
{
    if (g_cfg.timer_resolution_ms <= 0) {
        return;
    }
    /* Since Windows 10 2004 the timer resolution is per process: a game that
     * never calls timeBeginPeriod gets the default ~15.6 ms granularity for
     * Sleep() and waits, no matter what other programs requested. The engine
     * only imports timeGetTime, so request the resolution on its behalf.
     * winmm is a static import of this DLL, hence already initialised. */
    if (timeBeginPeriod((UINT)g_cfg.timer_resolution_ms) == TIMERR_NOERROR) {
        g_timer_period = (UINT)g_cfg.timer_resolution_ms;
        LOGI("timing: requested %u ms system timer resolution for this process", g_timer_period);
    } else {
        LOGE("timing: timeBeginPeriod(%d) failed", g_cfg.timer_resolution_ms);
    }
}

static int already_attached(HMODULE mod)
{
    LONG i;
    for (i = 0; i < g_attached_count && i < MAX_TRACKED_MODULES; ++i) {
        if (g_attached[i] == mod) {
            return 1;
        }
    }
    return 0;
}

/* Applies code patches and hooks to one game module. */
static void attach_module(HMODULE mod, const char *name, int is_engine)
{
    pei_section sections[32];
    unsigned nsec, i;
    unsigned fixed = 0, applied = 0, foreign = 0, rejected = 0;
    const char *variant = NULL;
    unsigned hooks = 0;
    int seen;

    if (!pei_valid(mod)) {
        LOGE("%s: not a 32-bit PE image, skipped", name);
        return;
    }
    /* The game unloads and reloads systemdetection.dll. A reloaded module is a
     * fresh, unpatched image (possibly at the same address), so the work below
     * is always redone - every step is idempotent. "seen" only keeps the log
     * quiet when a repeated LoadLibrary call found nothing left to do. */
    EnterCriticalSection(&g_attach_lock);
    seen = already_attached(mod);
    if (!seen && g_attached_count < MAX_TRACKED_MODULES) {
        g_attached[g_attached_count++] = mod;
    }

    if (g_cfg.fix_core_enumeration) {
        nsec = pei_sections(mod, sections, 32);
        if (nsec > 32) {
            nsec = 32;
        }
        for (i = 0; i < nsec; ++i) {
            patch_report rep;
            if (!(sections[i].characteristics & IMAGE_SCN_MEM_EXECUTE) || !sections[i].size) {
                continue;
            }
            fixed += patch_enum_loop(sections[i].start, sections[i].size, mem_patch_writer, NULL, &rep);
            applied += rep.applied;
            foreign += rep.foreign;
            rejected += rep.rejected;
            if (rep.variant) {
                variant = rep.variant;
            }
        }
        if (applied) {
            LOGI("%s: fixed the processor-enumeration loop (%u site%s, '%s' build variant)", name, applied,
                 applied == 1 ? "" : "s", variant ? variant : "?");
        } else if (fixed) {
            if (!seen) {
                LOGI("%s: processor-enumeration loop is already fixed", name);
            }
        } else if (foreign) {
            if (!seen) {
                LOGI("%s: processor-enumeration loop carries a third-party fix; left as is", name);
            }
        } else {
            if (!seen) {
                LOGI("%s: processor-enumeration loop signature not found%s; relying on the affinity-mask hook",
                     name, rejected ? " (candidates failed validation)" : "");
            }
            g_need_clamp = 1;
        }
    }

    if (shim_current_kind() == SHIM_NONE && g_need_clamp && g_cfg.fix_core_enumeration && !fixed && !foreign) {
        /* No topology layer and no code patch: install the minimal hook that
         * keeps the reported affinity mask below bit 31. */
        shim_configure(SHIM_CLAMP, NULL, NULL, &g_cfg);
    }
    hooks = shim_install_cpu_hooks(mod);
    if (is_engine) {
        hooks += shim_install_loader_hooks(mod, on_module_loaded);
        if (g_cfg.sleep_zero_yield || g_cfg.log_level >= LOG_DEBUG) {
            hooks += shim_install_sleep_hook(mod);
        }
        if (g_cfg.restore_fullscreen || g_cfg.menu_enabled) {
            unsigned dxgi = display_install_hooks(mod, g_cfg.restore_fullscreen,
                                                  g_cfg.menu_enabled ? menu_on_present : NULL);
            if (!dxgi && !seen) {
                menu_set_unavailable(); /* no DXGI: the DX9 renderer */
            }
            hooks += dxgi;
        } else {
            menu_set_unavailable();
        }
        padswitch_install(mod, name, g_cfg.auto_switch_input, g_cfg.input_without_focus);
        hooks += menu_install_hooks(mod);
        hooks += gfx_profile_install(mod, g_cfg.draw_distance);
        hooks += audio_install(mod, &g_cfg);
        if (g_cfg.texture_shared) {
            texlink_install(mod, name);
        }
        if (g_cfg.texture_guard) {
            texguard_install(mod, name, g_cfg.texture_scan);
        }
        tex_stale_start(); /* the archive scan, for whichever of the two asked for it */
    }
    if (!seen || applied) {
        LOGI("%s: %u import hook%s installed", name, hooks, hooks == 1 ? "" : "s");
    }
    LeaveCriticalSection(&g_attach_lock);
}

/* Called after a hooked LoadLibrary returned: patch late game modules. */
static void on_module_loaded(HMODULE mod)
{
    char path[MAX_PATH];
    const char *base;
    size_t i;

    if (!g_active) {
        return;
    }
    if (!GetModuleFileNameA(mod, path, sizeof(path))) {
        return;
    }
    path[sizeof(path) - 1] = '\0';
    base = log_basename(path);
    for (i = 0; i < sizeof(LATE_MODULES) / sizeof(LATE_MODULES[0]); ++i) {
        if (ini_streq_nocase(base, LATE_MODULES[i])) {
            attach_module(mod, LATE_MODULES[i], 0);
            return;
        }
    }
}

static void setup_cpu(void)
{
    cpu_topology topo;
    cpu_selection sel;
    cpu_policy policy;
    unsigned long process_mask = 0, system_mask = 0;
    char list[160];
    size_t pos = 0;
    unsigned i;
    static const char *const sources[] = {"none", "GetLogicalProcessorInformationEx",
                                          "GetLogicalProcessorInformation", "GetSystemInfo (flat)"};

    shim_set_host_root(g_cfg.host_sys_root);
    shim_query_topology(&topo, &process_mask, &system_mask);
    LOGI("cpu: %u physical core%s detected via %s; process affinity 0x%08lX, system 0x%08lX", topo.core_count,
         topo.core_count == 1 ? "" : "s", sources[topo.source], process_mask, system_mask);

    if (g_cfg.mode == CPU_MODE_OFF) {
        if (g_cfg.simulate_affinity_mask) {
            shim_configure(SHIM_SIMULATE, NULL, NULL, &g_cfg);
            LOGI("cpu: DEBUG simulation active - the engine is told the affinity mask is 0x%08lX",
                 g_cfg.simulate_affinity_mask);
        } else {
            shim_configure(SHIM_NONE, NULL, NULL, &g_cfg);
            LOGI("cpu: Mode=off - the engine's own processor detection is left untouched");
        }
        return;
    }

    memset(&policy, 0, sizeof(policy));
    policy.max_cores = g_cfg.max_cores;
    policy.use_smt = g_cfg.use_smt;
    policy.prefer_pcores = g_cfg.prefer_performance_cores;
    policy.skip_core0 = g_cfg.skip_core0;
    policy.explicit_mask = g_cfg.cores_mask;
    policy.spare_fastest = g_cfg.spare_fast_cores != CPU_SPARE_AUTO ? g_cfg.spare_fast_cores
                           : topo.ranked_by_capacity || running_under_fex() ? CPU_SPARE_WHEN_RANKED
                                                                    : 0;
    if (g_cfg.cores_list_invalid) {
        LOGE("cpu: the Cores= list could not be parsed; using automatic selection");
    }
    cpu_select(&topo, process_mask, &policy, &sel);
    if (sel.count == 0) {
        shim_configure(SHIM_NONE, NULL, NULL, &g_cfg);
        LOGE("cpu: no usable logical processors found; CPU handling disabled");
        return;
    }

    list[0] = '\0';
    for (i = 0; i < sel.count && pos + 5 < sizeof(list); ++i) {
        int n = snprintf(list + pos, sizeof(list) - pos, i ? ",%u" : "%u", (unsigned)sel.real[i]);
        if (n > 0) {
            pos += (size_t)n;
        }
    }
    LOGI("cpu: Mode=%s - engine sees %u logical processor%s on %u core%s (real processors %s, mask 0x%08lX)",
         config_mode_name(g_cfg.mode), sel.count, sel.count == 1 ? "" : "s", sel.cores, sel.cores == 1 ? "" : "s",
         list, sel.real_mask);
    if (sel.spared) {
        LOGI("cpu: the %u fastest core%s (mask 0x%08lX) get%s no worker of the game pinned to them: its main "
             "and render threads, which it does not pin, can have them ([CPU] SpareFastCores)",
             sel.spared, sel.spared == 1 ? "" : "s", sel.spared_mask, sel.spared == 1 ? "s" : "");
    }
    {
        char status[96];
        snprintf(status, sizeof(status), "%u of %u cores, %u threads", sel.cores, topo.core_count, sel.count);
        menu_set_status(MENU_STATUS_PROCESSOR, status);
    }

    shim_configure(SHIM_VIRTUALIZE, &topo, &sel, &g_cfg);

    if (g_cfg.mode == CPU_MODE_PROCESS) {
        unsigned long whole = sel.real_mask | sel.spared_mask;
        if (SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)whole)) {
            LOGI("cpu: process affinity restricted to 0x%08lX", whole);
        } else {
            LOGE("cpu: SetProcessAffinityMask(0x%08lX) failed (error %lu)", whole,
                 (unsigned long)GetLastError());
        }
    }
}

static void fc3pp_init(HINSTANCE self)
{
    wchar_t dir[MAX_PATH];
    wchar_t ini_path[MAX_PATH + 40];
    wchar_t log_path[MAX_PATH + 40];
    wchar_t host[MAX_PATH];
    wchar_t *slash;
    char *text;
    size_t text_len = 0;
    int have_ini;
    int found_engine = 0;
    size_t i;

    InitializeCriticalSection(&g_attach_lock);
    if (!GetModuleFileNameW(self, dir, MAX_PATH)) {
        return;
    }
    dir[MAX_PATH - 1] = L'\0';
    slash = wcsrchr(dir, L'\\');
    if (!slash) {
        return;
    }
    *slash = L'\0';
    _snwprintf(ini_path, sizeof(ini_path) / sizeof(ini_path[0]), L"%ls\\" FC3PP_INI_NAME, dir);
    _snwprintf(log_path, sizeof(log_path) / sizeof(log_path[0]), L"%ls\\" FC3PP_LOG_NAME, dir);
    ini_path[sizeof(ini_path) / sizeof(ini_path[0]) - 1] = L'\0';
    log_path[sizeof(log_path) / sizeof(log_path[0]) - 1] = L'\0';

    text = read_text_file(ini_path, &text_len);
    have_ini = text != NULL;
    if (config_load_text(&g_cfg, text, text_len) != 0) {
        config_defaults(&g_cfg);
    }
    free(text);
    if (!g_cfg.enabled) {
        return; /* pure pass-through proxy */
    }

    if (!GetModuleFileNameW(NULL, host, MAX_PATH)) {
        host[0] = L'\0';
    }
    host[MAX_PATH - 1] = L'\0';

    /* Only act inside the game. Other executables in the same folder (the
     * updater, the editor) also load version.dll and must be left alone. */
    for (i = 0; i < sizeof(ENGINE_MODULES) / sizeof(ENGINE_MODULES[0]); ++i) {
        if (GetModuleHandleA(ENGINE_MODULES[i])) {
            found_engine = 1;
        }
    }
    if (!found_engine) {
        return; /* passive: forward exports only, write nothing */
    }

    log_init(log_path, g_cfg.log_level);
    log_environment(log_basename_w(host));
    if (!have_ini) {
        LOGI("config: " FC3PP_NAME ".ini not found next to version.dll; using built-in defaults");
    } else if (g_cfg.malformed_lines) {
        LOGI("config: %u line%s in the INI file could not be understood and were ignored",
             (unsigned)g_cfg.malformed_lines, g_cfg.malformed_lines == 1 ? "" : "s");
    }

    g_active = 1;
    if (g_cfg.draw_distance_invalid) {
        LOGE("config: the [Graphics] DrawDistance= value is not a number from -2 to 2; the original distances are used");
    }
    if (g_cfg.menu_key_invalid) {
        LOGE("config: the [Menu] Key= value is not a key name this patch knows (F1 to F12, Home, End, Insert, "
             "Delete, Pause, ScrollLock, PageUp, PageDown, Tab); using F8");
    }
    if (g_cfg.menu_pad_invalid) {
        LOGE("config: the [Menu] PadButtons= value is not a combination of two or more gamepad buttons this "
             "patch knows (A B X Y LB RB L3 R3 Back Start Up Down Left Right, joined with +, or None); "
             "using L3+R3");
    }
    menu_init(&g_cfg, ini_path);
    threads_start(g_cfg.thread_report, (unsigned)g_cfg.thread_report_seconds);
    apply_laa(host);
    apply_timer_resolution();
    /* user32 is a static import of this DLL, hence already initialised. */
    display_apply_process_settings(&g_cfg);
    setup_cpu();

    for (i = 0; i < sizeof(ENGINE_MODULES) / sizeof(ENGINE_MODULES[0]); ++i) {
        HMODULE mod = GetModuleHandleA(ENGINE_MODULES[i]);
        if (mod) {
            attach_module(mod, ENGINE_MODULES[i], 1);
        }
    }
    /* A late module may already be present if something loaded it early. */
    for (i = 0; i < sizeof(LATE_MODULES) / sizeof(LATE_MODULES[0]); ++i) {
        HMODULE mod = GetModuleHandleA(LATE_MODULES[i]);
        if (mod) {
            attach_module(mod, LATE_MODULES[i], 0);
        }
    }
    LOGI("initialised");
}

static void fc3pp_shutdown(int process_terminating)
{
    if (!g_active) {
        return;
    }
    g_active = 0;
    if (process_terminating) {
        /* Other threads are already gone and may have died holding locks:
         * write the closing lines without taking any, and free nothing. */
        log_set_terminating();
        shim_log_stats();
        audio_log_stats();
        texguard_log_stats();
        texlink_log_stats();
        padswitch_log_stats();
        gfx_profile_log_stats();
        threads_log_stats();
        LOGI("process exit");
        return;
    }
    if (g_timer_period) {
        timeEndPeriod(g_timer_period);
        g_timer_period = 0;
    }
    shim_log_stats();
    audio_log_stats();
    texguard_log_stats();
    texlink_log_stats();
    padswitch_log_stats();
    gfx_profile_log_stats();
    threads_log_stats();
    LOGI("unloaded");
    log_shutdown();
    proxy_shutdown();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        fc3pp_init(instance);
    } else if (reason == DLL_PROCESS_DETACH) {
        fc3pp_shutdown(reserved != NULL);
    }
    return TRUE;
}
