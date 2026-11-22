/* Audio: keeps XAudio2 2.7 loaded, and an optional diagnostic trace.
 *
 * The game's sound engine (Ubisoft DARE) renders through XAudio2 2.7, which
 * it creates with CoCreateInstance; a second instance serves voice chat.
 *
 * Keeping the DLL loaded ([Audio] KeepXAudio2Loaded). XAudio2 2.7 has a
 * known defect (see "Known Issues: XAudio 2.7" on the DirectX SDK blog): in
 * some situations, normally at exit, XAudio2_7.dll is unloaded before the
 * XAudio2 objects are completely destroyed, and the next call into it is an
 * access violation in "XAudio2_7.dll_unloaded". The published remedy is for
 * the application to hold its own reference to the DLL while it uses XAudio2;
 * here the module is pinned the moment the engine creates its first XAudio2
 * object.
 *
 * The trace ([Debug] TraceAudio) watches that object and reports, in the log:
 *   - the output device's format (channels, sample rate) - never its name,
 *   - how the mastering and submix voices are set up,
 *   - every distinct source-voice format, i.e. the sample rates of the sounds
 *     that are actually played,
 *   - the first output matrices, which show how channels are folded down.
 *
 * XAudio2 2.7's header is not part of current Windows SDKs, so the few
 * structures and virtual-table slots needed are spelled out here (they match
 * the June 2010 DirectX SDK). Voices are not COM objects: their table starts
 * with GetVoiceDetails, not QueryInterface.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "audio.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "pe_image.h"
#include "vtable.h"

/* IXAudio2 */
#define SLOT_ENGINE_GET_DEVICE_DETAILS 4
#define SLOT_ENGINE_CREATE_SOURCE_VOICE 8
#define SLOT_ENGINE_CREATE_SUBMIX_VOICE 9
#define SLOT_ENGINE_CREATE_MASTERING_VOICE 10
/* IXAudio2Voice */
#define SLOT_VOICE_GET_DETAILS 0
#define SLOT_VOICE_SET_OUTPUT_MATRIX 16

#define MAX_FORMATS 48
#define MAX_MASTERS 4
#define MAX_MATRIX_LOGS 40
#define MAX_SUBMIX_LOGS 48

#pragma pack(push, 1)
typedef struct xa_format { /* WAVEFORMATEXTENSIBLE */
    WORD tag, channels;
    DWORD rate, bytes_per_second;
    WORD block_align, bits, extra_size;
    WORD valid_bits;
    DWORD channel_mask;
    GUID sub_format;
} xa_format;

typedef struct xa_device_details {
    WCHAR device_id[256];
    WCHAR display_name[256];
    DWORD role;
    xa_format format;
} xa_device_details;

typedef struct xa_voice_details {
    UINT32 flags, channels, rate;
} xa_voice_details;

typedef struct xa_sends {
    UINT32 count;
    void *sends;
} xa_sends;
#pragma pack(pop)

typedef HRESULT(WINAPI *co_create_fn)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID *);
typedef HRESULT(__stdcall *get_device_details_fn)(void *self, UINT32 index, xa_device_details *details);
typedef HRESULT(__stdcall *create_source_fn)(void *self, void **voice, const xa_format *format, UINT32 flags,
                                             float max_ratio, void *callback, const xa_sends *sends,
                                             const void *effects);
typedef HRESULT(__stdcall *create_submix_fn)(void *self, void **voice, UINT32 channels, UINT32 rate, UINT32 flags,
                                             UINT32 stage, const xa_sends *sends, const void *effects);
typedef HRESULT(__stdcall *create_master_fn)(void *self, void **voice, UINT32 channels, UINT32 rate, UINT32 flags,
                                             UINT32 device, const void *effects);
typedef void(__stdcall *get_voice_details_fn)(void *self, xa_voice_details *details);
typedef HRESULT(__stdcall *set_output_matrix_fn)(void *self, void *destination, UINT32 source_channels,
                                                 UINT32 destination_channels, const float *matrix, UINT32 set);

typedef struct format_stat {
    WORD tag, channels, bits;
    DWORD rate;
    unsigned long count;
    float max_ratio;
} format_stat;

/* {5a508685-a254-4fba-9b82-9a24b00306af} XAudio2 2.7, {db05ea35-...} its debug build */
static const GUID CLSID_XAUDIO2_27 = {0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
static const GUID CLSID_XAUDIO2_27_DEBUG = {0xdb05ea35, 0x0329, 0x4d4b,
                                            {0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52}};

static SRWLOCK g_lock = SRWLOCK_INIT;
static co_create_fn g_orig_co_create;
static vt_hook g_source_hooks[VT_MAX_TABLES];
static vt_hook g_submix_hooks[VT_MAX_TABLES];
static vt_hook g_master_hooks[VT_MAX_TABLES];
static vt_hook g_matrix_hooks[VT_MAX_TABLES];
static format_stat g_formats[MAX_FORMATS];
static unsigned g_format_count;
static unsigned long g_source_total, g_source_other;
static void *g_masters[MAX_MASTERS];
static unsigned g_master_count;
static unsigned g_matrix_logs, g_submix_logs;
static unsigned g_engines;
static int g_pin, g_trace, g_pinned;

static const char *tag_name(WORD tag)
{
    switch (tag) {
    case 0x0001:
        return "PCM";
    case 0x0002:
        return "MS ADPCM";
    case 0x0003:
        return "float";
    case 0x0166:
        return "XMA2";
    case 0xFFFE:
        return "extensible";
    default:
        return "other";
    }
}

static int is_master(const void *voice)
{
    unsigned i;
    for (i = 0; i < g_master_count; ++i) {
        if (g_masters[i] == voice) {
            return 1;
        }
    }
    return 0;
}

/* ---- voices --------------------------------------------------------------------- */

static HRESULT __stdcall hook_set_output_matrix(void *self, void *destination, UINT32 source_channels,
                                                UINT32 destination_channels, const float *matrix, UINT32 set)
{
    set_output_matrix_fn original;
    int log_it = 0, to_master;

    AcquireSRWLockExclusive(&g_lock);
    original = (set_output_matrix_fn)vt_original(g_matrix_hooks, VT_OF(self));
    to_master = destination == NULL || is_master(destination);
    /* Interesting are the matrices that change the channel count and the ones
     * that feed the mastering voice. */
    if (g_matrix_logs < MAX_MATRIX_LOGS && matrix && (to_master || source_channels != destination_channels) &&
        source_channels >= 1 && source_channels <= 8 && destination_channels >= 1 && destination_channels <= 8) {
        ++g_matrix_logs;
        log_it = 1;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (log_it) {
        char text[8 * 8 * 6 + 8 * 4 + 8];
        size_t pos = 0;
        UINT32 d, s;
        /* Row = one destination channel; columns = source channels. */
        for (d = 0; d < destination_channels; ++d) {
            for (s = 0; s < source_channels && pos + 8 < sizeof(text); ++s) {
                int n = snprintf(text + pos, sizeof(text) - pos, s ? " %.2f" : "%.2f",
                                 (double)matrix[source_channels * d + s]);
                if (n > 0) {
                    pos += (size_t)n;
                }
            }
            if (d + 1 < destination_channels && pos + 4 < sizeof(text)) {
                memcpy(text + pos, " | ", 3);
                pos += 3;
            }
        }
        text[pos] = '\0';
        LOGI("audio: output matrix %u -> %u channels%s: %s", (unsigned)source_channels,
             (unsigned)destination_channels, to_master ? " (to the mastering voice)" : "", text);
    }
    return original ? original(self, destination, source_channels, destination_channels, matrix, set) : E_FAIL;
}

static void watch_voice_class(void *voice)
{
    AcquireSRWLockExclusive(&g_lock);
    vt_hook_slot(g_matrix_hooks, VT_OF(voice), SLOT_VOICE_SET_OUTPUT_MATRIX, (void *)hook_set_output_matrix);
    ReleaseSRWLockExclusive(&g_lock);
}

/* ---- engine --------------------------------------------------------------------- */

static HRESULT __stdcall hook_create_source(void *self, void **voice, const xa_format *format, UINT32 flags,
                                            float max_ratio, void *callback, const xa_sends *sends,
                                            const void *effects)
{
    create_source_fn original;
    format_stat first;
    unsigned long total;
    int is_new = 0;
    unsigned i;
    HRESULT hr;

    AcquireSRWLockShared(&g_lock);
    original = (create_source_fn)vt_original(g_source_hooks, VT_OF(self));
    ReleaseSRWLockShared(&g_lock);
    if (!original) {
        return E_FAIL;
    }
    hr = original(self, voice, format, flags, max_ratio, callback, sends, effects);
    if (FAILED(hr) || !format) {
        return hr;
    }

    memset(&first, 0, sizeof(first));
    AcquireSRWLockExclusive(&g_lock);
    total = ++g_source_total;
    for (i = 0; i < g_format_count; ++i) {
        if (g_formats[i].tag == format->tag && g_formats[i].channels == format->channels &&
            g_formats[i].rate == format->rate && g_formats[i].bits == format->bits) {
            break;
        }
    }
    if (i == g_format_count && g_format_count < MAX_FORMATS) {
        g_formats[i].tag = format->tag;
        g_formats[i].channels = format->channels;
        g_formats[i].rate = format->rate;
        g_formats[i].bits = format->bits;
        ++g_format_count;
        is_new = 1;
    }
    if (i < g_format_count) {
        ++g_formats[i].count;
        if (max_ratio > g_formats[i].max_ratio) {
            g_formats[i].max_ratio = max_ratio;
        }
        first = g_formats[i];
    } else {
        ++g_source_other;
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (total == 250 || total == 1000 || total == 4000 || total == 16000 || total == 64000) {
        audio_log_stats(); /* the game is often ended without a clean shutdown */
    }
    if (is_new) {
        LOGI("audio: first source voice with %s, %u channel%s, %lu Hz, %u bit (flags 0x%lX, max pitch ratio %.1f)",
             tag_name(first.tag), (unsigned)first.channels, first.channels == 1 ? "" : "s",
             (unsigned long)first.rate, (unsigned)first.bits, (unsigned long)flags, (double)max_ratio);
    }
    if (voice && *voice) {
        watch_voice_class(*voice);
    }
    return hr;
}

static HRESULT __stdcall hook_create_submix(void *self, void **voice, UINT32 channels, UINT32 rate, UINT32 flags,
                                            UINT32 stage, const xa_sends *sends, const void *effects)
{
    create_submix_fn original;
    int log_it;
    HRESULT hr;

    AcquireSRWLockExclusive(&g_lock);
    original = (create_submix_fn)vt_original(g_submix_hooks, VT_OF(self));
    log_it = g_submix_logs < MAX_SUBMIX_LOGS;
    if (log_it) {
        ++g_submix_logs;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!original) {
        return E_FAIL;
    }
    hr = original(self, voice, channels, rate, flags, stage, sends, effects);
    if (log_it) {
        LOGI("audio: submix voice: %u channels, %lu Hz, stage %lu, %s effects, %lu send%s (result 0x%08lX)",
             (unsigned)channels, (unsigned long)rate, (unsigned long)stage, effects ? "with" : "no",
             (unsigned long)(sends ? sends->count : 0), sends && sends->count == 1 ? "" : "s", (unsigned long)hr);
    }
    if (SUCCEEDED(hr) && voice && *voice) {
        watch_voice_class(*voice);
    }
    return hr;
}

static void log_device(void *engine, UINT32 index)
{
    static xa_device_details details; /* 1 KB: keep it off the stack; guarded by the caller being rare */
    get_device_details_fn get_details = (get_device_details_fn)VT_OF(engine)[SLOT_ENGINE_GET_DEVICE_DETAILS];
    HRESULT hr;

    memset(&details, 0, sizeof(details));
    hr = get_details(engine, index, &details);
    if (FAILED(hr)) {
        LOGI("audio: output device %lu: details not available (0x%08lX)", (unsigned long)index, (unsigned long)hr);
        return;
    }
    /* The device name is deliberately not logged. */
    LOGI("audio: output device %lu: %u channels (mask 0x%lX), %lu Hz, %u bit%s", (unsigned long)index,
         (unsigned)details.format.channels, (unsigned long)details.format.channel_mask,
         (unsigned long)details.format.rate, (unsigned)details.format.bits,
         (details.role & 0x8) ? ", default game device" : "");
}

static HRESULT __stdcall hook_create_master(void *self, void **voice, UINT32 channels, UINT32 rate, UINT32 flags,
                                            UINT32 device, const void *effects)
{
    create_master_fn original;
    HRESULT hr;

    AcquireSRWLockShared(&g_lock);
    original = (create_master_fn)vt_original(g_master_hooks, VT_OF(self));
    ReleaseSRWLockShared(&g_lock);
    if (!original) {
        return E_FAIL;
    }
    log_device(self, device);
    hr = original(self, voice, channels, rate, flags, device, effects);
    LOGI("audio: mastering voice requested with %u channels, %lu Hz, device %lu, %s effects (result 0x%08lX; "
         "0 means 'use the device's value')",
         (unsigned)channels, (unsigned long)rate, (unsigned long)device, effects ? "with" : "no", (unsigned long)hr);
    if (SUCCEEDED(hr) && voice && *voice) {
        xa_voice_details details;
        memset(&details, 0, sizeof(details));
        ((get_voice_details_fn)VT_OF(*voice)[SLOT_VOICE_GET_DETAILS])(*voice, &details);
        LOGI("audio: mastering voice input is %u channels at %lu Hz; XAudio2 converts that to the device format",
             (unsigned)details.channels, (unsigned long)details.rate);
        AcquireSRWLockExclusive(&g_lock);
        if (g_master_count < MAX_MASTERS) {
            g_masters[g_master_count++] = *voice;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    return hr;
}

static void watch_engine(void *engine)
{
    int ok;
    unsigned number;

    AcquireSRWLockExclusive(&g_lock);
    ok = vt_hook_slot(g_source_hooks, VT_OF(engine), SLOT_ENGINE_CREATE_SOURCE_VOICE, (void *)hook_create_source) &&
         vt_hook_slot(g_submix_hooks, VT_OF(engine), SLOT_ENGINE_CREATE_SUBMIX_VOICE, (void *)hook_create_submix) &&
         vt_hook_slot(g_master_hooks, VT_OF(engine), SLOT_ENGINE_CREATE_MASTERING_VOICE,
                      (void *)hook_create_master);
    number = ++g_engines;
    ReleaseSRWLockExclusive(&g_lock);
    if (ok) {
        LOGI("audio: trace attached to XAudio2 engine #%u (the game creates one for sound and one for voice chat)",
             number);
    } else {
        LOGE("audio: could not attach the trace to the XAudio2 engine");
    }
}

/* Pins the module that implements the object, found through the address of
 * its first virtual function (works for Microsoft's DLL and for Wine's). */
static void pin_implementation(void *engine)
{
    HMODULE module = NULL;
    char path[MAX_PATH];
    int first;

    AcquireSRWLockExclusive(&g_lock);
    first = !g_pinned;
    g_pinned = 1;
    ReleaseSRWLockExclusive(&g_lock);
    if (!first) {
        return;
    }
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCSTR)VT_OF(engine)[0], &module) ||
        !module) {
        LOGE("audio: could not pin the XAudio2 module (error %lu)", (unsigned long)GetLastError());
        return;
    }
    path[0] = 0;
    GetModuleFileNameA(module, path, sizeof(path));
    path[sizeof(path) - 1] = 0;
    LOGI("audio: %s will stay loaded until the game exits", path[0] ? log_basename(path) : "the XAudio2 module");
}

static HRESULT WINAPI hook_co_create_instance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid,
                                              LPVOID *object)
{
    HRESULT hr = g_orig_co_create(clsid, outer, context, iid, object);

    if (clsid && (IsEqualGUID(clsid, &CLSID_XAUDIO2_27) || IsEqualGUID(clsid, &CLSID_XAUDIO2_27_DEBUG))) {
        if (SUCCEEDED(hr) && object && *object) {
            if (g_pin) {
                pin_implementation(*object);
            }
            if (g_trace) {
                watch_engine(*object);
            }
        } else {
            LOGE("audio: the game could not create XAudio2 2.7 (0x%08lX); "
                 "is the DirectX June 2010 runtime installed?",
                 (unsigned long)hr);
        }
    }
    return hr;
}

unsigned audio_install(HMODULE engine, const fc3pp_config *cfg)
{
    void **slot = pei_find_import(engine, "ole32.dll", "CoCreateInstance");
    void *previous = NULL;

    g_pin = cfg->keep_xaudio2_loaded;
    g_trace = cfg->trace_audio;
    if ((!g_pin && !g_trace) || !slot || *slot == (void *)hook_co_create_instance) {
        return 0;
    }
    if (pei_hook_import(engine, "ole32.dll", "CoCreateInstance", (void *)hook_co_create_instance, &previous) != 1 ||
        !previous) {
        LOGE("audio: could not hook CoCreateInstance");
        return 0;
    }
    g_orig_co_create = (co_create_fn)previous;
    if (g_trace) {
        LOGI("audio: trace enabled");
    }
    return 1;
}

void audio_log_stats(void)
{
    unsigned i;

    if (!g_trace || !g_source_total) {
        return;
    }
    LOGI("audio: %lu source voices created in %u format%s", g_source_total, g_format_count,
         g_format_count == 1 ? "" : "s");
    for (i = 0; i < g_format_count; ++i) {
        LOGI("audio:   %7lu x %s, %u ch, %lu Hz, %u bit", g_formats[i].count, tag_name(g_formats[i].tag),
             (unsigned)g_formats[i].channels, (unsigned long)g_formats[i].rate, (unsigned)g_formats[i].bits);
    }
    if (g_source_other) {
        LOGI("audio:   %7lu x further formats (table full)", g_source_other);
    }
}
