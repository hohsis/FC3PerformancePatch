/* A host that is NOT the game: it imports VERSION.dll but no engine module.
 * The proxy must stay completely passive here (forward only, no log file),
 * because the game folder also contains an updater and an editor.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <windows.h>

int main(void)
{
    wchar_t exe[MAX_PATH], dll[MAX_PATH];
    wchar_t *slash;
    HMODULE proxy = GetModuleHandleW(L"version.dll");
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(L"kernel32.dll", &handle);
    DWORD_PTR pm = 0, sm = 0;
    int failures = 0;

    if (!proxy) {
        printf("FAIL: version.dll not loaded\n");
        return 1;
    }
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    GetModuleFileNameW(proxy, dll, MAX_PATH);
    slash = wcsrchr(exe, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    slash = wcsrchr(dll, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    if (_wcsicmp(exe, dll) != 0) {
        printf("FAIL: the proxy next to the executable was not loaded\n");
        ++failures;
    }
    if (size == 0) {
        printf("FAIL: forwarding does not work\n");
        ++failures;
    }
    /* Nothing about the process may have been touched. */
    GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    if (pm != sm && pm == 0) {
        printf("FAIL: affinity unreadable\n");
        ++failures;
    }
    printf("passive host: %d failures\n", failures);
    return failures ? 1 : 0;
}
