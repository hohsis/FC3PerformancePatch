# Far Cry 3 on ARM handhelds

For devices with an ARM processor that run the Windows version of the game through an x86 emulator:
Linux systems with ARM64 Steam, FEX and Proton (Armada OS and similar), and Android launchers that work
the same way.

## What an "ARM version" can and cannot be

There is no ARM version of the game and this project cannot make one. The game is about 29 MB of
32-bit x86 machine code in Ubisoft's own files, without source code; turning that into ARM code is
exactly what the emulator on your device does every time you play, and shipping a converted copy
would mean distributing Ubisoft's files, which this project does not do.

There is also no ARM build of the patch, on purpose. The patch is a DLL that is loaded *into* the
game, and the game is a 32-bit x86 process: everything inside it, the patch included, is x86 code
that the emulator translates. An ARM DLL could not be loaded there. (Windows and Wine can mix native
ARM code into emulated programs only for 64-bit ones.)

So the patch you install on an ARM device is the same `version.dll` as on a PC. What it can do there
is make the emulated game behave well: put its threads on the right cores, give it less to do, and
tell you (and this project) what actually happened on the device.

## Installing

1. Copy `version.dll` and `FC3PerformancePatch.ini` into the game's `bin` folder, as on a PC.
2. In Steam, set this launch option for the game once. Without it Wine ignores the file:

   ```
   WINEDLLOVERRIDES="version=n,b" %command%
   ```

3. Start the game with Direct3D 11 (the normal *Play* entry, not the DX9 one). The in-game menu and
   the texture-mod fixes only exist there.
4. In the game, hold both thumbsticks pressed in for a moment: the patch's menu opens and can be
   used with the D-pad, A and B. `[Menu] Scale` or *This menu > Size* makes it larger.

`FC3PerformancePatch.log` in the same folder should contain the line
`running under the FEX x86 emulator (ARM processor)`. If the file is not there, the launch option
is missing.

## What the patch does on such a device by itself

- **It tells faster and slower cores apart.** Wine reports all ARM cores as alike. The patch reads the
  capacity Linux publishes for each core and ranks them.
- **It keeps the two fastest cores free of the game's workers.** The game pins one worker thread to
  every core it is given and does not place its main and render threads, which are the ones that
  limit the frame rate. On a Snapdragon 8 Elite (2 prime cores, 6 performance cores) the workers
  therefore get the six performance cores, and the system can put the busy unpinned threads on the
  prime cores. The log says so: `the 2 fastest cores ... get no worker of the game pinned to them`.
- **It copes with launchers that move the game** to other processors after it has started.
- **It writes a thread report** when the game exits: the busiest threads, whether the game pinned
  them, and on which processors the threads that present the picture and read the gamepad were seen.

## Settings worth trying, one at a time

All of these are in the patch's in-game menu; those marked *(restart)* take effect at the next start
of the game.

| Setting | Try | What to look for |
|---|---|---|
| The game's own video options | Lowest quality, lower resolution | The base line. Do this first |
| *Draw distance* *(restart)* | *Near*, then *Very near* | Frame rate up; things appear closer to you |
| *Cores for the game* *(restart)* | 4 instead of 6 | Fewer workers leave more of the processor to the emulator's and the graphics layer's own threads |
| *Fast cores kept free* *(restart)* | 0 instead of Auto | The other layout: workers on the fastest cores. Compare the frame rate |
| `[Timing] SleepZeroYield=1` (INI file only) | 1 | Experimental: the game's busy-waiting becomes real waiting. Might lower heat; might cost frame rate |

To compare two settings, use *Diagnostics > Measure processor use for 30 s* at the same place in the
game with each: the log then holds two lists that can be laid side by side.

## The emulator's own settings

Not part of this patch, and nothing here has been tried by this project; listed because it is what
the game's code suggests. Counting the instructions of `FC3_d3d11.dll` (10.4 million):

| Kind | Share | What it means under emulation |
|---|---|---|
| SSE floating point | 5.9 % | Translates well to ARM |
| x87 floating point | 1.6 % | Mostly values handed back from functions, little arithmetic. A "reduced x87 precision" option in the emulator matters less here than in older games |
| `lock`-prefixed (atomic) instructions | 0.4 % | The game shares data between threads all the time. Leave the emulator's memory-ordering (TSO) emulation on |
| `pause` | none | The game does not spin in tight loops; it waits with `Sleep(0)` |

## What to send back

`FC3PerformancePatch.log` from the `bin` folder after a session in which you played for a few
minutes (it is overwritten at the next start, so copy it first). It contains no paths, user names or
device names. It answers the questions nobody can answer without the device: which cores the game got,
which threads were busy and where they ran, and whether everything else in the patch found its place
in the game under the emulator.

## Status

Tested: the whole test suite of this project, including the code patches executed as machine code
and the hooks inside a stand-in for the game, passes on an ARM64 machine under Wine with FEX (the
same arrangement as ARM builds of Proton), and the core selection is tested with the layouts of the
Snapdragon 8 Gen 2 and 8 Elite.

Not tested: **the game itself on an ARM device.** Which core layout is faster, what the *Near*
presets gain, and whether the game runs well enough at all are open until someone tries it there.
