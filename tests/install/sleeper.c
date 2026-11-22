/* Installer test helper: a process that just stays alive for a while.
 *
 * The installer tests copy it under a game executable's name to check that a
 * running game is detected. It exits on its own so a crashed test run cannot
 * leave it behind for long.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <windows.h>

int main(void)
{
    Sleep(120000);
    return 0;
}
