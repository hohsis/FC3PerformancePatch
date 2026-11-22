/* CPU topology model and core-selection policy tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cpu_topology.h"
#include "test.h"

/* n cores with two SMT threads each: logical 2k and 2k+1 share core k. */
static void make_smt(cpu_topology *t, unsigned cores)
{
    unsigned i;
    cpu_topology_init(t);
    for (i = 0; i < cores; ++i) {
        cpu_topology_add_core(t, 3ul << (2 * i), 0);
    }
}

/* Intel hybrid: p P-cores with SMT (efficiency class 1) followed by e E-cores. */
static void make_hybrid(cpu_topology *t, unsigned p, unsigned e)
{
    unsigned i;
    cpu_topology_init(t);
    for (i = 0; i < p; ++i) {
        cpu_topology_add_core(t, 3ul << (2 * i), 1);
    }
    for (i = 0; i < e; ++i) {
        cpu_topology_add_core(t, 1ul << (2 * p + i), 0);
    }
}

static cpu_policy policy(int max_cores, int use_smt, int prefer_p, int skip0)
{
    cpu_policy p;
    memset(&p, 0, sizeof(p));
    p.max_cores = max_cores;
    p.use_smt = use_smt;
    p.prefer_pcores = prefer_p;
    p.skip_core0 = skip0;
    return p;
}

static void test_default_policy_8c16t(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(6, 0, 1, 0);

    make_smt(&t, 8);
    CHECK_EQ_U(t.core_count, 8);
    cpu_select(&t, 0xFFFF, &p, &s);
    CHECK_EQ_U(s.count, 6);
    CHECK_EQ_U(s.cores, 6);
    CHECK_EQ_U(s.real_mask, 0x555); /* processors 0,2,4,6,8,10: no SMT siblings */
    CHECK_EQ_U(s.real[0], 0);
    CHECK_EQ_U(s.real[5], 10);
    CHECK_EQ_U(cpu_virtual_full_mask(&s), 0x3F);
}

static void test_smt_pairs(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(4, 1, 1, 0);

    make_smt(&t, 8);
    cpu_select(&t, 0xFFFF, &p, &s);
    CHECK_EQ_U(s.count, 8);
    CHECK_EQ_U(s.cores, 4);
    CHECK_EQ_U(s.real_mask, 0xFF);
    /* siblings are adjacent, which is what the engine assumes */
    CHECK_EQ_U(s.real[0], 0);
    CHECK_EQ_U(s.real[1], 1);
    CHECK_EQ_U(s.real[6], 6);
    CHECK_EQ_U(s.real[7], 7);
}

static void test_unlimited_and_small_machines(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(0, 0, 1, 0);

    make_smt(&t, 4);
    cpu_select(&t, 0xFF, &p, &s);
    CHECK_EQ_U(s.count, 4);
    CHECK_EQ_U(s.real_mask, 0x55);

    p.max_cores = 6; /* more than the machine has */
    cpu_select(&t, 0xFF, &p, &s);
    CHECK_EQ_U(s.count, 4);

    /* dual core without SMT */
    cpu_topology_flat(&t, 0x3);
    cpu_select(&t, 0x3, &p, &s);
    CHECK_EQ_U(s.count, 2);
    CHECK_EQ_U(s.real_mask, 0x3);

    /* single processor */
    cpu_topology_flat(&t, 0x1);
    cpu_select(&t, 0x1, &p, &s);
    CHECK_EQ_U(s.count, 1);
}

static void test_hybrid_prefers_performance_cores(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(6, 0, 1, 0);

    make_hybrid(&t, 8, 8); /* 8P+8E = 24 threads: P on 0..15, E on 16..23 */
    cpu_select(&t, 0xFFFFFF, &p, &s);
    CHECK_EQ_U(s.count, 6);
    CHECK_EQ_U(s.real_mask, 0x555); /* six P-cores, first thread of each */

    /* ask for more cores than there are P-cores: E-cores fill up the rest */
    p.max_cores = 10;
    cpu_select(&t, 0xFFFFFF, &p, &s);
    CHECK_EQ_U(s.count, 10);
    CHECK_EQ_U(s.real_mask, 0x5555ul | (3ul << 16));

    /* without the preference, cores are taken in processor order */
    p = policy(6, 0, 0, 0);
    make_hybrid(&t, 2, 8);
    cpu_select(&t, 0xFFF, &p, &s);
    CHECK_EQ_U(s.real_mask, 0x05ul | (0xFul << 4));

    /* E-cores listed first (as some firmware does): P-cores still win */
    cpu_topology_init(&t);
    cpu_topology_add_core(&t, 1ul << 0, 0);
    cpu_topology_add_core(&t, 1ul << 1, 0);
    cpu_topology_add_core(&t, 3ul << 2, 1);
    cpu_topology_add_core(&t, 3ul << 4, 1);
    p = policy(2, 0, 1, 0);
    cpu_select(&t, 0x3F, &p, &s);
    CHECK_EQ_U(s.real_mask, (1ul << 2) | (1ul << 4));
}

static void test_skip_core0(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(6, 0, 1, 1);

    make_smt(&t, 8);
    cpu_select(&t, 0xFFFF, &p, &s);
    CHECK_EQ_U(s.count, 6);
    CHECK_EQ_U(s.real_mask, 0x555ul << 2); /* cores 1..6 */

    /* not enough other cores: core 0 is used after all */
    make_smt(&t, 4);
    cpu_select(&t, 0xFF, &p, &s);
    CHECK_EQ_U(s.count, 4);
    CHECK_EQ_U(s.real_mask, 0x55);
}

static void test_respects_process_affinity(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(6, 0, 1, 0);

    make_smt(&t, 8);
    /* the user restricted the process to processors 4..11 beforehand */
    cpu_select(&t, 0x0FF0, &p, &s);
    CHECK_EQ_U(s.count, 4);
    CHECK_EQ_U(s.real_mask, 0x0550);

    /* only the second thread of a core is allowed: it is still usable */
    cpu_select(&t, 0x0002, &p, &s);
    CHECK_EQ_U(s.count, 1);
    CHECK_EQ_U(s.real[0], 1);

    /* nothing allowed */
    cpu_select(&t, 0, &p, &s);
    CHECK_EQ_U(s.count, 0);
}

static void test_many_processors(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(0, 1, 1, 0);

    /* 32 visible threads (e.g. a 64-thread CPU seen from a 32-bit process) */
    make_smt(&t, 16);
    cpu_select(&t, 0xFFFFFFFFul, &p, &s);
    CHECK(s.count <= FC3PP_MAX_ENGINE_CPUS);
    CHECK_EQ_U(s.count, 30);
    /* the engine-visible mask must never reach bit 31 */
    CHECK((cpu_virtual_full_mask(&s) & 0x80000000ul) == 0);
    CHECK_EQ_U(cpu_virtual_full_mask(&s), 0x3FFFFFFFul);

    /* 32 cores without SMT */
    cpu_topology_flat(&t, 0xFFFFFFFFul);
    p = policy(0, 0, 1, 0);
    cpu_select(&t, 0xFFFFFFFFul, &p, &s);
    CHECK_EQ_U(s.count, 30);
    CHECK((cpu_virtual_full_mask(&s) & 0x80000000ul) == 0);
}

static void test_explicit_list(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(2, 0, 1, 1);

    make_smt(&t, 8);
    p.explicit_mask = (1ul << 3) | (1ul << 8) | (1ul << 9) | (1ul << 20); /* 20 does not exist */
    cpu_select(&t, 0xFFFF, &p, &s);
    CHECK_EQ_U(s.count, 3);
    CHECK_EQ_U(s.real[0], 3);
    CHECK_EQ_U(s.real[1], 8);
    CHECK_EQ_U(s.real[2], 9);
    CHECK_EQ_U(s.cores, 2);

    /* a list with nothing usable falls back to the automatic policy */
    p.explicit_mask = 1ul << 30;
    cpu_select(&t, 0xFFFF, &p, &s);
    CHECK_EQ_U(s.count, 2);
}

static void test_mask_translation(void)
{
    cpu_topology t;
    cpu_selection s;
    cpu_policy p = policy(4, 0, 1, 0);
    unsigned long v;

    make_smt(&t, 8);
    cpu_select(&t, 0xFFFF, &p, &s); /* virtual 0..3 -> real 0,2,4,6 */
    CHECK_EQ_U(cpu_v2r_mask(&s, 0x1), 0x01);
    CHECK_EQ_U(cpu_v2r_mask(&s, 0x2), 0x04);
    CHECK_EQ_U(cpu_v2r_mask(&s, 0x8), 0x40);
    CHECK_EQ_U(cpu_v2r_mask(&s, 0xF), 0x55);
    CHECK_EQ_U(cpu_v2r_mask(&s, 0x10), 0);          /* beyond the virtual processors */
    CHECK_EQ_U(cpu_v2r_mask(&s, 0xFFFFFFFFul), 0x55);
    CHECK_EQ_U(cpu_r2v_mask(&s, 0x55), 0xF);
    CHECK_EQ_U(cpu_r2v_mask(&s, 0x04), 0x2);
    CHECK_EQ_U(cpu_r2v_mask(&s, 0xAA), 0);          /* only unselected siblings */
    for (v = 0; v < 16; ++v) {
        CHECK_EQ_U(cpu_r2v_mask(&s, cpu_v2r_mask(&s, v)), v); /* round trip */
    }
}

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Appends one RelationProcessorCore record in the 32-bit Ex layout. */
static size_t ex_core(unsigned char *buf, size_t off, unsigned long mask, unsigned group, unsigned eff)
{
    memset(buf + off, 0, 44);
    put32(buf + off, 0);       /* Relationship = RelationProcessorCore */
    put32(buf + off + 4, 44);  /* Size */
    buf[off + 8] = 1;          /* Flags: LTP_PC_SMT */
    buf[off + 9] = (unsigned char)eff;
    put16(buf + off + 30, 1);  /* GroupCount */
    put32(buf + off + 32, mask);
    put16(buf + off + 36, group);
    return off + 44;
}

static void test_parse_glpi_ex(void)
{
    unsigned char buf[512];
    size_t len = 0;
    cpu_topology t;

    len = ex_core(buf, len, 0x3, 0, 1);
    len = ex_core(buf, len, 0xC, 0, 1);
    len = ex_core(buf, len, 0x10, 0, 0);
    len = ex_core(buf, len, 0x3, 1, 0); /* another processor group: ignored */
    /* a record of a different relationship is skipped by its size */
    memset(buf + len, 0, 24);
    put32(buf + len, 3);
    put32(buf + len + 4, 24);
    len += 24;

    cpu_topology_init(&t);
    CHECK_EQ_U(cpu_topology_from_glpi_ex(&t, buf, len, 0), 3);
    CHECK(t.source == CPU_SRC_GLPI_EX);
    CHECK(t.cpu[0].present && t.cpu[1].present && t.cpu[4].present && !t.cpu[5].present);
    CHECK(t.cpu[0].core == t.cpu[1].core);
    CHECK(t.cpu[2].core == t.cpu[3].core);
    CHECK(t.cpu[0].core != t.cpu[2].core);
    CHECK_EQ_U(t.cpu[0].eff_class, 1);
    CHECK_EQ_U(t.cpu[4].eff_class, 0);

    /* group 1 view */
    cpu_topology_init(&t);
    CHECK_EQ_U(cpu_topology_from_glpi_ex(&t, buf, len, 1), 1);

    /* corrupt sizes must not run off the buffer */
    cpu_topology_init(&t);
    put32(buf + 4, 0xFFFFFFF0ul);
    CHECK_EQ_U(cpu_topology_from_glpi_ex(&t, buf, len, 0), 0);
    put32(buf + 4, 0);
    CHECK_EQ_U(cpu_topology_from_glpi_ex(&t, buf, len, 0), 0);
    CHECK_EQ_U(cpu_topology_from_glpi_ex(&t, buf, 7, 0), 0);
    CHECK_EQ_U(cpu_topology_from_glpi_ex(&t, NULL, 0, 0), 0);
}

static void test_parse_glpi(void)
{
    unsigned char buf[24 * 4];
    cpu_topology t;

    memset(buf, 0, sizeof(buf));
    put32(buf + 0, 0x3);
    put32(buf + 4, 0); /* core */
    put32(buf + 24, 0xF);
    put32(buf + 28, 2); /* cache: ignored */
    put32(buf + 48, 0xC);
    put32(buf + 52, 0); /* core */
    put32(buf + 72, 0xF);
    put32(buf + 76, 3); /* package: ignored */
    cpu_topology_init(&t);
    CHECK_EQ_U(cpu_topology_from_glpi(&t, buf, sizeof(buf)), 2);
    CHECK(t.source == CPU_SRC_GLPI);
    CHECK(t.cpu[2].core == t.cpu[3].core && t.cpu[0].core != t.cpu[2].core);
    CHECK_EQ_U(cpu_topology_from_glpi(&t, buf, 10), 0);
}

static void test_parse_list(void)
{
    unsigned long m = 99;
    CHECK(cpu_parse_list("0,2,4,6", &m) == 0);
    CHECK_EQ_U(m, 0x55);
    CHECK(cpu_parse_list(" 2 - 5 ; 31 ", &m) == 0);
    CHECK_EQ_U(m, 0x8000003Cul);
    CHECK(cpu_parse_list("", &m) == 0);
    CHECK_EQ_U(m, 0);
    CHECK(cpu_parse_list(NULL, &m) == 0);
    CHECK(cpu_parse_list("32", &m) == -1);  /* out of range */
    CHECK(cpu_parse_list("5-2", &m) == -1); /* reversed */
    CHECK(cpu_parse_list("1,a", &m) == -1);
    CHECK(cpu_parse_list("3-", &m) == -1);
    CHECK(cpu_parse_list("99999999999", &m) == -1);
    CHECK_EQ_U(cpu_popcount(0xFFFFFFFFul), 32);
    CHECK_EQ_U(cpu_popcount(0), 0);
}

void suite_cpu(void)
{
    RUN(test_default_policy_8c16t);
    RUN(test_smt_pairs);
    RUN(test_unlimited_and_small_machines);
    RUN(test_hybrid_prefers_performance_cores);
    RUN(test_skip_core0);
    RUN(test_respects_process_affinity);
    RUN(test_many_processors);
    RUN(test_explicit_list);
    RUN(test_mask_translation);
    RUN(test_parse_glpi_ex);
    RUN(test_parse_glpi);
    RUN(test_parse_list);
}
