/* Large Address Aware: on-disk patching of the host executable.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_LAA_H
#define FC3PP_LAA_H

#include <wchar.h>

typedef enum laa_result {
    LAA_ALREADY = 0,   /* flag already present, nothing to do */
    LAA_PATCHED,       /* file replaced; takes effect on the next launch */
    LAA_NOT_PE,        /* the file is not a 32-bit x86 executable */
    LAA_IO_ERROR,      /* read/write/rename failed (read-only folder?) */
    LAA_NO_MEMORY
} laa_result;

/* The original executable is kept as
 *     <file>.fc3pp-YYYYMMDD-HHMMSS.bak        (UTC)
 * Renaming it back undoes the change. */
#define LAA_BACKUP_INFIX L".fc3pp-"
#define LAA_BACKUP_SUFFIX L".bak"

/* Sets IMAGE_FILE_LARGE_ADDRESS_AWARE in the given executable. The original
 * is kept as a timestamped backup next to it. Works while the executable is
 * running (the running image is renamed, never overwritten). */
laa_result laa_apply_to_file(const wchar_t *exe_path);

/* Non-zero when the running process was started large-address-aware. */
int laa_process_is_aware(void);

const char *laa_result_text(laa_result r);

#endif /* FC3PP_LAA_H */
