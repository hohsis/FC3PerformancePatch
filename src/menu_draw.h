/* Turns the settings menu into coloured, textured rectangles. Pure C: the
 * renderer supplies the font's measurements and draws the result.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_MENU_DRAW_H
#define FC3PP_MENU_DRAW_H

#include <stddef.h>

#include "menu_model.h"

#define MD_FIRST_CHAR 32
#define MD_LAST_CHAR 126
#define MD_GLYPHS (MD_LAST_CHAR - MD_FIRST_CHAR + 1)

/* Colours are bytes R, G, B, A in memory: 0xAABBGGRR on x86. */
typedef unsigned long md_color;

typedef struct md_vertex {
    float x, y; /* pixels, origin top left */
    float u, v; /* font texture coordinates */
    md_color color;
} md_vertex;

typedef struct md_glyph {
    float u0, v0, u1, v1; /* where the glyph's cell is in the texture */
    float w, h;           /* size of the cell in pixels */
    float x_offset;       /* from the pen position to the cell's left edge */
    float advance;        /* how far the pen moves */
} md_glyph;

typedef struct md_font {
    md_glyph glyph[MD_GLYPHS];
    float line_height;
    float solid_u, solid_v; /* a fully opaque texel, for plain rectangles */
} md_font;

typedef struct md_list {
    md_vertex *v; /* six per rectangle (two triangles) */
    unsigned count, cap;
    unsigned dropped; /* rectangles that did not fit */
} md_list;

void md_rect(md_list *out, const md_font *font, float x, float y, float w, float h, md_color color);

/* Draws one line of text with its top left corner at (x, y), no wider than
 * max_width (0 = unlimited; longer text is cut and ends in "..."). Returns
 * the x position after the last glyph. */
float md_text(md_list *out, const md_font *font, float x, float y, const char *text, md_color color, float max_width);

float md_text_width(const md_font *font, const char *text);

/* Breaks text into lines of at most max_width pixels at spaces. Calls back
 * with each line (not NUL-terminated); returns the number of lines. */
typedef void (*md_line_fn)(void *ctx, const char *line, size_t len, unsigned index);
unsigned md_wrap(const md_font *font, const char *text, float max_width, unsigned max_lines, md_line_fn fn, void *ctx);

/* How many rows of the item list fit on a screen of this height. */
unsigned md_visible_rows(const md_font *font, const mm_menu *menu, float screen_h);

/* The whole menu. hints is the line of key names shown at the bottom. Call
 * mm_scroll_into_view(menu, md_visible_rows(...)) first. */
void md_menu(md_list *out, const md_font *font, const mm_menu *menu, float screen_w, float screen_h, const char *hints);

#endif /* FC3PP_MENU_DRAW_H */
