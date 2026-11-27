/* Draw-distance presets. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gfx_preset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* What a preset multiplies. For the level-of-detail scales smaller means
 * "switch to the simpler model later", i.e. more detail further away; the
 * original ultra setting is 0.5 where high is 1.0. Shadow ranges are the far
 * ends of the three sun shadow cascades in metres; scaling all three by the
 * same factor keeps the order the engine needs (each range plus the fade
 * range below the next). */
typedef struct scale_rule {
    const char *section;
    const char *attribute;
    float factor[GP_LEVELS]; /* indexed by gp_level */
} scale_rule;

/* A factor of 1 means: this preset leaves the value alone (the text is not
 * even rewritten). The "near" presets also thin out the vegetation clusters
 * (ClustersLodScale), which the "far" ones do not touch. */
static const scale_rule RULES[] = {
    /*                                                    original far   v.far  near  v.near */
    {"Geometry", "LodScale", {1.0f, 0.7f, 0.5f, 1.5f, 2.0f}},
    {"Geometry", "KillLodScale", {1.0f, 0.7f, 0.5f, 1.5f, 2.0f}},
    {"Geometry", "ClustersLodScale", {1.0f, 1.0f, 1.0f, 1.5f, 2.0f}},
    {"Geometry", "RealTreesLodScale", {1.0f, 0.7f, 0.5f, 1.5f, 2.0f}},
    {"Geometry", "RealTreesVistaMinViewDistanceScale", {1.0f, 0.7f, 0.5f, 1.5f, 2.0f}},
    {"Shadow", "SunShadowRange0", {1.0f, 1.5f, 2.0f, 0.7f, 0.5f}},
    {"Shadow", "SunShadowRange1", {1.0f, 1.5f, 2.0f, 0.7f, 0.5f}},
    {"Shadow", "SunShadowRange2", {1.0f, 1.5f, 2.0f, 0.7f, 0.5f}},
    {"Terrain", "TerrainLodScale", {1.0f, 0.7f, 0.5f, 1.5f, 2.0f}},
    {"Terrain", "TerrainDetailViewDistance", {1.0f, 1.5f, 2.0f, 0.7f, 0.5f}},
    {"Terrain", "TerrainDetailBlendViewDistance", {1.0f, 1.5f, 2.0f, 0.7f, 0.5f}},
};

static const int STEP_OF_LEVEL[GP_LEVELS] = {0, 1, 2, -1, -2};

int gp_level_to_step(gp_level level)
{
    return (int)level >= 0 && level < GP_LEVELS ? STEP_OF_LEVEL[level] : 0;
}

int gp_level_from_step(int step, gp_level *level)
{
    int i;
    for (i = 0; i < GP_LEVELS; ++i) {
        if (STEP_OF_LEVEL[i] == step) {
            *level = (gp_level)i;
            return 1;
        }
    }
    return 0;
}

const char *gp_level_name(gp_level level)
{
    static const char *const names[GP_LEVELS] = {"Original", "Far", "Very far", "Near", "Very near"};
    return (int)level >= 0 && level < GP_LEVELS ? names[level] : names[GP_ORIGINAL];
}

static const char *const SECTIONS[] = {"Geometry", "Shadow", "Terrain"};

/* First occurrence of needle in [text, text+len). */
static const char *find(const char *text, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;

    if (!n || n > len) {
        return NULL;
    }
    for (i = 0; i + n <= len; ++i) {
        if (text[i] == needle[0] && memcmp(text + i, needle, n) == 0) {
            return text + i;
        }
    }
    return NULL;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* The element <name> ... </name> (a start tag without attributes, as the
 * sections are written). Returns its start and sets *len. */
static const char *element(const char *text, size_t len, const char *name, size_t *elem_len)
{
    char open[48], close[48];
    const char *start, *end;

    snprintf(open, sizeof(open), "<%s>", name);
    snprintf(close, sizeof(close), "</%s>", name);
    start = find(text, len, open);
    if (!start) {
        return NULL;
    }
    end = find(start, len - (size_t)(start - text), close);
    if (!end) {
        return NULL;
    }
    *elem_len = (size_t)(end - start) + strlen(close);
    return start;
}

/* Does an attribute `name="` that this preset changes start at p (which
 * follows white space)? */
static const scale_rule *rule_at(const char *p, size_t left, const char *section, gp_level level)
{
    size_t r;
    for (r = 0; r < sizeof(RULES) / sizeof(RULES[0]); ++r) {
        size_t n = strlen(RULES[r].attribute);
        if (strcmp(RULES[r].section, section) == 0 && left > n + 2 && memcmp(p, RULES[r].attribute, n) == 0 &&
            p[n] == '=' && p[n + 1] == '"') {
            return RULES[r].factor[level] != 1.0f ? &RULES[r] : NULL;
        }
    }
    return NULL;
}

long gp_scaled_section(const char *cfg, size_t cfg_len, const char *section, gp_level level, char *out, size_t cap,
                       unsigned *values)
{
    size_t len = 0, i = 0, o = 0;
    const char *src;
    unsigned changed = 0;

    if (values) {
        *values = 0;
    }
    if (!cfg || !out || level <= GP_ORIGINAL || level >= GP_LEVELS) {
        return -1;
    }
    src = element(cfg, cfg_len, section, &len);
    if (!src) {
        return -1;
    }
    while (i < len) {
        const scale_rule *rule =
            (i > 0 && is_space(src[i - 1])) ? rule_at(src + i, len - i, section, level) : NULL;
        if (rule) {
            size_t name_len = strlen(rule->attribute) + 2; /* name=" */
            size_t v = i + name_len, e = v;
            char number[32], text[32];
            double value;
            char *stop;
            int n;

            while (e < len && src[e] != '"') {
                ++e;
            }
            if (e >= len || e - v >= sizeof(number)) {
                return -1;
            }
            memcpy(number, src + v, e - v);
            number[e - v] = '\0';
            value = strtod(number, &stop);
            if (stop == number) {
                return -1; /* not a number: this is not the file we know */
            }
            n = snprintf(text, sizeof(text), "%.6g", value * (double)rule->factor[level]);
            if (n <= 0 || o + name_len + (size_t)n >= cap) {
                return -1;
            }
            memcpy(out + o, src + i, name_len);
            o += name_len;
            memcpy(out + o, text, (size_t)n);
            o += (size_t)n;
            i = e; /* the closing quote is copied by the loop */
            ++changed;
            continue;
        }
        if (o + 1 >= cap) {
            return -1;
        }
        out[o++] = src[i++];
    }
    if (values) {
        *values = changed;
    }
    return changed ? (long)o : -1;
}

long gp_overlay(const char *profile, size_t profile_len, const char *cfg, size_t cfg_len, gp_level level, char *out,
                size_t cap, unsigned *sections)
{
    const char *tag, *tag_end, *close, *insert_at;
    size_t o = 0, s;
    unsigned added = 0;
    int self_closing;

    if (sections) {
        *sections = 0;
    }
    if (!profile || !cfg || !out || profile_len < 16 || level <= GP_ORIGINAL || level >= GP_LEVELS) {
        return -1;
    }
    /* Room for the worst case, so that a section is never left out for lack of it. */
    if (cap < profile_len + cfg_len + GP_OVERLAY_SLACK) {
        return -1;
    }
    /* Only single-byte text (the game writes UTF-8). */
    if (memchr(profile, '\0', profile_len < 64 ? profile_len : 64)) {
        return -1;
    }
    tag = find(profile, profile_len, "<RenderProfile");
    if (!tag || (size_t)(tag - profile) + 15 > profile_len ||
        !(is_space(tag[14]) || tag[14] == '>' || tag[14] == '/')) {
        return -1;
    }
    tag_end = memchr(tag, '>', profile_len - (size_t)(tag - profile));
    if (!tag_end) {
        return -1;
    }
    self_closing = tag_end[-1] == '/';
    if (self_closing) {
        insert_at = tag_end - 1; /* "/>" becomes ">" + sections + "</RenderProfile>" */
        close = tag_end + 1;
    } else {
        close = find(tag_end, profile_len - (size_t)(tag_end - profile), "</RenderProfile>");
        if (!close) {
            return -1;
        }
        insert_at = close;
    }

    if ((size_t)(insert_at - profile) + 2 >= cap) {
        return -1;
    }
    memcpy(out, profile, (size_t)(insert_at - profile));
    o = (size_t)(insert_at - profile);
    if (self_closing) {
        out[o++] = '>';
    }
    for (s = 0; s < sizeof(SECTIONS) / sizeof(SECTIONS[0]); ++s) {
        char open[48];
        long n;

        /* The player's own section of this kind wins. */
        snprintf(open, sizeof(open), "<%s", SECTIONS[s]);
        if (!self_closing) {
            const char *own = find(tag_end, (size_t)(close - tag_end), open);
            if (own && (own[strlen(open)] == '>' || is_space(own[strlen(open)]))) {
                continue;
            }
        }
        n = gp_scaled_section(cfg, cfg_len, SECTIONS[s], level, out + o, cap - o, NULL);
        if (n > 0) {
            o += (size_t)n;
            if (o + 2 >= cap) {
                return -1;
            }
            out[o++] = '\r';
            out[o++] = '\n';
            ++added;
        }
    }
    if (!added) {
        return -1;
    }
    if (self_closing) {
        static const char end_tag[] = "</RenderProfile>";
        if (o + sizeof(end_tag) >= cap) {
            return -1;
        }
        memcpy(out + o, end_tag, sizeof(end_tag) - 1);
        o += sizeof(end_tag) - 1;
    }
    s = profile_len - (size_t)(close - profile);
    if (o + s >= cap) {
        return -1;
    }
    memcpy(out + o, close, s);
    o += s;
    if (sections) {
        *sections = added;
    }
    return (long)o;
}
