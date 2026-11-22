/* Byte-pattern (signature) scanner with wildcards. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_SIGSCAN_H
#define FC3PP_SIGSCAN_H

#include <stddef.h>

#define SIG_MAX_LEN 64

typedef struct sig_pattern {
    unsigned char bytes[SIG_MAX_LEN];
    unsigned char mask[SIG_MAX_LEN]; /* 0xFF = must match, 0x00 = wildcard */
    size_t len;
} sig_pattern;

/* Parses "41 BE ?? 0F" style text. Tokens are two hex digits or "??"/"?".
 * Returns 0 on success, -1 on syntax error, empty pattern, an all-wildcard
 * pattern or one longer than SIG_MAX_LEN. */
int sig_parse(sig_pattern *p, const char *text);

/* Returns the first match in [start, start+size) or NULL. */
const unsigned char *sig_find(const unsigned char *start, size_t size, const sig_pattern *p);

/* Stores up to max match addresses in out (may be NULL) and returns the total
 * number of matches, including those that did not fit. Matches may overlap. */
size_t sig_find_all(const unsigned char *start, size_t size, const sig_pattern *p,
                    const unsigned char **out, size_t max);

#endif /* FC3PP_SIGSCAN_H */
