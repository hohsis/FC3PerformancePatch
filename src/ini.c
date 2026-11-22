/* Minimal INI reader. Pure C, no Win32 dependencies.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ini.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int ini_streq_nocase(const char *a, const char *b)
{
    if (!a || !b) {
        return a == b;
    }
    while (*a && *b) {
        if (lower(*a) != lower(*b)) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

/* Trims in place; returns the new start. */
static char *trim(char *s)
{
    char *end;
    while (*s && is_space(*s)) {
        ++s;
    }
    end = s + strlen(s);
    while (end > s && is_space(end[-1])) {
        --end;
    }
    *end = '\0';
    return s;
}

static int push(ini_doc *doc, const char *section, const char *key, const char *value)
{
    if (doc->count == doc->capacity) {
        size_t cap = doc->capacity ? doc->capacity * 2 : 32;
        ini_entry *e = (ini_entry *)realloc(doc->entries, cap * sizeof(*e));
        if (!e) {
            return -1;
        }
        doc->entries = e;
        doc->capacity = cap;
    }
    doc->entries[doc->count].section = section;
    doc->entries[doc->count].key = key;
    doc->entries[doc->count].value = value;
    doc->count++;
    return 0;
}

/* Removes an unquoted trailing comment (" ;..." or " #...") and surrounding
 * quotes. A comment marker only counts when preceded by whitespace, so values
 * such as "a;b" or "#ff00ff" survive. */
static char *clean_value(char *v)
{
    size_t n;
    v = trim(v);
    n = strlen(v);
    if (n >= 2 && (v[0] == '"' || v[0] == '\'')) {
        char q = v[0];
        char *close = strchr(v + 1, q);
        if (close) {
            *close = '\0';
            return v + 1;
        }
    }
    {
        char *p;
        for (p = v; *p; ++p) {
            if ((*p == ';' || *p == '#') && p > v && is_space(p[-1])) {
                *p = '\0';
                break;
            }
        }
    }
    return trim(v);
}

int ini_parse(ini_doc *doc, const char *text, size_t len)
{
    char *line;
    char *end;
    const char *section = "";

    memset(doc, 0, sizeof(*doc));
    if (!text) {
        len = 0;
    }
    /* Skip a UTF-8 byte order mark. */
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text += 3;
        len -= 3;
    }
    doc->storage = (char *)malloc(len + 1);
    if (!doc->storage) {
        return -1;
    }
    if (len) {
        memcpy(doc->storage, text, len);
    }
    doc->storage[len] = '\0';
    /* Embedded NULs would truncate the document; turn them into spaces. */
    {
        size_t i;
        for (i = 0; i < len; ++i) {
            if (doc->storage[i] == '\0') {
                doc->storage[i] = ' ';
            }
        }
    }

    line = doc->storage;
    end = doc->storage + len;
    while (line < end) {
        char *nl = (char *)memchr(line, '\n', (size_t)(end - line));
        char *next = nl ? nl + 1 : end;
        char *s;
        if (nl) {
            *nl = '\0';
        }
        s = trim(line);
        if (*s == '\0' || *s == ';' || *s == '#') {
            /* blank or comment */
        } else if (*s == '[') {
            char *close = strchr(s, ']');
            if (close) {
                *close = '\0';
                section = trim(s + 1);
            } else {
                doc->malformed++;
            }
        } else {
            char *eq = strchr(s, '=');
            if (eq && eq != s) {
                char *key;
                char *value;
                *eq = '\0';
                key = trim(s);
                value = clean_value(eq + 1);
                if (*key == '\0') {
                    doc->malformed++;
                } else if (push(doc, section, key, value) != 0) {
                    ini_free(doc);
                    return -1;
                }
            } else {
                doc->malformed++;
            }
        }
        line = next;
    }
    return 0;
}

void ini_free(ini_doc *doc)
{
    if (!doc) {
        return;
    }
    free(doc->entries);
    free(doc->storage);
    memset(doc, 0, sizeof(*doc));
}

const char *ini_get(const ini_doc *doc, const char *section, const char *key)
{
    size_t i;
    if (!doc || !key) {
        return NULL;
    }
    if (!section) {
        section = "";
    }
    for (i = doc->count; i > 0; --i) {
        const ini_entry *e = &doc->entries[i - 1];
        if (ini_streq_nocase(e->section, section) && ini_streq_nocase(e->key, key)) {
            return e->value;
        }
    }
    return NULL;
}

static int parse_long(const char *s, long *out)
{
    char *endp = NULL;
    long v;
    if (!s || !*s) {
        return 0;
    }
    errno = 0;
    v = strtol(s, &endp, 0);
    if (errno == ERANGE || endp == s || *endp != '\0') {
        return 0;
    }
    *out = v;
    return 1;
}

long ini_get_int(const ini_doc *doc, const char *section, const char *key, long def)
{
    long v;
    return parse_long(ini_get(doc, section, key), &v) ? v : def;
}

long ini_get_int_clamped(const ini_doc *doc, const char *section, const char *key, long def, long lo, long hi)
{
    long v = ini_get_int(doc, section, key, def);
    if (v < lo) {
        v = lo;
    }
    if (v > hi) {
        v = hi;
    }
    return v;
}

unsigned long ini_get_uint(const ini_doc *doc, const char *section, const char *key, unsigned long def)
{
    const char *s = ini_get(doc, section, key);
    char *endp = NULL;
    unsigned long v;
    if (!s || !*s || *s == '-') {
        return def;
    }
    errno = 0;
    v = strtoul(s, &endp, 0);
    if (errno == ERANGE || endp == s || *endp != '\0') {
        return def;
    }
    return v;
}

int ini_get_bool(const ini_doc *doc, const char *section, const char *key, int def)
{
    const char *s = ini_get(doc, section, key);
    long v;
    if (!s || !*s) {
        return def;
    }
    if (ini_streq_nocase(s, "true") || ini_streq_nocase(s, "yes") || ini_streq_nocase(s, "on")) {
        return 1;
    }
    if (ini_streq_nocase(s, "false") || ini_streq_nocase(s, "no") || ini_streq_nocase(s, "off")) {
        return 0;
    }
    if (parse_long(s, &v)) {
        return v != 0;
    }
    return def;
}
