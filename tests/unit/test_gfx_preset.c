/* Unit tests: draw-distance presets.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gfx_preset.h"
#include "test.h"

/* The shape of the game's default render configuration, shortened. */
static const char CONFIG[] =
    "<Profile MSAALevel=\"0\" Quality=\"custom\">\r\n"
    "\t<Geometry>\r\n"
    "\t\t<quality id=\"low\" LodScale=\"1.5\" KillLodScale=\"2\" ClustersLodScale=\"1.25\" RealTreesLodScale=\"1\" "
    "RealTreesVistaMinViewDistanceScale=\"1\" MaxGeomBatches=\"64\" />\r\n"
    "\t\t<quality id=\"ultrahigh\" LodScale=\"0.5\" KillLodScale=\"0.5\" ClustersLodScale=\"0.5\" "
    "RealTreesLodScale=\"0.7\" RealTreesVistaMinViewDistanceScale=\"0.4\" MaxGeomBatches=\"256\" />\r\n"
    "\t</Geometry>\r\n"
    "\t<Shadow>\r\n"
    "\t\t<quality id=\"ultrahigh\" ShadowMapSize=\"2048\" SunShadowFadeRange=\"10\" SunShadowRange0=\"3\" "
    "SunShadowRange1=\"12\" SunShadowRange2=\"60\" />\r\n"
    "\t</Shadow>\r\n"
    "\t<Terrain>\r\n"
    "\t\t<quality id=\"high\" TerrainLodScale=\"1.0\" TerrainDetailViewDistance=\"512\" "
    "TerrainDetailBlendViewDistance=\"64\" />\r\n"
    "\t</Terrain>\r\n"
    "\t<Water>\r\n"
    "\t\t<quality id=\"high\" LodScale=\"9\" />\r\n"
    "\t</Water>\r\n"
    "</Profile>\r\n";

static const char PROFILE[] =
    "<GamerProfile>\r\n"
    "\t<SoundProfile MusicEnabled=\"1\" />\r\n"
    "\t<RenderProfile MSAALevel=\"0\" ResolutionX=\"1920\" DisableMip0Loading=\"0\">\r\n"
    "\t\t<CustomQuality>\r\n"
    "\t\t\t<quality ResolutionX=\"1920\" GeometryQuality=\"ultrahigh\" ShadowQuality=\"ultrahigh\" />\r\n"
    "\t\t</CustomQuality>\r\n"
    "\t</RenderProfile>\r\n"
    "\t<NetworkProfile />\r\n"
    "</GamerProfile>\r\n";

static char g_out[8192];

static int has(const char *text, long len, const char *needle)
{
    size_t n = strlen(needle);
    long i;
    for (i = 0; len >= (long)n && i + (long)n <= len; ++i) {
        if (memcmp(text + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static long count(const char *text, long len, const char *needle)
{
    size_t n = strlen(needle);
    long i, c = 0;
    for (i = 0; i + (long)n <= len; ++i) {
        if (memcmp(text + i, needle, n) == 0) {
            ++c;
        }
    }
    return c;
}

static void test_section(void)
{
    unsigned values = 0;
    long n = gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Geometry", GP_FAR, g_out, sizeof(g_out), &values);

    CHECK(n > 0);
    CHECK_EQ_U(values, 8); /* four values in each of the two quality levels */
    CHECK(has(g_out, n, "<Geometry>") && has(g_out, n, "</Geometry>"));
    CHECK(!has(g_out, n, "Shadow"));
    CHECK(has(g_out, n, "id=\"low\" LodScale=\"1.05\" KillLodScale=\"1.4\""));
    CHECK(has(g_out, n, "id=\"ultrahigh\" LodScale=\"0.35\" KillLodScale=\"0.35\""));
    CHECK(has(g_out, n, " RealTreesLodScale=\"0.49\" RealTreesVistaMinViewDistanceScale=\"0.28\""));
    /* a name that merely ends like a known one, and everything else, stay as they are */
    CHECK(has(g_out, n, "ClustersLodScale=\"1.25\"") && has(g_out, n, "ClustersLodScale=\"0.5\""));
    CHECK(has(g_out, n, "MaxGeomBatches=\"256\" />"));

    n = gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Geometry", GP_VERY_FAR, g_out, sizeof(g_out), NULL);
    CHECK(has(g_out, n, "id=\"ultrahigh\" LodScale=\"0.25\" KillLodScale=\"0.25\""));

    n = gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Shadow", GP_FAR, g_out, sizeof(g_out), &values);
    CHECK(n > 0);
    CHECK_EQ_U(values, 3);
    /* the cascades grow together; the fade range and the map size are not distances to scale */
    CHECK(has(g_out, n, "ShadowMapSize=\"2048\" SunShadowFadeRange=\"10\" SunShadowRange0=\"4.5\" "
                        "SunShadowRange1=\"18\" SunShadowRange2=\"90\""));
    n = gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Shadow", GP_VERY_FAR, g_out, sizeof(g_out), NULL);
    CHECK(has(g_out, n, "SunShadowRange0=\"6\" SunShadowRange1=\"24\" SunShadowRange2=\"120\""));

    n = gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Terrain", GP_VERY_FAR, g_out, sizeof(g_out), &values);
    CHECK_EQ_U(values, 3);
    CHECK(has(g_out, n, "TerrainLodScale=\"0.5\" TerrainDetailViewDistance=\"1024\" "
                        "TerrainDetailBlendViewDistance=\"128\""));

    /* nothing to scale, no such section, no preset, no room */
    CHECK(gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Water", GP_FAR, g_out, sizeof(g_out), &values) == -1);
    CHECK(gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Post", GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Geometry", GP_ORIGINAL, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Geometry", (gp_level)7, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_scaled_section(CONFIG, sizeof(CONFIG) - 1, "Geometry", GP_FAR, g_out, 40, NULL) == -1);
    CHECK(gp_scaled_section(CONFIG, 60, "Geometry", GP_FAR, g_out, sizeof(g_out), NULL) == -1); /* cut short */
    CHECK(gp_scaled_section(NULL, 0, "Geometry", GP_FAR, g_out, sizeof(g_out), NULL) == -1);
}

static void test_unknown_values(void)
{
    static const char odd[] = "<Geometry><quality id=\"x\" LodScale=\"auto\" /></Geometry>";
    static const char open[] = "<Geometry><quality id=\"x\" LodScale=\"0.5 /></Geometry>";
    static const char longv[] = "<Geometry><quality id=\"x\" LodScale=\"0.50000000000000000000000000000000000\" />"
                                "</Geometry>";

    /* a value that is not a number means this is not the file we know: hands off */
    CHECK(gp_scaled_section(odd, sizeof(odd) - 1, "Geometry", GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_scaled_section(open, sizeof(open) - 1, "Geometry", GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_scaled_section(longv, sizeof(longv) - 1, "Geometry", GP_FAR, g_out, sizeof(g_out), NULL) == -1);
}

static void test_overlay(void)
{
    unsigned sections = 0;
    long n = gp_overlay(PROFILE, sizeof(PROFILE) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out),
                        &sections);
    const char *custom, *geometry, *end;

    CHECK(n > (long)sizeof(PROFILE));
    CHECK_EQ_U(sections, 3);
    g_out[n] = '\0';
    custom = strstr(g_out, "</CustomQuality>");
    geometry = strstr(g_out, "<Geometry>");
    end = strstr(g_out, "</RenderProfile>");
    CHECK(custom && geometry && end && custom < geometry && geometry < end);
    CHECK(strstr(g_out, "<Shadow>") > geometry && strstr(g_out, "</Terrain>") < end);
    CHECK(has(g_out, n, "id=\"ultrahigh\" LodScale=\"0.35\""));
    /* everything that was there is still there, once, in the same order */
    CHECK(strncmp(g_out, PROFILE, (size_t)(strstr(PROFILE, "\t</RenderProfile>") - PROFILE)) == 0);
    CHECK(strstr(g_out, "</RenderProfile>\r\n\t<NetworkProfile />\r\n</GamerProfile>\r\n") != NULL);
    CHECK_EQ_U(count(g_out, n, "<RenderProfile"), 1);
    CHECK_EQ_U(count(g_out, n, "</RenderProfile>"), 1);
    CHECK_EQ_U(count(g_out, n, "<Geometry>"), 1);

    /* the preset "original" adds nothing */
    CHECK(gp_overlay(PROFILE, sizeof(PROFILE) - 1, CONFIG, sizeof(CONFIG) - 1, GP_ORIGINAL, g_out, sizeof(g_out),
                     &sections) == -1);
    CHECK_EQ_U(sections, 0);
    /* a buffer that might not hold everything is refused outright, and nothing is written */
    memset(g_out, '#', sizeof(g_out));
    CHECK(gp_overlay(PROFILE, sizeof(PROFILE) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out,
                     sizeof(PROFILE) + sizeof(CONFIG) + GP_OVERLAY_SLACK - 3, NULL) == -1);
    CHECK(g_out[0] == '#' && g_out[sizeof(g_out) - 1] == '#');
    CHECK(gp_overlay(PROFILE, sizeof(PROFILE) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out,
                     sizeof(PROFILE) + sizeof(CONFIG) + GP_OVERLAY_SLACK - 2, NULL) > 0);
    CHECK(gp_overlay(PROFILE, sizeof(PROFILE) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, 100, NULL) == -1);
}

static void test_overlay_respects_the_player(void)
{
    static const char own[] =
        "<GamerProfile>\r\n"
        "\t<RenderProfile MSAALevel=\"0\">\r\n"
        "\t\t<CustomQuality><quality GeometryQuality=\"ultrahigh\" /></CustomQuality>\r\n"
        "\t\t<Geometry><quality id=\"ultrahigh\" LodScale=\"0.1\" /></Geometry>\r\n"
        "\t</RenderProfile>\r\n"
        "</GamerProfile>\r\n";
    static const char all[] = "<GamerProfile><RenderProfile A=\"1\"><Geometry></Geometry><Shadow></Shadow>"
                              "<Terrain></Terrain></RenderProfile></GamerProfile>";
    unsigned sections = 0;
    long n = gp_overlay(own, sizeof(own) - 1, CONFIG, sizeof(CONFIG) - 1, GP_VERY_FAR, g_out, sizeof(g_out), &sections);

    /* the player's own section of a kind is kept and not doubled */
    CHECK(n > 0);
    CHECK_EQ_U(sections, 2);
    CHECK_EQ_U(count(g_out, n, "<Geometry>"), 1);
    CHECK(has(g_out, n, "LodScale=\"0.1\""));
    CHECK(has(g_out, n, "<Shadow>") && has(g_out, n, "<Terrain>"));

    CHECK(gp_overlay(all, sizeof(all) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out), &sections) == -1);
    CHECK_EQ_U(sections, 0);
}

static void test_overlay_odd_profiles(void)
{
    static const char closed[] = "<GamerProfile>\r\n\t<RenderProfile MSAALevel=\"0\" />\r\n\t<Rest />\r\n</GamerProfile>";
    static const char none[] = "<GamerProfile>\r\n\t<SoundProfile />\r\n</GamerProfile>";
    static const char other[] = "<GamerProfile>\r\n\t<RenderProfileX A=\"1\"></RenderProfileX>\r\n</GamerProfile>";
    static const char torn[] = "<GamerProfile>\r\n\t<RenderProfile MSAALevel=\"0\">\r\n\t\t<CustomQuality>";
    static const char wide[] = "<\0G\0a\0m\0e\0r\0P\0r\0o\0f\0i\0l\0e\0>\0<\0R\0e\0n\0d\0e\0r\0";
    unsigned sections = 0;
    long n = gp_overlay(closed, sizeof(closed) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out), &sections);

    /* an element without content gets content and an end tag */
    CHECK(n > 0);
    CHECK_EQ_U(sections, 3);
    g_out[n > 0 ? n : 0] = '\0';
    CHECK(strstr(g_out, "<RenderProfile MSAALevel=\"0\" ><Geometry>") != NULL);
    CHECK(strstr(g_out, "</Terrain>\r\n</RenderProfile>\r\n\t<Rest />\r\n</GamerProfile>") != NULL);
    CHECK_EQ_U(count(g_out, n, "/>\r\n\t<Rest"), 0);

    CHECK(gp_overlay(none, sizeof(none) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_overlay(other, sizeof(other) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_overlay(torn, sizeof(torn) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_overlay(wide, sizeof(wide) - 1, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_overlay(PROFILE, 10, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    CHECK(gp_overlay(NULL, 0, CONFIG, sizeof(CONFIG) - 1, GP_FAR, g_out, sizeof(g_out), NULL) == -1);
    /* a render configuration without the sections: nothing to add */
    CHECK(gp_overlay(PROFILE, sizeof(PROFILE) - 1, "<Profile></Profile>", 19, GP_FAR, g_out, sizeof(g_out), NULL) ==
          -1);
}

void suite_gfx_preset(void)
{
    RUN(test_section);
    RUN(test_unknown_values);
    RUN(test_overlay);
    RUN(test_overlay_respects_the_player);
    RUN(test_overlay_odd_profiles);
}
