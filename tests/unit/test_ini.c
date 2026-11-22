/* INI parser and configuration tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config.h"
#include "ini.h"
#include "test.h"

static void parse(ini_doc *doc, const char *text)
{
    CHECK(ini_parse(doc, text, strlen(text)) == 0);
}

static void test_basic(void)
{
    ini_doc d;
    parse(&d, "[General]\nEnabled=1\nLogLevel = 3\n\n[CPU]\nMaxCores=6\n");
    CHECK_STR(ini_get(&d, "General", "Enabled"), "1");
    CHECK_STR(ini_get(&d, "General", "LogLevel"), "3");
    CHECK_STR(ini_get(&d, "CPU", "MaxCores"), "6");
    CHECK(ini_get(&d, "CPU", "Missing") == NULL);
    CHECK(ini_get(&d, "Nope", "MaxCores") == NULL);
    CHECK_EQ_U(d.malformed, 0);
    ini_free(&d);
}

static void test_case_and_whitespace(void)
{
    ini_doc d;
    parse(&d, "  [ cpu ]  \r\n\t  maxCORES\t=\t 12 \r\n");
    CHECK_STR(ini_get(&d, "CPU", "MaxCores"), "12");
    CHECK_EQ_U(ini_get_int(&d, "cpu", "MAXCORES", 0), 12);
    ini_free(&d);
}

static void test_comments(void)
{
    ini_doc d;
    parse(&d, "; leading comment\n# another\n[A]\nk=v ; trailing\nc=#ff00ff\nu=a;b\nh=1 # note\n");
    CHECK_STR(ini_get(&d, "A", "k"), "v");
    CHECK_STR(ini_get(&d, "A", "c"), "#ff00ff"); /* '#' not preceded by whitespace */
    CHECK_STR(ini_get(&d, "A", "u"), "a;b");
    CHECK_STR(ini_get(&d, "A", "h"), "1");
    ini_free(&d);
}

static void test_quotes_and_empty(void)
{
    ini_doc d;
    parse(&d, "[A]\nq=\"  spaced ; value  \"\ns='x'\nempty=\nonlyspace=   \n");
    CHECK_STR(ini_get(&d, "A", "q"), "  spaced ; value  ");
    CHECK_STR(ini_get(&d, "A", "s"), "x");
    CHECK_STR(ini_get(&d, "A", "empty"), "");
    CHECK_STR(ini_get(&d, "A", "onlyspace"), "");
    CHECK_EQ_U(ini_get_int(&d, "A", "empty", 7), 7);
    ini_free(&d);
}

static void test_bom_crlf_no_trailing_newline(void)
{
    ini_doc d;
    parse(&d, "\xEF\xBB\xBF[S]\r\nKey=Value\r\nLast=end");
    CHECK_STR(ini_get(&d, "S", "Key"), "Value");
    CHECK_STR(ini_get(&d, "S", "Last"), "end");
    ini_free(&d);
}

static void test_malformed(void)
{
    ini_doc d;
    parse(&d, "global=1\n[Unclosed\nnoequals\n=novalue\n[Good]\nx=1\n[Good]\nx=2\n");
    CHECK_STR(ini_get(&d, "", "global"), "1");
    CHECK_STR(ini_get(&d, NULL, "global"), "1");
    CHECK_EQ_U(d.malformed, 3);
    CHECK_STR(ini_get(&d, "Good", "x"), "2"); /* last duplicate wins */
    ini_free(&d);
}

static void test_numbers(void)
{
    ini_doc d;
    parse(&d, "[N]\ndec=42\nneg=-5\nhex=0xFFFFFFFF\nbad=12abc\nbig=99999999999999999999\noct=010\n"
              "t=true\nf=No\non=ON\nnum=2\nword=maybe\n");
    CHECK(ini_get_int(&d, "N", "dec", 0) == 42);
    CHECK(ini_get_int(&d, "N", "neg", 0) == -5);
    CHECK_EQ_U(ini_get_uint(&d, "N", "hex", 0), 0xFFFFFFFFul);
    CHECK(ini_get_uint(&d, "N", "neg", 9) == 9);
    CHECK(ini_get_int(&d, "N", "bad", 3) == 3);
    CHECK(ini_get_int(&d, "N", "big", 4) == 4);
    CHECK(ini_get_int(&d, "N", "oct", 0) == 8);
    CHECK(ini_get_int_clamped(&d, "N", "dec", 0, 0, 10) == 10);
    CHECK(ini_get_int_clamped(&d, "N", "neg", 0, 0, 10) == 0);
    CHECK(ini_get_bool(&d, "N", "t", 0) == 1);
    CHECK(ini_get_bool(&d, "N", "f", 1) == 0);
    CHECK(ini_get_bool(&d, "N", "on", 0) == 1);
    CHECK(ini_get_bool(&d, "N", "num", 0) == 1);
    CHECK(ini_get_bool(&d, "N", "word", 1) == 1);
    CHECK(ini_get_bool(&d, "N", "absent", 0) == 0);
    ini_free(&d);
}

static void test_degenerate_input(void)
{
    ini_doc d;
    char nul_text[] = {'[', 'A', ']', '\n', 'k', '=', 'a', '\0', 'b', '\n'};
    CHECK(ini_parse(&d, NULL, 0) == 0);
    CHECK_EQ_U(d.count, 0);
    ini_free(&d);
    CHECK(ini_parse(&d, "", 0) == 0);
    ini_free(&d);
    CHECK(ini_parse(&d, "\n\n\r\n   \n", 9) == 0);
    CHECK_EQ_U(d.count, 0);
    ini_free(&d);
    CHECK(ini_parse(&d, nul_text, sizeof(nul_text)) == 0);
    CHECK_STR(ini_get(&d, "A", "k"), "a b"); /* embedded NUL becomes a space */
    ini_free(&d);
    ini_free(&d); /* double free must be harmless */
    CHECK(ini_get(NULL, "A", "k") == NULL);
}

static void test_many_entries(void)
{
    char text[8192];
    size_t pos = 0;
    int i;
    ini_doc d;
    pos += (size_t)sprintf(text + pos, "[Big]\n");
    for (i = 0; i < 300; ++i) {
        pos += (size_t)sprintf(text + pos, "key%d=%d\n", i, i * 3);
    }
    CHECK(ini_parse(&d, text, pos) == 0);
    CHECK_EQ_U(d.count, 300);
    CHECK(ini_get_int(&d, "Big", "key299", -1) == 897);
    CHECK(ini_get_int(&d, "Big", "key0", -1) == 0);
    ini_free(&d);
}

void suite_ini(void)
{
    RUN(test_basic);
    RUN(test_case_and_whitespace);
    RUN(test_comments);
    RUN(test_quotes_and_empty);
    RUN(test_bom_crlf_no_trailing_newline);
    RUN(test_malformed);
    RUN(test_numbers);
    RUN(test_degenerate_input);
    RUN(test_many_entries);
}

/* ---- configuration ------------------------------------------------------ */

static void test_config_defaults(void)
{
    fc3pp_config c;
    CHECK(config_load_text(&c, NULL, 0) == 0);
    CHECK(c.enabled == 1);
    CHECK(c.log_level == 2);
    CHECK(c.fix_core_enumeration == 1);
    CHECK(c.mode == CPU_MODE_ENGINE);
    CHECK(c.max_cores == 6);
    CHECK(c.use_smt == 0);
    CHECK(c.timer_resolution_ms == 1);
    CHECK(c.sleep_zero_yield == 0); /* experimental features default to off */
    CHECK(c.apply_large_address_aware == 1);
    CHECK(c.simulate_affinity_mask == 0);
    CHECK(c.trace_cpu_api == 0);
    CHECK(c.restore_fullscreen == 1);
    CHECK(c.high_dpi_aware == 1);
    CHECK(c.disable_window_ghosting == 1);
    CHECK(c.keep_xaudio2_loaded == 1);
    CHECK(c.texture_guard == 1);
    CHECK(c.texture_scan == 1);
    CHECK(c.menu_enabled == 1);
    CHECK(c.menu_key == 0x77); /* F8 */
    CHECK(c.trace_audio == 0); /* diagnostics default to off */
}

static void test_config_values(void)
{
    static const char text[] = "[General]\nEnabled=0\nLogLevel=9\n[CPU]\nMode=Process\nMaxCores=99\nUseSMT=yes\n"
                               "Cores=0,2,4-6\nFixCoreEnumeration=0\n[Timing]\nTimerResolutionMs=-3\n"
                               "[Memory]\nApplyLargeAddressAware=off\n[Debug]\nSimulateAffinityMask=0xFFFFFFFF\n"
                               "[Display]\nRestoreFullscreenOnFocus=0\nHighDpiAware=no\nDisableWindowGhosting=off\n"
                               "[Audio]\nKeepXAudio2Loaded=0\n[Debug]\nTraceAudio=1\n"
                               "[Textures]\nSkipMismatchedMip0=0\nSkipReplacedTextureMip0=0\n[Menu]\nEnabled=0\nKey=Home\n";
    fc3pp_config c;
    CHECK(config_load_text(&c, text, sizeof(text) - 1) == 0);
    CHECK(c.enabled == 0);
    CHECK(c.log_level == 3);   /* clamped */
    CHECK(c.mode == CPU_MODE_PROCESS);
    CHECK(c.max_cores == 32);  /* clamped */
    CHECK(c.use_smt == 1);
    CHECK_EQ_U(c.cores_mask, 0x75);
    CHECK(c.cores_list_invalid == 0);
    CHECK(c.fix_core_enumeration == 0);
    CHECK(c.timer_resolution_ms == 0);
    CHECK(c.apply_large_address_aware == 0);
    CHECK_EQ_U(c.simulate_affinity_mask, 0xFFFFFFFFul);
    CHECK(c.restore_fullscreen == 0);
    CHECK(c.high_dpi_aware == 0);
    CHECK(c.disable_window_ghosting == 0);
    CHECK(c.keep_xaudio2_loaded == 0);
    CHECK(c.texture_guard == 0);
    CHECK(c.texture_scan == 0);
    CHECK(c.menu_enabled == 0);
    CHECK(c.menu_key == 0x24 && c.menu_key_invalid == 0);
    CHECK(c.trace_audio == 1);
}

static void test_config_bad_values(void)
{
    static const char text[] = "[CPU]\nMode=banana\nCores=1,x\nMaxCores=lots\n";
    fc3pp_config c;
    CHECK(config_load_text(&c, text, sizeof(text) - 1) == 0);
    CHECK(c.mode == CPU_MODE_ENGINE); /* unknown mode keeps the default */
    CHECK(c.cores_list_invalid == 1);
    CHECK_EQ_U(c.cores_mask, 0);
    CHECK(c.max_cores == 6);
    CHECK_STR(config_mode_name(CPU_MODE_OFF), "off");
    CHECK_STR(config_mode_name(CPU_MODE_ENGINE), "engine");
    CHECK_STR(config_mode_name(CPU_MODE_PROCESS), "process");
}

void suite_config(void)
{
    RUN(test_config_defaults);
    RUN(test_config_values);
    RUN(test_config_bad_values);
}
