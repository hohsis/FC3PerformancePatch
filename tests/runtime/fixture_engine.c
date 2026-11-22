/* Test fixture standing in for the game's engine module.
 *
 * It is built under the engine's file names (FC3_d3d11.dll, FC3.dll) so the
 * proxy treats it exactly like the real thing: it statically imports
 * VERSION.dll, calls the same Win32 processor APIs through its import table,
 * resolves GetLogicalProcessorInformation dynamically like the Havok thread
 * pool does, loads "systemdetection.dll" at run time, and carries a replica
 * of the buggy processor-enumeration loop in an executable section.
 *
 * It contains no game code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

#include "../unit/loop_blobs.h"

#define FX_API __declspec(dllexport)

typedef unsigned(__stdcall *probe_fn)(unsigned mask);
typedef unsigned(__cdecl *loop_fn)(unsigned system_mask, probe_fn probe);

#ifndef FX_NO_LOOP
#if defined(_MSC_VER)
#pragma section(".fxcode", execute, read)
__declspec(allocate(".fxcode")) static const unsigned char fx_loop_code[] = {LOOP_BLOB_RELEASE};
#else
/* The trailing '#' turns the compiler's own section flags into a comment, so
 * the section really is "executable, read-only". */
static const unsigned char fx_loop_code[] __attribute__((section(".fxcode,\"xr\"#"), used)) = {LOOP_BLOB_RELEASE};
#endif
#endif

static volatile LONG g_probe_calls;
static volatile LONG g_abort;

static unsigned __stdcall fx_probe(unsigned mask)
{
    (void)mask;
    InterlockedIncrement(&g_probe_calls);
    if (g_abort) {
        ExitThread(2); /* lets a test end a loop that would otherwise spin forever */
    }
    return 1;
}

FX_API long fx_loop_calls(void)
{
    return (long)g_probe_calls;
}

FX_API void fx_abort_loop(int abort)
{
    InterlockedExchange(&g_abort, abort);
}

/* Runs the enumeration loop for an explicit system affinity mask. */
FX_API unsigned fx_enum_loop(unsigned long system_mask)
{
#ifndef FX_NO_LOOP
    g_probe_calls = 0;
    return ((loop_fn)(void *)fx_loop_code)((unsigned)system_mask, fx_probe);
#else
    (void)system_mask;
    (void)fx_probe;
    return 0xFFFFFFFEu;
#endif
}

/* Runs it the way the engine does: with whatever GetProcessAffinityMask says. */
FX_API unsigned fx_enum_loop_auto(void)
{
    DWORD_PTR process_mask = 0, system_mask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
        return 0xFFFFFFFFu;
    }
    return fx_enum_loop((unsigned long)system_mask);
}

FX_API unsigned fx_nproc(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}

FX_API int fx_affinity(unsigned long *process_mask, unsigned long *system_mask)
{
    DWORD_PTR pm = 0, sm = 0;
    BOOL ok = GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm);
    *process_mask = (unsigned long)pm;
    *system_mask = (unsigned long)sm;
    return ok ? 1 : 0;
}

FX_API unsigned long fx_set_thread_affinity(unsigned long mask)
{
    return (unsigned long)SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)mask);
}

FX_API int fx_set_process_affinity(unsigned long mask)
{
    return SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)mask) ? 1 : 0;
}

FX_API unsigned long fx_set_ideal(unsigned long index)
{
    return SetThreadIdealProcessor(GetCurrentThread(), index);
}

/* Counts physical cores the way the engine's physics thread pool does. */
FX_API unsigned fx_glpi_cores(void)
{
    typedef BOOL(WINAPI * glpi_fn)(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION, PDWORD);
    static SYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer[128];
    HMODULE kernel = LoadLibraryA("kernel32.dll");
    glpi_fn glpi;
    DWORD length = 0;
    unsigned cores = 0;
    DWORD i;

    if (!kernel) {
        return 0xFFFFFFFFu;
    }
    glpi = (glpi_fn)(void *)GetProcAddress(kernel, "GetLogicalProcessorInformation");
    if (!glpi) {
        FreeLibrary(kernel);
        return 0xFFFFFFFFu;
    }
    glpi(NULL, &length);
    if (length == 0 || length > sizeof(buffer) || !glpi(buffer, &length)) {
        FreeLibrary(kernel);
        return 0xFFFFFFFFu;
    }
    for (i = 0; i < length / sizeof(buffer[0]); ++i) {
        if (buffer[i].Relationship == RelationProcessorCore) {
            ++cores;
        }
    }
    FreeLibrary(kernel);
    return cores;
}

/* The engine imports three functions from VERSION.dll; so does the fixture. */
FX_API unsigned long fx_version_size(void)
{
    DWORD handle = 0;
    return GetFileVersionInfoSizeA("kernel32.dll", &handle);
}

FX_API void *fx_load_sysdet(void)
{
    return (void *)LoadLibraryA("systemdetection.dll");
}

FX_API void fx_sleep(unsigned ms, unsigned count)
{
    while (count--) {
        Sleep(ms);
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
