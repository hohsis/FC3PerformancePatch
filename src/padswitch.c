/* Gamepad <-> keyboard/mouse switching: the part that touches the game.
 *
 * The engine re-evaluates its input mode every frame from one byte of its UI
 * manager, "the mode asked for" (padmode.h). The options menu writes that
 * byte; so does this file, when the input hooks in menu.c report that the
 * player has picked up the other device. Nothing else of the game is changed:
 * no code is patched, and the "Use gamepad" setting in the player's profile
 * stays what the options menu last made it.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "padswitch.h"

#include <string.h>

#include "log.h"
#include "padmode.h"
#include "pe_image.h"

static unsigned char **g_manager_slot; /* the engine's variable that points to its UI manager */
static const unsigned char *g_image;   /* the engine module, for checking what that points to */
static size_t g_image_size;
static const unsigned char *g_checked; /* a manager object that passed the check */

static volatile LONG g_on;
static volatile LONG g_switches;
static volatile LONG g_distrust; /* the object did not look like the UI manager: stopped */
static pm_state g_state;
static SRWLOCK g_lock = SRWLOCK_INIT;

static DWORD g_device_read_ms; /* when the game last read its keyboard or mouse */
static DWORD g_fallback_ms;

int padswitch_available(void)
{
    return g_manager_slot != NULL && !g_distrust;
}

int padswitch_enabled(void)
{
    return g_on != 0 && padswitch_available();
}

void padswitch_enable(int on)
{
    if (!padswitch_available()) {
        return;
    }
    AcquireSRWLockExclusive(&g_lock);
    pm_reset(&g_state);
    ReleaseSRWLockExclusive(&g_lock);
    if (InterlockedExchange(&g_on, on ? 1 : 0) != (on ? 1 : 0)) {
        LOGI("controls: switching between gamepad and keyboard/mouse by use is %s", on ? "on" : "off");
    }
}

/* The UI manager, or NULL while the game has none (early start-up) or the
 * object is not what this build of the game is expected to have there. */
static unsigned char *manager(void)
{
    unsigned char *object = *g_manager_slot;
    MEMORY_BASIC_INFORMATION info;
    const unsigned char *vtable;

    if (!object) {
        return NULL;
    }
    if (object == g_checked) {
        return object;
    }
    /* A C++ object on the heap whose virtual table lies in the engine module. */
    if (VirtualQuery(object, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
        !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) ||
        (info.Protect & PAGE_GUARD) ||
        (size_t)((const unsigned char *)info.BaseAddress + info.RegionSize - object) <= PM_OFFSET_PENDING) {
        InterlockedExchange(&g_distrust, 1);
        return NULL;
    }
    memcpy(&vtable, object, sizeof(vtable));
    if (vtable < g_image || vtable >= g_image + g_image_size) {
        InterlockedExchange(&g_distrust, 1);
        return NULL;
    }
    g_checked = object;
    return object;
}

static int g_assume_focus; /* testing aid */

static int game_has_focus(void)
{
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

typedef enum source { FROM_GAMEPAD, FROM_KEYBOARD, FROM_MOUSE } source;

static void note(source from, long a, int b)
{
    unsigned char *mgr;
    pm_mode wanted;
    DWORD now = GetTickCount();

    AcquireSRWLockExclusive(&g_lock);
    mgr = manager();
    if (!mgr) {
        ReleaseSRWLockExclusive(&g_lock);
        if (g_distrust && InterlockedExchange(&g_on, 0)) {
            LOGE("controls: the game's input-mode object is not where this patch expects it; switching by use "
                 "is off for this session");
        }
        return;
    }
    /* The byte is the truth: the options menu may have changed it meanwhile. */
    g_state.mode = mgr[PM_OFFSET_REQUESTED] ? PM_GAMEPAD : PM_KEYBOARD_MOUSE;
    switch (from) {
    case FROM_GAMEPAD:
        wanted = pm_note_gamepad(&g_state, b, now);
        break;
    case FROM_KEYBOARD:
        wanted = pm_note_keyboard(&g_state, b, now);
        break;
    default:
        wanted = pm_note_mouse(&g_state, a, b, now);
        break;
    }
    if (wanted != PM_UNKNOWN) {
        /* The game's keyboard and mouse devices only deliver while it has the
         * focus; a gamepad reports to every program, also to one in the
         * background. */
        if (from != FROM_GAMEPAD || g_assume_focus || game_has_focus()) {
            /* Only "asked for" is written. The engine acts on it in its next
             * frame, and only if that changes anything (a gamepad must be
             * connected for gamepad mode), so asking twice does no harm. */
            mgr[PM_OFFSET_REQUESTED] = (unsigned char)(wanted == PM_GAMEPAD);
            if (InterlockedIncrement(&g_switches) == 1) {
                LOGI("controls: asked the game for %s mode (first switch of this session)",
                     wanted == PM_GAMEPAD ? "gamepad" : "keyboard/mouse");
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/* In gamepad mode the game may stop reading its keyboard and mouse devices;
 * then the hooks on those see nothing. Ask Windows instead, a few times a
 * second, whether a key or mouse button is down. */
static void keyboard_fallback(DWORD now)
{
    int vk, down = 0;

    if (now - g_device_read_ms < 1000 || now - g_fallback_ms < 100) {
        return;
    }
    g_fallback_ms = now;
    if (!game_has_focus()) {
        return;
    }
    for (vk = 1; vk < 0xC0 && !down; ++vk) {
        if (vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) {
            continue; /* toggles, not actions */
        }
        down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    if (down) {
        note(FROM_KEYBOARD, 0, 1);
    }
}

void padswitch_gamepad(unsigned buttons, int left_trigger, int right_trigger, int lx, int ly, int rx, int ry)
{
    if (!g_on || !g_manager_slot) {
        return;
    }
    if (pm_gamepad_active(buttons, left_trigger, right_trigger, lx, ly, rx, ry)) {
        note(FROM_GAMEPAD, 0, 1);
    } else {
        keyboard_fallback(GetTickCount());
    }
}

void padswitch_keyboard(int any_key_down)
{
    if (!g_on || !g_manager_slot) {
        return;
    }
    g_device_read_ms = GetTickCount();
    if (any_key_down) {
        note(FROM_KEYBOARD, 0, 1);
    }
}

void padswitch_mouse(long travel, int any_button_down)
{
    if (!g_on || !g_manager_slot) {
        return;
    }
    g_device_read_ms = GetTickCount();
    if (travel || any_button_down) {
        note(FROM_MOUSE, travel, any_button_down);
    }
}

int padswitch_install(HMODULE engine, const char *name, int enabled, int assume_focus)
{
    pei_section sections[32];
    unsigned count, i;
    unsigned long pointer = 0;
    const char *variant = NULL;
    int found = 0;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)engine;
    const IMAGE_NT_HEADERS32 *nt;

    if (g_manager_slot) {
        return 1; /* the engine is one module per process */
    }
    if (!pei_valid(engine)) {
        return 0;
    }
    nt = (const IMAGE_NT_HEADERS32 *)((const unsigned char *)engine + dos->e_lfanew);
    count = pei_sections(engine, sections, 32);
    if (count > 32) {
        count = 32;
    }
    for (i = 0; i < count; ++i) {
        unsigned long p = 0;
        int r;

        if (!(sections[i].characteristics & IMAGE_SCN_MEM_EXECUTE) || !sections[i].size) {
            continue;
        }
        r = pm_find_manager(sections[i].start, sections[i].size, (unsigned long)(ULONG_PTR)sections[i].start, &p,
                            &variant);
        if (r < 0 || (r > 0 && found && p != pointer)) {
            found = -1;
            break;
        }
        if (r > 0) {
            pointer = p;
            found = 1;
        }
    }
    /* The variable must be in the module itself, with room for a pointer. */
    if (found == 1 && (pointer < (unsigned long)(ULONG_PTR)engine ||
                       pointer - (unsigned long)(ULONG_PTR)engine > nt->OptionalHeader.SizeOfImage - sizeof(void *))) {
        found = -1;
    }
    if (found != 1) {
        if (enabled) {
            LOGE("%s: the game's input-mode code was not recognised%s; switching between gamepad and keyboard/mouse "
                 "by use is not available",
                 name, found < 0 ? " (ambiguous match)" : "");
        } else {
            LOGD("%s: input-mode code not recognised", name);
        }
        return 0;
    }
    g_assume_focus = assume_focus;
    g_image = (const unsigned char *)engine;
    g_image_size = nt->OptionalHeader.SizeOfImage;
    pm_reset(&g_state);
    g_manager_slot = (unsigned char **)(ULONG_PTR)pointer;
    InterlockedExchange(&g_on, enabled ? 1 : 0);
    LOGI("%s: input-mode switch found ('%s' build variant); switching between gamepad and keyboard/mouse by use "
         "is %s",
         name, variant ? variant : "?", enabled ? "on" : "off");
    return 1;
}

void padswitch_log_stats(void)
{
    if (g_manager_slot && g_switches) {
        LOGI("controls: the input mode was switched %ld time%s", (long)g_switches, g_switches == 1 ? "" : "s");
    }
}
