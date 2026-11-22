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

/* The renderer shows a frame. 0 on success, <0 on error. */
FX_API int fx_dxgi_present(void)
{
#ifndef FX_NO_DXGI
    typedef HRESULT(WINAPI * fx_present_fn)(void *self, UINT sync_interval, UINT flags);
    if (!g_fx_chain || FAILED(((fx_present_fn)fx_vtbl(g_fx_chain)[8])(g_fx_chain, 0, 0))) {
        return -1;
    }
    return 0;
#else
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

/* ---- XAudio2 2.7, created and used the way the sound engine does ------------------
 * Returns a bit mask of how far it got: 1 engine created, 2 initialised,
 * 4 mastering voice, 8 source voice, 16 output matrix set, 32 source voice
 * re-tuned to another sample rate. 0 means XAudio2 2.7
 * is not installed (it comes with the DirectX June 2010 runtime). Nothing is
 * played. */
#pragma pack(push, 1)
typedef struct fx_wave_format {
    WORD tag, channels;
    DWORD rate, bytes_per_second;
    WORD block_align, bits, extra_size;
} fx_wave_format;
typedef struct fx_voice_details {
    UINT32 flags, channels, rate;
} fx_voice_details;
#pragma pack(pop)

FX_API int fx_audio_probe(void)
{
    typedef HRESULT(__stdcall * init_fn)(void *, UINT32, UINT32);
    typedef ULONG(__stdcall * release_fn)(void *);
    typedef HRESULT(__stdcall * count_fn)(void *, UINT32 *);
    typedef HRESULT(__stdcall * master_fn)(void *, void **, UINT32, UINT32, UINT32, UINT32, const void *);
    typedef HRESULT(__stdcall * source_fn)(void *, void **, const fx_wave_format *, UINT32, float, void *,
                                           const void *, const void *);
    typedef void(__stdcall * details_fn)(void *, fx_voice_details *);
    typedef HRESULT(__stdcall * matrix_fn)(void *, void *, UINT32, UINT32, const float *, UINT32);
    typedef void(__stdcall * destroy_fn)(void *);
    typedef HRESULT(__stdcall * rate_fn)(void *, UINT32);
    static const GUID clsid = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
    static const GUID iid = {0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};
    static const fx_wave_format format = {1, 1, 22050, 44100, 2, 16, 0};
    void *engine = NULL, *master = NULL, *source = NULL;
    HRESULT com = CoInitializeEx(NULL, 0);
    int result = 0;

    if (SUCCEEDED(CoCreateInstance(&clsid, NULL, 1, &iid, &engine)) && engine) {
        void **vt = *(void ***)engine;
        result |= 1;
        if (SUCCEEDED(((init_fn)vt[5])(engine, 0, 0xFFFFFFFFu))) {
            UINT32 devices = 0;
            result |= 2;
            /* Without sound hardware (CI) stop here: Wine's XAudio2 crashes
             * when asked for a mastering voice on a device that does not exist. */
            if (SUCCEEDED(((count_fn)vt[3])(engine, &devices)) && devices > 0 &&
                SUCCEEDED(((master_fn)vt[10])(engine, &master, 0, 0, 0, 0, NULL)) && master) {
                result |= 4;
                if (SUCCEEDED(((source_fn)vt[8])(engine, &source, &format, 0, 2.0f, NULL, NULL, NULL)) && source) {
                    fx_voice_details details;
                    float matrix[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
                    result |= 8;
                    memset(&details, 0, sizeof(details));
                    ((details_fn)(*(void ***)master)[0])(master, &details);
                    if (details.channels >= 1 && details.channels <= 8 &&
                        SUCCEEDED(((matrix_fn)(*(void ***)source)[16])(source, NULL, 1, details.channels, matrix,
                                                                       0))) {
                        result |= 16;
                    }
                    if (SUCCEEDED(((rate_fn)(*(void ***)source)[28])(source, 16000))) {
                        result |= 32;
                    }
                    ((destroy_fn)(*(void ***)source)[18])(source);
                }
                ((destroy_fn)(*(void ***)master)[18])(master);
            }
        }
        ((release_fn)vt[2])(engine);
    }
    if (SUCCEEDED(com)) {
        CoUninitialize();
    }
    return result;
}

/* ---- textures, streamed the way the DX11 renderer streams them ---------------------
 * A texture object with the engine's layout (Direct3D texture at +1Ch) and a
 * virtual table whose slots 9 and 10 are "add top mip level" and "drop top mip
 * level". Both begin with the instructions of the real methods and then just
 * keep count and resize the fake Direct3D texture, so the harness can see
 * which calls got through. The DX9-named fixture has no such class. */
#ifndef FX_NO_TEXTURES
#include "../unit/tex_blobs.h"

#if defined(_MSC_VER)
#pragma section(".fxtex", execute, read)
__declspec(allocate(".fxtex")) static const unsigned char fx_tex_add_code[] = {TEX_BLOB_ADD_RELEASE};
__declspec(allocate(".fxtex")) static const unsigned char fx_tex_drop_code[] = {TEX_BLOB_DROP_RELEASE};
__declspec(allocate(".fxtex")) static const unsigned char fx_tex_destroy_code[] = {TEX_BLOB_DESTROY};
#else
static const unsigned char fx_tex_destroy_code[] __attribute__((section(".fxtex,\"xr\"#"), used)) = {TEX_BLOB_DESTROY};
static const unsigned char fx_tex_add_code[] __attribute__((section(".fxtex,\"xr\"#"), used)) = {TEX_BLOB_ADD_RELEASE};
static const unsigned char fx_tex_drop_code[] __attribute__((section(".fxtex,\"xr\"#"), used)) = {
    TEX_BLOB_DROP_RELEASE};
#endif

typedef struct fx_d3d_texture {
    void *const *vtbl;
    UINT width, height, levels, format;
} fx_d3d_texture;

typedef struct fx_texture {
    void *const *vtbl;       /* +00h */
    LONG adds;               /* +04h */
    LONG drops;              /* +08h */
    LONG destroyed;          /* +0Ch */
    UINT last_size;          /* +10h */
    UINT fail_next;          /* +14h */
    UINT reserved;           /* +18h */
    fx_d3d_texture *texture; /* +1Ch */
    fx_d3d_texture storage;
} fx_texture;

typedef struct fx_texture_desc { /* D3D11_TEXTURE2D_DESC */
    UINT width, height, levels, array_size, format, sample_count, sample_quality, usage, bind, cpu, misc;
} fx_texture_desc;

static void __stdcall fx_d3d_get_desc(fx_d3d_texture *self, fx_texture_desc *desc)
{
    memset(desc, 0, sizeof(*desc));
    desc->width = self->width;
    desc->height = self->height;
    desc->levels = self->levels;
    desc->array_size = 1;
    desc->format = self->format;
    desc->sample_count = 1;
}

static void *const g_fx_d3d_vtbl[11] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, /* 0-9: never called */
    (void *)fx_d3d_get_desc,                                    /* 10: GetDesc */
};

/* The methods are __thiscall; written as __fastcall with a spare register. */
static void __fastcall fx_tex_unused_method(fx_texture *self, void *unused)
{
    (void)self;
    (void)unused;
}

static void *const g_fx_tex_vtbl[12] = {
    (void *)fx_tex_destroy_code,   /* 0  scalar deleting destructor */
    (void *)fx_tex_unused_method,  /* 1 */
    (void *)fx_tex_unused_method,  /* 2 */
    (void *)fx_tex_unused_method,  /* 3 */
    (void *)fx_tex_unused_method,  /* 4 */
    (void *)fx_tex_unused_method,  /* 5 */
    (void *)fx_tex_unused_method,  /* 6 */
    (void *)fx_tex_unused_method,  /* 7 */
    (void *)fx_tex_unused_method,  /* 8 */
    (void *)fx_tex_add_code,       /* 9  AddTopLevel */
    (void *)fx_tex_drop_code,      /* 10 DropTopLevel */
    (void *)fx_tex_unused_method,  /* 11 */
};

typedef unsigned char(__fastcall *fx_add_fn)(fx_texture *, void *, const void *, unsigned, void *, unsigned);
typedef unsigned char(__fastcall *fx_drop_fn)(fx_texture *, void *);
typedef void *(__fastcall *fx_destroy_fn)(fx_texture *, void *, unsigned);

static fx_texture g_fx_textures[8];
#endif

/* (Re)creates texture object number `index` in place, as the engine does when
 * its allocator hands out the address of a destroyed object again. */
FX_API void *fx_tex_create(unsigned index, unsigned width, unsigned height, unsigned levels, unsigned dxgi_format)
{
#ifndef FX_NO_TEXTURES
    fx_texture *t;
    if (index >= sizeof(g_fx_textures) / sizeof(g_fx_textures[0])) {
        return NULL;
    }
    t = &g_fx_textures[index];
    memset(t, 0, sizeof(*t));
    t->vtbl = g_fx_tex_vtbl;
    t->texture = &t->storage;
    t->storage.vtbl = g_fx_d3d_vtbl;
    t->storage.width = width;
    t->storage.height = height;
    t->storage.levels = levels;
    t->storage.format = dxgi_format;
    return t;
#else
    (void)index;
    (void)width;
    (void)height;
    (void)levels;
    (void)dxgi_format;
    return NULL;
#endif
}

/* The streaming job: a mip0 file was read, hand it to the texture. 1/0 as the
 * method answered, -1 without a texture class. */
FX_API int fx_tex_add(void *texture, const void *dds, unsigned size)
{
#ifndef FX_NO_TEXTURES
    fx_texture *t = (fx_texture *)texture;
    return ((fx_add_fn)t->vtbl[9])(t, NULL, dds, size, NULL, 0) ? 1 : 0;
#else
    (void)texture;
    (void)dds;
    (void)size;
    return -1;
#endif
}

/* The same with the whole texture file in hand, as the engine has it: the
 * method gets the DDS image inside the file and the file buffer itself. */
FX_API int fx_tex_add_file(void *texture, const void *xbt, unsigned size)
{
#ifndef FX_NO_TEXTURES
    fx_texture *t = (fx_texture *)texture;
    const unsigned char *file = (const unsigned char *)xbt;
    unsigned header = (unsigned)file[8] | ((unsigned)file[9] << 8);
    if (size < header) {
        return -2;
    }
    return ((fx_add_fn)t->vtbl[9])(t, NULL, file + header, size - header, (void *)file, 1) ? 1 : 0;
#else
    (void)texture;
    (void)xbt;
    (void)size;
    return -1;
#endif
}

FX_API int fx_tex_drop(void *texture)
{
#ifndef FX_NO_TEXTURES
    fx_texture *t = (fx_texture *)texture;
    return ((fx_drop_fn)t->vtbl[10])(t, NULL) ? 1 : 0;
#else
    (void)texture;
    return -1;
#endif
}

FX_API void fx_tex_destroy(void *texture)
{
#ifndef FX_NO_TEXTURES
    fx_texture *t = (fx_texture *)texture;
    ((fx_destroy_fn)t->vtbl[0])(t, NULL, 0);
#else
    (void)texture;
#endif
}

FX_API void fx_tex_fail_next_add(void *texture, int fail)
{
#ifndef FX_NO_TEXTURES
    ((fx_texture *)texture)->fail_next = fail ? 1u : 0u;
#else
    (void)texture;
    (void)fail;
#endif
}

/* out: calls that reached add, calls that reached drop, destructor calls,
 * width, height, levels of the Direct3D texture. */
FX_API void fx_tex_state(void *texture, unsigned out[6])
{
#ifndef FX_NO_TEXTURES
    fx_texture *t = (fx_texture *)texture;
    out[0] = (unsigned)t->adds;
    out[1] = (unsigned)t->drops;
    out[2] = (unsigned)t->destroyed;
    out[3] = t->storage.width;
    out[4] = t->storage.height;
    out[5] = t->storage.levels;
#else
    (void)texture;
    memset(out, 0, 6 * sizeof(out[0]));
#endif
}

/* ---- input -----------------------------------------------------------------------------
 * The engine reads the gamepad through xinput1_3.dll (imported by ordinal)
 * and keyboard and mouse through DirectInput devices; next to the host both
 * DLLs are test fixtures. Its UI manager holds the input mode in three bytes
 * at +30h; the code that copies the profile's setting there is reproduced
 * below byte for byte, because the patch finds the manager through it. */
__declspec(dllimport) DWORD WINAPI XInputGetState(DWORD index, void *state);
__declspec(dllimport) HRESULT WINAPI DirectInput8Create(HINSTANCE instance, DWORD version, const GUID *iid,
                                                        void **out, void *outer);

static const GUID FX_GUID_Keyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID FX_GUID_Mouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID FX_GUID_Other = {0x6F1D2B70, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

typedef HRESULT(__stdcall *fx_create_device_fn)(void *self, const GUID *guid, void **device, void *outer);
typedef HRESULT(__stdcall *fx_get_state_fn)(void *self, DWORD size, void *data);
typedef HRESULT(__stdcall *fx_get_data_fn)(void *self, DWORD object_size, void *data, DWORD *count, DWORD flags);

static void *g_fx_input_devices[3]; /* keyboard, mouse, something else */

FX_API unsigned long fx_pad_read(unsigned index, void *state)
{
    return XInputGetState(index, state);
}

FX_API int fx_input_open(void)
{
    static const GUID *const kinds[3] = {&FX_GUID_Keyboard, &FX_GUID_Mouse, &FX_GUID_Other};
    void *di = NULL;
    unsigned i;

    if (FAILED(DirectInput8Create(GetModuleHandleW(NULL), 0x0800, &FX_GUID_Other, &di, NULL)) || !di) {
        return -1;
    }
    for (i = 0; i < 3; ++i) {
        fx_create_device_fn create = (fx_create_device_fn)(*(void ***)di)[3];
        if (FAILED(create(di, kinds[i], &g_fx_input_devices[i], NULL)) || !g_fx_input_devices[i]) {
            return -2;
        }
    }
    return 0;
}

FX_API long fx_input_state(unsigned device, void *data, unsigned size)
{
    void *d = device < 3 ? g_fx_input_devices[device] : NULL;
    return d ? (long)((fx_get_state_fn)(*(void ***)d)[9])(d, size, data) : -1;
}

FX_API long fx_input_events(unsigned device, void *data, unsigned object_size, unsigned long *count, unsigned flags)
{
    void *d = device < 3 ? g_fx_input_devices[device] : NULL;
    return d ? (long)((fx_get_data_fn)(*(void ***)d)[10])(d, object_size, data, count, flags) : -1;
}

typedef struct fx_ui_manager_object {
    void *const *vtbl;
    unsigned char other[0x2C];
    unsigned char mode, requested, pending; /* +30h, +31h, +32h */
    unsigned char rest[0x1D];
} fx_ui_manager_object;

static void fx_ui_method(void)
{
}

static void *const g_fx_ui_vtbl[2] = {(void *)fx_ui_method, (void *)fx_ui_method};
static fx_ui_manager_object g_fx_ui_object = {g_fx_ui_vtbl, {0}, 0, 0, 0, {0}};
void *g_fx_ui_manager; /* the engine's variable: NULL until the manager exists */

#pragma pack(push, 1)
typedef struct fx_pad_code_bytes {
    unsigned char head[7]; /* mov cl,[eax+0D0h] / mov eax,[imm32] */
    void **pointer;
    unsigned char tail[8]; /* mov [eax+31h],cl / mov byte [eax+32h],1 / ret */
} fx_pad_code_bytes;
#pragma pack(pop)

#if defined(_MSC_VER)
#pragma section(".fxpad", execute, read)
__declspec(allocate(".fxpad")) const fx_pad_code_bytes fx_pad_code = {
#else
const fx_pad_code_bytes fx_pad_code __attribute__((section(".fxpad,\"xr\"#"), used)) = {
#endif
    {0x8A, 0x88, 0xD0, 0x00, 0x00, 0x00, 0xA1}, &g_fx_ui_manager, {0x88, 0x48, 0x31, 0xC6, 0x40, 0x32, 0x01, 0xC3}};

/* The manager object (64 bytes). create: the engine now has one. */
FX_API unsigned char *fx_ui_manager(int create)
{
    if (create) {
        g_fx_ui_manager = &g_fx_ui_object;
    }
    return fx_pad_code.pointer == &g_fx_ui_manager ? (unsigned char *)&g_fx_ui_object : NULL;
}

/* ---- files -----------------------------------------------------------------------------
 * The engine's file class: CreateFileW (or CreateFileA), size, read, close. */
FX_API long fx_read_file(int wide, const wchar_t *path, char *out, unsigned cap)
{
    HANDLE h;
    LARGE_INTEGER size;
    DWORD got = 0;
    long result = -1;

    if (wide) {
        /* as the engine's file class does: the specific right, shared for reading and writing */
        h = CreateFileW(path, FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    } else {
        char narrow[MAX_PATH];
        if (!WideCharToMultiByte(CP_ACP, 0, path, -1, narrow, sizeof(narrow), NULL, NULL)) {
            return -1;
        }
        h = CreateFileA(narrow, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    if (GetFileSizeEx(h, &size) && size.QuadPart <= (LONGLONG)cap && ReadFile(h, out, (DWORD)size.QuadPart, &got, NULL) &&
        got == (DWORD)size.QuadPart) {
        result = (long)got;
    }
    CloseHandle(h);
    return result;
}

/* Appending, as the engine's file class opens a file for it: an existing file, for writing. */
FX_API int fx_append_file(const wchar_t *path, const char *data, unsigned size)
{
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    int ok;

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    ok = WriteFile(h, data, size, &written, NULL) && written == size;
    CloseHandle(h);
    return ok ? 0 : -1;
}

FX_API int fx_write_file(const wchar_t *path, const char *data, unsigned size)
{
    HANDLE h = CreateFileW(path, FILE_WRITE_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    int ok;

    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    ok = WriteFile(h, data, size, &written, NULL) && written == size;
    CloseHandle(h);
    return ok ? 0 : -1;
}

/* ---- top level resources ----------------------------------------------------------------
 * The class that loads a texture's top mip level from its own file. Each such
 * object is linked to one texture; Load and Unload follow that link and write
 * to the texture and its record (tex_link.h). The two methods begin like the
 * engine's and sit in slots 26 and 27 of the class's table, as there. */
#ifdef FX_NO_TEXTURES
#include "../unit/tex_blobs.h"
#endif

#if defined(_MSC_VER)
#pragma section(".fxtop", execute, read)
__declspec(allocate(".fxtop")) static const unsigned char fx_top_load_code[] = {TOP_BLOB_LOAD};
__declspec(allocate(".fxtop")) static const unsigned char fx_top_unload_code[] = {TOP_BLOB_UNLOAD};
#else
static const unsigned char fx_top_load_code[] __attribute__((section(".fxtop,\"xr\"#"), used)) = {TOP_BLOB_LOAD};
static const unsigned char fx_top_unload_code[] __attribute__((section(".fxtop,\"xr\"#"), used)) = {TOP_BLOB_UNLOAD};
#endif

static void __fastcall fx_top_unused_method(void *self, void *unused)
{
    (void)self;
    (void)unused;
}

#define FX_TOP_UNUSED (void *)fx_top_unused_method
static void *const g_fx_top_vtbl[28] = {
    FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED,
    FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED,
    FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED,
    FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED, FX_TOP_UNUSED,
    (void *)fx_top_load_code,   /* 26 Load   */
    (void *)fx_top_unload_code, /* 27 Unload */
};

typedef struct fx_top_record {
    unsigned char other[0x20];
    LONG counter; /* +20h */
    unsigned char rest[0x1C];
} fx_top_record;

typedef struct fx_top_texture {
    unsigned char other[0x28];
    LONG added;            /* +28h */
    unsigned reserved;     /* +2Ch */
    fx_top_record *record; /* +30h */
    unsigned char more[0x14];
    unsigned users;        /* +48h */
    unsigned char rest[0x0C];
} fx_top_texture;

typedef struct fx_top_resource {
    void *const *vtbl;         /* +00h */
    LONG loads;                /* +04h */
    LONG unloads;              /* +08h */
    unsigned char other[0x14];
    unsigned name_low;         /* +20h */
    unsigned name_high;        /* +24h */
    unsigned size;             /* +28h */
    unsigned flags;            /* +2Ch */
    fx_top_texture *link;      /* +30h */
    unsigned char valid;       /* +34h */
    unsigned char rest[0x0B];
} fx_top_resource;

typedef unsigned char(__fastcall *fx_top_load_fn)(fx_top_resource *, void *, const void *, unsigned);
typedef void(__fastcall *fx_top_unload_fn)(fx_top_resource *, void *);

static fx_top_resource g_fx_tops[4];
static fx_top_texture g_fx_top_textures[4];
static fx_top_record g_fx_top_records[4];

/* (Re)creates top level resource number `index`, named by the two halves of
 * the file's name hash. link: 0 = to a texture with a record, as it should
 * be; 1 = to a texture that was destroyed (its memory is gone); 2 = to a
 * texture whose record was destroyed. */
FX_API void *fx_top_create(unsigned index, unsigned name_low, unsigned name_high, int link)
{
    fx_top_resource *r = &g_fx_tops[index & 3];
    fx_top_texture *t = &g_fx_top_textures[index & 3];
    void *gone = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (gone) {
        VirtualFree(gone, 0, MEM_RELEASE);
    }
    memset(r, 0, sizeof(*r));
    memset(t, 0, sizeof(*t));
    memset(&g_fx_top_records[index & 3], 0, sizeof(fx_top_record));
    g_fx_top_records[index & 3].counter = 100;
    t->users = 1;
    t->record = link == 2 ? (fx_top_record *)gone : &g_fx_top_records[index & 3];
    r->vtbl = g_fx_top_vtbl;
    r->name_low = name_low;
    r->name_high = name_high;
    r->size = 4096;
    r->link = link == 1 ? (fx_top_texture *)gone : t;
    r->valid = 1;
    return r;
}

/* The engine hands a top level file to its resource. Returns what Load returns: 0 = added. */
FX_API int fx_top_load(void *resource, const void *file, unsigned size)
{
    fx_top_resource *r = (fx_top_resource *)resource;
    return ((fx_top_load_fn)r->vtbl[26])(r, NULL, file, size);
}

FX_API void fx_top_unload(void *resource)
{
    fx_top_resource *r = (fx_top_resource *)resource;
    ((fx_top_unload_fn)r->vtbl[27])(r, NULL);
}

/* out: calls that reached Load, calls that reached Unload, top levels added
 * to the linked texture, the record's counter, the resource's size field. */
FX_API void fx_top_state(unsigned index, long out[5])
{
    out[0] = g_fx_tops[index & 3].loads;
    out[1] = g_fx_tops[index & 3].unloads;
    out[2] = g_fx_top_textures[index & 3].added;
    out[3] = g_fx_top_records[index & 3].counter;
    out[4] = (long)g_fx_tops[index & 3].size;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
