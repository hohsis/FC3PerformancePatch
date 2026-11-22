/* Builds a minimal, structurally valid PE32 (x86) file image for tests.
 * The image is headers plus one empty section; it is never executed.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_DUMMY_PE_H
#define FC3PP_DUMMY_PE_H

#include <stddef.h>

#define DUMMY_PE_SIZE 0x400

/* characteristics: COFF Characteristics field (0x0102 = executable, 32-bit).
 * with_checksum: store a valid checksum in the optional header. */
void dummy_pe_build(unsigned char *buf, unsigned characteristics, int with_checksum);
unsigned dummy_pe_characteristics(const unsigned char *buf);

#endif /* FC3PP_DUMMY_PE_H */
