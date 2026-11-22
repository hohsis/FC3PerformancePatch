/* Test fixture standing in for dxgi.dll.
 *
 * It is built as "dxgi.dll" and placed next to the test host, where the
 * engine fixture's import of CreateDXGIFactory1 binds to it. It hands out a
 * fake factory and fake swap chains with the same virtual table layout as the
 * real ones (read-only tables, like the real ones), and lets the test play
 * the part of Windows: change the fullscreen state behind the "game's" back,
 * move the output window, and make SetFullscreenState fail.
 *
 * It contains no Microsoft code and draws nothing.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <windows.h>

#define FXD_API __declspec(dllexport)

#define FX_E_NOINTERFACE ((HRESULT)0x80004002L)
#define FX_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE ((HRESULT)0x887A0022L)

/* IDXGISwapChain / IDXGIFactory slot numbers. */
#define SLOT_FACTORY_CREATE_SWAP_CHAIN 10
#define SLOT_CHAIN_SET_FULLSCREEN 10
#define SLOT_CHAIN_GET_FULLSCREEN 11
#define SLOT_CHAIN_GET_DESC 12

/* DXGI_SWAP_CHAIN_DESC, spelled out so the fixture needs no SDK header. */
typedef struct fx_swap_chain_desc {
    UINT width, height, refresh_num, refresh_den, format, scanline, scaling;
    UINT sample_count, sample_quality;
    UINT buffer_usage, buffer_count;
    HWND output_window;
    BOOL windowed;
    UINT swap_effect, flags;
} fx_swap_chain_desc;

typedef struct fx_object {
    void *const *vtbl;
    LONG refs;
} fx_object;

static HWND g_window;
static volatile LONG g_fullscreen;
static volatile LONG g_set_calls;
static volatile LONG g_get_calls;
static volatile LONG g_fail_set;
static volatile LONG g_chains_created;

/* ---- IUnknown ---------------------------------------------------------------------- */

static HRESULT WINAPI fx_query_interface(fx_object *self, const GUID *iid, void **out)
{
    (void)self;
    (void)iid;
    if (out) {
        *out = NULL;
    }
    return FX_E_NOINTERFACE;
}

static ULONG WINAPI fx_add_ref(fx_object *self)
{
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG WINAPI fx_release(fx_object *self)
{
    return (ULONG)InterlockedDecrement(&self->refs); /* the objects are static */
}

/* ---- swap chain -------------------------------------------------------------------- */

static HRESULT WINAPI fx_set_fullscreen_state(fx_object *self, BOOL fullscreen, void *target)
{
    (void)self;
    (void)target;
    InterlockedIncrement(&g_set_calls);
    if (fullscreen && g_fail_set) {
        return FX_DXGI_ERROR_NOT_CURRENTLY_AVAILABLE;
    }
    InterlockedExchange(&g_fullscreen, fullscreen ? 1 : 0);
    return S_OK;
}

static HRESULT WINAPI fx_get_fullscreen_state(fx_object *self, BOOL *fullscreen, void **target)
{
    (void)self;
    InterlockedIncrement(&g_get_calls);
    if (fullscreen) {
        *fullscreen = g_fullscreen ? TRUE : FALSE;
    }
    if (target) {
        *target = NULL;
    }
    return S_OK;
}

static HRESULT WINAPI fx_get_desc(fx_object *self, fx_swap_chain_desc *desc)
{
    (void)self;
    memset(desc, 0, sizeof(*desc));
    desc->width = 1280;
    desc->height = 720;
    desc->buffer_count = 1;
    desc->output_window = g_window;
    desc->windowed = g_fullscreen ? FALSE : TRUE;
    return S_OK;
}

/* No monitor behind the fake swap chain. */
static HRESULT WINAPI fx_get_containing_output(fx_object *self, void **output)
{
    (void)self;
    if (output) {
        *output = NULL;
    }
    return ((HRESULT)0x887A0004L); /* DXGI_ERROR_UNSUPPORTED */
}

/* Slots the tests never call are NULL: a stray call fails loudly. */
static void *const g_chain_vtbl[18] = {
    (void *)fx_query_interface,      /* 0 */
    (void *)fx_add_ref,              /* 1 */
    (void *)fx_release,              /* 2 */
    NULL, NULL, NULL, NULL,          /* 3-6  IDXGIObject */
    NULL,                            /* 7    GetDevice */
    NULL,                            /* 8    Present */
    NULL,                            /* 9    GetBuffer */
    (void *)fx_set_fullscreen_state, /* 10 */
    (void *)fx_get_fullscreen_state, /* 11 */
    (void *)fx_get_desc,             /* 12 */
    NULL, NULL,                      /* 13-14 ResizeBuffers, ResizeTarget */
    (void *)fx_get_containing_output, /* 15 */
    NULL, NULL,                      /* 16-17 */
};

static fx_object g_chains[4] = {
    {g_chain_vtbl, 1}, {g_chain_vtbl, 1}, {g_chain_vtbl, 1}, {g_chain_vtbl, 1}};

/* ---- factory ----------------------------------------------------------------------- */

static HRESULT WINAPI fx_create_swap_chain(fx_object *self, void *device, fx_swap_chain_desc *desc, void **chain)
{
    LONG index;
    (void)self;
    (void)device;
    if (!desc || !chain) {
        return E_INVALIDARG;
    }
    index = InterlockedIncrement(&g_chains_created) - 1;
    g_window = desc->output_window;
    InterlockedExchange(&g_fullscreen, 0); /* swap chains start out windowed */
    *chain = &g_chains[index % 4];
    return S_OK;
}

static void *const g_factory_vtbl[14] = {
    (void *)fx_query_interface,   /* 0 */
    (void *)fx_add_ref,           /* 1 */
    (void *)fx_release,           /* 2 */
    NULL, NULL, NULL, NULL,       /* 3-6  IDXGIObject */
    NULL,                         /* 7    EnumAdapters */
    NULL,                         /* 8    MakeWindowAssociation */
    NULL,                         /* 9    GetWindowAssociation */
    (void *)fx_create_swap_chain, /* 10 */
    NULL,                         /* 11   CreateSoftwareAdapter */
    NULL,                         /* 12   EnumAdapters1 */
    NULL,                         /* 13   IsCurrent */
};

static fx_object g_factory = {g_factory_vtbl, 1};
/* A second factory nobody handed to the engine: its swap chains must be left alone. */
static fx_object g_foreign_factory = {g_factory_vtbl, 1};

/* ---- exports ----------------------------------------------------------------------- */

#if defined(_MSC_VER)
/* Export the stdcall function under its plain name, as dxgi.dll does. */
#pragma comment(linker, "/EXPORT:CreateDXGIFactory1=_CreateDXGIFactory1@8")
HRESULT WINAPI CreateDXGIFactory1(const GUID *iid, void **factory)
#else
/* MinGW: linked with --kill-at, which strips the @8 from the export name. */
FXD_API HRESULT WINAPI CreateDXGIFactory1(const GUID *iid, void **factory)
#endif
{
    (void)iid;
    if (!factory) {
        return E_INVALIDARG;
    }
    *factory = &g_factory;
    return S_OK;
}

/* What Windows does on its own: change the state without telling the game. */
FXD_API void fxdxgi_force_state(int fullscreen)
{
    InterlockedExchange(&g_fullscreen, fullscreen ? 1 : 0);
}

FXD_API int fxdxgi_state(void)
{
    return (int)g_fullscreen;
}

FXD_API void fxdxgi_set_window(void *window)
{
    g_window = (HWND)window;
}

FXD_API long fxdxgi_set_calls(void)
{
    return (long)g_set_calls;
}

FXD_API void fxdxgi_fail_set(int fail)
{
    InterlockedExchange(&g_fail_set, fail);
}

/* A swap chain created through a factory the engine never obtained itself,
 * the way an overlay or capture tool would. Returns the swap chain. */
FXD_API void *fxdxgi_foreign_chain(void)
{
    typedef HRESULT(WINAPI * create_fn)(fx_object *, void *, fx_swap_chain_desc *, void **);
    fx_swap_chain_desc desc;
    void *chain = NULL;
    HWND keep = g_window;
    LONG state = g_fullscreen;

    memset(&desc, 0, sizeof(desc));
    /* Through the table, so a hook on the shared slot is exercised. */
    ((create_fn)g_foreign_factory.vtbl[SLOT_FACTORY_CREATE_SWAP_CHAIN])(&g_foreign_factory, NULL, &desc, &chain);
    g_window = keep;
    InterlockedExchange(&g_fullscreen, state);
    return chain;
}

/* Reads a swap chain's fullscreen state through its table. */
FXD_API int fxdxgi_query(void *chain)
{
    typedef HRESULT(WINAPI * get_fn)(fx_object *, BOOL *, void **);
    fx_object *object = (fx_object *)chain;
    BOOL fullscreen = FALSE;

    if (!object || FAILED(((get_fn)object->vtbl[SLOT_CHAIN_GET_FULLSCREEN])(object, &fullscreen, NULL))) {
        return -1;
    }
    return fullscreen ? 1 : 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
