/* Runtime configuration (FC3PerformancePatch.ini). Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.h"

#include <string.h>

#include "cpu_topology.h"
#include "fc3pp.h"
#include "ini.h"

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
    cfg->timer_resolution_ms = 1;
    cfg->sleep_zero_yield = 0;
    cfg->apply_large_address_aware = 1;
    cfg->restore_fullscreen = 1;
    cfg->high_dpi_aware = 1;
    cfg->disable_window_ghosting = 1;
    cfg->keep_xaudio2_loaded = 1;
    cfg->texture_guard = 1;
    cfg->texture_scan = 1;
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

    cfg->keep_xaudio2_loaded = ini_get_bool(&doc, "Audio", "KeepXAudio2Loaded", cfg->keep_xaudio2_loaded);

    cfg->trace_cpu_api = ini_get_bool(&doc, "Debug", "TraceCpuApi", cfg->trace_cpu_api);
    cfg->trace_audio = ini_get_bool(&doc, "Debug", "TraceAudio", cfg->trace_audio);
    cfg->simulate_affinity_mask =
        ini_get_uint(&doc, "Debug", "SimulateAffinityMask", cfg->simulate_affinity_mask) & 0xFFFFFFFFul;

    ini_free(&doc);
    return 0;
}
