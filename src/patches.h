/* Engine code patches located by signature.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_PATCHES_H
#define FC3PP_PATCHES_H

#include <stddef.h>

/* Writes len bytes to code memory. Returns 0 on success. */
typedef int (*patch_write_fn)(void *ctx, unsigned char *dst, const unsigned char *src, size_t len);

typedef struct patch_report {
    unsigned applied;        /* sites rewritten by this call */
    unsigned already;        /* sites that already carry this patch */
    unsigned foreign;        /* sites modified by a third-party fix that we left alone */
    unsigned rejected;       /* signature matched but validation failed */
    const char *variant;     /* name of the last variant that matched, or NULL */
    const unsigned char *site; /* address of the last matching site, or NULL */
} patch_report;

/* Fixes the legacy "count processors by walking the affinity mask" loop
 *
 *     mask = 1; i = 0;
 *     while (mask != 0 && mask <= systemAffinity) { ...; ++i; mask = 1 << i; }
 *
 * x86 masks shift counts to 5 bits, so 1 << 32 is 1 again and mask never
 * becomes 0. With 32 or more logical processors (systemAffinity == 0xFFFFFFFF)
 * the loop never terminates. The patch rewrites the loop tail so that it stops
 * once i reaches 32.
 *
 * Scans [text, text+size). With write == NULL nothing is modified and
 * report->applied counts the sites that would be patched (dry run).
 * Returns the number of sites that are fixed after the call
 * (applied + already). */
unsigned patch_enum_loop(unsigned char *text, size_t size, patch_write_fn write, void *ctx, patch_report *report);

#endif /* FC3PP_PATCHES_H */
