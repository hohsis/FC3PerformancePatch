/* Engine-facing CPU layer.
 *
 * Import-table hooks installed only in the game's own modules. They give the
 * engine a small, consistent set of logical processors (one per physical core
 * by default) and translate every affinity request onto the real processors
 * that were selected. Other modules in the process (graphics driver, DXVK,
 * audio, ...) are not affected.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_CPU_SHIM_H
#define FC3PP_CPU_SHIM_H

#include <windows.h>

#include "config.h"
#include "cpu_topology.h"

typedef enum shim_kind {
    SHIM_NONE = 0,
    SHIM_VIRTUALIZE, /* full topology translation */
    SHIM_CLAMP,      /* only keep reported affinity masks below bit 31 */
    SHIM_SIMULATE    /* debug: report a fixed affinity mask (reproduces the hang) */
} shim_kind;

/* Callback invoked (outside the loader lock) after a hooked LoadLibrary call
 * returned a module. */
typedef void (*shim_module_loaded_fn)(HMODULE mod);

/* Testing aid: read the host's processor files ("/sys/devices/system/cpu/...")
 * from under this folder, on any system. Call before shim_query_topology. */
void shim_set_host_root(const char *folder);

/* Queries the real topology. Returns 0 on success. */
int shim_query_topology(cpu_topology *topo, unsigned long *process_mask, unsigned long *system_mask);

void shim_configure(shim_kind kind, const cpu_topology *topo, const cpu_selection *sel, const fc3pp_config *cfg);
shim_kind shim_current_kind(void);

/* Installs the CPU hooks appropriate for the configured kind into mod.
 * Returns the number of imports hooked. */
unsigned shim_install_cpu_hooks(HMODULE mod);

/* Hooks LoadLibrary{A,W,ExA,ExW} in mod so late-loaded game modules can be
 * patched. Returns the number of imports hooked. */
unsigned shim_install_loader_hooks(HMODULE mod, shim_module_loaded_fn callback);

/* Hooks Sleep in mod (statistics and the optional Sleep(0) yield mode). */
unsigned shim_install_sleep_hook(HMODULE mod);

/* Writes Sleep statistics to the log (debug level). */
void shim_log_stats(void);

#endif /* FC3PP_CPU_SHIM_H */
