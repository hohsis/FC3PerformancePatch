/* PE (Portable Executable) file-buffer helpers: Large Address Aware flag and
 * checksum. Pure C, operates on an in-memory copy of the file.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_PE_FILE_H
#define FC3PP_PE_FILE_H

#include <stddef.h>

typedef enum pef_status {
    PEF_OK = 0,
    PEF_NOT_PE,     /* missing MZ / PE signatures */
    PEF_TRUNCATED,  /* headers run past the end of the buffer */
    PEF_NOT_X86     /* not a 32-bit x86 (PE32, machine 0x14C) image */
} pef_status;

#define PEF_LARGE_ADDRESS_AWARE 0x0020u

/* Reads the IMAGE_FILE_LARGE_ADDRESS_AWARE flag. */
pef_status pef_get_laa(const unsigned char *buf, size_t len, int *laa);

/* Sets or clears the flag. *changed (optional) is set to 1 when the buffer was
 * modified. When the header carried a non-zero checksum it is recomputed so
 * the file stays self-consistent; a zero checksum is left at zero. */
pef_status pef_set_laa(unsigned char *buf, size_t len, int enable, int *changed);

/* Reads the checksum stored in the optional header. */
pef_status pef_stored_checksum(const unsigned char *buf, size_t len, unsigned long *checksum);

/* Computes the standard PE checksum of the buffer (the stored checksum field
 * is treated as zero). */
pef_status pef_compute_checksum(const unsigned char *buf, size_t len, unsigned long *checksum);

const char *pef_status_text(pef_status s);

#endif /* FC3PP_PE_FILE_H */
