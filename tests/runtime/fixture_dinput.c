/* Test fixture standing in for dinput8.dll: a DirectInput object that hands
 * out a keyboard, a mouse and one other device, with the real interfaces'
 * virtual table layout as far as the patch and the engine fixture use it
 * (CreateDevice in slot 3; GetDeviceState and GetDeviceData in slots 9, 10).
 * The harness sets what the devices report.
 *
 * It contains no Microsoft code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <windows.h>

#define FXDI_API __declspec(dllexport)
#define FXDI_QUEUE 16
#define FXDI_PEEK 1u

typedef struct fxdi_event {
    DWORD offset, data, time, sequence;
    UINT_PTR app;
} fxdi_event;

typedef struct fxdi_device {
    void *const *vtbl;
    int kind; /* 0 keyboard, 1 mouse, 2 something else */
    unsigned char state[256];
    unsigned state_size;
    fxdi_event queue[FXDI_QUEUE];
    unsigned queued;
    long reads;
} fxdi_device;

static const GUID GUID_Keyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID GUID_Mouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

static HRESULT STDMETHODCALLTYPE fxdi_query(void *self, const GUID *iid, void **out)
{
    (void)self;
    (void)iid;
    if (out) {
        *out = NULL;
    }
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE fxdi_addref(void *self)
{
    (void)self;
    return 2;
}

static ULONG STDMETHODCALLTYPE fxdi_release(void *self)
{
    (void)self;
    return 1;
}

/* Slots nobody calls in these tests. */
static HRESULT STDMETHODCALLTYPE fxdi_unused(void *self)
{
    (void)self;
    return E_NOTIMPL;
}

static HRESULT STDMETHODCALLTYPE fxdi_get_state(fxdi_device *self, DWORD size, void *data)
{
    if (!data || size > sizeof(self->state)) {
        return E_INVALIDARG;
    }
    self->reads++;
    memcpy(data, self->state, size);
    if (self->kind == 1) {
        memset(self->state, 0, 12); /* movement is relative: reported once */
    }
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE fxdi_get_data(fxdi_device *self, DWORD object_size, void *data, DWORD *count,
                                               DWORD flags)
{
    DWORD n, i;

    if (!count || object_size < 16 || object_size > sizeof(fxdi_event)) {
        return E_INVALIDARG;
    }
    self->reads++;
    n = *count < self->queued ? *count : self->queued;
    for (i = 0; data && i < n; ++i) {
        memcpy((unsigned char *)data + i * object_size, &self->queue[i], object_size);
    }
    if (!(flags & FXDI_PEEK)) {
        memmove(self->queue, self->queue + n, (self->queued - n) * sizeof(fxdi_event));
        self->queued -= n;
    }
    *count = n;
    return S_OK;
}

static void *const g_device_vtbl[11] = {
    (void *)fxdi_query,  (void *)fxdi_addref, (void *)fxdi_release, (void *)fxdi_unused,
    (void *)fxdi_unused, (void *)fxdi_unused, (void *)fxdi_unused,  (void *)fxdi_unused,
    (void *)fxdi_unused, (void *)fxdi_get_state, (void *)fxdi_get_data,
};

static fxdi_device g_devices[3] = {
    {g_device_vtbl, 0, {0}, 256, {{0, 0, 0, 0, 0}}, 0, 0},
    {g_device_vtbl, 1, {0}, 16, {{0, 0, 0, 0, 0}}, 0, 0},
    {g_device_vtbl, 2, {0}, 80, {{0, 0, 0, 0, 0}}, 0, 0},
};

static HRESULT STDMETHODCALLTYPE fxdi_create_device(void *self, const GUID *guid, void **device, void *outer)
{
    (void)self;
    (void)outer;
    if (!guid || !device) {
        return E_INVALIDARG;
    }
    if (memcmp(guid, &GUID_Keyboard, sizeof(GUID)) == 0) {
        *device = &g_devices[0];
    } else if (memcmp(guid, &GUID_Mouse, sizeof(GUID)) == 0) {
        *device = &g_devices[1];
    } else {
        *device = &g_devices[2];
    }
    return S_OK;
}

static void *const g_di_vtbl[4] = {
    (void *)fxdi_query,
    (void *)fxdi_addref,
    (void *)fxdi_release,
    (void *)fxdi_create_device,
};

static struct fxdi_object {
    void *const *vtbl;
} g_di = {g_di_vtbl};

#if defined(_MSC_VER)
/* Export the stdcall function under its plain name, as dinput8.dll does. */
#pragma comment(linker, "/EXPORT:DirectInput8Create=_DirectInput8Create@20")
HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, const GUID *iid, void **out, void *outer)
#else
/* MinGW: linked with --kill-at, which strips the @20 from the export name. */
FXDI_API HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, const GUID *iid, void **out, void *outer)
#endif
{
    (void)instance;
    (void)version;
    (void)iid;
    (void)outer;
    if (!out) {
        return E_INVALIDARG;
    }
    *out = &g_di;
    return S_OK;
}

/* ---- what the harness sets ------------------------------------------------------------ */

static void queue(fxdi_device *d, DWORD offset, DWORD data)
{
    static DWORD sequence;
    if (d->queued < FXDI_QUEUE) {
        fxdi_event *e = &d->queue[d->queued++];
        e->offset = offset;
        e->data = data;
        e->time = GetTickCount();
        e->sequence = ++sequence;
        e->app = 0;
    }
}

/* A key goes down or up: seen in the state and as a buffered event. */
FXDI_API void fxdi_key(unsigned scan_code, int down)
{
    g_devices[0].state[scan_code & 0xFF] = down ? 0x80 : 0;
    queue(&g_devices[0], scan_code & 0xFF, down ? 0x80u : 0u);
}

FXDI_API void fxdi_mouse_move(long dx, long dy)
{
    LONG axis[3];
    memcpy(axis, g_devices[1].state, sizeof(axis));
    axis[0] += dx;
    axis[1] += dy;
    memcpy(g_devices[1].state, axis, sizeof(axis));
    if (dx) {
        queue(&g_devices[1], 0, (DWORD)dx);
    }
    if (dy) {
        queue(&g_devices[1], 4, (DWORD)dy);
    }
}

FXDI_API void fxdi_mouse_button(unsigned button, int down)
{
    g_devices[1].state[12 + (button & 3)] = down ? 0x80 : 0;
    queue(&g_devices[1], 12 + (button & 3), down ? 0x80u : 0u);
}

/* The third device: a controller whose axes rest at mid-range, not at zero. */
FXDI_API void fxdi_other_fill(unsigned char value)
{
    memset(g_devices[2].state, value, sizeof(g_devices[2].state));
}

/* Forgets everything that was set or queued. */
FXDI_API void fxdi_reset(void)
{
    unsigned i;
    for (i = 0; i < 3; ++i) {
        memset(g_devices[i].state, 0, sizeof(g_devices[i].state));
        g_devices[i].queued = 0;
    }
}

FXDI_API long fxdi_reads(unsigned device)
{
    return device < 3 ? g_devices[device].reads : -1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
