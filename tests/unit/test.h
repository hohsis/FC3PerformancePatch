/* Tiny test harness. No source paths are embedded in the binary.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_TEST_H
#define FC3PP_TEST_H

#include <stdio.h>
#include <string.h>

extern int g_checks;
extern int g_failures;
extern const char *g_current;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            printf("  FAIL %s:%d: %s\n", g_current ? g_current : "?", __LINE__, #cond); \
        }                                                                              \
    } while (0)

#define CHECK_EQ_U(actual, expected)                                                              \
    do {                                                                                          \
        unsigned long a_ = (unsigned long)(actual);                                               \
        unsigned long e_ = (unsigned long)(expected);                                             \
        ++g_checks;                                                                               \
        if (a_ != e_) {                                                                           \
            ++g_failures;                                                                         \
            printf("  FAIL %s:%d: %s == 0x%lX, expected 0x%lX\n", g_current ? g_current : "?",     \
                   __LINE__, #actual, a_, e_);                                                    \
        }                                                                                         \
    } while (0)

#define CHECK_STR(actual, expected)                                                               \
    do {                                                                                          \
        const char *a_ = (actual);                                                                \
        const char *e_ = (expected);                                                              \
        ++g_checks;                                                                               \
        if (!a_ || strcmp(a_, e_) != 0) {                                                         \
            ++g_failures;                                                                         \
            printf("  FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", g_current ? g_current : "?",  \
                   __LINE__, #actual, a_ ? a_ : "(null)", e_);                                    \
        }                                                                                         \
    } while (0)

#define RUN(fn)            \
    do {                   \
        g_current = #fn;   \
        fn();              \
    } while (0)

void suite_ini(void);
void suite_config(void);
void suite_sigscan(void);
void suite_pe_file(void);
void suite_cpu(void);
void suite_patches(void);
void suite_pe_image(void);

#endif /* FC3PP_TEST_H */
