/* Runtime configuration (FC3PerformancePatch.ini). Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.h"

#include <string.h>

#include "cpu_topology.h"
#include "fc3pp.h"
#include "gfx_preset.h"
#include "ini.h"
#include "menu_model.h"

void config_defaults(fc3pp_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled = 1;
    cfg->log_level = 2;
    cfg->fix_core_enumeration = 1;
    cfg->mode = CPU_MODE_ENGINE;
    cfg->max_cores = 6;
    cfg->use_smt = 0;
    cfg->prefer_performance_cores = 1;
    cfg->skip_core0 = 0;
    cfg->spare_fast_cores = CPU_SPARE_AUTO;
    cfg->thread_report = 1;
    cfg->timer_resolution_ms = 1;
    cfg->sleep_zero_yield = 0;
    cfg->apply_large_address_aware = 1;
    cfg->restore_fullscreen = 1;
    cfg->high_dpi_aware = 1;
    cfg->disable_window_ghosting = 1;
    cfg->keep_xaudio2_loaded = 1;
    cfg->texture_guard = 1;
    cfg->texture_scan = 1;
    cfg->texture_shared = 1;
    cfg->menu_enabled = 1;
    cfg->menu_key = 0x77; /* F8 */
    cfg->menu_pad = MM_PAD_L3 | MM_PAD_R3;
    cfg->menu_scale = 100;
}

const char *config_mode_name(cpu_mode mode)
{
    switch (mode) {
    case CPU_MODE_OFF:
        return "off";
    case CPU_MODE_ENGINE:
        return "engine";
    case CPU_MODE_PROCESS:
        return "process";
    default:
        return "?";
    }
}

/* -2 to 2 or the names shown in the menu. Returns a gp_level, or -1 when not understood. */
static int parse_distance(const char *s)
{
    if (ini_streq_nocase(s, "0") || ini_streq_nocase(s, "original") || ini_streq_nocase(s, "off")) {
        return GP_ORIGINAL;
    }
    if (ini_streq_nocase(s, "1") || ini_streq_nocase(s, "far")) {
        return GP_FAR;
    }
    if (ini_streq_nocase(s, "2") || ini_streq_nocase(s, "veryfar") || ini_streq_nocase(s, "very far")) {
        return GP_VERY_FAR;
    }
    if (ini_streq_nocase(s, "-1") || ini_streq_nocase(s, "near")) {
        return GP_NEAR;
    }
    if (ini_streq_nocase(s, "-2") || ini_streq_nocase(s, "verynear") || ini_streq_nocase(s, "very near")) {
        return GP_VERY_NEAR;
    }
    return -1;
}

static cpu_mode parse_mode(const char *s, cpu_mode def)
{
    if (!s || !*s) {
        return def;
    }
    if (ini_streq_nocase(s, "off") || ini_streq_nocase(s, "0") || ini_streq_nocase(s, "none") ||
        ini_streq_nocase(s, "false")) {
        return CPU_MODE_OFF;
    }
    if (ini_streq_nocase(s, "engine") || ini_streq_nocase(s, "1")) {
        return CPU_MODE_ENGINE;
    }
    if (ini_streq_nocase(s, "process") || ini_streq_nocase(s, "2")) {
        return CPU_MODE_PROCESS;
    }
    return def;
}

int config_load_text(fc3pp_config *cfg, const char *text, size_t len)
{
    ini_doc doc;
    const char *cores;

    config_defaults(cfg);
    if (!text || !len) {
        return 0;
    }
    if (ini_parse(&doc, text, len) != 0) {
        return -1;
    }
    cfg->malformed_lines = doc.malformed;

    cfg->enabled = ini_get_bool(&doc, "General", "Enabled", cfg->enabled);
    cfg->log_level = (int)ini_get_int_clamped(&doc, "General", "LogLevel", cfg->log_level, 0, 3);

    cfg->fix_core_enumeration = ini_get_bool(&doc, "CPU", "FixCoreEnumeration", cfg->fix_core_enumeration);
    cfg->mode = parse_mode(ini_get(&doc, "CPU", "Mode"), cfg->mode);
    cfg->max_cores = (int)ini_get_int_clamped(&doc, "CPU", "MaxCores", cfg->max_cores, 0, FC3PP_MAX_CPUS);
    cfg->use_smt = ini_get_bool(&doc, "CPU", "UseSMT", cfg->use_smt);
    cfg->prefer_performance_cores =
        ini_get_bool(&doc, "CPU", "PreferPerformanceCores", cfg->prefer_performance_cores);
    cfg->skip_core0 = ini_get_bool(&doc, "CPU", "SkipCore0", cfg->skip_core0);
    {
        const char *spare = ini_get(&doc, "CPU", "SpareFastCores");
        if (spare && *spare) {
            cfg->spare_fast_cores =
                ini_streq_nocase(spare, "auto")
                    ? CPU_SPARE_AUTO
                    : (int)ini_get_int_clamped(&doc, "CPU", "SpareFastCores", 0, 0, CPU_SPARE_MAX);
        }
    }
    cores = ini_get(&doc, "CPU", "Cores");
    if (cores && *cores) {
        unsigned long mask = 0;
        if (cpu_parse_list(cores, &mask) == 0) {
            cfg->cores_mask = mask;
        } else {
            cfg->cores_list_invalid = 1;
        }
    }

    cfg->timer_resolution_ms =
        (int)ini_get_int_clamped(&doc, "Timing", "TimerResolutionMs", cfg->timer_resolution_ms, 0, 16);
    cfg->sleep_zero_yield = ini_get_bool(&doc, "Timing", "SleepZeroYield", cfg->sleep_zero_yield);

    cfg->apply_large_address_aware =
        ini_get_bool(&doc, "Memory", "ApplyLargeAddressAware", cfg->apply_large_address_aware);

    cfg->restore_fullscreen = ini_get_bool(&doc, "Display", "RestoreFullscreenOnFocus", cfg->restore_fullscreen);
    cfg->high_dpi_aware = ini_get_bool(&doc, "Display", "HighDpiAware", cfg->high_dpi_aware);
    cfg->disable_window_ghosting =
        ini_get_bool(&doc, "Display", "DisableWindowGhosting", cfg->disable_window_ghosting);

    cfg->texture_guard = ini_get_bool(&doc, "Textures", "SkipMismatchedMip0", cfg->texture_guard);
    cfg->texture_scan = ini_get_bool(&doc, "Textures", "SkipReplacedTextureMip0", cfg->texture_scan);
    cfg->texture_shared = ini_get_bool(&doc, "Textures", "SkipSharedMip0", cfg->texture_shared);

    cfg->menu_enabled = ini_get_bool(&doc, "Menu", "Enabled", cfg->menu_enabled);
    {
        const char *key = ini_get(&doc, "Menu", "Key");
        if (key && *key) {
            unsigned vk = mm_key_from_name(key);
            if (vk) {
                cfg->menu_key = vk;
            } else {
                cfg->menu_key_invalid = 1;
            }
        }
    }

    cfg->auto_switch_input = ini_get_bool(&doc, "Controls", "AutoSwitchInput", cfg->auto_switch_input);
    {
        const char *distance = ini_get(&doc, "Graphics", "DrawDistance");
        if (distance && *distance) {
            int level = parse_distance(distance);
            if (level >= 0) {
                cfg->draw_distance = level;
            } else {
                cfg->draw_distance_invalid = 1;
            }
        }
    }

    {
        const char *pad = ini_get(&doc, "Menu", "PadButtons");
        if (pad && *pad) {
            unsigned mask = 0;
            if (mm_pad_combo_from_name(pad, &mask)) {
                cfg->menu_pad = mask;
            } else {
                cfg->menu_pad_invalid = 1;
            }
        }
    }
    cfg->menu_scale = (int)ini_get_int_clamped(&doc, "Menu", "Scale", cfg->menu_scale, 100, 200);

    cfg->keep_xaudio2_loaded = ini_get_bool(&doc, "Audio", "KeepXAudio2Loaded", cfg->keep_xaudio2_loaded);

    cfg->trace_cpu_api = ini_get_bool(&doc, "Debug", "TraceCpuApi", cfg->trace_cpu_api);
    cfg->trace_audio = ini_get_bool(&doc, "Debug", "TraceAudio", cfg->trace_audio);
    cfg->menu_open_at_start = ini_get_bool(&doc, "Debug", "MenuOpenAtStart", cfg->menu_open_at_start);
    cfg->input_without_focus = ini_get_bool(&doc, "Debug", "InputWithoutFocus", cfg->input_without_focus);
    cfg->simulate_affinity_mask =
        ini_get_uint(&doc, "Debug", "SimulateAffinityMask", cfg->simulate_affinity_mask) & 0xFFFFFFFFul;
    cfg->thread_report = ini_get_bool(&doc, "Debug", "ThreadReport", cfg->thread_report);
    cfg->thread_report_seconds =
        (int)ini_get_int_clamped(&doc, "Debug", "ThreadReportSeconds", cfg->thread_report_seconds, 0, 600);
    {
        const char *root = ini_get(&doc, "Debug", "HostSysRoot");
        if (root && strlen(root) < sizeof(cfg->host_sys_root)) {
            strcpy(cfg->host_sys_root, root);
        }
    }

    ini_free(&doc);
    return 0;
}
