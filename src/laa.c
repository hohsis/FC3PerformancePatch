/* Large Address Aware: on-disk patching of the host executable.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "laa.h"

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "pe_file.h"

#define MAX_EXE_SIZE (64u * 1024u * 1024u)

int laa_process_is_aware(void)
{
    const unsigned char *base = (const unsigned char *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *nt;
    if (!base || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

static unsigned char *read_file(const wchar_t *path, DWORD *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    unsigned char *buf = NULL;
    DWORD len, got = 0;

    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    len = GetFileSize(h, NULL);
    if (len != INVALID_FILE_SIZE && len > 0 && len <= MAX_EXE_SIZE) {
        buf = (unsigned char *)malloc(len);
        if (buf && (!ReadFile(h, buf, len, &got, NULL) || got != len)) {
            free(buf);
            buf = NULL;
        }
    }
    CloseHandle(h);
    if (buf) {
        *size = len;
    }
    return buf;
}

static int write_file(const wchar_t *path, const unsigned char *buf, DWORD size)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    ok = WriteFile(h, buf, size, &put, NULL) && put == size;
    ok = FlushFileBuffers(h) && ok;
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(path);
        return -1;
    }
    return 0;
}

laa_result laa_apply_to_file(const wchar_t *exe_path)
{
    wchar_t tmp[MAX_PATH + 64];
    wchar_t bak[MAX_PATH + 64];
    SYSTEMTIME st;
    DWORD size = 0;
    unsigned char *buf;
    int laa = 0;
    int changed = 0;
    laa_result result = LAA_IO_ERROR;

    if (!exe_path || wcslen(exe_path) >= MAX_PATH) {
        return LAA_IO_ERROR;
    }
    buf = read_file(exe_path, &size);
    if (!buf) {
        return LAA_IO_ERROR;
    }
    if (pef_get_laa(buf, size, &laa) != PEF_OK) {
        free(buf);
        return LAA_NOT_PE;
    }
    if (laa) {
        free(buf);
        return LAA_ALREADY;
    }
    if (pef_set_laa(buf, size, 1, &changed) != PEF_OK || !changed) {
        free(buf);
        return LAA_NOT_PE;
    }

    GetSystemTime(&st);
    _snwprintf(tmp, sizeof(tmp) / sizeof(tmp[0]), L"%ls.fc3pp-new", exe_path);
    _snwprintf(bak, sizeof(bak) / sizeof(bak[0]), L"%ls" LAA_BACKUP_INFIX L"%04u%02u%02u-%02u%02u%02u" LAA_BACKUP_SUFFIX,
               exe_path, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    tmp[sizeof(tmp) / sizeof(tmp[0]) - 1] = L'\0';
    bak[sizeof(bak) / sizeof(bak[0]) - 1] = L'\0';

    if (write_file(tmp, buf, size) == 0) {
        /* A running image cannot be overwritten, but it can be renamed. */
        if (MoveFileExW(exe_path, bak, 0)) {
            if (MoveFileExW(tmp, exe_path, 0)) {
                result = LAA_PATCHED;
            } else {
                MoveFileExW(bak, exe_path, 0); /* roll back */
                DeleteFileW(tmp);
            }
        } else {
            DeleteFileW(tmp);
        }
    }
    free(buf);
    return result;
}

const char *laa_result_text(laa_result r)
{
    switch (r) {
    case LAA_ALREADY:
        return "already large-address-aware";
    case LAA_PATCHED:
        return "patched (takes effect on the next launch)";
    case LAA_NOT_PE:
        return "not a 32-bit x86 executable";
    case LAA_IO_ERROR:
        return "could not read or replace the file";
    case LAA_NO_MEMORY:
        return "out of memory";
    default:
        return "?";
    }
}
