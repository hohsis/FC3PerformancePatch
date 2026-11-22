/* Switching between gamepad and keyboard/mouse while playing. Pure C.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "padmode.h"

#include <string.h>

#include "sigscan.h"

/* ---- locating the UI manager -----------------------------------------------------
 *
 * Far Cry 3 (FC3.dll and FC3_d3d11.dll), the setter is inlined:
 *     8A 88 D0 00 00 00     mov  cl, [eax+0D0h]        ; profile: UseGamePad
 *     A1 imm32              mov  eax, [ui_manager]
 *     88 48 31              mov  [eax+31h], cl
 *     C6 40 32 01           mov  byte [eax+32h], 1
 *     C3                    ret
 *
 * Blood Dragon (size-optimised), the setter is a function of its own:
 *     0F B6 81 D0 00 00 00  movzx eax, byte [ecx+0D0h]
 *     8B 0D imm32           mov  ecx, [ui_manager]
 *     50                    push eax
 *     E8 rel32              call SetGamepad
 * with
 *     55 8B EC 8A 45 08 88 41 31 C6 41 32 01 5D C2 04 00
 *                           mov al,[ebp+8] / mov [ecx+31h],al / mov byte [ecx+32h],1 / ret 4
 */
static const char INLINE_SIG[] = "8A 88 D0 00 00 00 A1 ?? ?? ?? ?? 88 48 31 C6 40 32 01 C3";
static const char CALL_SIG[] = "0F B6 81 D0 00 00 00 8B 0D ?? ?? ?? ?? 50 E8 ?? ?? ?? ??";
static const char SETTER_SIG[] = "55 8B EC 8A 45 08 88 41 31 C6 41 32 01 5D C2 04 00";

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) |
           ((unsigned long)p[3] << 24);
}

int pm_find_manager(const unsigned char *text, size_t size, unsigned long text_va, unsigned long *pointer_va,
                    const char **variant)
{
    sig_pattern sig, setter;
    const unsigned char *hits[4];
    size_t n, i;
    unsigned long found = 0;
    const char *name = NULL;
    int count = 0;

    (void)text_va;
    if (!text || !size || !pointer_va) {
        return 0;
    }
    if (sig_parse(&sig, INLINE_SIG) == 0) {
        n = sig_find_all(text, size, &sig, hits, 4);
        if (n > 4) {
            return -1;
        }
        for (i = 0; i < n; ++i) {
            unsigned long p = rd32(hits[i] + 7);
            if (!count || p != found) {
                ++count;
            }
            found = p;
            name = "release";
        }
    }
    if (sig_parse(&sig, CALL_SIG) == 0 && sig_parse(&setter, SETTER_SIG) == 0) {
        n = sig_find_all(text, size, &sig, hits, 4);
        if (n > 4) {
            return -1;
        }
        for (i = 0; i < n; ++i) {
            /* the call must go to the setter */
            long long target = (long long)(hits[i] - text) + 19 + (long long)(int)rd32(hits[i] + 15);
            unsigned long p;
            if (target < 0 || target + (long long)setter.len > (long long)size ||
                sig_find(text + target, setter.len, &setter) != text + target) {
                continue;
            }
            p = rd32(hits[i] + 9);
            if (!count || p != found) {
                ++count;
            }
            found = p;
            name = "release-size";
        }
    }
    if (count != 1) {
        return count ? -1 : 0;
    }
    *pointer_va = found;
    if (variant) {
        *variant = name;
    }
    return 1;
}

/* ---- deciding ----------------------------------------------------------------------- */

void pm_reset(pm_state *s)
{
    memset(s, 0, sizeof(*s));
    s->mode = PM_UNKNOWN;
}

static int beyond(int v, int limit)
{
    return v > limit || v < -limit;
}

int pm_gamepad_active(unsigned buttons, int left_trigger, int right_trigger, int lx, int ly, int rx, int ry)
{
    return buttons != 0 || left_trigger > PM_TRIGGER_THRESHOLD || right_trigger > PM_TRIGGER_THRESHOLD ||
           beyond(lx, PM_STICK_THRESHOLD) || beyond(ly, PM_STICK_THRESHOLD) || beyond(rx, PM_STICK_THRESHOLD) ||
           beyond(ry, PM_STICK_THRESHOLD);
}

/* The device `wanted` is in use. Switch unless that would be a flip-flop:
 * right after a switch the previous device is given a moment to come to rest
 * (a hand leaving the mouse nudges it; a pad put down rocks its sticks). */
static pm_mode want(pm_state *s, pm_mode wanted, unsigned long now_ms)
{
    if (s->mode == wanted) {
        s->switched_ms = now_ms; /* still in use: keeps the other device waiting */
        return PM_UNKNOWN;
    }
    if (s->mode != PM_UNKNOWN && now_ms - s->switched_ms < PM_SETTLE_MS) {
        return PM_UNKNOWN;
    }
    s->mode = wanted;
    s->switched_ms = now_ms;
    s->switches++;
    return wanted;
}

pm_mode pm_note_gamepad(pm_state *s, int active, unsigned long now_ms)
{
    return active ? want(s, PM_GAMEPAD, now_ms) : PM_UNKNOWN;
}

pm_mode pm_note_keyboard(pm_state *s, int any_key_down, unsigned long now_ms)
{
    return any_key_down ? want(s, PM_KEYBOARD_MOUSE, now_ms) : PM_UNKNOWN;
}

pm_mode pm_note_mouse(pm_state *s, long travel, int any_button_down, unsigned long now_ms)
{
    if (travel < 0) {
        travel = -travel;
    }
    if (now_ms - s->mouse_since_ms > PM_MOUSE_WINDOW_MS) {
        s->mouse_since_ms = now_ms;
        s->mouse_travel = 0;
    }
    s->mouse_travel += travel;
    /* A mouse lying on the desk jitters by a count or two; a hand moves it. */
    if (any_button_down || s->mouse_travel >= PM_MOUSE_THRESHOLD) {
        s->mouse_travel = 0;
        return want(s, PM_KEYBOARD_MOUSE, now_ms);
    }
    return PM_UNKNOWN;
}
