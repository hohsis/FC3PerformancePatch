/* Texture streaming: the "top mip level" rule, its bookkeeping and the
 * signatures that locate the engine's texture class. Pure C.
 *
 * Background. The Dunia engine stores most textures in two files: a base file
 * with every mip level but the largest, and a separate file holding only the
 * largest level ("mip0"). The base file names its mip0 file. While playing,
 * the engine streams the mip0 file in when a texture gets close to the camera
 * and drops the level again later. When it adds the level it does not look at
 * the size or format of what it loaded: it builds a texture twice as wide and
 * twice as high as the one it has and tells Direct3D to fill the new top level
 * from the loaded data. Every pair in the original game data fits that
 * assumption. Texture mods often break it (a replaced base texture next to the
 * game's original mip0 file), and Direct3D then reads far past the end of the
 * loaded data, which crashes the game inside the graphics driver.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_TEX_RULE_H
#define FC3PP_TEX_RULE_H

#include <stddef.h>

/* ---- pixel formats ---------------------------------------------------------- */

typedef enum tex_kind {
    TEX_KIND_UNKNOWN = 0,
    TEX_KIND_BC1, /* DXT1 */
    TEX_KIND_BC2, /* DXT2/DXT3 */
    TEX_KIND_BC3, /* DXT4/DXT5 */
    TEX_KIND_BC4, /* ATI1 */
    TEX_KIND_BC5, /* ATI2 */
    TEX_KIND_BC6,
    TEX_KIND_BC7,
    TEX_KIND_RAW  /* uncompressed; bits holds the bits per pixel */
} tex_kind;

typedef struct tex_format {
    tex_kind kind;
    unsigned bits;
} tex_format;

/* Classifies a DXGI_FORMAT value. */
tex_format tex_format_from_dxgi(unsigned dxgi_format);

/* Short name for log lines: "BC1", "32-bit", "unknown format". */
const char *tex_format_name(tex_format f, char *buf, size_t len);

/* ---- DDS images --------------------------------------------------------------- */

#define TEX_DDS_HEADER_SIZE 128u

typedef struct tex_dds {
    unsigned width, height, mips;
    tex_format format;
} tex_dds;

/* Reads the header of a DDS image held in memory. Returns 0 on success, -1
 * when the data is not a DDS image this code can describe. */
int tex_dds_parse(const void *data, size_t size, tex_dds *out);

/* ---- the rule ------------------------------------------------------------------- */

typedef enum tex_verdict {
    TEX_FITS = 0,
    TEX_UNREADABLE,  /* not a DDS image we can judge: leave it to the engine */
    TEX_TOO_SMALL,   /* fewer rows or columns than the new top level needs: read overrun */
    TEX_TOO_LARGE,   /* more than the new top level holds: wrong picture */
    TEX_WRONG_FORMAT, /* right size, different pixel format */
    TEX_STALE         /* fits, but belongs to a picture a mod has replaced (tex_scan.h) */
} tex_verdict;

#define TEX_VERDICT_COUNT 6

/* May the image in dds become the new top level of a texture that is currently
 * width x height in dxgi_format? *seen (optional) receives what the header says. */
tex_verdict tex_top_level_verdict(unsigned width, unsigned height, unsigned dxgi_format, const void *dds, size_t size,
                                  tex_dds *seen);

const char *tex_verdict_text(tex_verdict v);

/* ---- objects that were refused a level -------------------------------------------
 * The engine's resource layer counts levels on its own: after asking for a
 * level to be added it asks for one to be dropped later, whether or not the
 * add worked. An object that never got its level must therefore not act on
 * the matching drop, or it loses a level it really has - one more on every
 * load/unload cycle. The set remembers such objects. The tag (the Direct3D
 * texture the object held at the time) guards against a new object reusing
 * the address of a destroyed one. */

#define TEX_SKIP_CAPACITY 8192u /* power of two; at most three quarters are used */

typedef struct tex_skip_entry {
    const void *key;
    const void *tag;
    unsigned count;
} tex_skip_entry;

typedef struct tex_skipset {
    tex_skip_entry e[TEX_SKIP_CAPACITY];
    unsigned used;
} tex_skipset;

void tex_skip_reset(tex_skipset *s);

/* Object key was refused one level. Returns 0 when the table is full. */
int tex_skip_note(tex_skipset *s, const void *key, const void *tag);

/* Object key is asked to drop a level. Returns 1 when that is a level it was
 * refused (the request must be ignored), 0 when it is a real one. */
int tex_skip_take(tex_skipset *s, const void *key, const void *tag);

/* Object key is being destroyed. */
void tex_skip_forget(tex_skipset *s, const void *key);

/* ---- locating the texture class in an engine module ---------------------------------
 * Both methods are virtual and sit next to each other in the class's table,
 * which starts with the destructor:
 *
 *     slot 0      void *Destroy(unsigned flags)       (scalar deleting destructor)
 *     slot n      bool AddTopLevel(const void *dds, unsigned size, void *buffer, bool flag)
 *     slot n + 1  bool DropTopLevel()
 *
 * n is 9 in Far Cry 3 and 8 in Blood Dragon, whose class has one method less.
 * The Direct3D 11 texture is at offset 0x1C of the object in both. Addresses
 * are plain numbers so the search works on any mapping of the module. */

#define TEX_SLOT_DESTROY 0u
#define TEX_OFFSET_D3D_TEXTURE 0x1Cu

typedef struct tex_class_site {
    const char *variant; /* build variant that matched */
    unsigned long add;   /* address of AddTopLevel */
    unsigned long drop;  /* address of DropTopLevel */
    unsigned add_slot;   /* its slot in the class's table; DropTopLevel is the next one */
} tex_class_site;

/* Looks for both methods in a code section mapped at text_va. Returns 1 when
 * exactly one pair was found, 0 when none, -1 when the match is ambiguous. */
int tex_find_methods(const unsigned char *text, size_t size, unsigned long text_va, tex_class_site *out);

/* Counts the places in a data section mapped at data_va where a pointer to add
 * is directly followed by a pointer to drop, and stores the address of the
 * first one (the table's AddTopLevel slot) in *slot_va. */
unsigned tex_find_slots(const unsigned char *data, size_t size, unsigned long data_va, const tex_class_site *site,
                        unsigned long *slot_va);

/* Non-zero when code begins like the compiler's scalar deleting destructor
 * (call the destructor, then free the object if bit 0 of the argument is
 * set). Used to confirm that the table really starts where it is expected. */
int tex_is_destructor(const unsigned char *code, size_t size);

#endif /* FC3PP_TEX_RULE_H */
