/* CPU topology model and core-selection policy.
 *
 * The selection logic is pure (no OS calls) so it can be unit tested against
 * synthetic topologies: SMT, hybrid P/E cores, >32 logical processors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_CPU_TOPOLOGY_H
#define FC3PP_CPU_TOPOLOGY_H

#include <stddef.h>

#include "fc3pp.h"

typedef enum cpu_topology_source {
    CPU_SRC_NONE = 0,
    CPU_SRC_GLPI_EX,   /* GetLogicalProcessorInformationEx */
    CPU_SRC_GLPI,      /* GetLogicalProcessorInformation */
    CPU_SRC_FLAT       /* GetSystemInfo only: every logical CPU is its own core */
} cpu_topology_source;

typedef struct cpu_logical {
    unsigned char present;   /* 1 if this logical processor exists */
    unsigned char core;      /* dense core id; SMT siblings share it */
    unsigned char eff_class; /* higher = faster core (Windows EfficiencyClass) */
} cpu_logical;

typedef struct cpu_topology {
    cpu_logical cpu[FC3PP_MAX_CPUS]; /* indexed by logical processor number */
    unsigned core_count;
    cpu_topology_source source;
} cpu_topology;

typedef struct cpu_policy {
    int max_cores;       /* 0 = no limit */
    int use_smt;         /* 0 = one logical processor per physical core */
    int prefer_pcores;   /* prefer the highest efficiency class */
    int skip_core0;      /* avoid the core that owns logical processor 0 when possible */
    unsigned long explicit_mask; /* non-zero: use exactly these logical processors */
} cpu_policy;

typedef struct cpu_selection {
    unsigned count;                      /* engine-visible logical processors */
    unsigned char real[FC3PP_MAX_CPUS];  /* virtual index -> real logical processor */
    unsigned long real_mask;             /* union of the selected real processors */
    unsigned cores;                      /* physical cores represented */
} cpu_selection;

void cpu_topology_init(cpu_topology *t);

/* Every allowed logical processor becomes its own core. */
void cpu_topology_flat(cpu_topology *t, unsigned long present_mask);

/* Adds one physical core given the mask of its logical processors. */
void cpu_topology_add_core(cpu_topology *t, unsigned long core_mask, unsigned eff_class);

/* Parses a SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buffer (32-bit layout,
 * RelationProcessorCore records) for the given processor group.
 * Returns the number of cores added. */
unsigned cpu_topology_from_glpi_ex(cpu_topology *t, const unsigned char *buf, size_t len, unsigned group);

/* Parses a SYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer (32-bit layout). */
unsigned cpu_topology_from_glpi(cpu_topology *t, const unsigned char *buf, size_t len);

/* Chooses the logical processors the engine should use. allowed_mask is the
 * real process affinity mask. The result never has more than
 * FC3PP_MAX_ENGINE_CPUS entries; out->count is 0 only if nothing is usable. */
void cpu_select(const cpu_topology *t, unsigned long allowed_mask, const cpu_policy *policy, cpu_selection *out);

/* Engine-visible (virtual) mask covering every selected processor. */
unsigned long cpu_virtual_full_mask(const cpu_selection *s);

/* Virtual -> real and real -> virtual affinity mask translation. */
unsigned long cpu_v2r_mask(const cpu_selection *s, unsigned long vmask);
unsigned long cpu_r2v_mask(const cpu_selection *s, unsigned long rmask);

/* Parses "0,2,4-7" into a mask of logical processor numbers (< 32).
 * Returns 0 on success, -1 on syntax error. An empty string yields mask 0. */
int cpu_parse_list(const char *text, unsigned long *mask);

unsigned cpu_popcount(unsigned long v);

#endif /* FC3PP_CPU_TOPOLOGY_H */
