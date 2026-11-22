/* Signature scanner tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sigscan.h"
#include "test.h"

static void test_parse(void)
{
    sig_pattern p;
    CHECK(sig_parse(&p, "41 BE ?? 0f") == 0);
    CHECK_EQ_U(p.len, 4);
    CHECK_EQ_U(p.bytes[0], 0x41);
    CHECK_EQ_U(p.bytes[3], 0x0F);
    CHECK_EQ_U(p.mask[2], 0x00);
    CHECK_EQ_U(p.mask[1], 0xFF);
    CHECK(sig_parse(&p, "  AA\t?  BB  ") == 0);
    CHECK_EQ_U(p.len, 3);
    /* errors */
    CHECK(sig_parse(&p, "") == -1);
    CHECK(sig_parse(&p, "?? ??") == -1);  /* nothing concrete to anchor on */
    CHECK(sig_parse(&p, "4") == -1);      /* half a byte */
    CHECK(sig_parse(&p, "GG") == -1);
    CHECK(sig_parse(&p, "4141") == -1);   /* tokens must be separated */
    CHECK(sig_parse(&p, NULL) == -1);
    CHECK(sig_parse(NULL, "41") == -1);
}

static void test_parse_length_limit(void)
{
    char text[SIG_MAX_LEN * 3 + 8];
    sig_pattern p;
    size_t i, pos = 0;
    for (i = 0; i < SIG_MAX_LEN; ++i) {
        pos += (size_t)sprintf(text + pos, "90 ");
    }
    CHECK(sig_parse(&p, text) == 0);
    CHECK_EQ_U(p.len, SIG_MAX_LEN);
    sprintf(text + pos, "90");
    CHECK(sig_parse(&p, text) == -1);
}

static void test_find(void)
{
    static const unsigned char buf[] = {0x00, 0x11, 0x41, 0xBE, 0x77, 0x0F, 0x41, 0xBE, 0x99, 0x0F, 0x41};
    sig_pattern p;
    const unsigned char *hits[4];

    CHECK(sig_parse(&p, "41 BE ?? 0F") == 0);
    CHECK(sig_find(buf, sizeof(buf), &p) == buf + 2);
    CHECK_EQ_U(sig_find_all(buf, sizeof(buf), &p, hits, 4), 2);
    CHECK(hits[0] == buf + 2);
    CHECK(hits[1] == buf + 6);
    /* counting without storing, and with a smaller output array */
    CHECK_EQ_U(sig_find_all(buf, sizeof(buf), &p, NULL, 0), 2);
    hits[1] = NULL;
    CHECK_EQ_U(sig_find_all(buf, sizeof(buf), &p, hits, 1), 2);
    CHECK(hits[1] == NULL);
}

static void test_edges(void)
{
    static const unsigned char buf[] = {0xAA, 0xBB, 0xCC, 0xDD};
    sig_pattern p;

    CHECK(sig_parse(&p, "AA BB") == 0);
    CHECK(sig_find(buf, sizeof(buf), &p) == buf); /* match at the very start */
    CHECK(sig_parse(&p, "CC DD") == 0);
    CHECK(sig_find(buf, sizeof(buf), &p) == buf + 2); /* match at the very end */
    CHECK(sig_parse(&p, "DD ??") == 0);
    CHECK(sig_find(buf, sizeof(buf), &p) == NULL); /* would read past the end */
    CHECK(sig_parse(&p, "?? BB ?? DD") == 0);
    CHECK(sig_find(buf, sizeof(buf), &p) == buf); /* leading wildcard */
    CHECK(sig_parse(&p, "?? AA") == 0);
    CHECK(sig_find(buf, sizeof(buf), &p) == NULL); /* nothing before the first byte */
    CHECK(sig_parse(&p, "AA BB CC DD EE") == 0);
    CHECK(sig_find(buf, sizeof(buf), &p) == NULL); /* pattern longer than buffer */
    CHECK(sig_find(NULL, 0, &p) == NULL);
    CHECK(sig_find(buf, 0, &p) == NULL);
}

static void test_overlap_and_volume(void)
{
    unsigned char big[4096];
    sig_pattern p;
    size_t i;

    memset(big, 0x90, sizeof(big));
    CHECK(sig_parse(&p, "90 90 90") == 0);
    CHECK_EQ_U(sig_find_all(big, sizeof(big), &p, NULL, 0), sizeof(big) - 2); /* overlapping matches */

    for (i = 0; i < sizeof(big); ++i) {
        big[i] = (unsigned char)(i * 31u + 7u);
    }
    big[4000] = 0xDE;
    big[4001] = 0xAD;
    big[4002] = 0x00;
    big[4003] = 0xEF;
    CHECK(sig_parse(&p, "DE AD ?? EF") == 0);
    CHECK(sig_find(big, sizeof(big), &p) == big + 4000);
}

void suite_sigscan(void)
{
    RUN(test_parse);
    RUN(test_parse_length_limit);
    RUN(test_find);
    RUN(test_edges);
    RUN(test_overlap_and_volume);
}
