/* CPU topology model and core-selection policy. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cpu_topology.h"

#include <string.h>

static unsigned rd16(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

unsigned cpu_popcount(unsigned long v)
{
    unsigned n = 0;
    v &= 0xFFFFFFFFul;
    while (v) {
        v &= v - 1;
        ++n;
    }
    return n;
}

void cpu_topology_init(cpu_topology *t)
{
    memset(t, 0, sizeof(*t));
}

void cpu_topology_add_core(cpu_topology *t, unsigned long core_mask, unsigned eff_class)
{
    unsigned i;
    int added = 0;
    if (t->core_count >= FC3PP_MAX_CPUS) {
        return;
    }
    for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
        if ((core_mask >> i) & 1ul) {
            if (!t->cpu[i].present) {
                t->cpu[i].present = 1;
                t->cpu[i].core = (unsigned char)t->core_count;
                t->cpu[i].eff_class = (unsigned char)(eff_class > 255 ? 255 : eff_class);
                added = 1;
            }
        }
    }
    if (added) {
        t->core_count++;
    }
}

void cpu_topology_flat(cpu_topology *t, unsigned long present_mask)
{
    unsigned i;
    cpu_topology_init(t);
    for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
        if ((present_mask >> i) & 1ul) {
            cpu_topology_add_core(t, 1ul << i, 0);
        }
    }
    t->source = CPU_SRC_FLAT;
}

unsigned cpu_topology_from_glpi_ex(cpu_topology *t, const unsigned char *buf, size_t len, unsigned group)
{
    size_t off = 0;
    unsigned before = t->core_count;

    while (buf && off + 8 <= len) {
        unsigned long relationship = rd32(buf + off);
        unsigned long size = rd32(buf + off + 4);
        if (size < 8 || size > len - off) {
            break;
        }
        if (relationship == 0 /* RelationProcessorCore */ && size >= 32) {
            unsigned eff = buf[off + 9];
            unsigned group_count = rd16(buf + off + 30);
            unsigned g;
            for (g = 0; g < group_count; ++g) {
                size_t gm = off + 32 + (size_t)g * 12;
                if (gm + 12 > off + size) {
                    break;
                }
                if (rd16(buf + gm + 4) == group) {
                    cpu_topology_add_core(t, rd32(buf + gm), eff);
                }
            }
        }
        off += size;
    }
    if (t->core_count > before) {
        t->source = CPU_SRC_GLPI_EX;
    }
    return t->core_count - before;
}

unsigned cpu_topology_from_glpi(cpu_topology *t, const unsigned char *buf, size_t len)
{
    size_t off;
    unsigned before = t->core_count;

    for (off = 0; buf && off + 24 <= len; off += 24) {
        if (rd32(buf + off + 4) == 0 /* RelationProcessorCore */) {
            cpu_topology_add_core(t, rd32(buf + off), 0);
        }
    }
    if (t->core_count > before) {
        t->source = CPU_SRC_GLPI;
    }
    return t->core_count - before;
}

unsigned cpu_topology_rank_by_capacity(cpu_topology *t, const unsigned long *capacity)
{
    unsigned long distinct[FC3PP_MAX_CPUS];
    unsigned n = 0, i, j;
    int first = -1;

    if (!capacity) {
        return 0;
    }
    for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
        if (!t->cpu[i].present) {
            continue;
        }
        if (first < 0) {
            first = (int)i;
        }
        if (t->cpu[i].eff_class != t->cpu[first].eff_class || capacity[i] == 0) {
            return 0; /* the system already told us, or we do not know enough */
        }
        for (j = 0; j < n && distinct[j] != capacity[i]; ++j) {
        }
        if (j == n) {
            distinct[n++] = capacity[i];
        }
    }
    if (n < 2) {
        return 0;
    }
    /* ascending: class 0 is the slowest, as with EfficiencyClass */
    for (i = 1; i < n; ++i) {
        unsigned long key = distinct[i];
        for (j = i; j > 0 && distinct[j - 1] > key; --j) {
            distinct[j] = distinct[j - 1];
        }
        distinct[j] = key;
    }
    for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
        if (t->cpu[i].present) {
            for (j = 0; distinct[j] != capacity[i]; ++j) {
            }
            t->cpu[i].eff_class = (unsigned char)j;
        }
    }
    return n;
}

typedef struct sel_core {
    unsigned char cpus[FC3PP_MAX_CPUS];
    unsigned char n;
    unsigned char eff;
    unsigned char min_cpu;
    unsigned char has0;
} sel_core;

/* Returns non-zero when a should be chosen before b. */
static int ranks_before(const sel_core *a, const sel_core *b, const cpu_policy *p)
{
    if (p->prefer_pcores && a->eff != b->eff) {
        return a->eff > b->eff;
    }
    if (p->skip_core0 && a->has0 != b->has0) {
        return a->has0 < b->has0;
    }
    return a->min_cpu < b->min_cpu;
}

static void emit(cpu_selection *out, unsigned cpu)
{
    if (out->count < FC3PP_MAX_ENGINE_CPUS) {
        out->real[out->count++] = (unsigned char)cpu;
        out->real_mask |= 1ul << cpu;
    }
}

void cpu_select(const cpu_topology *t, unsigned long allowed_mask, const cpu_policy *policy, cpu_selection *out)
{
    sel_core cores[FC3PP_MAX_CPUS];
    sel_core *order[FC3PP_MAX_CPUS];
    unsigned char seen_core[FC3PP_MAX_CPUS];
    unsigned ncores = 0;
    unsigned limit;
    unsigned per_core;
    unsigned i, j;

    memset(out, 0, sizeof(*out));
    memset(cores, 0, sizeof(cores));
    memset(seen_core, 0, sizeof(seen_core));
    allowed_mask &= 0xFFFFFFFFul;

    /* Explicit list: use exactly what was asked for, as far as it exists. */
    if (policy->explicit_mask) {
        unsigned long m = policy->explicit_mask & allowed_mask;
        for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
            if (((m >> i) & 1ul) && t->cpu[i].present) {
                emit(out, i);
                if (!seen_core[t->cpu[i].core]) {
                    seen_core[t->cpu[i].core] = 1;
                    out->cores++;
                }
            }
        }
        if (out->count) {
            return;
        }
        memset(out, 0, sizeof(*out));
        memset(seen_core, 0, sizeof(seen_core));
    }

    /* Group the allowed logical processors by physical core. */
    for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
        if (((allowed_mask >> i) & 1ul) && t->cpu[i].present) {
            sel_core *c = &cores[t->cpu[i].core];
            if (c->n == 0) {
                c->min_cpu = (unsigned char)i;
                c->eff = t->cpu[i].eff_class;
            }
            c->cpus[c->n++] = (unsigned char)i;
            if (i == 0) {
                c->has0 = 1;
            }
        }
    }
    for (i = 0; i < FC3PP_MAX_CPUS; ++i) {
        if (cores[i].n) {
            order[ncores++] = &cores[i];
        }
    }
    if (ncores == 0) {
        return;
    }

    /* Stable insertion sort by selection rank. */
    for (i = 1; i < ncores; ++i) {
        sel_core *key = order[i];
        j = i;
        while (j > 0 && ranks_before(key, order[j - 1], policy)) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }

    per_core = policy->use_smt ? 2u : 1u;
    limit = ncores;
    if (policy->max_cores > 0 && (unsigned)policy->max_cores < limit) {
        limit = (unsigned)policy->max_cores;
    }
    while (limit > 1 && limit * per_core > FC3PP_MAX_ENGINE_CPUS) {
        --limit;
    }

    /* Present the chosen cores in ascending processor order. */
    for (i = 1; i < limit; ++i) {
        sel_core *key = order[i];
        j = i;
        while (j > 0 && key->min_cpu < order[j - 1]->min_cpu) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = key;
    }
    for (i = 0; i < limit; ++i) {
        emit(out, order[i]->cpus[0]);
        if (policy->use_smt && order[i]->n >= 2) {
            emit(out, order[i]->cpus[1]);
        }
    }
    out->cores = limit;
}

unsigned long cpu_virtual_full_mask(const cpu_selection *s)
{
    if (s->count >= 32) {
        return 0xFFFFFFFFul;
    }
    return (1ul << s->count) - 1ul;
}

unsigned long cpu_v2r_mask(const cpu_selection *s, unsigned long vmask)
{
    unsigned long r = 0;
    unsigned i;
    for (i = 0; i < s->count; ++i) {
        if ((vmask >> i) & 1ul) {
            r |= 1ul << s->real[i];
        }
    }
    return r;
}

unsigned long cpu_r2v_mask(const cpu_selection *s, unsigned long rmask)
{
    unsigned long v = 0;
    unsigned i;
    for (i = 0; i < s->count; ++i) {
        if ((rmask >> s->real[i]) & 1ul) {
            v |= 1ul << i;
        }
    }
    return v;
}

static int parse_number(const char **pp, unsigned *out)
{
    const char *p = *pp;
    unsigned v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned)(*p - '0');
        if (v > 1000) {
            return -1;
        }
        ++p;
        ++digits;
    }
    if (!digits) {
        return -1;
    }
    *pp = p;
    *out = v;
    return 0;
}

int cpu_parse_list(const char *text, unsigned long *mask)
{
    unsigned long m = 0;
    const char *p = text;

    if (mask) {
        *mask = 0;
    }
    if (!text) {
        return 0;
    }
    for (;;) {
        unsigned a, b;
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == ';') {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (parse_number(&p, &a) != 0) {
            return -1;
        }
        b = a;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '-') {
            ++p;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }
            if (parse_number(&p, &b) != 0) {
                return -1;
            }
        }
        if (a > b || b >= FC3PP_MAX_CPUS) {
            return -1;
        }
        for (; a <= b; ++a) {
            m |= 1ul << a;
        }
    }
    if (mask) {
        *mask = m;
    }
    return 0;
}
