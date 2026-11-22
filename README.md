# FC3PerformancePatch

A small, open-source compatibility and stability patch for **Far Cry 3** and **Far Cry 3 Blood Dragon** on
modern PCs: Windows 10/11 and Linux (Steam Proton, Steam Deck), with NVIDIA, AMD or Intel graphics.

It is a single 32-bit `version.dll` that the game loads by itself. It changes nothing on disk except,
optionally, one flag in the game executable. No game files are included or redistributed.

> Not affiliated with or endorsed by Ubisoft. Far Cry is a trademark of Ubisoft Entertainment.

## What it does

| | Problem in the original game | What the patch does |
|---|---|---|
| **Start-up hang** | On processors with **32 or more threads** (non-AMD), a processor-counting loop never ends: the game stays on the splash screen or crashes. | Fixes the loop in memory, in `FC3.dll`, `FC3_d3d11.dll` and `systemdetection.dll`. |
| **Processor handling** | The engine assumes every core has exactly two threads, pins one worker thread to *every* logical processor (both halves of each core), pins the whole process to single processors while it probes the CPU, and discards any affinity you set yourself. | Gives the engine a clean set of processors: by default **6 physical cores, one thread each** (performance cores first on hybrid Intel CPUs). The rest of the process (graphics driver, DXVK, audio) stays unrestricted. Fully configurable. |
| **2 GB memory limit** | The executables are not Large Address Aware, so the game can use only 2 GB of address space. | Enables the Large Address Aware flag (4 GB), keeping a backup of the original file. |
| **Timer resolution** | The engine calls `Sleep(1)` hundreds of times per second and relies on someone else having requested a fine system timer. | Requests 1 ms timer resolution itself (a safety net; see *What has been tested*). |
| **Fullscreen lost after Alt+Tab** (DX11) | Switch away from the fullscreen game and it comes back as a window, and stays one: the engine mistakes Windows dropping fullscreen on focus loss for you pressing Alt+Enter, and even overwrites its own Fullscreen setting. | Hides that drop from the engine and asks for fullscreen again when the game window is active. Alt+Enter and the in-game option still work. |
| **Sound engine crash** | The game's sound runs on XAudio2 2.7, which has a known defect: in some situations `XAudio2_7.dll` is unloaded while it is still in use. Players report crashes naming `XAudio2_7.dll_unloaded`. | Keeps the DLL loaded until the game exits. The sound itself is not changed. |
| **Display scaling** | The executables are not marked DPI aware. On a display scaled above 100 % Windows reports a smaller desktop to the game and blurs windowed and borderless output. | Marks the process DPI aware, and stops Windows replacing the window with a "Not responding" ghost during long loads. |

Details, evidence and the reverse-engineering notes are in [TECHNICAL.md](TECHNICAL.md).

**What it does not do:** it does not touch the game's DRM or online services, does not modify any Ubisoft
file on disk other than the Large Address Aware flag of the `.exe`, and it does not promise a specific
frame-rate gain. See [What has been tested](#what-has-been-tested) for what is verified and what is not.

## Install on Windows

Download `FC3PerformancePatch-<version>.zip` from the
[Releases page](https://github.com/hohsis/FC3PerformancePatch/releases) and extract it anywhere.

### Option A: drag and drop

Copy these two files into the game's `bin` folder (next to `farcry3.exe`):

```
version.dll
FC3PerformancePatch.ini
```

That is all. Start the game as usual. On the first start the patch enables the Large Address Aware flag
on the executable you launched (the original is kept as `<name>.exe.fc3pp-<date>.bak`); the larger address
space is active from the next start.

Typical `bin` locations:

- Steam: `C:\Program Files (x86)\Steam\steamapps\common\Far Cry 3\bin`
- Blood Dragon: `...\steamapps\common\Far Cry 3 Blood Dragon\bin`

### Option B: installer

Double-click **`install.bat`**. It finds the game (registry and Steam libraries, or asks you), copies the
two files, enables Large Address Aware on both executables and backs up everything it changes.
Running it twice is harmless.

Optional extras (run from a terminal in the extracted folder):

| Command | Effect |
|---|---|
| `install.bat -EnableDXVK` | Downloads the pinned [DXVK](https://github.com/doitsujin/dxvk) release, verifies its SHA-256 and installs the 32-bit `d3d11.dll`, `d3d9.dll`, `dxgi.dll`. Also sets `D3D11MultithreadedRendering="1"` in `GamerProfile.xml`. See [Graphics cards](#graphics-cards-nvidia-amd-intel). |
| `install.bat -DisableCFG` | Turns Windows *Control Flow Guard* off for the game executables only (asks for administrator permission). Some players report fewer long freezes; it lowers a security mitigation, so it is never done unless you ask. |
| `install.bat -GameDir "D:\Games\Far Cry 3"` | Skip detection and use this folder. |
| `install.bat -NoLAA` | Leave the executables alone. |

### Uninstall

Double-click **`uninstall.bat`**: removes the patch and restores every backup (executables, any
`version.dll` or Direct3D DLL that was there before, the `GamerProfile.xml` setting, the Control Flow Guard
setting). If you installed by drag and drop you can also just delete the two files and rename the `.bak`
file back, or use Steam's *Verify integrity of game files*.

## Install on Linux (Steam Proton, Steam Deck)

Drag and drop works here too, with one extra step that Wine makes unavoidable:

1. Download and extract the release zip (on the Steam Deck: in Desktop Mode).
2. Copy `version.dll` and `FC3PerformancePatch.ini` into the game's `bin` folder
   (in Steam: right-click the game > *Manage* > *Browse local files*, then open `bin`).
3. **Set the launch option** (required, once). In Steam: right-click the game > *Properties* > *General* >
   *Launch Options*:

   ```
   WINEDLLOVERRIDES="version=n,b" %command%
   ```

   Wine uses its own built-in `version.dll` unless told otherwise; without this line the patch is not
   loaded, however the files got there.

Prefer a script? `bash install.sh` does step 2 for you: it finds the game in your Steam libraries (native,
Flatpak or Snap Steam), copies the two files and enables Large Address Aware with backups.
`bash install.sh --set-launch-option` also writes the launch option (Steam must be closed).

The installer **never installs DXVK on Linux**: Proton already runs the game through its own DXVK.

`GamerProfile.xml` lives inside the game's Wine prefix:
`<SteamLibrary>/steamapps/compatdata/220240/pfx/drive_c/users/steamuser/Documents/My Games/Far Cry 3/`.
It is left alone unless you pass `--enable-mt-rendering`.

Uninstall with `bash uninstall.sh`. Run `bash install.sh --help` for all options.

## Check that it works

After starting the game, open `bin/FC3PerformancePatch.log`. A healthy log looks like this:

```
[INFO ] FC3PerformancePatch 1.1.0 (version.dll proxy)
[INFO ] host executable: farcry3_d3d11.exe
[INFO ] memory: process is large-address-aware (4 GB address space)
[INFO ] timing: requested 1 ms system timer resolution for this process
[INFO ] cpu: 8 physical cores detected via GetLogicalProcessorInformationEx; ...
[INFO ] cpu: Mode=engine - engine sees 6 logical processors on 6 cores (real processors 0,2,4,6,8,10, ...)
[INFO ] FC3_d3d11.dll: fixed the processor-enumeration loop (1 site, 'release' build variant)
[INFO ] initialised
[INFO ] systemdetection.dll: fixed the processor-enumeration loop (1 site, 'debug' build variant)
```

No log file means the DLL was not loaded: on Linux the launch option is missing; on Windows the files are
not in the `bin` folder. The log never contains folder paths, user names or computer names, so it is safe
to post when asking for help.

## Configuration

Everything is in `FC3PerformancePatch.ini` (each setting is explained in the file). The most useful ones:

| Setting | Default | Meaning |
|---|---|---|
| `[CPU] Mode` | `engine` | `engine`: the game's threads use the chosen cores, everything else is free. `process`: restrict the whole process to those cores (like Task Manager affinity). `off`: leave the game's processor handling untouched (the start-up fix still applies). |
| `[CPU] MaxCores` | `6` | Physical cores the game may use; `0` = all (the engine cannot use more than 15). |
| `[CPU] UseSMT` | `0` | `0`: one thread per core (skip Hyper-Threading/SMT siblings). `1`: both threads. |
| `[CPU] PreferPerformanceCores` | `1` | On hybrid CPUs choose P-cores before E-cores. |
| `[CPU] Cores` | empty | Explicit list such as `0,2,4,6` or `2-7`; replaces the automatic choice. |
| `[CPU] FixCoreEnumeration` | `1` | The start-up hang fix. |
| `[Timing] TimerResolutionMs` | `1` | `0` leaves the timer alone. |
| `[Timing] SleepZeroYield` | `0` | **Experimental.** Turns the engine's `Sleep(0)` spin-waits into real yields. Untested for gameplay benefit. |
| `[Memory] ApplyLargeAddressAware` | `1` | Let the DLL enable the 4 GB flag on the executable. |
| `[Display] RestoreFullscreenOnFocus` | `1` | DX11: return to fullscreen after Alt+Tab instead of staying windowed. |
| `[Display] HighDpiAware` | `1` | Mark the game DPI aware (same as the Windows compatibility setting *High DPI scaling override: Application*). |
| `[Display] DisableWindowGhosting` | `1` | No "Not responding" ghost window while the game loads. |
| `[Audio] KeepXAudio2Loaded` | `1` | Keep `XAudio2_7.dll` loaded for the whole session (see above). |
| `[Debug] TraceAudio` | `0` | Log the sound device format, the mixer set-up and the sample rates of the sounds played. |
| `[General] LogLevel` | `2` | `0` none, `1` errors, `2` normal, `3` debug. |

If something misbehaves, set `[General] Enabled=0`: the DLL then acts as a plain `version.dll`.

## Graphics cards (NVIDIA, AMD, Intel)

The patch itself contains nothing specific to a graphics vendor: it works at the processor, memory and
timing level, below the renderer. A few graphics-related recommendations:

- **`D3D11MultithreadedRendering`** (in `GamerProfile.xml`): leave it at `0` with the vendor's native
  Direct3D 11 driver. It is reported to cause flickering and crashes with some drivers. The installer
  only sets it to `1` together with DXVK (`-EnableDXVK`).
- **DXVK on Windows** (`install.bat -EnableDXVK`) translates Direct3D 9/11 to Vulkan, which can reduce the
  driver overhead of this draw-call-heavy game on NVIDIA, AMD and Intel Arc cards alike. It needs a
  reasonably recent graphics card and driver with up-to-date Vulkan support (see the DXVK project for
  the current requirements). If the game fails to start with DXVK, run `uninstall.bat` and install
  again without that option.
- **Intel integrated graphics**: prefer the DX11 executable; DXVK is the first thing to try if you see
  rendering problems.
- **Video memory detection**: the game reads the amount of video memory through an old Windows interface
  that cannot report more than 4 GB. This only affects the quality preset the game picks on its very
  first start; choose your settings in the options menu afterwards.
- **Linux**: Proton always uses DXVK. `--enable-mt-rendering` is therefore reasonable there, but it is not
  enabled by default because it has not been verified on every driver.

## What has been tested

Honest status, so you know what to expect:

**Verified**

- The start-up hang fix was tested by running the game's **own** processor-counting function (from the
  real, unmodified DLLs of the Steam versions of both games) with a simulated 32-processor machine:
  without the fix it never returns; with the fix it returns, and results for smaller machines are
  unchanged. All six engine modules of the two games pass.
- Both games start and run with the patch on Windows 11 (AMD Ryzen, NVIDIA RTX), with the DX11 and the DX9
  executable, including the Large Address Aware change.
- A trace of the running game confirmed the processor behaviour described above, and that with the patch
  the engine's worker threads end up on separate physical cores.
- The DLL, its installers and its tests pass under Wine 11 (Proton Experimental) on Linux and in CI
  (MSVC on Windows; MinGW-w64 cross build on Linux, tests under Wine).

**Not verified**

- Frame rate or stutter improvements have **not** been benchmarked. The processor defaults follow what
  the engine analysis and long-standing community advice suggest; tune `MaxCores` / `UseSMT` for your system.
- The timer-resolution setting made no measurable difference on the test system (something else in the
  game process already requested 1 ms). It is kept as a harmless safety net.
- The game itself was not run under Proton or on a Steam Deck, on AMD or Intel graphics, or on a real
  32-thread Intel processor. Reports are very welcome.
- `SleepZeroYield`, `-DisableCFG` and DXVK-on-Windows are optional extras without measured benefit here.
- **Fullscreen after Alt+Tab** (new in 1.1.0): the cause was found by decompiling the DX11 renderer, and
  the fix is tested against a stand-in for DXGI that reproduces the sequence (focus loss, Windows dropping
  fullscreen, return, Alt+Enter, failed mode switches). It has not yet been confirmed in the running game.
  It does nothing for the DX9 executable, whose behaviour was not analysed. If it misbehaves, set
  `RestoreFullscreenOnFocus=0`; `Borderless="1"` in `GamerProfile.xml` is an alternative that avoids
  exclusive fullscreen altogether.
- `KeepXAudio2Loaded` applies the published remedy for the XAudio2 2.7 unload defect, which is described
  as normally striking at exit. The crashes players report 10 to 15 minutes into a session name the same
  unloaded DLL, but that crash was not reproduced here, so whether this cures it is unconfirmed.
- **Sound quality** cannot be improved by a patch: the game mixes at 48 kHz, but the shipped sounds are
  low-rate recordings (dialogue 16 and 22 kHz, effects 24 and 32 kHz, music 32 kHz). See
  [TECHNICAL.md](TECHNICAL.md#8-audio).
- `DisableWindowGhosting` is a precaution; whether the game's loading screens trigger ghosting was not measured.

## Troubleshooting

- **Game does not start after installing**: set `Enabled=0` in the INI to confirm it is the patch, then
  please open an issue with `FC3PerformancePatch.log`.
- **Another mod already ships `version.dll`**: the installer backs it up and replaces it; both cannot be
  active at the same time. Uninstalling restores the other mod's file.
- **Antivirus warning**: DLLs that a game loads from its own folder are sometimes flagged generically.
  The source is here; you can build it yourself (below).
- **Steam "Verify integrity" restores the `.exe`**: that removes the Large Address Aware flag; the patch
  re-applies it on the next start (or run `install.bat` again).
- **Steam Deck**: use the launch option exactly as written above, including `%command%`.

## Building from source

Requirements: CMake 3.20+, Python 3 (tests only) and either

- **Windows:** Visual Studio 2022 Build Tools with the C++ x86 toolset, then

  ```powershell
  .\build.ps1          # configure, build (32-bit Release) and run all tests
  ```

- **Linux (cross build):** `gcc-mingw-w64-i686`, optionally `wine32` for the tests, then

  ```sh
  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake -DCMAKE_BUILD_TYPE=Release
  cmake --build build
  ctest --test-dir build --output-on-failure
  ```

The package (DLL, INI, scripts, documentation) is assembled in `build/package/FC3PerformancePatch`.
Builds are reproducible and contain no local paths.

### Tests

| Suite | What it covers |
|---|---|
| `unit` | INI parser, signature scanner, PE/LAA/checksum code, core-selection policy, the loop patch (executed on replicas of all four compiler variants), import hooking, the fullscreen-guard state machine |
| `runtime` | The DLL loaded into a 32-bit host that mimics the game's module chain: export forwarding, processor layer, late-loaded and re-loaded modules, LAA self-patching, the fullscreen guard against a fake `dxgi.dll`, pass-through modes, log privacy. Runs natively and under Wine. |
| `real_game_binaries` | The fix applied to your own game files in memory and executed (skipped without a game: set `FC3PP_GAME_BIN` to one or more `bin` folders separated by `;`) |
| `export_table` | The DLL exports exactly what the Windows `version.dll` exports; only universally available imports |
| `installer_windows` / `installer_linux` | Install, re-install and uninstall round trips in a fake Steam library, byte-for-byte restore |
| `opsec_*` | No personal data (paths, user names, e-mail addresses, Steam IDs) in sources, history or binaries |

Enable the commit hooks that run the privacy audit with `git config core.hooksPath tools/hooks`.

## License

Copyright (C) 2026 FC3PerformancePatch contributors.
Licensed under the [GNU General Public License v3.0 or later](LICENSE).

The optional DXVK download is a separate project under the zlib license and is not part of this repository.
