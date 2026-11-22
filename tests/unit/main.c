/* Unit test runner.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

int g_checks;
int g_failures;
const char *g_current;

typedef struct suite {
    const char *name;
    void (*run)(void);
} suite;

int main(int argc, char **argv)
{
    static const suite suites[] = {
        {"ini", suite_ini},         {"config", suite_config},   {"sigscan", suite_sigscan},
        {"pe_file", suite_pe_file}, {"cpu", suite_cpu},         {"patches", suite_patches},
        {"pe_image", suite_pe_image}, {"fs_guard", suite_fs_guard}, {"tex_rule", suite_tex_rule},
        {"dunia", suite_dunia},       {"menu", suite_menu},       {"padmode", suite_padmode},
        {"gfx_preset", suite_gfx_preset},
    };
    size_t i;
    int ran = 0;

    for (i = 0; i < sizeof(suites) / sizeof(suites[0]); ++i) {
        int before = g_failures;
        int checks = g_checks;
        if (argc > 1 && strcmp(argv[1], suites[i].name) != 0) {
            continue;
        }
        suites[i].run();
        ++ran;
        printf("[%s] %s: %d checks\n", g_failures == before ? " OK " : "FAIL", suites[i].name, g_checks - checks);
    }
    if (!ran) {
        printf("no such suite\n");
        return 2;
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
