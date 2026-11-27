/* The in-game settings menu.
 *
 * Opened with a key (F8 by default) or by holding both thumbsticks in on a
 * gamepad. It is drawn by overlay.c at the end of each frame and operated
 * with the arrow keys or the D-pad, which suits a handheld as well as a desk.
 *
 * While it is open the game must not see the same input. The engine reads the
 * gamepad through XInputGetState (imported by ordinal) and the keyboard and
 * mouse through DirectInput devices; both are wrapped so that the engine gets
 * "nothing pressed" until the menu has closed and the keys are up again.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#define COBJMACROS
#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "fc3pp.h"
#include "ini.h"
#include "log.h"
#include "menu_draw.h"
#include "menu_model.h"
#include "cpu_topology.h"
#include "gfx_preset.h"
#include "gfx_profile.h"
#include "overlay.h"
#include "padswitch.h"
#include "pe_image.h"
#include "texguard.h"
#include "vtable.h"

/* ---- entries --------------------------------------------------------------------- */

enum {
    ID_NONE = 0,
    ID_STATUS_VERSION,
    ID_STATUS_PROCESSOR,
    ID_STATUS_MEMORY,
    ID_STATUS_TEXTURES,
    ID_STATUS_DISTANCE,
    ID_MAX_CORES,
    ID_USE_SMT,
    ID_CPU_MODE,
    ID_SPARE_FAST,
    ID_RESTORE_FULLSCREEN,
    ID_DPI_AWARE,
    ID_TEX_MISMATCHED,
    ID_TEX_REPLACED,
    ID_TEX_SHARED,
    ID_KEEP_XAUDIO,
    ID_AUTO_SWITCH,
    ID_DRAW_DISTANCE,
    ID_MENU_SCALE,
    ID_CLOSE
};

static const char *const MODE_NAMES[] = {"off", "engine", "process"};
static const char *const SPARE_NAMES[] = {"Auto", "0", "1", "2", "3", "4"};
static const char *const DISTANCE_NAMES[] = {"Original", "Far", "Very far"};
static const char *const SCALE_NAMES[] = {"100 %", "125 %", "150 %", "175 %", "200 %"};
#define SCALE_STEPS 5

#define HEADING(text) {MM_HEADING, ID_NONE, text, NULL, 0, 0, 0, 0, NULL, 0, 0, "", 0}
#define INFO(id, text) {MM_INFO, id, text, NULL, 0, 0, 0, 0, NULL, 0, 0, "", 0}
#define TOGGLE(id, text, help) {MM_TOGGLE, id, text, help, 0, 0, 1, 1, NULL, 0, MM_RESTART, "", 0}

static mm_item g_items[] = {
    HEADING("Status"),
    INFO(ID_STATUS_VERSION, "Patch"),
    INFO(ID_STATUS_PROCESSOR, "Processor"),
    INFO(ID_STATUS_MEMORY, "Memory"),
    INFO(ID_STATUS_TEXTURES, "Texture levels left out"),
    INFO(ID_STATUS_DISTANCE, "Draw distance in use"),
    HEADING("Processor"),
    {MM_NUMBER, ID_MAX_CORES, "Cores for the game",
     "How many physical cores the game may use. 0 means all of them. 4 to 6 suits this engine best.", 0, 0,
     FC3PP_MAX_ENGINE_CPUS, 1, NULL, 0, MM_RESTART, "", 0},
    TOGGLE(ID_USE_SMT, "Both threads of each core",
           "Off: one thread per core, which avoids two of the game's workers sharing a core."),
    {MM_CHOICE, ID_CPU_MODE, "Mode",
     "engine: the game's own threads use the chosen cores. process: drivers and sound are limited to them too. "
     "off: the game's own handling.",
     1, 0, 2, 1, MODE_NAMES, 3, MM_RESTART, "", 0},
    {MM_CHOICE, ID_SPARE_FAST, "Fast cores kept free",
     "Where cores differ in speed (ARM handhelds), the game's workers stay off this many of the fastest "
     "cores, so its main and render threads get them. Auto: 2 there, 0 elsewhere.",
     0, 0, CPU_SPARE_MAX + 1, 1, SPARE_NAMES, CPU_SPARE_MAX + 2, MM_RESTART, "", 0},
    HEADING("Display"),
    TOGGLE(ID_RESTORE_FULLSCREEN, "Fullscreen after Alt+Tab",
           "Return to fullscreen when you switch back to the game, instead of staying in a window."),
    TOGGLE(ID_DPI_AWARE, "DPI aware", "Stops Windows from scaling the game's window on displays set above 100 %."),
    {MM_CHOICE, ID_DRAW_DISTANCE, "Draw distance",
     "Shows detailed models, trees, terrain and sun shadows further away. Costs frame rate, 'Very far' a lot. "
     "Read once when the game starts: you will not see it change here.",
     0, 0, GP_LEVELS - 1, 1, DISTANCE_NAMES, GP_LEVELS, MM_RESTART, "", 0},
    HEADING("Controls"),
    {MM_TOGGLE, ID_AUTO_SWITCH, "Gamepad or keyboard by use",
     "The game follows whichever you touch: gamepad, or keyboard and mouse. Works at once, no restart.", 0, 0, 1, 1,
     NULL, 0, 0, "", 0},
    HEADING("Texture mods"),
    TOGGLE(ID_TEX_MISMATCHED, "Leave out levels that do not fit",
           "Texture mods crash the game without this, unless mip0 loading is switched off in GamerProfile.xml."),
    TOGGLE(ID_TEX_REPLACED, "Leave out originals of replaced textures",
           "Keeps the game's original close-up texture from showing through a texture a mod has repainted."),
    TOGGLE(ID_TEX_SHARED, "Leave out levels several textures share",
           "Some texture mods make several textures use one close-up texture file. The game is not built for "
           "that and can crash after hours of play."),
    HEADING("Sound"),
    TOGGLE(ID_KEEP_XAUDIO, "Keep the sound library loaded",
           "Guards against a known defect of XAudio2 2.7 that can crash the game."),
    HEADING("This menu"),
    {MM_CHOICE, ID_MENU_SCALE, "Size",
     "Larger text for a handheld or a television across the room. Works at once.", 0, 0, SCALE_STEPS - 1, 1,
     SCALE_NAMES, SCALE_STEPS, 0, "", 0},
    HEADING(""),
    {MM_ACTION, ID_CLOSE, "Close", "", 0, 0, 0, 0, NULL, 0, 0, "", 0},
};

typedef struct xi_state {
    DWORD packet;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
} xi_state;

#define XI_UP 0x0001u
#define XI_DOWN 0x0002u
#define XI_LEFT 0x0004u
#define XI_RIGHT 0x0008u
#define XI_LEFT_THUMB 0x0040u
#define XI_RIGHT_THUMB 0x0080u
#define XI_A 0x1000u
#define XI_B 0x2000u
#define XI_STICK 16000
#define XI_COMBO_DEFAULT (XI_LEFT_THUMB | XI_RIGHT_THUMB)

static mm_menu g_menu = {FC3PP_NAME " " FC3PP_VERSION, g_items, sizeof(g_items) / sizeof(g_items[0]), 0, 0, 0};
static wchar_t g_ini_path[MAX_PATH + 40];
static unsigned g_key = VK_F8;
static int g_enabled;
static int g_auto_switch; /* asked for in the INI file */
static unsigned g_combo = XI_COMBO_DEFAULT; /* gamepad buttons that open the menu when held together */
static int g_assume_focus;                  /* testing aid: read the gamepad without the focus */

static volatile LONG g_open;          /* read by the input hooks on other threads */
static volatile LONG g_hold_keyboard; /* closed, but keys that operated it are still down */
static volatile LONG g_hold_gamepad;

static void set_value(int id, int value)
{
    mm_item *it = mm_find(&g_menu, id);
    if (it) {
        mm_set(it, value); /* what the game was started with */
    }
}

void menu_set_status(menu_status which, const char *text)
{
    mm_item *it = mm_find(&g_menu, which == MENU_STATUS_PROCESSOR ? ID_STATUS_PROCESSOR : ID_STATUS_MEMORY);
    if (it && text) {
        snprintf(it->text, sizeof(it->text), "%s", text);
        it->text[sizeof(it->text) - 1] = '\0';
    }
}

void menu_init(const fc3pp_config *cfg, const wchar_t *ini_path)
{
    mm_item *it;

    g_enabled = cfg->menu_enabled;
    if (cfg->menu_key) {
        g_key = cfg->menu_key;
    }
    if (ini_path) {
        wcsncpy(g_ini_path, ini_path, sizeof(g_ini_path) / sizeof(g_ini_path[0]) - 1);
    }
    set_value(ID_MAX_CORES, cfg->max_cores);
    set_value(ID_USE_SMT, cfg->use_smt);
    set_value(ID_CPU_MODE, (int)cfg->mode);
    set_value(ID_SPARE_FAST, cfg->spare_fast_cores < 0 ? 0 : cfg->spare_fast_cores + 1);
    set_value(ID_RESTORE_FULLSCREEN, cfg->restore_fullscreen);
    set_value(ID_DPI_AWARE, cfg->high_dpi_aware);
    set_value(ID_TEX_MISMATCHED, cfg->texture_guard);
    set_value(ID_TEX_REPLACED, cfg->texture_scan);
    set_value(ID_TEX_SHARED, cfg->texture_shared);
    set_value(ID_KEEP_XAUDIO, cfg->keep_xaudio2_loaded);
    set_value(ID_AUTO_SWITCH, cfg->auto_switch_input);
    set_value(ID_DRAW_DISTANCE, cfg->draw_distance);
    g_auto_switch = cfg->auto_switch_input;
    g_combo = cfg->menu_pad & 0xFFFFu;
    g_assume_focus = cfg->input_without_focus;
    set_value(ID_MENU_SCALE, (cfg->menu_scale - 100 + 12) / 25);
    overlay_set_scale(cfg->menu_scale);
    it = mm_find(&g_menu, ID_STATUS_VERSION);
    if (it) {
        snprintf(it->text, sizeof(it->text), "%s", FC3PP_VERSION);
    }
    if (cfg->menu_open_at_start) {
        mm_open(&g_menu, 1);
        InterlockedExchange(&g_open, 1);
    }
}

void menu_set_unavailable(void)
{
    if (g_enabled) {
        g_enabled = 0;
        mm_open(&g_menu, 0);
        InterlockedExchange(&g_open, 0);
        LOGI("menu: not available with this renderer (the in-game menu needs the DirectX 11 executable); "
             "the settings are in the INI file");
    }
}

int menu_is_open(void)
{
    return g_open != 0;
}

/* Writes one changed setting back to the INI file, keeping its comments. */
static void save(int id)
{
    const mm_item *it = mm_find(&g_menu, id);
    const wchar_t *section = NULL, *key = NULL;
    wchar_t value[32];

    if (!it || !g_ini_path[0]) {
        return;
    }
    _snwprintf(value, 32, L"%d", it->value);
    switch (id) {
    case ID_MAX_CORES:
        section = L"CPU", key = L"MaxCores";
        break;
    case ID_USE_SMT:
        section = L"CPU", key = L"UseSMT";
        break;
    case ID_CPU_MODE:
        section = L"CPU", key = L"Mode";
        _snwprintf(value, 32, L"%hs", MODE_NAMES[it->value >= 0 && it->value < 3 ? it->value : 1]);
        break;
    case ID_SPARE_FAST:
        section = L"CPU", key = L"SpareFastCores";
        if (it->value <= 0) {
            _snwprintf(value, 32, L"auto");
        } else {
            _snwprintf(value, 32, L"%d", it->value - 1);
        }
        break;
    case ID_RESTORE_FULLSCREEN:
        section = L"Display", key = L"RestoreFullscreenOnFocus";
        break;
    case ID_DPI_AWARE:
        section = L"Display", key = L"HighDpiAware";
        break;
    case ID_TEX_MISMATCHED:
        section = L"Textures", key = L"SkipMismatchedMip0";
        break;
    case ID_TEX_REPLACED:
        section = L"Textures", key = L"SkipReplacedTextureMip0";
        break;
    case ID_TEX_SHARED:
        section = L"Textures", key = L"SkipSharedMip0";
        break;
    case ID_KEEP_XAUDIO:
        section = L"Audio", key = L"KeepXAudio2Loaded";
        break;
    case ID_AUTO_SWITCH:
        section = L"Controls", key = L"AutoSwitchInput";
        padswitch_enable(it->value);
        break;
    case ID_DRAW_DISTANCE:
        section = L"Graphics", key = L"DrawDistance";
        break;
    case ID_MENU_SCALE:
        section = L"Menu", key = L"Scale";
        _snwprintf(value, 32, L"%d", 100 + 25 * it->value);
        overlay_set_scale(100 + 25 * it->value);
        break;
    default:
        return;
    }
    value[31] = L'\0';
    if (!WritePrivateProfileStringW(section, key, value, g_ini_path)) {
        LOGE("menu: could not save %ls to the INI file (error %lu); is the game folder writable?", key,
             (unsigned long)GetLastError());
    } else {
        LOGI("menu: [%ls] %ls=%ls saved%s", section, key, value,
             (it->flags & MM_RESTART) ? "; it takes effect the next time the game starts" : "");
    }
}

/* ---- gamepad ----------------------------------------------------------------------- */

typedef DWORD(WINAPI *xi_get_state_fn)(DWORD index, xi_state *state);

static xi_get_state_fn g_xi_real; /* what the engine's import pointed to */

static xi_get_state_fn xi_function(void)
{
    static xi_get_state_fn found;
    static int looked;

    if (g_xi_real) {
        return g_xi_real;
    }
    if (!looked) {
        HMODULE mod = GetModuleHandleA("xinput1_3.dll"); /* the version the game uses, if it is loaded */
        looked = 1;
        if (mod) {
            FARPROC p = GetProcAddress(mod, (LPCSTR)(ULONG_PTR)2); /* XInputGetState */
            memcpy(&found, &p, sizeof(p));
        }
    }
    return found;
}

/* Buttons of the first connected pad, with the left stick folded into the
 * D-pad bits. 0 when there is none. */
static unsigned gamepad_buttons(void)
{
    xi_get_state_fn get = xi_function();
    DWORD i;

    for (i = 0; get && i < 4; ++i) {
        xi_state s;
        memset(&s, 0, sizeof(s));
        if (get(i, &s) == ERROR_SUCCESS) {
            unsigned b = s.buttons;
            if (s.ly > XI_STICK) {
                b |= XI_UP;
            }
            if (s.ly < -XI_STICK) {
                b |= XI_DOWN;
            }
            if (s.lx < -XI_STICK) {
                b |= XI_LEFT;
            }
            if (s.lx > XI_STICK) {
                b |= XI_RIGHT;
            }
            return b;
        }
    }
    return 0;
}

/* The engine's XInputGetState: a pad at rest while the menu has the input. */
static DWORD WINAPI hook_xinput_get_state(DWORD index, xi_state *state)
{
    DWORD result = g_xi_real ? g_xi_real(index, state) : ERROR_DEVICE_NOT_CONNECTED;

    if (result == ERROR_SUCCESS && state) {
        if (g_open || g_hold_gamepad) {
            DWORD packet = state->packet;
            memset(state, 0, sizeof(*state));
            state->packet = packet;
        } else {
            padswitch_gamepad(state->buttons, state->left_trigger, state->right_trigger, state->lx, state->ly,
                              state->rx, state->ry);
            /* The buttons that open the menu, once they are all down: the game
             * is not to act on them while they are held to open it. */
            if (g_enabled && g_combo && (state->buttons & g_combo) == g_combo) {
                state->buttons = (WORD)(state->buttons & ~g_combo);
            }
        }
    }
    return result;
}

/* ---- keyboard and mouse (DirectInput) ------------------------------------------------ */

#define SLOT_DI_CREATE_DEVICE 3
#define SLOT_DEVICE_GET_STATE 9
#define SLOT_DEVICE_GET_DATA 10
#define MAX_DEVICES 8

static const GUID GUID_Keyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID GUID_Mouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

typedef HRESULT(WINAPI *di_create_fn)(HINSTANCE inst, DWORD version, REFIID iid, void **out, void *outer);
typedef HRESULT(STDMETHODCALLTYPE *di_create_device_fn)(void *self, REFGUID guid, void **device, void *outer);
typedef HRESULT(STDMETHODCALLTYPE *di_get_state_fn)(void *self, DWORD size, void *data);
typedef HRESULT(STDMETHODCALLTYPE *di_get_data_fn)(void *self, DWORD object_size, void *data, DWORD *count,
                                                   DWORD flags);

static di_create_fn g_di_create;
static vt_hook g_di_vtables[VT_MAX_TABLES];
static vt_hook g_state_vtables[VT_MAX_TABLES];
static vt_hook g_data_vtables[VT_MAX_TABLES];
static void *g_devices[MAX_DEVICES]; /* the keyboard and mouse devices the engine created */
static unsigned char g_device_is_mouse[MAX_DEVICES];
static SRWLOCK g_di_lock = SRWLOCK_INIT;

enum { DEVICE_OTHER = 0, DEVICE_KEYBOARD, DEVICE_MOUSE };

static int device_kind(const void *device)
{
    unsigned i;
    for (i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i] == device) {
            return g_device_is_mouse[i] ? DEVICE_MOUSE : DEVICE_KEYBOARD;
        }
    }
    return DEVICE_OTHER;
}

/* What the game just read, for switching between gamepad and keyboard/mouse.
 * The standard formats: 256 key bytes; three axes (relative) and 4 or 8
 * button bytes. */
static void note_device_state(int kind, const unsigned char *data, DWORD size)
{
    DWORD i;
    int down = 0;

    if (kind == DEVICE_KEYBOARD) {
        for (i = 0; i < size && !down; ++i) {
            down = (data[i] & 0x80) != 0;
        }
        padswitch_keyboard(down);
    } else if (size == 16 || size == 20) {
        LONG axis[3];
        long travel = 0;
        memcpy(axis, data, sizeof(axis));
        for (i = 0; i < 3; ++i) {
            travel += axis[i] < 0 ? -axis[i] : axis[i];
        }
        for (i = 12; i < size && !down; ++i) {
            down = (data[i] & 0x80) != 0;
        }
        padswitch_mouse(travel, down);
    }
}

/* The same from buffered events: { offset, data, time, sequence, ... } each. */
static void note_device_events(int kind, const unsigned char *events, DWORD object_size, DWORD count)
{
    DWORD i;
    long travel = 0;
    int down = 0;

    if (object_size < 8) {
        return;
    }
    for (i = 0; i < count; ++i) {
        DWORD offset, value;
        memcpy(&offset, events + (size_t)i * object_size, 4);
        memcpy(&value, events + (size_t)i * object_size + 4, 4);
        if (kind == DEVICE_MOUSE && offset < 12) {
            LONG moved = (LONG)value;
            travel += moved < 0 ? -moved : moved;
        } else if (value & 0x80) {
            down = 1;
        }
    }
    if (kind == DEVICE_KEYBOARD) {
        padswitch_keyboard(down);
    } else {
        padswitch_mouse(travel, down);
    }
}

static HRESULT STDMETHODCALLTYPE hook_get_device_state(void *self, DWORD size, void *data)
{
    di_get_state_fn original;
    HRESULT hr;
    int ours;

    AcquireSRWLockShared(&g_di_lock);
    original = (di_get_state_fn)vt_original(g_state_vtables, VT_OF(self));
    ours = device_kind(self);
    ReleaseSRWLockShared(&g_di_lock);
    if (!original) {
        return E_FAIL;
    }
    hr = original(self, size, data);
    if (SUCCEEDED(hr) && ours && data) {
        if (g_open || g_hold_keyboard) {
            memset(data, 0, size); /* no key down, no mouse movement */
        } else if (padswitch_enabled()) {
            note_device_state(ours, (const unsigned char *)data, size);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_get_device_data(void *self, DWORD object_size, void *data, DWORD *count,
                                                      DWORD flags)
{
    di_get_data_fn original;
    HRESULT hr;
    int ours;

    AcquireSRWLockShared(&g_di_lock);
    original = (di_get_data_fn)vt_original(g_data_vtables, VT_OF(self));
    ours = device_kind(self);
    ReleaseSRWLockShared(&g_di_lock);
    if (!original) {
        return E_FAIL;
    }
    hr = original(self, object_size, data, count, flags);
    if (SUCCEEDED(hr) && ours && count) {
        if (g_open || g_hold_keyboard) {
            *count = 0; /* the buffered events are dropped */
        } else if (data && *count && padswitch_enabled()) {
            note_device_events(ours, (const unsigned char *)data, object_size, *count);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE hook_create_device(void *self, REFGUID guid, void **device, void *outer)
{
    di_create_device_fn original;
    HRESULT hr;
    unsigned i;

    AcquireSRWLockShared(&g_di_lock);
    original = (di_create_device_fn)vt_original(g_di_vtables, VT_OF(self));
    ReleaseSRWLockShared(&g_di_lock);
    if (!original) {
        return E_FAIL;
    }
    hr = original(self, guid, device, outer);
    if (FAILED(hr) || !device || !*device || !guid) {
        return hr;
    }
    /* Game controllers are left alone: "all zero" is not a resting position for them. */
    if (memcmp(guid, &GUID_Keyboard, sizeof(GUID)) != 0 && memcmp(guid, &GUID_Mouse, sizeof(GUID)) != 0) {
        return hr;
    }
    AcquireSRWLockExclusive(&g_di_lock);
    if (vt_hook_slot(g_state_vtables, VT_OF(*device), SLOT_DEVICE_GET_STATE, (void *)hook_get_device_state) &&
        vt_hook_slot(g_data_vtables, VT_OF(*device), SLOT_DEVICE_GET_DATA, (void *)hook_get_device_data)) {
        for (i = 0; i < MAX_DEVICES && g_devices[i] && g_devices[i] != *device; ++i) {
        }
        i = i < MAX_DEVICES ? i : 0;
        g_devices[i] = *device;
        g_device_is_mouse[i] = memcmp(guid, &GUID_Mouse, sizeof(GUID)) == 0;
    }
    ReleaseSRWLockExclusive(&g_di_lock);
    return hr;
}

static HRESULT WINAPI hook_directinput_create(HINSTANCE inst, DWORD version, REFIID iid, void **out, void *outer)
{
    HRESULT hr = g_di_create(inst, version, iid, out, outer);

    if (SUCCEEDED(hr) && out && *out) {
        AcquireSRWLockExclusive(&g_di_lock);
        vt_hook_slot(g_di_vtables, VT_OF(*out), SLOT_DI_CREATE_DEVICE, (void *)hook_create_device);
        ReleaseSRWLockExclusive(&g_di_lock);
    }
    return hr;
}

unsigned menu_install_hooks(HMODULE engine)
{
    unsigned hooks = 0;
    void *previous = NULL;
    mm_item *it = mm_find(&g_menu, ID_AUTO_SWITCH);

    if (it && !padswitch_available()) {
        /* nothing to switch in this build of the game: show that instead */
        it->kind = MM_INFO;
        snprintf(it->text, sizeof(it->text), "not available");
    }
    /* The input hooks serve the menu and the switching; without both, the
     * game's input is left entirely alone. */
    if (!g_enabled && !(g_auto_switch && padswitch_available())) {
        return 0;
    }
    if (pei_hook_import(engine, "xinput1_3.dll", "#2", (void *)hook_xinput_get_state, &previous) == 1 && previous) {
        memcpy(&g_xi_real, &previous, sizeof(previous));
        ++hooks;
    }
    previous = NULL;
    if (pei_hook_import(engine, "dinput8.dll", "DirectInput8Create", (void *)hook_directinput_create, &previous) == 1 &&
        previous) {
        memcpy(&g_di_create, &previous, sizeof(previous));
        ++hooks;
    }
    LOGD("input: the game's gamepad and keyboard/mouse reads go through the patch (%u of 2 hooks)", hooks);
    if (!g_enabled) {
        return hooks;
    }
    {
        const char *pad = g_combo == XI_COMBO_DEFAULT ? "; on a gamepad, hold both thumbsticks in"
                          : g_combo ? "; on a gamepad, hold the buttons set in the INI file" : "";
        if (g_key >= VK_F1 && g_key <= VK_F12) {
            LOGI("menu: F%u opens the settings menu%s", g_key - VK_F1 + 1, pad);
        } else {
            LOGI("menu: the key set in the INI file opens the settings menu%s", pad);
        }
    }
    if (hooks != 2) {
        LOGI("menu: the game's own input cannot be held back in this renderer; it will also react to the keys "
             "used in the menu");
    }
    return hooks;
}

/* ---- per frame ------------------------------------------------------------------------ */

static int key_down(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static int game_has_focus(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

static void update_status(void)
{
    long added = 0, left_out = 0;
    mm_item *it = mm_find(&g_menu, ID_STATUS_TEXTURES);

    if (it) {
        if (texguard_counts(&added, &left_out)) {
            snprintf(it->text, sizeof(it->text), "%ld of %ld", left_out, added + left_out);
        } else {
            snprintf(it->text, sizeof(it->text), "guard not active");
        }
    }
    it = mm_find(&g_menu, ID_STATUS_DISTANCE);
    if (it) {
        int level = GP_ORIGINAL;
        gfx_profile_result result = gfx_profile_state(&level);
        const char *name = DISTANCE_NAMES[level > GP_ORIGINAL && level < GP_LEVELS ? level : GP_ORIGINAL];

        snprintf(it->text, sizeof(it->text), "%s%s", name,
                 result == GFX_PROFILE_FAILED    ? ": not applied, see the log"
                 : result == GFX_PROFILE_WAITING ? ": the game has not read it yet"
                                                 : "");
    }
}

void menu_on_present(void *swap_chain)
{
    static mm_repeat r_up, r_down, r_left, r_right;
    static int was_accept, was_back, was_key;
    static mm_chord chord;
    static md_vertex vertices[24576];
    static int used_gamepad;
    static unsigned frame;
    static unsigned ignore_pad; /* buttons still down from opening the menu */
    unsigned long now;
    unsigned pad;
    int focus, pad_focus, toggle, changed, down;
    const md_font *font = NULL;
    float w = 0, h = 0;
    md_list list;

    if (!g_enabled) {
        return;
    }
    now = GetTickCount();
    focus = game_has_focus();
    pad_focus = focus || g_assume_focus;
    pad = pad_focus ? gamepad_buttons() : 0;

    down = focus && key_down((int)g_key);
    toggle = down && !was_key;
    was_key = down;
    if (mm_chord_step(&chord, g_combo && (pad & g_combo) == g_combo, now)) {
        toggle = 1;
        used_gamepad = 1;
        ignore_pad = g_combo;
    }
    /* A button of the opening combination does nothing in the menu until it was let go. */
    ignore_pad &= pad;
    if (toggle) {
        mm_open(&g_menu, !g_menu.open);
    }

    if (g_menu.open && pad_focus && !toggle) {
        mm_input input = MM_IN_NONE;
        unsigned nav = pad & ~ignore_pad;
        int k_up = focus && key_down(VK_UP), k_down = focus && key_down(VK_DOWN);
        int k_left = focus && key_down(VK_LEFT), k_right = focus && key_down(VK_RIGHT);
        int accept_key = focus && key_down(VK_RETURN);
        int back_key = focus && (key_down(VK_BACK) || key_down(VK_ESCAPE));
        int accept = accept_key || (nav & XI_A), back = back_key || (nav & XI_B);
        int by_pad = 0;

        /* Directions repeat while held; accept and back act once per press. */
        if (mm_repeat_step(&r_up, k_up || (nav & XI_UP), now)) {
            input = MM_IN_UP;
            by_pad = !k_up;
        } else if (mm_repeat_step(&r_down, k_down || (nav & XI_DOWN), now)) {
            input = MM_IN_DOWN;
            by_pad = !k_down;
        } else if (mm_repeat_step(&r_left, k_left || (nav & XI_LEFT), now)) {
            input = MM_IN_LEFT;
            by_pad = !k_left;
        } else if (mm_repeat_step(&r_right, k_right || (nav & XI_RIGHT), now)) {
            input = MM_IN_RIGHT;
            by_pad = !k_right;
        } else if (accept && !was_accept) {
            input = MM_IN_ACCEPT;
            by_pad = !accept_key;
        } else if (back && !was_back) {
            input = MM_IN_BACK;
            by_pad = !back_key;
        }
        was_accept = accept;
        was_back = back;
        if (input != MM_IN_NONE) {
            used_gamepad = by_pad;
            changed = mm_handle(&g_menu, input);
            if (changed == ID_CLOSE) {
                mm_open(&g_menu, 0);
            } else if (changed != MM_NO_CHANGE) {
                save(changed);
            }
        }
    }

    /* Tell the input hooks. After closing, the game stays deaf until the keys
     * that closed the menu are up, or Escape and B would act in the game too. */
    if (g_menu.open) {
        InterlockedExchange(&g_open, 1);
        InterlockedExchange(&g_hold_keyboard, 1);
        InterlockedExchange(&g_hold_gamepad, 1);
    } else {
        InterlockedExchange(&g_open, 0);
        if (g_hold_keyboard && !key_down(VK_ESCAPE) && !key_down(VK_BACK) && !key_down(VK_RETURN) &&
            !key_down((int)g_key)) {
            InterlockedExchange(&g_hold_keyboard, 0);
        }
        if (g_hold_gamepad && !(pad & (XI_A | XI_B | g_combo))) {
            InterlockedExchange(&g_hold_gamepad, 0);
        }
        return;
    }

    if (!overlay_begin(swap_chain, &w, &h, &font)) {
        /* Nothing can be shown: do not leave the game deaf behind an invisible menu. */
        mm_open(&g_menu, 0);
        InterlockedExchange(&g_open, 0);
        InterlockedExchange(&g_hold_keyboard, 0);
        InterlockedExchange(&g_hold_gamepad, 0);
        g_enabled = 0;
        return;
    }
    if ((frame++ & 15) == 0) {
        update_status();
    }
    mm_scroll_into_view(&g_menu, md_visible_rows(font, &g_menu, h));
    list.v = vertices;
    list.count = 0;
    list.cap = sizeof(vertices) / sizeof(vertices[0]);
    list.dropped = 0;
    md_menu(&list, font, &g_menu, w, h,
            used_gamepad ? "D-pad or left stick: select and change     A: switch     B: close"
                         : "Arrow keys: select and change     Enter: switch     Backspace: close");
    overlay_draw(&list);
}
