First release.

**Install:** extract the zip, then either copy `version.dll` and `FC3PerformancePatch.ini` into the game's
`bin` folder, or run `install.bat` (Windows) / `bash install.sh` (Linux, Steam Deck).
On Linux also set the Steam launch option `WINEDLLOVERRIDES="version=n,b" %command%`.
See the [README](https://github.com/hohsis/FC3PerformancePatch#readme) for details.

**What it does**

- Fixes the start-up hang/crash on processors with 32 or more threads (Far Cry 3 and Blood Dragon,
  DX9 and DX11).
- Gives the engine a clean processor layout: by default 6 physical cores, one thread each, without
  restricting the rest of the process. Configurable in the INI file.
- Enables the Large Address Aware flag (4 GB address space) on the game executable, with a backup.
- Requests a 1 ms system timer for the game.
- Optional installer extras on Windows: DXVK (`-EnableDXVK`), Control Flow Guard off (`-DisableCFG`).

**Tested:** Windows 11 with both games; Wine 11 / Proton Experimental with the test suite.
Frame-rate gains have not been benchmarked; see "What has been tested" in the README.

Verify the download with the `.sha256` file next to the zip.
