/* version.dll forwarders.
 *
 * Every export of the system version.dll is re-exported here and forwarded to
 * the real library, which is loaded from the system directory on first use
 * (never from DllMain, so no loader-lock recursion).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include "proxy.h"

enum {
    IDX_GetFileVersionInfoA,
    IDX_GetFileVersionInfoByHandle,
    IDX_GetFileVersionInfoExA,
    IDX_GetFileVersionInfoExW,
    IDX_GetFileVersionInfoSizeA,
    IDX_GetFileVersionInfoSizeExA,
    IDX_GetFileVersionInfoSizeExW,
    IDX_GetFileVersionInfoSizeW,
    IDX_GetFileVersionInfoW,
    IDX_VerFindFileA,
    IDX_VerFindFileW,
    IDX_VerInstallFileA,
    IDX_VerInstallFileW,
    IDX_VerLanguageNameA,
    IDX_VerLanguageNameW,
    IDX_VerQueryValueA,
    IDX_VerQueryValueW,
    IDX_COUNT
};

static const char *const g_names[IDX_COUNT] = {
    "GetFileVersionInfoA",       "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",     "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",    "GetFileVersionInfoW",
    "VerFindFileA",              "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",           "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",            "VerQueryValueW",
};

static void *g_real[IDX_COUNT];
static volatile LONG g_state; /* 0 = unresolved, 1 = resolving, 2 = done */
static HMODULE g_real_module;

/* Target of the one export whose signature is undocumented. It is forwarded
 * with a tail jump, so the argument layout never has to be known. Not static:
 * the forwarder below refers to it from assembly (cdecl symbols carry a
 * leading underscore on 32-bit x86 with both MSVC and MinGW). */
void *fc3pp_byhandle_target;

static DWORD WINAPI missing_export(void)
{
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

const char *const *proxy_export_names(unsigned *count)
{
    if (count) {
        *count = IDX_COUNT;
    }
    return g_names;
}

int proxy_is_resolved(void)
{
    return g_state == 2 && g_real_module != NULL;
}

void fc3pp_proxy_resolve(void)
{
    wchar_t path[MAX_PATH + 16];
    UINT n;
    HMODULE real = NULL;
    DWORD saved_error;
    int i;

    if (g_state == 2) {
        return;
    }
    if (InterlockedCompareExchange(&g_state, 1, 0) != 0) {
        /* Another thread is resolving; wait for it. */
        while (g_state != 2) {
            Sleep(0);
        }
        return;
    }
    saved_error = GetLastError();
    n = GetSystemDirectoryW(path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        lstrcatW(path, L"\\version.dll");
        real = LoadLibraryW(path);
    }
    for (i = 0; i < IDX_COUNT; ++i) {
        g_real[i] = real ? (void *)GetProcAddress(real, g_names[i]) : NULL;
    }
    fc3pp_byhandle_target =
        g_real[IDX_GetFileVersionInfoByHandle] ? g_real[IDX_GetFileVersionInfoByHandle] : (void *)missing_export;
    g_real_module = real;
    SetLastError(saved_error);
    InterlockedExchange(&g_state, 2);
}

void proxy_shutdown(void)
{
    /* Deliberately does not FreeLibrary: other modules may still be calling
     * through the forwarders during process teardown. */
}

#define REAL(name, type) ((type)g_real[IDX_##name])
#define MISSING(value)                         \
    do {                                       \
        SetLastError(ERROR_PROC_NOT_FOUND);    \
        return (value);                        \
    } while (0)

typedef BOOL(WINAPI *fn_GetFileVersionInfoA)(LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI *fn_GetFileVersionInfoW)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI *fn_GetFileVersionInfoExA)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
typedef BOOL(WINAPI *fn_GetFileVersionInfoExW)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
typedef DWORD(WINAPI *fn_GetFileVersionInfoSizeA)(LPCSTR, LPDWORD);
typedef DWORD(WINAPI *fn_GetFileVersionInfoSizeW)(LPCWSTR, LPDWORD);
typedef DWORD(WINAPI *fn_GetFileVersionInfoSizeExA)(DWORD, LPCSTR, LPDWORD);
typedef DWORD(WINAPI *fn_GetFileVersionInfoSizeExW)(DWORD, LPCWSTR, LPDWORD);
typedef DWORD(WINAPI *fn_VerFindFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
typedef DWORD(WINAPI *fn_VerFindFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
typedef DWORD(WINAPI *fn_VerInstallFileA)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
typedef DWORD(WINAPI *fn_VerInstallFileW)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
typedef DWORD(WINAPI *fn_VerLanguageNameA)(DWORD, LPSTR, DWORD);
typedef DWORD(WINAPI *fn_VerLanguageNameW)(DWORD, LPWSTR, DWORD);
typedef BOOL(WINAPI *fn_VerQueryValueA)(LPCVOID, LPCSTR, LPVOID *, PUINT);
typedef BOOL(WINAPI *fn_VerQueryValueW)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

BOOL WINAPI fc3pp_GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD len, LPVOID data)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_GetFileVersionInfoA]) {
        MISSING(FALSE);
    }
    return REAL(GetFileVersionInfoA, fn_GetFileVersionInfoA)(file, handle, len, data);
}

BOOL WINAPI fc3pp_GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD len, LPVOID data)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_GetFileVersionInfoW]) {
        MISSING(FALSE);
    }
    return REAL(GetFileVersionInfoW, fn_GetFileVersionInfoW)(file, handle, len, data);
}

BOOL WINAPI fc3pp_GetFileVersionInfoExA(DWORD flags, LPCSTR file, DWORD handle, DWORD len, LPVOID data)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_GetFileVersionInfoExA]) {
        MISSING(FALSE);
    }
    return REAL(GetFileVersionInfoExA, fn_GetFileVersionInfoExA)(flags, file, handle, len, data);
}

BOOL WINAPI fc3pp_GetFileVersionInfoExW(DWORD flags, LPCWSTR file, DWORD handle, DWORD len, LPVOID data)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_GetFileVersionInfoExW]) {
        MISSING(FALSE);
    }
    return REAL(GetFileVersionInfoExW, fn_GetFileVersionInfoExW)(flags, file, handle, len, data);
}

DWORD WINAPI fc3pp_GetFileVersionInfoSizeA(LPCSTR file, LPDWORD handle)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_GetFileVersionInfoSizeA]) {
        MISSING(0);
    }
    return REAL(GetFileVersionInfoSizeA, fn_GetFileVersionInfoSizeA)(file, handle);
}

DWORD WINAPI fc3pp_GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_GetFileVersionInfoSizeW]) {
        MISSING(0);
    }
    return REAL(GetFileVersionInfoSizeW, fn_GetFileVersionInfoSizeW)(file, handle);
}

DWORD WINAPI fc3pp_GetFileVersionInfoSizeExA(DWORD flags, LPCSTR file, LPDWORD handle)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_GetFileVersionInfoSizeExA]) {
        MISSING(0);
    }
    return REAL(GetFileVersionInfoSizeExA, fn_GetFileVersionInfoSizeExA)(flags, file, handle);
}

DWORD WINAPI fc3pp_GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR file, LPDWORD handle)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_GetFileVersionInfoSizeExW]) {
        MISSING(0);
    }
    return REAL(GetFileVersionInfoSizeExW, fn_GetFileVersionInfoSizeExW)(flags, file, handle);
}

DWORD WINAPI fc3pp_VerFindFileA(DWORD flags, LPCSTR file, LPCSTR win_dir, LPCSTR app_dir, LPSTR cur_dir,
                                PUINT cur_dir_len, LPSTR dest_dir, PUINT dest_dir_len)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_VerFindFileA]) {
        MISSING(0);
    }
    return REAL(VerFindFileA, fn_VerFindFileA)(flags, file, win_dir, app_dir, cur_dir, cur_dir_len, dest_dir,
                                               dest_dir_len);
}

DWORD WINAPI fc3pp_VerFindFileW(DWORD flags, LPCWSTR file, LPCWSTR win_dir, LPCWSTR app_dir, LPWSTR cur_dir,
                                PUINT cur_dir_len, LPWSTR dest_dir, PUINT dest_dir_len)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_VerFindFileW]) {
        MISSING(0);
    }
    return REAL(VerFindFileW, fn_VerFindFileW)(flags, file, win_dir, app_dir, cur_dir, cur_dir_len, dest_dir,
                                               dest_dir_len);
}

DWORD WINAPI fc3pp_VerInstallFileA(DWORD flags, LPCSTR src_file, LPCSTR dest_file, LPCSTR src_dir, LPCSTR dest_dir,
                                   LPCSTR cur_dir, LPSTR tmp_file, PUINT tmp_file_len)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_VerInstallFileA]) {
        MISSING(0);
    }
    return REAL(VerInstallFileA, fn_VerInstallFileA)(flags, src_file, dest_file, src_dir, dest_dir, cur_dir,
                                                     tmp_file, tmp_file_len);
}

DWORD WINAPI fc3pp_VerInstallFileW(DWORD flags, LPCWSTR src_file, LPCWSTR dest_file, LPCWSTR src_dir,
                                   LPCWSTR dest_dir, LPCWSTR cur_dir, LPWSTR tmp_file, PUINT tmp_file_len)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_VerInstallFileW]) {
        MISSING(0);
    }
    return REAL(VerInstallFileW, fn_VerInstallFileW)(flags, src_file, dest_file, src_dir, dest_dir, cur_dir,
                                                     tmp_file, tmp_file_len);
}

DWORD WINAPI fc3pp_VerLanguageNameA(DWORD lang, LPSTR name, DWORD size)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_VerLanguageNameA]) {
        MISSING(0);
    }
    return REAL(VerLanguageNameA, fn_VerLanguageNameA)(lang, name, size);
}

DWORD WINAPI fc3pp_VerLanguageNameW(DWORD lang, LPWSTR name, DWORD size)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_VerLanguageNameW]) {
        MISSING(0);
    }
    return REAL(VerLanguageNameW, fn_VerLanguageNameW)(lang, name, size);
}

BOOL WINAPI fc3pp_VerQueryValueA(LPCVOID block, LPCSTR sub_block, LPVOID *buffer, PUINT len)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_VerQueryValueA]) {
        MISSING(FALSE);
    }
    return REAL(VerQueryValueA, fn_VerQueryValueA)(block, sub_block, buffer, len);
}

BOOL WINAPI fc3pp_VerQueryValueW(LPCVOID block, LPCWSTR sub_block, LPVOID *buffer, PUINT len)
{
    fc3pp_proxy_resolve();
    if (!g_real[IDX_VerQueryValueW]) {
        MISSING(FALSE);
    }
    return REAL(VerQueryValueW, fn_VerQueryValueW)(block, sub_block, buffer, len);
}

/* GetFileVersionInfoByHandle: undocumented signature, forwarded by tail jump.
 * The resolver only touches caller-saved registers, so the caller's stack
 * arguments reach the real function untouched. */
#if defined(_MSC_VER)
__declspec(naked) void fc3pp_GetFileVersionInfoByHandle(void)
{
    __asm {
        call fc3pp_proxy_resolve
        jmp dword ptr [fc3pp_byhandle_target]
    }
}
#else
__attribute__((naked)) void fc3pp_GetFileVersionInfoByHandle(void)
{
    __asm__("call _fc3pp_proxy_resolve\n\t"
            "jmp *_fc3pp_byhandle_target");
}
#endif
