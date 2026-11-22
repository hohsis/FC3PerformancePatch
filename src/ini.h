/* Minimal INI reader. Pure C, no Win32 dependencies.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_INI_H
#define FC3PP_INI_H

#include <stddef.h>

typedef struct ini_entry {
    const char *section; /* "" for keys that appear before any [section] */
    const char *key;
    const char *value;
} ini_entry;

typedef struct ini_doc {
    ini_entry *entries;
    size_t count;
    size_t capacity;
    char *storage;      /* private copy of the text, edited in place */
    size_t malformed;   /* number of lines that could not be understood */
} ini_doc;

/* Parses len bytes of text (need not be NUL terminated). Returns 0 on success,
 * -1 on allocation failure. The document must be released with ini_free(). */
int ini_parse(ini_doc *doc, const char *text, size_t len);
void ini_free(ini_doc *doc);

/* Section and key lookups are ASCII case-insensitive; the last duplicate wins.
 * Returns NULL when the key is absent. */
const char *ini_get(const ini_doc *doc, const char *section, const char *key);

/* Typed getters return def when the key is absent or not parseable. */
long ini_get_int(const ini_doc *doc, const char *section, const char *key, long def);
unsigned long ini_get_uint(const ini_doc *doc, const char *section, const char *key, unsigned long def);
int ini_get_bool(const ini_doc *doc, const char *section, const char *key, int def);

/* Clamped integer getter: values outside [lo, hi] are clamped. */
long ini_get_int_clamped(const ini_doc *doc, const char *section, const char *key, long def, long lo, long hi);

/* ASCII case-insensitive string equality. */
int ini_streq_nocase(const char *a, const char *b);

#endif /* FC3PP_INI_H */
