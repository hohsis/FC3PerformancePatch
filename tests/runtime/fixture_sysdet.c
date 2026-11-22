/* Test fixture standing in for the game's systemdetection.dll: a module that
 * is loaded at run time (LoadLibrary) and carries the unoptimised form of the
 * processor-enumeration loop. Contains no game code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include "../unit/loop_blobs.h"

#define SD_API __declspec(dllexport)

typedef unsigned(__stdcall *probe_fn)(unsigned mask);
typedef unsigned(__cdecl *loop_fn)(unsigned system_mask, probe_fn probe);

#if defined(_MSC_VER)
#pragma section(".fxcode", execute, read)
__declspec(allocate(".fxcode")) static const unsigned char sd_loop_code[] = {LOOP_BLOB_DEBUG};
#else
static const unsigned char sd_loop_code[] __attribute__((section(".fxcode,\"xr\"#"), used)) = {LOOP_BLOB_DEBUG};
#endif

static volatile LONG g_probe_calls;
static volatile LONG g_abort;

static unsigned __stdcall sd_probe(unsigned mask)
{
    (void)mask;
    InterlockedIncrement(&g_probe_calls);
    if (g_abort) {
        ExitThread(2); /* lets a test end a loop that would otherwise spin forever */
    }
    return 1;
}

SD_API long sd_loop_calls(void)
{
    return (long)g_probe_calls;
}

SD_API void sd_abort_loop(int abort)
{
    InterlockedExchange(&g_abort, abort);
}

SD_API unsigned sd_enum_loop(unsigned long system_mask)
{
    g_probe_calls = 0;
    return ((loop_fn)(void *)sd_loop_code)((unsigned)system_mask, sd_probe);
}

SD_API int sd_affinity(unsigned long *process_mask, unsigned long *system_mask)
{
    DWORD_PTR pm = 0, sm = 0;
    BOOL ok = GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    *process_mask = (unsigned long)pm;
    *system_mask = (unsigned long)sm;
    return ok ? 1 : 0;
}

SD_API unsigned sd_nproc(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
