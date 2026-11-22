**Install:** extract the zip, then copy `version.dll` and `FC3PerformancePatch.ini` into the game's
`bin` folder. On Linux also set the Steam launch option `WINEDLLOVERRIDES="version=n,b" %command%`.
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
  leaves out only the levels that do not fit (`[Textures] SkipMismatchedMip0`), and the original
  close-up textures of textures a mod has repainted, which would otherwise show through a few metres
  ahead of you (`[Textures] SkipReplacedTextureMip0`).
- **A settings menu in the game (DX11).** F8, or both thumbsticks held in on a gamepad, opens a menu
  drawn by the patch: what it is doing, and its settings, which are saved to the INI file. The game
  does not see the keys used in it. `[Menu] Key` changes the key, `[Menu] Enabled=0` removes it.
- **ARM handhelds (Wine with FEX).** Fast and slow cores are told apart there (Wine does not report
  them), and the patch copes with launchers that move the game to other processors after it has
  started. The test suite passes under FEX; the game itself has not been tried on such a device yet.
- **DPI awareness.** The game is marked DPI aware, so on displays scaled above 100 % Windows no longer
  reports a smaller desktop to it or blurs windowed and borderless output (`[Display] HighDpiAware`).
- Windows no longer swaps the game window for a "Not responding" ghost during long loads
  (`[Display] DisableWindowGhosting`).
- The game's sound library (`XAudio2_7.dll`) is kept loaded for the whole session, the published remedy
  for a known XAudio2 2.7 defect in which the DLL is unloaded while still in use
  (`[Audio] KeepXAudio2Loaded`).
- Diagnostics: `[Debug] TraceAudio=1` logs how the game uses the sound system.
- The log of the previous start is kept as `FC3PerformancePatch.previous.log`, so starting the game
  again after a crash no longer destroys the evidence.
- **The installer scripts are gone.** Copying the two files is the one way to install; everything the
  patch does works from the DLL alone. If you installed 1.0.0 with `install.bat` or `install.sh`, run
  that version's uninstaller first if you want its backups restored, then copy the new files. The
  optional extras the installer offered (DXVK download, Control Flow Guard switch) are no longer part
  of this project.

**Unchanged from 1.0.0**

- Fixes the start-up hang/crash on processors with 32 or more threads (Far Cry 3 and Blood Dragon,
  DX9 and DX11).
- Gives the engine a clean processor layout: by default 6 physical cores, one thread each, without
  restricting the rest of the process. Configurable in the INI file.
- Enables the Large Address Aware flag (4 GB address space) on the game executable, with a backup.
- Requests a 1 ms system timer for the game.

**Tested:** Windows 11 with both games; Wine 11 / Proton Experimental with the test suite.
Frame-rate gains have not been benchmarked; see "What has been tested" in the README for the status
of each feature.

Verify the download with the `.sha256` file next to the zip.
