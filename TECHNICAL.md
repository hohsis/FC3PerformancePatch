# Technical notes: engine root-cause analysis

This document explains what FC3PerformancePatch changes in Far Cry 3 (Dunia Engine 2) and why, with the
evidence behind each change. It is written for developers and reverse engineers.

Analysed builds (Steam, 32-bit x86):

| Game | Module | PE timestamp | Notes |
|---|---|---|---|
| Far Cry 3 | `FC3.dll`, `FC3_d3d11.dll` | Oct 2022 | optimised MSVC build, not packed |
| Far Cry 3 | `systemdetection.dll` | Nov 2012 | **unoptimised debug-style build** (run-time stack checks, `.textbss`) |
| Blood Dragon | `FC3.dll`, `FC3_d3d11.dll` | Oct 2019 | size-optimised build |
| Blood Dragon | `systemdetection.dll` | Mar 2013 | optimised build |

No offsets are hard-coded: everything is located by byte signature or through the import table, so other
builds (Ubisoft Connect, Epic) work as long as the code shape is the same. When a signature is not found
the patch says so in its log and falls back to import-table hooks.

Addresses below are relative virtual addresses (RVAs) in the Far Cry 3 `FC3_d3d11.dll` named above and
are given only as landmarks.

## 1. How the patch gets loaded

`farcry3.exe` / `farcry3_d3d11.exe` are 200 KB stubs that statically import `FC3.dll` / `FC3_d3d11.dll`.
Those import three functions from `VERSION.dll` (`GetFileVersionInfoA`, `GetFileVersionInfoSizeA`,
`VerQueryValueA`). A `version.dll` in the application folder is therefore loaded as part of process
start-up.

The loader maps every statically imported module and binds all import tables **before** it runs any
initialisation routine, and it initialises dependencies first. When the proxy's `DllMain` runs, the engine
DLL is already mapped and bound but has not executed a single instruction. That is the ideal moment to
patch code and replace import-table entries, with no races.

`systemdetection.dll` is loaded later with `LoadLibrary`, **unloaded, and loaded again** a few seconds
later. The proxy hooks the engine's `LoadLibrary*` imports and re-applies its work each time; a reloaded
module is a fresh image (usually at the same address), so nothing may be cached by module handle.

The proxy forwards all 17 exports of the system `version.dll`, which it loads lazily from the system
directory on first use (never from `DllMain`). In any process that does not contain an engine module (the
updater and editor in the same folder also load `version.dll`) it stays completely passive.

On Wine/Proton the built-in `version.dll` wins unless `WINEDLLOVERRIDES="version=n,b"` is set; with the
override Wine loads the proxy and the proxy's `LoadLibrary` of the system-directory path yields the
built-in. Verified with Wine 11.0 (Proton Experimental).

## 2. The start-up hang: a processor-counting loop that cannot terminate

### The bug

Every engine module of both games (`FC3.dll`, `FC3_d3d11.dll` and `systemdetection.dll`) contains a copy
of a classic CPUID-based processor-counting routine (RVA `0xC51030` in `FC3_d3d11.dll`). It walks the affinity mask one bit at a time, pins the thread to each
processor and reads its APIC ID:

```c
GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
if (processMask != systemMask) return false;

i = 0; mask = 1;
while (mask != 0 && mask <= systemMask) {
    if (SetThreadAffinityMask(thread, mask)) {
        Sleep(0);
        tblPkgID[count] = ...; tblCoreID[count] = ...;   // 256-entry tables on the stack
        count++;
    }
    i++;
    mask = 1 << i;
}
```

The loop relies on `mask` becoming 0 after the top bit. On x86 the shift count is taken modulo 32, so
`1 << 32` is `1`, not `0`: `mask` never becomes zero. The only remaining exit is `mask > systemMask`,
which is impossible once `systemMask == 0xFFFFFFFF`, that is, with **32 or more logical processors**
visible to the 32-bit process. The loop then repeats over the same 32 masks forever while `count` keeps
growing and the stack tables are overrun, which explains both symptoms players report: a hang on the
splash screen, or a crash.

Which machines are affected: the engine selects its detection routine by CPUID vendor string.
`AuthenticAMD` (and `AMD ISBETTER`) take a different routine (section 3); every other vendor, in practice
**Intel**, takes the routine above. So the trigger is an Intel processor with 32 or more threads.

### The fix

The loop tail is rewritten in place so that it stops after bit 31. The same source compiles to four
different instruction sequences across the two games, each with its own signature and same-size
replacement (`src/patches.c`):

| Variant | Found in | Original tail | Replacement |
|---|---|---|---|
| `release` | FC3 `FC3.dll`, `FC3_d3d11.dll` | `inc ecx; mov esi,1; shl esi,cl; mov [ebp+d8],ecx; test esi,esi; jnz head` | `inc ecx; mov [ebp+d8],ecx; xor esi,esi; bts esi,ecx; cmp ecx,32; jb head; nop` |
| `debug` | FC3 `systemdetection.dll` | `mov eax,[i]; add eax,1; mov [i],eax; mov eax,1; mov ecx,[i]; shl eax,cl; mov [mask],eax; jmp head` | `mov ecx,[i]; inc ecx; mov [i],ecx; xor eax,eax; cmp ecx,32; jae +3; bts eax,ecx; mov [mask],eax; jmp head` (mask becomes 0 at i = 32, which the existing `mask != 0` test turns into a normal exit) |
| `release-disp32` | Blood Dragon `systemdetection.dll` | as `release`, with 32-bit stack displacements | as `release` |
| `release-ebx` | Blood Dragon `FC3.dll`, `FC3_d3d11.dll` | `inc [i]; mov ecx,[i]; mov esi,ebx; shl esi,cl; test esi,esi; jnz head` (ebx holds 1) | only `test esi,esi` becomes `cmp esi,ebx`: since i is at least 1 here, `1 << i == 1` can only mean the shift count wrapped |

Before writing, each match is validated (backward branch, plausible distance, expected compare at the loop
head); anything unexpected is left untouched and reported. A known third-party fix that makes the loop run
exactly once (so the game counts a single processor) is recognised; its `release` form is upgraded to the
proper fix.

A second, build-independent line of defence: in the default configuration the engine is never shown an
affinity mask that reaches bit 31 (section 3), and if a signature is not found with the processor layer
switched off, a minimal hook clamps the reported masks instead.

### Verification

`tests/realbin/test_realbin.c` maps the user's real, unmodified DLLs (`DONT_RESOLVE_DLL_REFERENCES`: no
game code runs on load), replaces the five Win32 imports the routine uses with fakes that simulate a
machine, and calls the game's **own** function:

```
FC3_d3d11.dll
  enumeration loop: 'release' variant at RVA 0x00C511B1
  unpatched, 16 processors: returns (16 logical, 1 cores, 1 packages)
  unpatched, 32 processors: never returns (bug reproduced)
  patched,   16 processors: returns (16 logical, 1 cores, 1 packages) - unchanged
  patched,   32 processors: returns (32 logical, 1 cores, 1 packages)
```

All six modules of both games behave this way. The core/package numbers are meaningless in
this test because the fake does not actually move the thread between processors. The test is skipped
where no game is installed; CI covers the same logic with hand-assembled replicas of the four loop shapes
(`tests/unit/loop_blobs.h`) that are executed before and after patching.

Limitation: the hang could not be reproduced end to end in the running game, because the test machine has
an AMD processor and the engine never reaches this routine there.

## 3. How the engine treats processors, and the processor layer

Observed by decompiling the relevant functions and confirmed with a call trace from the running game
(`[Debug] TraceCpuApi=1`):

1. **AMD detection routine** (RVA `0xC50E90`). For each processor it calls
   `SetProcessAffinityMask(process, 1 << i)` (the whole process, not the thread), reads CPUID, and finally
   "restores" with `SetProcessAffinityMask(process, 1 << initialApicId)`. That last call pins the entire
   process to one logical processor. On the 16-thread test machine the trace shows masks
   `0x1, 0x2, ... , 0x8000`, i.e. the process ends up on logical processor 15.
2. **Job manager initialisation** (RVA `0x6EB890`) then calls
   `SetProcessAffinityMask(process, systemMask)`. This undoes the pin above, and also **discards any
   affinity set from outside** before the game started.
3. The job manager derives its layout from `GetSystemInfo` and one CPUID bit: if the HTT flag is set (it
   is on every modern multi-core processor, with or without SMT) it assumes `cores = logical / 2` with
   exactly two threads per core, adjacent in numbering; capped at 15 cores. Roughly one worker thread is
   created per logical processor and each is **hard-pinned to one logical processor** with
   `SetThreadAffinityMask`.
   On an SMT machine that puts two busy workers on every physical core; with SMT disabled it halves the
   core count; on hybrid processors the pairing assumption is simply wrong.
4. **`systemdetection.dll`** repeats the per-processor probing with its own code.
5. **Havok thread pool** (RVA `0x8BFD40`): thread count is `min(dwNumberOfProcessors, 12)`, replaced by
   the number of `RelationProcessorCore` records from `GetLogicalProcessorInformation` (resolved with
   `GetProcAddress`) when that buffer is at most 3072 bytes. Workers get
   `SetThreadIdealProcessor((index + 3) % dwNumberOfProcessors)`.
6. A `-affinity <mask>` command-line switch exists and is passed straight to `SetProcessAffinityMask`.

### The processor layer (`[CPU] Mode=engine`, default)

Instead of only restricting the process, the patch gives the engine a small, consistent machine and
translates what the engine does onto the real one. Import-table hooks are installed **only in the game's
own modules** (`src/cpu_shim.c`):

| Import | Behaviour |
|---|---|
| `GetSystemInfo` | reports the selected processor count and mask |
| `GetProcessAffinityMask` | process mask = system mask = the virtual processors |
| `SetThreadAffinityMask` | virtual mask translated to the real processors; "all" means the selected set |
| `SetThreadIdealProcessor` | virtual index translated |
| `SetProcessAffinityMask` | accepted and ignored (process affinity is owned by the patch) |
| `GetProcAddress("GetLogicalProcessorInformation")` | returns a synthesised topology that matches the virtual machine |

The selection (`src/cpu_topology.c`) is built from `GetLogicalProcessorInformationEx` (falling back to
`GetLogicalProcessorInformation`, then `GetSystemInfo`): by default up to 6 physical cores, one logical
processor each, highest `EfficiencyClass` first, within the affinity the process was started with. At most
30 processors are ever presented, so no engine-visible mask reaches bit 31.

Effect, as traced in the running game on an 8-core/16-thread processor: the engine sees 6 processors, six
threads are pinned one each to real processors 0, 2, 4, 6, 8, 10 (six different physical cores), Havok is
told there are 6 cores, and the process affinity stays `0xFFFF`, so driver, audio and DXVK threads can use
the other ten logical processors. In the main menu the stock game ran 143 threads, the patched one 130.

`Mode=process` additionally applies the selection as real process affinity (the behaviour of the usual
"limit the game to N cores" advice). `Mode=off` installs none of these hooks.

What is **not** established: that this layout is faster. The design removes behaviour that is clearly
wrong (process-wide pinning, SMT-oblivious worker placement, discarded user affinity), and 4 to 6 cores
without SMT is the long-standing community recommendation for this engine, but no gameplay benchmark was
run for this document.

### ARM processors: Wine with the FEX emulator

On ARM64 devices the game runs as x86 code translated by FEX inside an ARM64 build of Wine (Valve's ARM
Proton and Android launchers work this way). Three things matter to this patch there:

- **Code patches.** Translated code is cached, so a patch must be announced. `mem_patch` brackets every
  write with `VirtualProtect` and calls `FlushInstructionCache`; Wine forwards both to the emulator, which
  discards its translation of the range. The patches are also applied before the engine has run.
- **Fast and slow cores.** Wine fills in `EfficiencyClass` only for Intel hybrid processors (it reads the
  Intel-specific `/sys/devices/cpu_core` node). On an ARM "big.LITTLE" processor every core is class 0,
  and the small cores have the lowest numbers (Snapdragon 8 Gen 2: cpu0-2 small, cpu3-6 big, cpu7 prime),
  so "six cores, performance cores first" would have taken every small core and never the fastest one.
  Under Wine, when no classes are reported, the patch reads each core's
  `/sys/devices/system/cpu/cpuN/cpu_capacity` (through `\\?\unix\...`, falling back to drive `Z:`) and
  ranks the cores by it. If the files cannot be read, nothing changes.
- **Being moved afterwards.** Android launchers confine the game to a set of processors from outside,
  after its window appears. An engine request for a processor the process has since lost would fail;
  the patch then places the thread on what is left of the request, or anywhere in the process's set.

FEX reports itself through the CPUID hypervisor leaf (`FEXIFEXIEMU`); the log notes it. Status: the whole
test suite, including the code patches executed as machine code and the hooks inside a host process, passes
on a GitHub ARM64 runner under Wine 11 with FEX as the 32-bit CPU back end (Hangover's build). That machine
has four identical cores and no GPU: core ranking is covered by unit tests with a big.LITTLE layout, the
confinement case by the runtime test, and **the game itself has not been run on an ARM device.**

## 4. Timing

The engine imports only `timeGetTime` from `winmm.dll`; it never calls `timeBeginPeriod`. Since Windows 10
2004 the timer resolution is a per-process property, so a process that does not ask gets ~15.6 ms
granularity for `Sleep` and timed waits.

With the Sleep import hooked (debug log level), about two minutes in the main menu produced:

```
Sleep(0) = 1,069,489 calls      Sleep(1) = 80,932 calls      longer = 4,426 calls
```

So the engine spins on `Sleep(0)` about 9,000 times per second and issues about 700 `Sleep(1)` per second;
a coarse timer would turn each of those into a 15 ms stall.

Measured, however: with the patch's timer request **disabled** an engine `Sleep(1)` took 1.8 to 1.9 ms on
average on the test system (Windows 11), the same as with it enabled. Some other component in the game
process (launcher overlay or middleware) already requests a 1 ms timer. `TimerResolutionMs=1` is therefore
a safety net for configurations where that component is absent, not a demonstrated improvement.

`SleepZeroYield=1` (experimental, off by default) turns `Sleep(0)`, which only yields to ready threads of
equal priority, into `SwitchToThread()`. Its effect on frame pacing has not been measured.

No changes are made to the main loop or to frame limiting. `GamerProfile.xml` has a `MaxFps` attribute
that the engine reads if present.

## 5. Memory

Both executables of both games ship without `IMAGE_FILE_LARGE_ADDRESS_AWARE` (`0x0020`), limiting the
process to 2 GB of address space. The flag lives in the executable's file header and is read when the
process is created, so it cannot be applied in memory.

The DLL (`src/laa.c`) sets the flag on disk and recomputes the PE checksum. Because a
running image cannot be overwritten, the DLL writes a patched copy, renames the running executable to
`<name>.exe.fc3pp-<UTC timestamp>.bak` and moves the copy into place; this also works under Wine. The
executables are Authenticode-signed; changing the flag invalidates the signature, which has no practical
effect (the launcher and the game start normally, verified for both games).

The engine's allocator and streaming code were **not** modified. Beyond the address-space limit no
allocator defect was identified in this analysis.

## 6. Graphics hardware detection

- `systemdetection.dll` queries WMI `Win32_VideoController.AdapterRAM` and stores the 32-bit result
  zero-extended into a 64-bit field. There is no signed overflow in that path, but the WMI property itself
  cannot express more than 4 GB, so large cards are under-reported. The value feeds the hardware score
  used for first-run quality presets. Not patched.
- The engine modules contain a handful of comparisons against the PCI vendor IDs `0x10DE` (NVIDIA),
  `0x1002` (AMD) and `0x8086` (Intel), alongside references to NVAPI and the AMD display library. These
  vendor-specific branches were not analysed individually and are not altered. No failing hardware check
  was observed (tested on NVIDIA only).
- `D3D11MultithreadedRendering` is left at the user's value.

## 7. Display: fullscreen after focus loss, DPI awareness

### Why the DX11 game comes back from Alt+Tab as a window

The game window's procedure (RVA 0x17B5FF0) does very little on `WM_ACTIVATE`: it mutes or unmutes audio
and switches the Sticky/Toggle/Filter Keys shortcuts off and on. Nothing there touches the display mode.
Fullscreen is managed entirely by polling. After every `Present`, the D3D11 renderer runs (RVA 0x14EDF10):

```c
swapChain->GetFullscreenState(&state, &output);
bool now = (state == TRUE);
if (renderer->fullscreen != now) {
    swapChain->SetFullscreenState(now, NULL);
    PostMessage(hwnd, WM_USER + 0x393, now, 0);
    renderer->fullscreen = now;
    config->Fullscreen   = now;          // the user's setting is overwritten
}
```

That is how Alt+Enter is supported: the window association is made with `DXGI_MWA_NO_PRINT_SCREEN` only,
so DXGI handles the key itself and the engine follows whatever state it finds. But DXGI also leaves
exclusive fullscreen when the window loses focus. The engine cannot tell the two apart: it adopts
"windowed", confirms it with an explicit `SetFullscreenState(FALSE)`, stores it as the setting, and the
next function in the frame (RVA 0x14EE830) restyles and resizes the window for windowed mode. Nothing ever
asks for fullscreen again.

### The fullscreen guard (`[Display] RestoreFullscreenOnFocus`)

The patch hooks the engine module's import of `CreateDXGIFactory1`, and through the factory's virtual
table the creation of swap chains, and through the swap chain's virtual table `GetFullscreenState`. COM
objects of one class share a table, so the hooks filter on the object: only factories returned to the
engine and swap chains created from them are affected; an overlay's or capture tool's objects pass
through untouched. No engine code is modified, so the same mechanism serves both games and works with
DXVK's DXGI as well.

The hook feeds a small state machine (`src/fs_guard.c`):

| Observation | The engine is told | Action |
|---|---|---|
| Fullscreen | fullscreen | remember that the game means to be fullscreen |
| Windowed, never was fullscreen | windowed | none |
| Windowed, and the game window **was the foreground window** when it happened | windowed | none: Alt+Enter or the options menu |
| Windowed, and the game window was **not** in the foreground | **fullscreen** | none while in the background |
| ... and the window is in the foreground again | fullscreen | `SetFullscreenState(TRUE)`, at most every 250 ms |
| 20 failed attempts | windowed | give up; the game continues as it would without the patch |

The swap chain's own description (`GetDesc`) supplies the window, so a recreated swap chain or window is
followed automatically. The engine recreates its swap chain when the output size changes; each new one
starts with a fresh state.

Known limits: a loss of fullscreen that happens while the game window is still reported as the foreground
window (the secure desktop of a UAC prompt, for example) is indistinguishable from Alt+Enter and is passed
on unchanged. The DX9 renderer (`FC3.dll`) uses the Direct3D 9 device-lost protocol instead and was not
analysed.

Status: derived from the decompiled code above, tested against a stand-in `dxgi.dll` that reproduces the
sequence, and observed in the running game (Far Cry 3, DX11, Windows 11): in a 33-minute session the log
shows three focus-loss drops, each followed by fullscreen being reported again. The first drop came 0.4 s
after the game entered fullscreen during start-up, before any Alt+Tab; without the guard that alone would
leave the game windowed. Blood Dragon was not played with it.

### DPI awareness and window ghosting

None of the four executables has a DPI awareness entry in its manifest, and the engine never calls
`SetProcessDPIAware`. On a display scaled above 100 % the process is DPI-virtualised unless the user
(or Windows, through a compatibility layer) overrides it: `GetSystemMetrics` reports a smaller desktop and
the compositor bitmap-scales windowed and borderless output. `[Display] HighDpiAware=1` calls
`SetProcessDPIAware` from the proxy's `DllMain`, before the engine creates its first window. System
awareness is chosen deliberately: the engine sizes its window with `AdjustWindowRectEx`, which is only
correct for the system DPI.

`[Display] DisableWindowGhosting=1` calls `DisableProcessWindowsGhosting` so that a window whose thread is
busy loading is not replaced by a "Not responding" ghost. This is a precaution; ghosting during the game's
loading screens was not measured.

## 8. Audio

### What the game ships and how it plays it

The sound engine is Ubisoft's DARE, rendering through **XAudio2 2.7** (`XAudio2_7.dll`, DirectX June 2010
runtime), created with `CoCreateInstance`. A second XAudio2 instance (8 kHz mono) serves voice chat.

The engine side is not the bottleneck. The mastering voice and every submix voice are created with the
constant 48000 Hz (RVA 0x20BBED4) and six channels (5.1); source voices are created with a maximum pitch
ratio of 6. XAudio2 resamples each sound to 48 kHz and folds the 5.1 mix down to whatever the output
device has. The device's own channel count is never queried.

The sounds themselves are low-rate. A survey of the Steam release (sound objects parsed straight from the
archives; LZO-compressed packages were not unpacked, so counts are a large sample, not a census):

| Content | Container | Codec | Sample rates found |
|---|---|---|---|
| English dialogue (9,461 clips) | `fc3_main_english.dat`, `.spk` packages | Ogg Vorbis, mono, 40-48 kbit/s | 16 kHz: 76 %, 22.05 kHz: 24 %, a few at 11 kHz |
| Effects and ambience held in memory (935 headers parsed) | `fc3_main.dat`, `.spk` packages | Vorbis and a second, non-Vorbis format | 24 kHz: 42 %, 32 kHz: 51 %, 48 kHz: under 2 % |
| Streamed music and ambience (545 streams) | `sound.dat`, stream objects | Ogg Vorbis, 90-112 kbit/s stereo | 32 kHz: 66 %, 24 kHz: 31 %, 11 kHz: 3 % |

A 16 kHz recording contains nothing above 8 kHz and a 32 kHz one nothing above 16 kHz, so dialogue in
particular sounds band-limited however it is played back. No patch can restore what was never shipped;
nothing in this project alters the sound.

A trace of the running game on a stereo, 48 kHz output device confirms this: the mastering voice is
created with six channels, the final submix feeds it through an identity matrix, and about 30 six-channel
and mono submix voices sit in between, all at 48 kHz. DARE does not adapt to the stereo device; the fold-down
from 5.1 is left entirely to XAudio2. Source voices come from a small pool (83 were created in 33 minutes)
that is re-tuned per sound, so creation formats say little about what is played.

Not established: the interpolation quality of XAudio2 2.7's sample-rate converter and the 5.1 to stereo
coefficients it applies at the mastering voice. Microsoft's documentation describes the default mapping
only in prose (each channel goes to the nearest speakers) and gives no figures, in particular none for the
LFE channel.

`[Debug] TraceAudio=1` logs the device format, the mixer set-up, every sample rate a source voice is tuned
to, each distinct output matrix and, per source layout, the highest level ever sent to each of the six
output channels. It never logs the device name.

### Keeping XAudio2 loaded (`[Audio] KeepXAudio2Loaded`)

XAudio2 2.7 has a known defect, described in "Known Issues: XAudio 2.7" on the DirectX SDK blog: it relies
on COM reference counting for its lifetime, and in some situations, normally at exit, the DLL is unloaded
before the XAudio2 objects are completely destroyed, which ends in an access violation. The published
remedy is for the application to hold its own reference to the DLL for as long as it uses XAudio2.
Players report the game crashing 10 to 15 minutes into a session with the faulting module
`XAudio2_7.dll_unloaded`. The patch hooks the engine's `CoCreateInstance` import and pins the module that
implements the first XAudio2 object (found through the object's virtual table, so it also works with
Wine's implementation). Pinning has no effect other than keeping the DLL mapped until the process ends.

The crash was not reproduced in this analysis. The reports name the same unloaded DLL, but they describe
a crash in mid-session rather than at exit, so the link to the known defect is plausible, not proven.

## 9. Textures: top mip levels that do not fit (texture mods)

### What the engine does

Most textures are stored as two files: `name.xbt` holds every mip level except the largest, and
`name_mip0.xbt` holds only the largest level. The base file's header names its mip0 file. The base is
loaded with the scene; the mip0 file is streamed in when the texture gets close to the camera and dropped
again later. (`DisableMip0Loading="1"` in `GamerProfile.xml` switches that streaming off for all textures.)

In the DX11 renderer the texture class has two virtual methods for this, next to each other in its table.
Decompiled from `FC3_d3d11.dll` (Far Cry 3, RVA 0x14F3FA0 and 0x14F3DC0):

```
bool Texture::AddTopLevel(const void *dds, unsigned size, ...)
    desc = d3dTexture->GetDesc()
    new  = CreateTexture2D(2 * desc.Width, 2 * desc.Height, desc.MipLevels + 1, desc.Format)
    UpdateSubresource(new, level 0, pixels of the DDS image, row pitch of the DDS image)
    for n in 0 .. desc.MipLevels - 1: CopySubresourceRegion(new, n + 1, old, n)
    swap(new, old)

bool Texture::DropTopLevel()
    the reverse: a texture of half the size, every level copied down by one
```

`AddTopLevel` never compares the image it was given with the texture it is building. The size and format
of the new texture come from the texture that is already loaded; only the pixel pointer and the row pitch
come from the mip0 file. `UpdateSubresource` then copies as many rows and bytes as the *new texture* needs.

### Why texture mods crash

A crash with a large HD texture mod installed (and the game's default `DisableMip0Loading="0"`) was
analysed from its dump: access violation in the graphics driver, reading, called from
`FC3_d3d11.dll+0x14F42A9`, the instruction after the `UpdateSubresource` call above. The arguments on the
stack: the loaded texture was 2048x2048 BC1 with 11 levels (the mod's replacement), the new texture
4096x4096 with 12 levels, and the source a 1024x1024 DXT1 image of 0x80000 bytes with a row pitch of
0x800: the original game's mip0 file. The driver was asked to read 8 MB from a buffer of 0.5 MB and
faulted at the first unmapped page behind it.

A survey of the archives confirms the pattern. In the original game every one of the 2,259 base textures
that names a mip0 file is paired with one of exactly twice its width and height in the same format. In
that mod, 608 replaced base textures name a mip0 file, and 400 of the pairs break the rule:

| Pair | Count | Effect in the unpatched game |
|---|---|---|
| mip0 exactly twice the base, same format | 208 | works |
| mip0 smaller than twice the base | 294 | read past the end of the file data: crash |
| mip0 in a different block format (DXT1 against DXT5) | 80 | wrong picture, or overrun when the texture's blocks are larger |
| mip0 larger than twice the base | 26 | wrong picture |

That is why such mods instruct users to disable mip0 loading altogether. The cost of that workaround is
that the textures the mod did not touch lose their largest level too, and so do the 208 pairs the mod got
right.

### The texture guard (`[Textures] SkipMismatchedMip0`)

The guard wraps `AddTopLevel`, `DropTopLevel` and the class's destructor in the class's virtual table.
Before the engine's `AddTopLevel` runs, it reads the texture's description and the DDS header and applies
the rule the original data obeys: width and height exactly twice the texture's, same pixel format family.
An image that fits goes to the engine unchanged. One that does not is refused: the method returns false,
which is the engine's own failure path (the caller frees the file data either way), and the texture keeps
what it has, which is what the mod's own recommended settings would show.

Refusing is not enough on its own. The resource layer above the texture keeps its own count: after asking
for a level to be added it doubles its recorded size, and when the texture is no longer needed up close it
asks for a level to be dropped and halves it, whether or not the add succeeded. An object that never
received its level would then drop a level it really has, and lose another one on every load/unload
cycle. The guard therefore remembers each refused object and swallows the one `DropTopLevel` that belongs
to it. The same imbalance exists in the unmodified game when `AddTopLevel` fails for its own reasons
(`CreateTexture2D` failing when memory is short); the guard handles that case the same way.

Locating the class: both methods are found by signature (their opening instructions, which ask the
Direct3D texture at `[this+0x1C]` for its description), then the one data location where a pointer to
`AddTopLevel` is directly followed by a pointer to `DropTopLevel` is the class's table. The table must
begin, the expected number of slots earlier, with a function that looks like a compiler-generated deleting
destructor; otherwise nothing is touched. Far Cry 3 has the pair in slots 9 and 10, Blood Dragon (a
size-optimised build with one method less) in slots 8 and 9. Each signature matches exactly once in its
engine module and not at all in the other three.

Status: the cause is established from the crash dump and the decompiled code. The guard is tested on a
stand-in class whose methods begin with the real instruction sequences, and on the real `FC3_d3d11.dll`
of both games mapped into a test process: a 1024x1024 top level offered to a 2048x2048 texture through
the game's own table is refused before any game code runs, and the following drop is swallowed. In play
(Far Cry 3, DX11, the mod above, `DisableMip0Loading="0"`) the game loads and runs where it crashed
before, and the log shows levels of all three kinds being left out. Blood Dragon was not played with it.
The DX9 renderer has its own texture class, which was not analysed; the guard does nothing there. Texture
mods also raise memory use, which this guard does not address: the process has 4 GB of address space at
most.

### Original top levels of replaced textures (`[Textures] SkipReplacedTextureMip0`)

The size rule leaves one case open. Where a mod repaints a texture at its original size, or under a new
name, the game's original mip0 file still fits, so it is added, and within a few metres of the camera the
original picture replaces the mod's. On the ground this is plain to see: the texture changes a short
distance ahead as you walk. (With the mod's own `DisableMip0Loading="1"` it cannot happen, because no top
level is ever loaded.)

Neither the engine nor a hook on its texture class can tell such a file from a good one: both only see
pixels of the right size. The information is in the archives. A mod is installed as `patch.fat` /
`patch.dat`, which override every other archive, and the archives can be read without the game:

- The index (`.fat`) is a sorted list of 20-byte entries: name hash, content size and compression scheme,
  offset and stored size. Names are not stored; the hash is a CRC-64 (polynomial 0x1B, reflected) of the
  lower-case path. Entries are stored raw or LZO1X-compressed.
- A texture file (`.xbt`) starts with a small header that, in a base texture, names its mip0 file, and
  that carries a 12-byte source id. In every pair of the original game (2,259 in Far Cry 3, 1,440 in
  Blood Dragon) the base and its mip0 file have the same id.

At start-up a background thread reads the indexes (35 archives with 490,000 entries take about 30 ms here)
and looks at every base texture in `patch.fat` that names a mip0 file which only the game's own archives
provide and whose size fits. If the game has a file of the same name, the two bases are compared (size
and the first 16 KB of content); for a new name the source ids are compared. A base that differs was
made from another picture, so the mip0 file is recorded by an identity that can be recomputed from what
the engine later holds in memory: header fields including the source id, dimensions, format, size and
the first 256 bytes of pixel data. `AddTopLevel` refuses recorded files like unfitting ones.

In the surveyed mod, of 608 base textures that name a mip0 file, 91 bring their own, 382 would get an
original of the wrong size (the crash, handled by the size rule), 38 are unchanged copies of the game's
files, and 97 are repainted textures whose original mip0 file still fits. Those 97 include the jungle
and coast ground textures; 96 distinct files are left out.

Limits: only `patch.fat` is treated as a mod (that is where mods for this game are installed); a base is
considered unchanged if its size and first 16 KB match the game's; and a recorded file is refused for
every texture, so an untouched texture that shares the same mip0 file with a replaced one loses it too
(one such case in the surveyed mod). Status: the scan is tested on archives built in memory and by the
runtime tests on archives built on disk, and on the real installation it returns exactly the files a
separate reference implementation finds. **The effect on screen has not been checked yet.**

## 10. The in-game menu (`[Menu]`)

A settings menu that the patch draws itself, over the game's picture, with the DX11 renderer. The game's
own menus are built from files in a proprietary Flash-like format ("Fire", `.feu`) for which no editing
tools exist, so extending them was not an option.

- **Where it draws.** The swap chains the engine creates are already tracked for the fullscreen guard;
  their `Present` (slot 8) is wrapped the same way. Before the frame is shown, the menu is drawn onto the
  back buffer with a small renderer of its own (`src/overlay.c`): one dynamic vertex buffer of coloured,
  textured rectangles, two shaders compiled at run time with the D3D compiler DLL the game itself needs,
  and a one-channel font texture rendered once with GDI from the system's UI font. Everything it sets on
  the device context is saved first and restored afterwards, and it keeps no reference to the back buffer
  between frames, so the engine can resize or recreate its swap chain at any time.
- **What it is made of.** The entries, their behaviour (`src/menu_model.c`) and the layout
  (`src/menu_draw.c`) are plain C without any system calls and are unit-tested; the layout scales with
  the picture's height.
- **Input.** The keys are read with `GetAsyncKeyState`, the gamepad with the game's own `XInputGetState`.
  So that the game does not act on the same input, the engine's import of `XInputGetState` (by ordinal,
  from `xinput1_3.dll`) and the keyboard and mouse devices it creates through `DirectInput8Create` are
  wrapped: while the menu is open, and until the keys that closed it are up again, the engine is told
  that nothing is pressed. DirectInput game controllers are left alone.
- **Settings.** A change is written to the INI file with `WritePrivateProfileString`, which keeps the
  file's comments. Most of the current settings are read once at start-up, so the menu says when a change
  needs a restart.

Status: the model and layout have unit tests; a test program draws the menu on a real Direct3D 11 device
at 1280x720 and 3840x2160, checks the picture (panel, text, untouched remainder) and that render target,
blend, rasteriser, viewport and topology are as it left them; the runtime tests cover the `Present` hook
on a stand-in swap chain, including a menu that cannot draw. **It has not been used inside the game
yet**, and nothing is known about how it behaves under the Steam or Ubisoft Connect overlays, with DXVK,
or in exclusive fullscreen. It does not exist for the DX9 executables.

## 11. Control Flow Guard

None of the game binaries are built with CFG (`IMAGE_DLLCHARACTERISTICS_GUARD_CF` is clear in all of them).
Disabling CFG per executable in Windows Exploit Protection is a frequently repeated community fix for long
freezes in this and other games; the evidence is anecdotal and no mechanism was identified here. The
patch does not change it: it would lower a security mitigation for the named executable on the strength
of anecdote.

## 12. Tests and their limits

| Layer | Method |
|---|---|
| Logic | Unit tests for every pure module; the four loop variants are executed as machine code before and after patching |
| Real engine code | The game's own function, called in isolation from the user's DLLs (section 2); the texture guard installed on the game's real texture class table and called through it (section 9) |
| Process integration | A 32-bit host with stand-in modules named like the engine's, in 24 scenarios, natively and under Wine: forwarding, processor layer against the real machine topology, late and re-loaded modules, self-applied LAA, the fullscreen guard against a stand-in `dxgi.dll`, the texture guard against a stand-in texture class, the audio module against the real XAudio2 2.7 where it is installed, pass-through and disabled modes |
| In game | Both games started (DX11; Far Cry 3 also DX9) with the default configuration; logs and API trace inspected |
| Rendering | The in-game menu drawn on a real Direct3D 11 device, the picture and the restored device state checked |
| Other processors | The same test suite on an ARM64 runner, the 32-bit test programs translated by FEX inside ARM64 Wine |
| Privacy | Sources, git history and binaries are scanned for paths, account names, addresses and IDs |

Not covered: gameplay benchmarks, the game under
Proton or on a Steam Deck, AMD and Intel GPUs, a physical 32-thread Intel processor, and the Ubisoft
Connect or Epic builds of the game.
