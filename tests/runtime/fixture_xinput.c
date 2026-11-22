/* Test fixture standing in for xinput1_3.dll: one gamepad whose state the
 * harness sets. XInputGetState is exported by ordinal 2 only (see the .def
 * files), which is how the game imports it.
 *
 * It contains no Microsoft code.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <windows.h>

typedef struct fxxi_state {
    DWORD packet;
    WORD buttons;
    BYTE left_trigger, right_trigger;
    SHORT lx, ly, rx, ry;
} fxxi_state;

static fxxi_state g_state;
static int g_connected = 1;
static volatile LONG g_calls;

DWORD WINAPI XInputGetState(DWORD index, fxxi_state *state)
{
    InterlockedIncrement(&g_calls);
    if (index != 0 || !g_connected || !state) {
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    g_state.packet++;
    *state = g_state;
    return ERROR_SUCCESS;
}

__declspec(dllexport) void fxxi_set(int connected, unsigned buttons, int right_trigger, int lx)
{
    g_connected = connected;
    g_state.buttons = (WORD)buttons;
    g_state.right_trigger = (BYTE)right_trigger;
    g_state.lx = (SHORT)lx;
}

__declspec(dllexport) long fxxi_calls(void)
{
    return g_calls;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}
