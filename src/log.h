/* Diagnostic log file with verbosity levels.
 *
 * Privacy: the log never contains absolute paths, user names or machine
 * names. Callers pass file *names* only; use log_basename() when in doubt.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef FC3PP_LOG_H
#define FC3PP_LOG_H

#include <wchar.h>

#define LOG_OFF 0
#define LOG_ERROR 1
#define LOG_INFO 2
#define LOG_DEBUG 3

/* Opens (truncates) the log. level LOG_OFF creates no file. */
void log_init(const wchar_t *path, int level);
void log_shutdown(void);
int log_level(void);

/* Call when the process is terminating: later writes skip the lock, because
 * a thread that was killed while holding it would deadlock the exit path. */
void log_set_terminating(void);

#if defined(__GNUC__)
void log_printf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#else
void log_printf(int level, const char *fmt, ...);
#endif

#define LOGE(...) log_printf(LOG_ERROR, __VA_ARGS__)
#define LOGI(...) log_printf(LOG_INFO, __VA_ARGS__)
#define LOGD(...) log_printf(LOG_DEBUG, __VA_ARGS__)

/* Returns the file-name part of a path (after the last slash or backslash). */
const char *log_basename(const char *path);
const wchar_t *log_basename_w(const wchar_t *path);

#endif /* FC3PP_LOG_H */
