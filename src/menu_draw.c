/* Turns the settings menu into coloured, textured rectangles. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "menu_draw.h"

#include <stdio.h>
#include <string.h>

#define COL_PANEL 0xEB1A1612ul     /* near-black, 92 % */
#define COL_EDGE 0xFF40B0FFul      /* amber */
#define COL_TITLE 0xFF40B0FFul
#define COL_TEXT 0xFFECECECul
#define COL_DIM 0xFFA8A29Cul
#define COL_HEADING 0xFF6CC4FFul
#define COL_RULE 0x26FFFFFFul      /* white, 15 % */
#define COL_SELECTED 0x4040B0FFul  /* amber, 25 % */
#define COL_SCROLL 0x66FFFFFFul

#define HELP_LINES 3
#define ROW_FACTOR 1.38f
#define MAX_PANEL_FACTOR 0.84f /* of the screen height */

static const md_glyph *glyph_of(const md_font *font, char c)
{
    unsigned char u = (unsigned char)c;
    if (u < MD_FIRST_CHAR || u > MD_LAST_CHAR) {
        u = '?';
    }
    return &font->glyph[u - MD_FIRST_CHAR];
}

static void quad(md_list *out, float x, float y, float w, float h, float u0, float v0, float u1, float v1,
                 md_color color)
{
    md_vertex *v;

    if (w <= 0 || h <= 0) {
        return;
    }
    if (!out->v || out->count + 6 > out->cap) {
        out->dropped++;
        return;
    }
    v = out->v + out->count;
    out->count += 6;
#define VTX(i, px, py, pu, pv) \
    v[i].x = (px);             \
    v[i].y = (py);             \
    v[i].u = (pu);             \
    v[i].v = (pv);             \
    v[i].color = color
    VTX(0, x, y, u0, v0);
    VTX(1, x + w, y, u1, v0);
    VTX(2, x, y + h, u0, v1);
    VTX(3, x + w, y, u1, v0);
    VTX(4, x + w, y + h, u1, v1);
    VTX(5, x, y + h, u0, v1);
#undef VTX
}

void md_rect(md_list *out, const md_font *font, float x, float y, float w, float h, md_color color)
{
    quad(out, x, y, w, h, font->solid_u, font->solid_v, font->solid_u, font->solid_v, color);
}

float md_text_width(const md_font *font, const char *text)
{
    float w = 0;
    for (; text && *text; ++text) {
        w += glyph_of(font, *text)->advance;
    }
    return w;
}

static float text_n(md_list *out, const md_font *font, float x, float y, const char *text, size_t len, md_color color)
{
    size_t i;
    for (i = 0; i < len; ++i) {
        const md_glyph *g = glyph_of(font, text[i]);
        if (text[i] != ' ') {
            quad(out, x + g->x_offset, y, g->w, g->h, g->u0, g->v0, g->u1, g->v1, color);
        }
        x += g->advance;
    }
    return x;
}

float md_text(md_list *out, const md_font *font, float x, float y, const char *text, md_color color, float max_width)
{
    size_t len = text ? strlen(text) : 0;

    if (max_width > 0 && md_text_width(font, text) > max_width) {
        float dots = 3 * glyph_of(font, '.')->advance;
        float w = 0;
        size_t keep = 0;
        while (keep < len && w + glyph_of(font, text[keep])->advance + dots <= max_width) {
            w += glyph_of(font, text[keep])->advance;
            ++keep;
        }
        x = text_n(out, font, x, y, text, keep, color);
        return text_n(out, font, x, y, "...", 3, color);
    }
    return text_n(out, font, x, y, text, len, color);
}

unsigned md_wrap(const md_font *font, const char *text, float max_width, unsigned max_lines, md_line_fn fn, void *ctx)
{
    unsigned lines = 0;
    const char *p = text;

    while (p && *p && lines < max_lines) {
        const char *start, *fit = NULL, *q;
        float w = 0;

        while (*p == ' ') {
            ++p;
        }
        if (!*p) {
            break;
        }
        start = p;
        for (q = p; *q; ++q) {
            if (*q == ' ') {
                fit = q; /* the line may end here */
            }
            w += glyph_of(font, *q)->advance;
            if (w > max_width && q > start) {
                break;
            }
        }
        if (!*q) {
            fit = q; /* the rest fits */
        } else if (!fit) {
            fit = q; /* one word longer than the line: cut it */
        }
        if (fn) {
            fn(ctx, start, (size_t)(fit - start), lines);
        }
        ++lines;
        p = fit;
    }
    return lines;
}

typedef struct layout {
    float line, row, pad;
    float x, y, w, h;
    float head, foot;
    unsigned rows;
} layout;

static void measure(layout *l, const md_font *font, const mm_menu *menu, float screen_w, float screen_h)
{
    float room;
    unsigned rows;

    l->line = font->line_height;
    l->row = l->line * ROW_FACTOR;
    l->pad = l->line * 0.7f;
    l->head = l->line * 1.9f;                    /* title and rule */
    l->foot = l->line * (HELP_LINES + 1 + 2.3f); /* help text, when a change applies, key hints */
    room = screen_h * MAX_PANEL_FACTOR - l->head - l->foot - 2 * l->pad;
    rows = room > l->row ? (unsigned)(room / l->row) : 1;
    if (menu && rows > menu->count) {
        rows = menu->count;
    }
    if (!rows) {
        rows = 1;
    }
    l->rows = rows;
    l->w = l->line * 30;
    if (l->w > screen_w * 0.9f) {
        l->w = screen_w * 0.9f;
    }
    l->h = l->head + (float)rows * l->row + l->foot + 2 * l->pad;
    l->x = screen_w * 0.05f;
    if (l->x + l->w > screen_w) {
        l->x = (screen_w - l->w) / 2;
    }
    l->y = (screen_h - l->h) / 2;
}

unsigned md_visible_rows(const md_font *font, const mm_menu *menu, float screen_h)
{
    layout l;
    measure(&l, font, menu, 1e6f, screen_h);
    return l.rows;
}

typedef struct help_ctx {
    md_list *out;
    const md_font *font;
    float x, y, line;
} help_ctx;

static void help_line(void *ctx, const char *line, size_t len, unsigned index)
{
    help_ctx *h = (help_ctx *)ctx;
    text_n(h->out, h->font, h->x, h->y + (float)index * h->line, line, len, COL_DIM);
}

void md_menu(md_list *out, const md_font *font, const mm_menu *menu, float screen_w, float screen_h, const char *hints)
{
    layout l;
    float inner_x, inner_w, y, text_dy;
    unsigned i;
    char value[MM_TEXT_MAX];

    if (!menu || !menu->open || !font || font->line_height <= 0) {
        return;
    }
    measure(&l, font, menu, screen_w, screen_h);
    inner_x = l.x + l.pad;
    inner_w = l.w - 2 * l.pad;
    text_dy = (l.row - l.line) / 2;

    md_rect(out, font, l.x, l.y, l.w, l.h, COL_PANEL);
    md_rect(out, font, l.x, l.y, l.line * 0.18f, l.h, COL_EDGE);

    y = l.y + l.pad;
    md_text(out, font, inner_x, y, menu->title ? menu->title : "", COL_TITLE, inner_w);
    md_rect(out, font, inner_x, y + l.line * 1.45f, inner_w, 1 + l.line / 24, COL_RULE);
    y += l.head;

    for (i = 0; i < l.rows && menu->top + i < menu->count; ++i) {
        const mm_item *it = &menu->items[menu->top + i];
        int selected = menu->top + i == menu->selected && mm_selectable(it);
        float row_y = y + (float)i * l.row;

        if (it->kind == MM_HEADING) {
            md_text(out, font, inner_x, row_y + text_dy + l.line * 0.12f, it->label, COL_HEADING, inner_w);
            continue;
        }
        if (selected) {
            md_rect(out, font, l.x + l.line * 0.18f, row_y, l.w - l.line * 0.18f, l.row, COL_SELECTED);
        }
        mm_value_text(it, value, sizeof(value));
        if (mm_pending(it)) { /* chosen, but the game still runs with another value */
            size_t n = strlen(value);
            snprintf(value + n, sizeof(value) - n, " (restart)");
        }
        {
            int marked = (it->flags & MM_RESTART) ? mm_pending(it) : (int)(it->flags & MM_CHANGED);
            md_color label_col = it->kind == MM_INFO ? COL_DIM : COL_TEXT;
            md_color value_col = it->kind == MM_INFO ? COL_DIM : (marked ? COL_TITLE : COL_TEXT);
            float value_w = md_text_width(font, value);
            float arrows = selected && it->kind != MM_ACTION ? md_text_width(font, "< ") : 0;
            float value_x = inner_x + inner_w - value_w - arrows;
            float label_max = value_x - arrows - inner_x - l.line * 0.8f;

            if (value_w > inner_w * 0.6f) { /* a long info text: let it take the room it needs */
                value_w = inner_w * 0.6f;
                value_x = inner_x + inner_w - value_w - arrows;
                label_max = value_x - arrows - inner_x - l.line * 0.8f;
            }
            md_text(out, font, inner_x + l.line * 0.6f, row_y + text_dy, it->label, label_col,
                    label_max > 0 ? label_max : 1);
            if (arrows > 0) {
                md_text(out, font, value_x - arrows, row_y + text_dy, "<", COL_TITLE, 0);
                md_text(out, font, inner_x + inner_w - md_text_width(font, ">"), row_y + text_dy, ">", COL_TITLE, 0);
            }
            md_text(out, font, value_x, row_y + text_dy, value, value_col, value_w + 1);
        }
    }
    /* scroll bar */
    if (menu->count > l.rows) {
        float track = (float)l.rows * l.row;
        float bar = track * (float)l.rows / (float)menu->count;
        float pos = track * (float)menu->top / (float)menu->count;
        md_rect(out, font, l.x + l.w - l.line * 0.22f, y + pos, l.line * 0.12f, bar, COL_SCROLL);
    }
    y += (float)l.rows * l.row + l.pad * 0.6f;

    md_rect(out, font, inner_x, y, inner_w, 1 + l.line / 24, COL_RULE);
    y += l.line * 0.45f;
    if (menu->selected < menu->count) {
        const mm_item *it = &menu->items[menu->selected];
        help_ctx h;
        float note_y = y + (float)HELP_LINES * l.line;
        h.out = out;
        h.font = font;
        h.x = inner_x;
        h.y = y;
        h.line = l.line;
        md_wrap(font, it->help ? it->help : "", inner_w, HELP_LINES, help_line, &h);
        /* A line of its own for when a change applies: it must not depend on
         * how much room the help text leaves. */
        if (mm_pending(it)) {
            char note[MM_TEXT_MAX + 64];
            mm_applied_text(it, value, sizeof(value));
            snprintf(note, sizeof(note), "In use now: %s. Restart the game to change it.", value);
            md_text(out, font, inner_x, note_y, note, COL_TITLE, inner_w);
        } else if (it->flags & MM_RESTART) {
            md_text(out, font, inner_x, note_y, "A change here applies after a restart of the game.", COL_DIM,
                    inner_w);
        } else if (it->kind == MM_TOGGLE || it->kind == MM_CHOICE || it->kind == MM_NUMBER) {
            md_text(out, font, inner_x, note_y, "A change here applies at once.", COL_DIM, inner_w);
        }
    }
    md_text(out, font, inner_x, l.y + l.h - l.pad - l.line, hints ? hints : "", COL_DIM, inner_w);
}
