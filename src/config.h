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
    int spare_fast_cores;     /* cores of the fastest kind to keep free of pinned workers; -1 = automatic */
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

    /* [Textures] */
    int texture_guard; /* leave out top mip levels that do not fit their texture (DX11) */
    int texture_scan;  /* ... and those made for a texture a mod has replaced */
    int texture_shared; /* do not load top mip level files that several textures name */

    /* [Menu] */
    int menu_enabled;       /* the in-game settings menu (DX11) */
    unsigned menu_key;      /* virtual-key code that opens it */
    int menu_key_invalid;   /* the Key= value was not understood */
    unsigned menu_pad;      /* gamepad buttons that open it when held together; 0 = none */
    int menu_pad_invalid;   /* the PadButtons= value was not understood */
    int menu_scale;         /* size of the menu in percent, 100 to 200 */

    /* [Controls] */
    int auto_switch_input; /* gamepad or keyboard/mouse mode follows what is used */

    /* [Graphics] */
    int draw_distance;         /* a gp_level (in the INI file: -2 very near ... 2 very far) */
    int draw_distance_invalid; /* the DrawDistance= value was not understood */

    /* [Audio] */
    int keep_xaudio2_loaded; /* pin XAudio2_7.dll (documented premature-unload defect) */

    /* [Debug] */
    int trace_cpu_api;
    int trace_audio;
    int menu_open_at_start;  /* testing aid */
    int input_without_focus; /* testing aid */
    unsigned long simulate_affinity_mask;
    char host_sys_root[200]; /* testing aid: a folder to read the host's processor files from */
    int thread_report;          /* which threads use the processor: written to the log at exit */
    int thread_report_seconds;  /* testing aid: time between two looks at the threads; 0 = 30 */

    size_t malformed_lines;
} fc3pp_config;

void config_defaults(fc3pp_config *cfg);

/* Parses INI text on top of the defaults. text may be NULL (defaults only).
 * Returns 0 on success, -1 if the text could not be processed at all. */
int config_load_text(fc3pp_config *cfg, const char *text, size_t len);

const char *config_mode_name(cpu_mode mode);

#endif /* FC3PP_CONFIG_H */
