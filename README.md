# FC3PerformancePatch

A small open-source fix pack for **Far Cry 3** and **Far Cry 3 Blood Dragon** on modern PCs: Windows 10/11
and Linux (Steam Proton, Steam Deck).

It is one 32-bit `version.dll` that the game loads by itself. Nothing is installed, no game file is
replaced, and no Ubisoft files are included.

> Not affiliated with or endorsed by Ubisoft. Far Cry is a trademark of Ubisoft Entertainment.

## What it fixes

| Problem in the original game | With the patch |
|---|---|
| **Hangs or crashes at start** on processors with 32 or more threads | Fixed in memory, in all engine modules |
| **Poor use of modern processors**: one worker pinned to every thread of every core | The engine gets 6 physical cores, one thread each (configurable); drivers and audio stay unrestricted |
| **2 GB memory limit** | 4 GB (Large Address Aware flag on the `.exe`, original kept as a backup) |
| **Fullscreen lost after Alt+Tab** (DX11): the game comes back windowed and stays windowed | Fullscreen is restored when you return |
| **Texture mods crash the game** (DX11) unless `DisableMip0Loading="1"` is set, which lowers detail everywhere | Only the texture levels that do not belong to the mod's textures are left out; no special `GamerProfile.xml` needed |
| **Blurry or mis-sized window** on displays scaled above 100 % | The game is marked DPI aware |
| **Sound library unloaded while in use** (`XAudio2_7.dll_unloaded` crashes) | The library stays loaded |
| Coarse system timer, "Not responding" window while loading | 1 ms timer requested, ghost window disabled |

How each problem was found and how the fix works is in [TECHNICAL.md](TECHNICAL.md).

The patch does not touch the game's DRM or online services and does not promise a frame-rate gain.

## Install

Download `FC3PerformancePatch-<version>.zip` from the
[Releases page](https://github.com/hohsis/FC3PerformancePatch/releases) and copy two files into the game's
`bin` folder (the one with `farcry3.exe`):

```
version.dll
FC3PerformancePatch.ini
```

In Steam: right-click the game > *Manage* > *Browse local files*, then open `bin`.

**Linux, Steam Deck:** also set this launch option once (right-click the game > *Properties* >
*Launch Options*). Wine ignores the file without it.

```
WINEDLLOVERRIDES="version=n,b" %command%
```

On the first start the patch switches the executable to 4 GB of address space, which takes effect from
the second start.

**Uninstall:** delete the two files. To undo the 4 GB change as well, rename
`<name>.exe.fc3pp-<date>.bak` back to `<name>.exe`, or use Steam's *Verify integrity of game files*.

## Check that it works

Start the game, then open `FC3PerformancePatch.log` in the `bin` folder. It should start like this:

```
[INFO ] FC3PerformancePatch 1.1.0 (version.dll proxy)
[INFO ] memory: process is large-address-aware (4 GB address space)
[INFO ] cpu: Mode=engine - engine sees 6 logical processors on 6 cores ...
[INFO ] FC3_d3d11.dll: fixed the processor-enumeration loop (1 site, 'release' build variant)
[INFO ] initialised
```

No log file means the DLL was not loaded: the files are not in `bin`, or on Linux the launch option is
missing. The log never contains folder paths, user names or computer names, so it is safe to post. The log
of the previous start is kept as `FC3PerformancePatch.previous.log`.

## Settings

`FC3PerformancePatch.ini` explains every setting. The ones most worth knowing:

| Setting | Default | Meaning |
|---|---|---|
| `[CPU] MaxCores` | `6` | Physical cores the game may use; `0` = all |
| `[CPU] UseSMT` | `0` | `1` = use both threads of each core |
| `[CPU] Mode` | `engine` | `process` also restricts drivers and audio to those cores; `off` leaves the game's own handling |
| `[Display] RestoreFullscreenOnFocus` | `1` | Fullscreen after Alt+Tab (DX11) |
| `[Textures] SkipMismatchedMip0` | `1` | Texture mod crash guard (DX11) |
| `[Textures] SkipReplacedTextureMip0` | `1` | Keeps the game's original close-up texture from showing through a mod's replacement |
| `[General] Enabled` | `1` | `0` turns the whole patch off |

## Notes

- **Texture mods:** install the mod's `patch.dat` and `patch.fat` as its instructions say, but keep your
  own `GamerProfile.xml`. DX11 only; with the DX9 executable keep the mod's `DisableMip0Loading="1"`.
- **Another mod that ships `version.dll`** cannot be active at the same time.
- **`D3D11MultithreadedRendering`** in `GamerProfile.xml`: leave it at `0` with the normal graphics
  driver; it is reported to cause flickering and crashes.
- **Sound quality** cannot be improved by a patch: the shipped sounds are low-rate recordings.
- **Steam's "Verify integrity"** restores the `.exe`; the patch applies the 4 GB flag again on the next start.

## Status

Verified:

- The start-up fix, by running the game's own processor-counting code from the real DLLs of both games on
  a simulated 32-thread machine.
- Both games start and run with the patch on Windows 11 (AMD Ryzen, NVIDIA), DX11 and DX9.
- Fullscreen after Alt+Tab, in play (Far Cry 3, DX11).
- The texture crash: cause established from a crash dump; the fix is tested on the real engine DLLs of
  both games, and Far Cry 3 (DX11) was played with a large HD texture mod and the game's normal settings:
  it loads and runs where it crashed before, and the log shows the unfitting levels being left out.
- The test suite passes on Windows and under Wine on Linux.

Not verified:

- Frame-rate or stutter improvements: not benchmarked.
- The texture guard with Blood Dragon or with the DX9 executable (not supported there).
- `SkipReplacedTextureMip0` in play: it is tested on synthetic archives and finds the expected files in a
  real modded installation, but the result on screen was not yet checked.
- The game under Proton or on a Steam Deck, on AMD or Intel graphics, or on a real 32-thread Intel processor.
- Whether keeping `XAudio2_7.dll` loaded cures the crashes players report mid-session.

Reports are welcome; please attach the log.

## Building

CMake 3.20+ and either Visual Studio 2022 Build Tools (`.\build.ps1`) or MinGW-w64 i686
(`cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake && cmake --build build`).
`ctest --test-dir build` runs the tests, under Wine when cross-compiling. See
[TECHNICAL.md](TECHNICAL.md#11-tests-and-their-limits) for what they cover.

## License

Copyright (C) 2026 FC3PerformancePatch contributors.
[GNU General Public License v3.0 or later](LICENSE).
