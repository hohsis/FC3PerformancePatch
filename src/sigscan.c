/* Byte-pattern (signature) scanner with wildcards. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sigscan.h"

#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int sig_parse(sig_pattern *p, const char *text)
{
    size_t n = 0;
    int concrete = 0;

    if (!p || !text) {
        return -1;
    }
    memset(p, 0, sizeof(*p));
    while (*text) {
        if (*text == ' ' || *text == '\t') {
            ++text;
            continue;
        }
        if (n >= SIG_MAX_LEN) {
            return -1;
        }
        if (*text == '?') {
            ++text;
            if (*text == '?') {
                ++text;
            }
            p->bytes[n] = 0;
            p->mask[n] = 0x00;
        } else {
            int hi = hexval(text[0]);
            int lo = hi >= 0 ? hexval(text[1]) : -1;
            if (hi < 0 || lo < 0) {
                return -1;
            }
            p->bytes[n] = (unsigned char)((hi << 4) | lo);
            p->mask[n] = 0xFF;
            concrete = 1;
            text += 2;
        }
        /* A token must be followed by a separator or the end of the string. */
        if (*text && *text != ' ' && *text != '\t') {
            return -1;
        }
        ++n;
    }
    if (n == 0 || !concrete) {
        return -1;
    }
    p->len = n;
    return 0;
}

static int match_at(const unsigned char *at, const sig_pattern *p)
{
    size_t i;
    for (i = 0; i < p->len; ++i) {
        if (p->mask[i] && at[i] != p->bytes[i]) {
            return 0;
        }
    }
    return 1;
}

/* Finds the next match at or after cur. The first concrete byte is used as an
 * anchor so memchr can skip most of the buffer. */
static const unsigned char *find_from(const unsigned char *start, size_t size, const unsigned char *cur,
                                      const sig_pattern *p)
{
    size_t anchor = 0;
    const unsigned char *last;

    if (!start || !p || p->len == 0 || size < p->len) {
        return NULL;
    }
    while (anchor < p->len && !p->mask[anchor]) {
        ++anchor;
    }
    if (anchor == p->len) {
        return NULL;
    }
    last = start + (size - p->len); /* last valid match start */
    while (cur <= last) {
        const unsigned char *hit =
            (const unsigned char *)memchr(cur + anchor, p->bytes[anchor], (size_t)(last - cur) + 1);
        if (!hit) {
            return NULL;
        }
        cur = hit - anchor;
        if (match_at(cur, p)) {
            return cur;
        }
        ++cur;
    }
    return NULL;
}

const unsigned char *sig_find(const unsigned char *start, size_t size, const sig_pattern *p)
{
    return find_from(start, size, start, p);
}

size_t sig_find_all(const unsigned char *start, size_t size, const sig_pattern *p, const unsigned char **out,
                    size_t max)
{
    size_t count = 0;
    const unsigned char *cur = start;
    for (;;) {
        const unsigned char *hit = find_from(start, size, cur, p);
        if (!hit) {
            break;
        }
        if (out && count < max) {
            out[count] = hit;
        }
        ++count;
        cur = hit + 1;
    }
    return count;
}
