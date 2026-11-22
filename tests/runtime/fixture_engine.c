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
#include <string.h>
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

/* ---- DXGI, used the way the DX11 renderer uses it ---------------------------------
 * The factory comes from an imported CreateDXGIFactory1; everything else goes
 * through the objects' virtual tables by slot number. The DX9-named fixture
 * has none of this, like the real DX9 engine module. */
#ifndef FX_NO_DXGI
__declspec(dllimport) HRESULT WINAPI CreateDXGIFactory1(const GUID *iid, void **factory);

typedef struct fx_desc {
    UINT width, height, refresh_num, refresh_den, format, scanline, scaling;
    UINT sample_count, sample_quality;
    UINT buffer_usage, buffer_count;
    HWND output_window;
    BOOL windowed;
    UINT swap_effect, flags;
} fx_desc;

typedef HRESULT(WINAPI *fx_create_chain_fn)(void *self, void *device, fx_desc *desc, void **chain);
typedef HRESULT(WINAPI *fx_set_fullscreen_fn)(void *self, BOOL fullscreen, void *target);
typedef HRESULT(WINAPI *fx_get_fullscreen_fn)(void *self, BOOL *fullscreen, void **target);

static void *g_fx_factory;
static void *g_fx_chain;

static void **fx_vtbl(void *object)
{
    return *(void ***)object;
}
#endif

/* Creates the factory (once) and a swap chain for the given window. */
FX_API int fx_dxgi_open(void *window)
{
#ifndef FX_NO_DXGI
    static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
    fx_desc desc;

    if (!g_fx_factory && FAILED(CreateDXGIFactory1(&iid_factory1, &g_fx_factory))) {
        return -1;
    }
    memset(&desc, 0, sizeof(desc));
    desc.width = 1280;
    desc.height = 720;
    desc.buffer_count = 1;
    desc.output_window = (HWND)window;
    desc.windowed = TRUE;
    g_fx_chain = NULL;
    if (FAILED(((fx_create_chain_fn)fx_vtbl(g_fx_factory)[10])(g_fx_factory, NULL, &desc, &g_fx_chain)) ||
        !g_fx_chain) {
        return -2;
    }
    return 0;
#else
    (void)window;
    return -100;
#endif
}

/* The renderer's once-per-frame question: 1 fullscreen, 0 windowed, <0 error. */
FX_API int fx_dxgi_fullscreen(void)
{
#ifndef FX_NO_DXGI
    BOOL fullscreen = FALSE;
    void *target = NULL;

    if (!g_fx_chain ||
        FAILED(((fx_get_fullscreen_fn)fx_vtbl(g_fx_chain)[11])(g_fx_chain, &fullscreen, &target))) {
        return -1;
    }
    return fullscreen ? 1 : 0;
#else
    return -100;
#endif
}

FX_API int fx_dxgi_set_fullscreen(int fullscreen)
{
#ifndef FX_NO_DXGI
    if (!g_fx_chain ||
        FAILED(((fx_set_fullscreen_fn)fx_vtbl(g_fx_chain)[10])(g_fx_chain, fullscreen ? TRUE : FALSE, NULL))) {
        return -1;
    }
    return 0;
#else
    (void)fullscreen;
    return -100;
#endif
}

FX_API void *fx_dxgi_chain(void)
{
#ifndef FX_NO_DXGI
    return g_fx_chain;
#else
    return NULL;
#endif
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
