/* Runtime configuration (FC3PerformancePatch.ini). Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_CONFIG_H
#define FC3PP_CONFIG_H

#include <stddef.h>

typedef enum cpu_mode {
    CPU_MODE_OFF = 0,     /* leave the engine's view of the CPU alone */
    CPU_MODE_ENGINE = 1,  /* engine threads use the selected cores; the rest of the process is free */
    CPU_MODE_PROCESS = 2  /* additionally restrict the whole process to the selected cores */
} cpu_mode;

typedef struct fc3pp_config {
    /* [General] */
    int enabled;
    int log_level;

    /* [CPU] */
    int fix_core_enumeration;
    cpu_mode mode;
    int max_cores;
    int use_smt;
    int prefer_performance_cores;
    int skip_core0;
    unsigned long cores_mask; /* explicit logical processor list, 0 = automatic */
    int cores_list_invalid;   /* the Cores= value could not be parsed */

    /* [Timing] */
    int timer_resolution_ms;  /* 0 = do not touch */
    int sleep_zero_yield;     /* experimental */

    /* [Memory] */
    int apply_large_address_aware;

    /* [Display] */
    int restore_fullscreen;      /* undo the fullscreen loss caused by Alt+Tab (DX11) */
    int high_dpi_aware;          /* mark the process DPI aware */
    int disable_window_ghosting; /* no "Not responding" ghost window while loading */

    /* [Audio] */
    int keep_xaudio2_loaded; /* pin XAudio2_7.dll (documented premature-unload defect) */

    /* [Debug] */
    int trace_cpu_api;
    int trace_audio;
    unsigned long simulate_affinity_mask;

    size_t malformed_lines;
} fc3pp_config;

void config_defaults(fc3pp_config *cfg);

/* Parses INI text on top of the defaults. text may be NULL (defaults only).
 * Returns 0 on success, -1 if the text could not be processed at all. */
int config_load_text(fc3pp_config *cfg, const char *text, size_t len);

const char *config_mode_name(cpu_mode mode);

#endif /* FC3PP_CONFIG_H */
