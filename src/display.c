/* Display compatibility layer.
 *
 * Fullscreen guard plumbing. The engine obtains its DXGI factory through an
 * imported CreateDXGIFactory1 and creates its swap chain(s) from it. COM
 * objects of one class share a virtual table, so the hooks below replace a
 * table slot once and then filter on the object: only factories handed to the
 * engine and only swap chains created from those are affected. Anything else
 * that uses DXGI in the process (overlays, capture tools) passes straight
 * through.
 *
 * This works the same with Microsoft's DXGI and with DXVK's, which lays its
 * objects out identically.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define COBJMACROS
#include "display.h"

#include <dxgi.h>
#include <string.h>

#include "fs_guard.h"
#include "log.h"
#include "pe_image.h"
#include "vtable.h"

/* Slot numbers in the IDXGIFactory and IDXGISwapChain virtual tables. */
#define SLOT_FACTORY_CREATE_SWAP_CHAIN 10
#define SLOT_CHAIN_PRESENT 8
#define SLOT_CHAIN_GET_FULLSCREEN_STATE 11

#define MAX_FACTORIES 4
#define MAX_CHAINS 8

typedef HRESULT(WINAPI *create_factory_fn)(REFIID riid, void **factory);
typedef HRESULT(STDMETHODCALLTYPE *create_swap_chain_fn)(IDXGIFactory *self, IUnknown *device,
                                                        DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **chain);
typedef HRESULT(STDMETHODCALLTYPE *get_fullscreen_fn)(IDXGISwapChain *self, BOOL *fullscreen, IDXGIOutput **target);
typedef HRESULT(STDMETHODCALLTYPE *present_fn)(IDXGISwapChain *self, UINT sync_interval, UINT flags);

typedef struct chain_entry {
    IDXGISwapChain *chain;
    fs_guard guard;
} chain_entry;

static SRWLOCK g_lock = SRWLOCK_INIT;
static create_factory_fn g_orig_create_factory;
static create_factory_fn g_orig_create_factory1;
static vt_hook g_factory_vtables[VT_MAX_TABLES];
static vt_hook g_chain_vtables[VT_MAX_TABLES];
static vt_hook g_present_vtables[VT_MAX_TABLES];
static int g_guard_fullscreen;
static display_present_fn g_on_present;
static IDXGIFactory *g_factories[MAX_FACTORIES];
static chain_entry g_chains[MAX_CHAINS];
static unsigned g_next_chain;

/* ---- process-wide settings ------------------------------------------------------ */

void display_apply_process_settings(const fc3pp_config *cfg)
{
    if (cfg->high_dpi_aware) {
        /* The game executables carry no DPI manifest. On a display scaled above
         * 100 % Windows then runs them "DPI unaware": the desktop is reported
         * smaller than it is and windowed / borderless output is bitmap-scaled.
         * System awareness is what the "High DPI aware" compatibility setting
         * selects; per-monitor awareness would need the engine's cooperation
         * (it sizes its window with AdjustWindowRectEx). */
        if (IsProcessDPIAware()) {
            LOGI("display: the process is already DPI aware");
        } else if (SetProcessDPIAware()) {
            LOGI("display: marked the process DPI aware (no bitmap scaling on scaled displays)");
        } else {
            LOGE("display: SetProcessDPIAware failed (error %lu)", (unsigned long)GetLastError());
        }
    }
    if (cfg->disable_window_ghosting) {
        /* While the game loads without pumping messages Windows would replace
         * its window by a "Not responding" ghost. */
        DisableProcessWindowsGhosting();
        LOGD("display: window ghosting disabled for this process");
    }
}

/* ---- bookkeeping (call with g_lock held) ----------------------------------------- */

static int is_engine_factory(const IDXGIFactory *factory)
{
    unsigned i;
    for (i = 0; i < MAX_FACTORIES; ++i) {
        if (g_factories[i] == factory) {
            return 1;
        }
    }
    return 0;
}

static chain_entry *find_chain(const IDXGISwapChain *chain)
{
    unsigned i;
    for (i = 0; i < MAX_CHAINS; ++i) {
        if (g_chains[i].chain == chain) {
            return &g_chains[i];
        }
    }
    return NULL;
}

/* ---- hooks ---------------------------------------------------------------------- */

static int chain_window_is_active(IDXGISwapChain *chain)
{
    DXGI_SWAP_CHAIN_DESC desc;

    if (FAILED(IDXGISwapChain_GetDesc(chain, &desc))) {
        return 0;
    }
    return GetForegroundWindow() == desc.OutputWindow && !IsIconic(desc.OutputWindow);
}

/* Reports the display mode the game ended up with: the refresh rate in
 * particular is chosen by DXGI, not by the player. */
static void log_fullscreen_mode(IDXGISwapChain *chain)
{
    DXGI_SWAP_CHAIN_DESC desc;
    IDXGIOutput *output = NULL;

    if (FAILED(IDXGISwapChain_GetDesc(chain, &desc))) {
        return;
    }
    if (desc.BufferDesc.RefreshRate.Denominator) {
        LOGI("display: fullscreen at %ux%u, %.2f Hz requested", desc.BufferDesc.Width, desc.BufferDesc.Height,
             (double)desc.BufferDesc.RefreshRate.Numerator / (double)desc.BufferDesc.RefreshRate.Denominator);
    } else {
        LOGI("display: fullscreen at %ux%u, refresh rate left to the system", desc.BufferDesc.Width,
             desc.BufferDesc.Height);
    }
    /* What the monitor is actually doing now (the device name is not logged). */
    if (SUCCEEDED(IDXGISwapChain_GetContainingOutput(chain, &output)) && output) {
        DXGI_OUTPUT_DESC output_desc;
        DEVMODEW mode;

        memset(&mode, 0, sizeof(mode));
        mode.dmSize = sizeof(mode);
        if (SUCCEEDED(IDXGIOutput_GetDesc(output, &output_desc)) &&
            EnumDisplaySettingsW(output_desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode)) {
            LOGI("display: the monitor is running at %lux%lu, %lu Hz", (unsigned long)mode.dmPelsWidth,
                 (unsigned long)mode.dmPelsHeight, (unsigned long)mode.dmDisplayFrequency);
        }
        IDXGIOutput_Release(output);
    }
}

static HRESULT STDMETHODCALLTYPE hook_get_fullscreen_state(IDXGISwapChain *self, BOOL *fullscreen,
                                                           IDXGIOutput **target)
{
    get_fullscreen_fn original;
    chain_entry *entry;
    fs_decision d;
    BOOL real = FALSE;
    HRESULT hr;
    int tracked, active, entered = 0;
    DWORD now;

    AcquireSRWLockShared(&g_lock);
    original = (get_fullscreen_fn)vt_original(g_chain_vtables, self->lpVtbl);
    tracked = find_chain(self) != NULL;
    ReleaseSRWLockShared(&g_lock);
    if (!original) {
        return E_FAIL; /* cannot happen: the slot is only patched after the original is stored */
    }
    if (!tracked) {
        return original(self, fullscreen, target);
    }

    hr = original(self, &real, target);
    if (FAILED(hr)) {
        return hr;
    }
    active = chain_window_is_active(self);
    now = GetTickCount();

    AcquireSRWLockExclusive(&g_lock);
    entry = find_chain(self);
    if (entry) {
        entered = real && !entry->guard.was_fullscreen;
        d = fs_guard_step(&entry->guard, real != FALSE, active, now);
    } else {
        d.report_fullscreen = real != FALSE;
        d.restore = 0;
        d.event = FS_EV_NONE;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (entered) {
        log_fullscreen_mode(self);
    }
    switch (d.event) {
    case FS_EV_LOST:
        LOGI("display: Windows took the game out of fullscreen on focus loss; "
             "keeping the Fullscreen setting until the window is active again");
        break;
    case FS_EV_RESTORED:
        LOGI("display: fullscreen restored");
        break;
    case FS_EV_USER_WINDOWED:
        LOGD("display: fullscreen ended while the window had focus (Alt+Enter or options menu); not interfering");
        break;
    case FS_EV_GAVE_UP:
        LOGE("display: fullscreen could not be restored after %u attempts; the game continues windowed "
             "(Alt+Enter switches back)",
             FS_GUARD_MAX_ATTEMPTS);
        break;
    default:
        break;
    }

    if (d.restore) {
        HRESULT set = IDXGISwapChain_SetFullscreenState(self, TRUE, NULL);
        AcquireSRWLockExclusive(&g_lock);
        entry = find_chain(self);
        if (entry) {
            fs_guard_attempted(&entry->guard, now);
        }
        ReleaseSRWLockExclusive(&g_lock);
        LOGD("display: requested fullscreen again (result 0x%08lX)", (unsigned long)set);
    }
    if (fullscreen) {
        *fullscreen = d.report_fullscreen ? TRUE : FALSE;
    }
    return hr;
}

/* The engine presents a frame: the place to draw over it. */
static HRESULT STDMETHODCALLTYPE hook_present(IDXGISwapChain *self, UINT sync_interval, UINT flags)
{
    present_fn original;
    int ours;

    AcquireSRWLockShared(&g_lock);
    original = (present_fn)vt_original(g_present_vtables, self->lpVtbl);
    ours = find_chain(self) != NULL;
    ReleaseSRWLockShared(&g_lock);
    if (!original) {
        return E_FAIL;
    }
    if (ours && g_on_present && !(flags & DXGI_PRESENT_TEST)) {
        g_on_present(self);
    }
    return original(self, sync_interval, flags);
}

static void track_chain(IDXGISwapChain *chain)
{
    chain_entry *entry;
    int guarded = 1, presenting = 1;

    AcquireSRWLockExclusive(&g_lock);
    if (g_guard_fullscreen) {
        guarded = vt_hook_slot(g_chain_vtables, VT_OF(chain), SLOT_CHAIN_GET_FULLSCREEN_STATE,
                               (void *)hook_get_fullscreen_state);
    }
    if (g_on_present) {
        presenting = vt_hook_slot(g_present_vtables, VT_OF(chain), SLOT_CHAIN_PRESENT, (void *)hook_present);
    }
    if (guarded || presenting) {
        /* A released swap chain's address may be reused by its successor. */
        entry = find_chain(chain);
        if (!entry) {
            entry = &g_chains[g_next_chain++ % MAX_CHAINS];
        }
        entry->chain = chain;
        fs_guard_reset(&entry->guard);
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (g_guard_fullscreen) {
        if (guarded) {
            LOGI("display: fullscreen guard attached to a swap chain of the game");
        } else {
            LOGE("display: could not attach the fullscreen guard to the swap chain");
        }
    }
    if (g_on_present && !presenting) {
        LOGE("display: could not attach to the swap chain's Present; the in-game menu is unavailable");
    }
}

static HRESULT STDMETHODCALLTYPE hook_create_swap_chain(IDXGIFactory *self, IUnknown *device,
                                                        DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **chain)
{
    create_swap_chain_fn original;
    HRESULT hr;
    int ours;

    AcquireSRWLockShared(&g_lock);
    original = (create_swap_chain_fn)vt_original(g_factory_vtables, self->lpVtbl);
    ours = is_engine_factory(self);
    ReleaseSRWLockShared(&g_lock);
    if (!original) {
        return E_FAIL;
    }
    hr = original(self, device, desc, chain);
    if (SUCCEEDED(hr) && ours && chain && *chain) {
        track_chain(*chain);
    }
    return hr;
}

static void track_factory(IDXGIFactory *factory)
{
    unsigned i;
    int hooked;

    AcquireSRWLockExclusive(&g_lock);
    hooked = vt_hook_slot(g_factory_vtables, VT_OF(factory), SLOT_FACTORY_CREATE_SWAP_CHAIN,
                          (void *)hook_create_swap_chain);
    if (hooked && !is_engine_factory(factory)) {
        for (i = 0; i < MAX_FACTORIES; ++i) {
            if (!g_factories[i]) {
                g_factories[i] = factory;
                break;
            }
        }
        if (i == MAX_FACTORIES) {
            g_factories[0] = factory;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!hooked) {
        LOGE("display: could not hook the DXGI factory; fullscreen guard inactive");
    }
}

static HRESULT WINAPI hook_create_factory1(REFIID riid, void **factory)
{
    HRESULT hr = g_orig_create_factory1(riid, factory);
    if (SUCCEEDED(hr) && factory && *factory) {
        track_factory((IDXGIFactory *)*factory);
    }
    return hr;
}

static HRESULT WINAPI hook_create_factory(REFIID riid, void **factory)
{
    HRESULT hr = g_orig_create_factory(riid, factory);
    if (SUCCEEDED(hr) && factory && *factory) {
        track_factory((IDXGIFactory *)*factory);
    }
    return hr;
}

static unsigned hook_factory_import(HMODULE engine, const char *name, void *hook, create_factory_fn *original)
{
    void **slot = pei_find_import(engine, "dxgi.dll", name);
    void *previous = NULL;

    if (!slot || *slot == hook) {
        return 0; /* not imported, or hooked before */
    }
    if (pei_hook_import(engine, "dxgi.dll", name, hook, &previous) != 1 || !previous) {
        LOGE("display: could not hook %s", name);
        return 0;
    }
    *original = (create_factory_fn)previous;
    return 1;
}

unsigned display_install_hooks(HMODULE engine, int guard_fullscreen, display_present_fn on_present)
{
    unsigned hooks = 0;

    g_guard_fullscreen = guard_fullscreen;
    g_on_present = on_present;

    hooks += hook_factory_import(engine, "CreateDXGIFactory1", (void *)hook_create_factory1, &g_orig_create_factory1);
    hooks += hook_factory_import(engine, "CreateDXGIFactory", (void *)hook_create_factory, &g_orig_create_factory);
    return hooks;
}
