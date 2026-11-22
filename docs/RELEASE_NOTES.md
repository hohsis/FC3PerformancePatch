**Install:** extract the zip, then copy `version.dll` and `FC3PerformancePatch.ini` into the game's
`bin` folder (or run `install.bat` on Windows / `bash install.sh` on Linux and the Steam Deck).
On Linux also set the Steam launch option `WINEDLLOVERRIDES="version=n,b" %command%`.
See the [README](https://github.com/hohsis/FC3PerformancePatch#readme) for details.

Updating from 1.0.0: replace `version.dll`. Your existing INI file keeps working; the new settings
default to on. Copy the new INI too if you want them listed.

**New in 1.1.0**

- **Fullscreen survives Alt+Tab (DX11).** Switching away from the fullscreen game used to bring it back
  as a window that stayed windowed, because the engine mistakes Windows dropping fullscreen on focus
  loss for Alt+Enter. The patch now restores fullscreen when the game window is active again.
  Alt+Enter and the in-game option work as before. `[Display] RestoreFullscreenOnFocus=0` turns it off.
- **Texture mods no longer need mip0 loading switched off (DX11).** The game crashes in the graphics
  driver when a texture's separate top level ("mip0" file) is not exactly twice the size of the base
  texture, which is what many HD texture mods produce; hence their instruction to set
  `DisableMip0Loading="1"`, at the cost of detail on every texture they did not replace. The patch now
  leaves out only the levels that do not fit (`[Textures] SkipMismatchedMip0`). Tested on the game's real
  engine code outside the game; reports from play are welcome.
- **DPI awareness.** The game is marked DPI aware, so on displays scaled above 100 % Windows no longer
  reports a smaller desktop to it or blurs windowed and borderless output (`[Display] HighDpiAware`).
- Windows no longer swaps the game window for a "Not responding" ghost during long loads
  (`[Display] DisableWindowGhosting`).
- The game's sound library (`XAudio2_7.dll`) is kept loaded for the whole session, the published remedy
  for a known XAudio2 2.7 defect in which the DLL is unloaded while still in use
  (`[Audio] KeepXAudio2Loaded`).
- Diagnostics: `[Debug] TraceAudio=1` logs how the game uses the sound system.
- Windows installer: a copy of the game running from a different folder no longer blocks installing.

**Unchanged from 1.0.0**

- Fixes the start-up hang/crash on processors with 32 or more threads (Far Cry 3 and Blood Dragon,
  DX9 and DX11).
- Gives the engine a clean processor layout: by default 6 physical cores, one thread each, without
  restricting the rest of the process. Configurable in the INI file.
- Enables the Large Address Aware flag (4 GB address space) on the game executable, with a backup.
- Requests a 1 ms system timer for the game.
- Optional installer extras on Windows: DXVK (`-EnableDXVK`), Control Flow Guard off (`-DisableCFG`).

**Tested:** Windows 11 with both games; Wine 11 / Proton Experimental with the test suite.
Frame-rate gains have not been benchmarked; see "What has been tested" in the README for the status
of each feature.

Verify the download with the `.sha256` file next to the zip.
