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
#include "overlay.h"
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
    ID_MAX_CORES,
    ID_USE_SMT,
    ID_CPU_MODE,
    ID_RESTORE_FULLSCREEN,
    ID_DPI_AWARE,
    ID_TEX_MISMATCHED,
    ID_TEX_REPLACED,
    ID_KEEP_XAUDIO,
    ID_CLOSE
};

static const char *const MODE_NAMES[] = {"off", "engine", "process"};

#define HEADING(text) {MM_HEADING, ID_NONE, text, NULL, 0, 0, 0, 0, NULL, 0, 0, ""}
#define INFO(id, text) {MM_INFO, id, text, NULL, 0, 0, 0, 0, NULL, 0, 0, ""}
#define TOGGLE(id, text, help) {MM_TOGGLE, id, text, help, 0, 0, 1, 1, NULL, 0, MM_RESTART, ""}

static mm_item g_items[] = {
    HEADING("Status"),
    INFO(ID_STATUS_VERSION, "Patch"),
    INFO(ID_STATUS_PROCESSOR, "Processor"),
    INFO(ID_STATUS_MEMORY, "Memory"),
    INFO(ID_STATUS_TEXTURES, "Texture levels left out"),
    HEADING("Processor"),
    {MM_NUMBER, ID_MAX_CORES, "Cores for the game",
     "How many physical cores the game may use. 0 means all of them. 4 to 6 suits this engine best.", 0, 0,
     FC3PP_MAX_ENGINE_CPUS, 1, NULL, 0, MM_RESTART, ""},
    TOGGLE(ID_USE_SMT, "Both threads of each core",
           "Off: one thread per core, which avoids two of the game's workers sharing a core."),
    {MM_CHOICE, ID_CPU_MODE, "Mode",
     "engine: the game's own threads use the chosen cores. process: drivers and sound are limited to them too. "
     "off: the game's own handling.",
     1, 0, 2, 1, MODE_NAMES, 3, MM_RESTART, ""},
    HEADING("Display"),
    TOGGLE(ID_RESTORE_FULLSCREEN, "Fullscreen after Alt+Tab",
           "Return to fullscreen when you switch back to the game, instead of staying in a window."),
    TOGGLE(ID_DPI_AWARE, "DPI aware", "Stops Windows from scaling the game's window on displays set above 100 %."),
    HEADING("Texture mods"),
    TOGGLE(ID_TEX_MISMATCHED, "Leave out levels that do not fit",
           "Texture mods crash the game without this, unless mip0 loading is switched off in GamerProfile.xml."),
    TOGGLE(ID_TEX_REPLACED, "Leave out originals of replaced textures",
           "Keeps the game's original close-up texture from showing through a texture a mod has repainted."),
    HEADING("Sound"),
    TOGGLE(ID_KEEP_XAUDIO, "Keep the sound library loaded",
           "Guards against a known defect of XAudio2 2.7 that can crash the game."),
    HEADING(""),
    {MM_ACTION, ID_CLOSE, "Close", "", 0, 0, 0, 0, NULL, 0, 0, ""},
};

static mm_menu g_menu = {FC3PP_NAME " " FC3PP_VERSION, g_items, sizeof(g_items) / sizeof(g_items[0]), 0, 0, 0};
static wchar_t g_ini_path[MAX_PATH + 40];
static unsigned g_key = VK_F8;
static int g_enabled;

static volatile LONG g_open;          /* read by the input hooks on other threads */
static volatile LONG g_hold_keyboard; /* closed, but keys that operated it are still down */
static volatile LONG g_hold_gamepad;

static void set_value(int id, int value)
{
    mm_item *it = mm_find(&g_menu, id);
    if (it) {
        it->value = value;
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
    set_value(ID_RESTORE_FULLSCREEN, cfg->restore_fullscreen);
    set_value(ID_DPI_AWARE, cfg->high_dpi_aware);
    set_value(ID_TEX_MISMATCHED, cfg->texture_guard);
    set_value(ID_TEX_REPLACED, cfg->texture_scan);
    set_value(ID_KEEP_XAUDIO, cfg->keep_xaudio2_loaded);
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
    case ID_KEEP_XAUDIO:
        section = L"Audio", key = L"KeepXAudio2Loaded";
        break;
    default:
        return;
    }
    value[31] = L'\0';
    if (!WritePrivateProfileStringW(section, key, value, g_ini_path)) {
        LOGE("menu: could not save %ls to the INI file (error %lu); is the game folder writable?", key,
             (unsigned long)GetLastError());
    } else {
        LOGI("menu: [%ls] %ls=%ls saved; it takes effect the next time the game starts", section, key, value);
    }
}

/* ---- gamepad ----------------------------------------------------------------------- */

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

    if (result == ERROR_SUCCESS && state && (g_open || g_hold_gamepad)) {
        DWORD packet = state->packet;
        memset(state, 0, sizeof(*state));
        state->packet = packet;
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
static SRWLOCK g_di_lock = SRWLOCK_INIT;

static int is_blocked_device(const void *device)
{
    unsigned i;
    for (i = 0; i < MAX_DEVICES; ++i) {
        if (g_devices[i] == device) {
            return 1;
        }
    }
    return 0;
}

static HRESULT STDMETHODCALLTYPE hook_get_device_state(void *self, DWORD size, void *data)
{
    di_get_state_fn original;
    HRESULT hr;
    int ours;

    AcquireSRWLockShared(&g_di_lock);
    original = (di_get_state_fn)vt_original(g_state_vtables, VT_OF(self));
    ours = is_blocked_device(self);
    ReleaseSRWLockShared(&g_di_lock);
    if (!original) {
        return E_FAIL;
    }
    hr = original(self, size, data);
    if (SUCCEEDED(hr) && ours && data && (g_open || g_hold_keyboard)) {
        memset(data, 0, size); /* no key down, no mouse movement */
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
    ours = is_blocked_device(self);
    ReleaseSRWLockShared(&g_di_lock);
    if (!original) {
        return E_FAIL;
    }
    hr = original(self, object_size, data, count, flags);
    if (SUCCEEDED(hr) && ours && count && (g_open || g_hold_keyboard)) {
        *count = 0; /* the buffered events are dropped */
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
        g_devices[i < MAX_DEVICES ? i : 0] = *device;
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

    if (!g_enabled) {
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
    if (g_key >= VK_F1 && g_key <= VK_F12) {
        LOGI("menu: F%u opens the settings menu; on a gamepad, hold both thumbsticks in", g_key - VK_F1 + 1);
    } else {
        LOGI("menu: the key set in the INI file opens the settings menu; on a gamepad, hold both thumbsticks in");
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
}

void menu_on_present(void *swap_chain)
{
    static mm_repeat r_up, r_down, r_left, r_right;
    static int was_accept, was_back, was_key;
    static mm_chord chord;
    static md_vertex vertices[24576];
    static int used_gamepad;
    static unsigned frame;
    unsigned long now;
    unsigned pad;
    int focus, toggle, changed, down;
    const md_font *font = NULL;
    float w = 0, h = 0;
    md_list list;

    if (!g_enabled) {
        return;
    }
    now = GetTickCount();
    focus = game_has_focus();
    pad = focus ? gamepad_buttons() : 0;

    down = focus && key_down((int)g_key);
    toggle = down && !was_key;
    was_key = down;
    if (mm_chord_step(&chord, (pad & (XI_LEFT_THUMB | XI_RIGHT_THUMB)) == (XI_LEFT_THUMB | XI_RIGHT_THUMB), now)) {
        toggle = 1;
        used_gamepad = 1;
    }
    if (toggle) {
        mm_open(&g_menu, !g_menu.open);
    }

    if (g_menu.open && focus && !toggle) {
        mm_input input = MM_IN_NONE;
        int accept_key = key_down(VK_RETURN), back_key = key_down(VK_BACK) || key_down(VK_ESCAPE);
        int accept = accept_key || (pad & XI_A), back = back_key || (pad & XI_B);
        int by_pad = 0;

        /* Directions repeat while held; accept and back act once per press. */
        if (mm_repeat_step(&r_up, key_down(VK_UP) || (pad & XI_UP), now)) {
            input = MM_IN_UP;
            by_pad = !key_down(VK_UP);
        } else if (mm_repeat_step(&r_down, key_down(VK_DOWN) || (pad & XI_DOWN), now)) {
            input = MM_IN_DOWN;
            by_pad = !key_down(VK_DOWN);
        } else if (mm_repeat_step(&r_left, key_down(VK_LEFT) || (pad & XI_LEFT), now)) {
            input = MM_IN_LEFT;
            by_pad = !key_down(VK_LEFT);
        } else if (mm_repeat_step(&r_right, key_down(VK_RIGHT) || (pad & XI_RIGHT), now)) {
            input = MM_IN_RIGHT;
            by_pad = !key_down(VK_RIGHT);
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
        if (g_hold_gamepad && !(pad & (XI_A | XI_B | XI_LEFT_THUMB | XI_RIGHT_THUMB))) {
            InterlockedExchange(&g_hold_gamepad, 0);
        }
        return;
    }

    if (!overlay_begin(swap_chain, &w, &h, &font)) {
        /* Nothing can be shown: do not leave the game deaf behind an invisible menu. */
        mm_open(&g_menu, 0);
        InterlockedExchange(&g_open, 0);
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
            used_gamepad ? "D-pad: select and change     A: switch     B: close"
                         : "Arrow keys: select and change     Enter: switch     Backspace: close");
    overlay_draw(&list);
}
