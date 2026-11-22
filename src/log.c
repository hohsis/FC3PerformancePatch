/* Diagnostic log file with verbosity levels.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

static HANDLE g_file = INVALID_HANDLE_VALUE;
static int g_level = LOG_OFF;
static DWORD g_start;
static CRITICAL_SECTION g_lock;
static int g_lock_ready;
static int g_terminating;

/* Keeps the log of the previous start as "<name>.previous.log": after a crash
 * the natural thing to do is to start the game again, and that must not
 * destroy the evidence. Fails harmlessly when there is no log yet. */
static void keep_previous(const wchar_t *path)
{
    static const wchar_t suffix[] = L".previous.log";
    wchar_t previous[MAX_PATH + 64];
    size_t len = wcslen(path);

    if (len < 4 || len + sizeof(suffix) / sizeof(suffix[0]) > sizeof(previous) / sizeof(previous[0]) ||
        _wcsicmp(path + len - 4, L".log") != 0) {
        return;
    }
    memcpy(previous, path, (len - 4) * sizeof(wchar_t));
    memcpy(previous + len - 4, suffix, sizeof(suffix));
    MoveFileExW(path, previous, MOVEFILE_REPLACE_EXISTING);
}

void log_init(const wchar_t *path, int level)
{
    if (level < LOG_OFF) {
        level = LOG_OFF;
    }
    if (level > LOG_DEBUG) {
        level = LOG_DEBUG;
    }
    g_level = level;
    g_start = GetTickCount();
    if (!g_lock_ready) {
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
    }
    if (level == LOG_OFF || !path) {
        return;
    }
    keep_previous(path);
    g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, NULL);
    /* A read-only game folder is not an error: logging is simply unavailable. */
}

void log_shutdown(void)
{
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    g_level = LOG_OFF;
}

int log_level(void)
{
    return g_level;
}

void log_printf(int level, const char *fmt, ...)
{
    static const char *const names[] = {"-----", "ERROR", "INFO ", "DEBUG"};
    char line[1024];
    int prefix;
    int body;
    DWORD elapsed;
    DWORD written = 0;
    DWORD saved_error;
    va_list ap;

    if (level > g_level || level <= LOG_OFF || g_file == INVALID_HANDLE_VALUE) {
        return;
    }
    saved_error = GetLastError();
    elapsed = GetTickCount() - g_start;
    prefix = snprintf(line, sizeof(line), "[%6lu.%03lu] [%s] ", (unsigned long)(elapsed / 1000),
                      (unsigned long)(elapsed % 1000), names[level]);
    if (prefix < 0) {
        prefix = 0;
    }
    va_start(ap, fmt);
    body = vsnprintf(line + prefix, sizeof(line) - (size_t)prefix - 3, fmt, ap);
    va_end(ap);
    if (body < 0) {
        body = 0;
    }
    if ((size_t)body > sizeof(line) - (size_t)prefix - 3) {
        body = (int)(sizeof(line) - (size_t)prefix - 3);
    }
    line[prefix + body] = '\r';
    line[prefix + body + 1] = '\n';

    if (g_terminating) {
        WriteFile(g_file, line, (DWORD)(prefix + body + 2), &written, NULL);
    } else {
        EnterCriticalSection(&g_lock);
        WriteFile(g_file, line, (DWORD)(prefix + body + 2), &written, NULL);
        LeaveCriticalSection(&g_lock);
    }
    SetLastError(saved_error);
}

void log_set_terminating(void)
{
    g_terminating = 1;
}

const char *log_basename(const char *path)
{
    const char *base = path;
    const char *p;
    if (!path) {
        return "";
    }
    for (p = path; *p; ++p) {
        if (*p == '\\' || *p == '/') {
            base = p + 1;
        }
    }
    return base;
}

const wchar_t *log_basename_w(const wchar_t *path)
{
    const wchar_t *base = path;
    const wchar_t *p;
    if (!path) {
        return L"";
    }
    for (p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') {
            base = p + 1;
        }
    }
    return base;
}
