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

The DLL (`src/laa.c`) and the installers set the flag on disk and recompute the PE checksum. Because a
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
- `D3D11MultithreadedRendering` is left at the user's value. The Windows installer sets it to `1` only
  together with DXVK; the Linux installer only on request.

## 7. Control Flow Guard

None of the game binaries are built with CFG (`IMAGE_DLLCHARACTERISTICS_GUARD_CF` is clear in all of them).
Disabling CFG per executable in Windows Exploit Protection is a frequently repeated community fix for long
freezes in this and other games; the evidence is anecdotal and no mechanism was identified here. The
installer offers it as an explicit option (`-DisableCFG`), records the previous state and restores it on
uninstall. It is never applied automatically, because it lowers a security mitigation system-wide for the
named executable.

## 8. Tests and their limits

| Layer | Method |
|---|---|
| Logic | Unit tests for every pure module; the four loop variants are executed as machine code before and after patching |
| Real engine code | The game's own function, called in isolation from the user's DLLs (section 2) |
| Process integration | A 32-bit host with stand-in modules named like the engine's, in 14 scenarios, natively and under Wine: forwarding, processor layer against the real machine topology, late and re-loaded modules, self-applied LAA, pass-through and disabled modes |
| Installers | Round trips in a fake Steam library on Windows and Linux; restore is compared byte for byte |
| In game | Both games started (DX11; Far Cry 3 also DX9) with the default configuration; logs and API trace inspected |
| Privacy | Sources, git history and binaries are scanned for paths, account names, addresses and IDs |

Not covered: gameplay benchmarks, the game under Proton or on a Steam Deck, AMD and Intel GPUs, a physical
32-thread Intel processor, and the Ubisoft Connect or Epic builds of the game.
