/* Helpers for PE images mapped into the current process: section enumeration,
 * import-table lookup/hooking and protected code writes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_PE_IMAGE_H
#define FC3PP_PE_IMAGE_H

#include <windows.h>

typedef struct pei_section {
    unsigned char *start;
    size_t size;
    DWORD characteristics;
    char name[9];
} pei_section;

/* Non-zero when mod points at a mapped 32-bit PE image. */
int pei_valid(HMODULE mod);

/* Fills out with up to max sections and returns the total section count. */
unsigned pei_sections(HMODULE mod, pei_section *out, unsigned max);

/* Returns the address of the import address table slot for dll!func, or NULL
 * when the module does not import it by name. */
void **pei_find_import(HMODULE mod, const char *dll, const char *func);

/* Replaces an import. *previous (optional) receives the old slot value.
 * Returns 1 when hooked, 0 when the import does not exist, -1 on failure. */
int pei_hook_import(HMODULE mod, const char *dll, const char *func, void *replacement, void **previous);

/* Copies len bytes into (possibly read-only or executable) memory.
 * Returns 0 on success. */
int mem_patch(void *dst, const void *src, size_t len);

/* patch_write_fn compatible wrapper around mem_patch. */
int mem_patch_writer(void *ctx, unsigned char *dst, const unsigned char *src, size_t len);

#endif /* FC3PP_PE_IMAGE_H */
