#!/usr/bin/env python3
"""Runtime smoke tests for the version.dll proxy.

Loads the freshly built 32-bit proxy into a 32-bit test host (natively on
Windows, or through Wine/Proton with --emulator) in a number of scenarios and
checks behaviour, the log file, Large Address Aware self-patching and that the
log never leaks paths, user names or host names.

    python run_runtime.py --bin-dir <dir with harness + fixtures> --dll <version.dll>
                          [--work-dir <dir>] [--emulator wine] [--only <scenario>]

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import getpass
import glob
import os
import platform
import shutil
import struct
import subprocess
import sys

HARNESS = "fc3pp_harness.exe"
HARNESS_DX9 = "fc3pp_harness_dx9.exe"
PASSIVE = "fc3pp_passive.exe"
FIXTURES = ["FC3_d3d11.dll", "FC3.dll", "systemdetection.dll", "dxgi.dll"]
LOG = "FC3PerformancePatch.log"
INI = "FC3PerformancePatch.ini"


def ini(**kw):
    values = {
        "enabled": 1, "log": 3, "fix": 1, "mode": "engine", "max_cores": 3, "smt": 0,
        "timer": 1, "sleep_yield": 0, "laa": 0, "trace": 1, "simulate": "0",
        "restore_fs": 1, "dpi": 1, "ghosting": 1, "keep_xaudio": 1, "trace_audio": 0, "texguard": 1,
        "texscan": 1,
    }
    values.update(kw)
    return (
        "[General]\nEnabled={enabled}\nLogLevel={log}\n"
        "[CPU]\nFixCoreEnumeration={fix}\nMode={mode}\nMaxCores={max_cores}\nUseSMT={smt}\n"
        "[Timing]\nTimerResolutionMs={timer}\nSleepZeroYield={sleep_yield}\n"
        "[Memory]\nApplyLargeAddressAware={laa}\n"
        "[Display]\nRestoreFullscreenOnFocus={restore_fs}\nHighDpiAware={dpi}\nDisableWindowGhosting={ghosting}\n"
        "[Textures]\nSkipMismatchedMip0={texguard}\nSkipReplacedTextureMip0={texscan}\n"
        "[Audio]\nKeepXAudio2Loaded={keep_xaudio}\n"
        "[Debug]\nTraceCpuApi={trace}\nTraceAudio={trace_audio}\nSimulateAffinityMask={simulate}\n"
    ).format(**values)


# name: (executable, ini text or None, harness args, expect a log file,
#        strings the log must contain, strings it must not contain)
SCENARIOS = {
    "engine": (HARNESS, ini(), ["engine", "3"], True,
               ["initialised", "FC3_d3d11.dll: fixed the processor-enumeration loop", "'release' build variant",
                "systemdetection.dll: fixed the processor-enumeration loop", "'debug' build variant",
                "Mode=engine", "requested 1 ms system timer resolution", "import hook",
                "trace: SetProcessAffinityMask", "ignored",
                "cpu: the process was confined to other processors after start-up"],
               ["ERROR"]),
    "engine-all-cores": (HARNESS, ini(max_cores=0), ["engine", "0"], True, ["Mode=engine", "initialised"], ["ERROR"]),
    "process": (HARNESS, ini(mode="process"), ["process", "3"], True,
                ["Mode=process", "process affinity restricted to"], ["ERROR"]),
    "off": (HARNESS, ini(mode="off"), ["off"], True,
            ["Mode=off", "fixed the processor-enumeration loop"], ["ERROR", "engine sees"]),
    "off-nofix": (HARNESS, ini(mode="off", fix=0), ["off-nofix"], True,
                  ["Mode=off"], ["fixed the processor-enumeration loop", "ERROR"]),
    "simulate": (HARNESS, ini(mode="off", simulate="0xFFFFFFFF"), ["simulate"], True,
                 ["DEBUG simulation active", "fixed the processor-enumeration loop"], ["ERROR"]),
    "simulate-nofix": (HARNESS, ini(mode="off", fix=0, simulate="0xFFFFFFFF"), ["simulate-nofix"], True,
                       ["DEBUG simulation active"], ["fixed the processor-enumeration loop"]),
    "clamp": (HARNESS_DX9, ini(mode="off"), ["clamp"], True,
              ["FC3.dll: processor-enumeration loop signature not found", "relying on the affinity-mask hook"],
              ["ERROR"]),
    "dx9-engine": (HARNESS_DX9, ini(), ["forward-only"], True,
                   ["FC3.dll:", "Mode=engine", "initialised", "FC3.dll: texture guard not available"], ["ERROR"]),
    "textures": (HARNESS, ini(), ["textures"], True,
                 ["FC3_d3d11.dll: texture guard active ('release' build variant)",
                  "textures: left out a top mip level that does not fit its texture: the texture is 2048x2048 BC1, "
                  "so the level must be 4096x4096, but the file holds 1024x1024 BC1 (too small)",
                  "the texture is 512x512 BC3, so the level must be 1024x1024, but the file holds 1024x1024 BC1 "
                  "(different pixel format)",
                  "the texture is 256x256 BC1, so the level must be 512x512, but the file holds 1024x1024 BC1 "
                  "(too large)",
                  "textures: the engine could not add a top mip level to a texture",
                  "textures: the patch archive holds 2 textures with a separate top mip level; 0 bring their own, "
                  "0 would get an original one of the wrong size, 1 an original one made for the texture they "
                  "replace (those 1 files will be left out, 1 are intact) - 2 archives read",
                  "textures: left out a top mip level (128x128 BC1) that was made for the original of a texture "
                  "a mod has replaced",
                  "textures: 4 top mip levels added, 6 left out because they did not fit (4 too small, 1 too large, "
                  "1 in a different pixel format), 2 left out because a mod replaced their texture, "
                  "1 not added by the engine itself; 7 matching unload requests ignored"],
                 ["ERROR"]),
    "textures-off": (HARNESS, ini(texguard=0), ["textures-off"], True,
                     ["initialised"], ["texture guard", "textures:", "ERROR"]),
    "textures-noscan": (HARNESS, ini(texscan=0), ["textures-noscan"], True,
                        ["texture guard active"], ["patch archive", "a mod has replaced", "ERROR"]),
    "fullscreen": (HARNESS, ini(), ["fullscreen"], True,
                   ["display: fullscreen guard attached to a swap chain of the game",
                    "display: Windows took the game out of fullscreen on focus loss",
                    "display: fullscreen restored", "DPI aware", "window ghosting disabled"],
                   ["ERROR"]),
    "fullscreen-off": (HARNESS, ini(restore_fs=0, dpi=0, ghosting=0), ["fullscreen-off"], True,
                       ["initialised"], ["display:", "ERROR"]),
    "disabled": (HARNESS, ini(enabled=0), ["disabled"], False, [], []),
    "noini": (HARNESS, None, ["noini", "6"], True,
              ["using built-in defaults", "Mode=engine", "initialised"], ["ERROR"]),
    "quiet": (HARNESS, ini(log=0), ["engine", "3"], False, [], []),
    "passive": (PASSIVE, ini(), [], False, [], []),
}


class Failure(Exception):
    pass


def pe_is_laa(path):
    with open(path, "rb") as f:
        data = f.read(0x400)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    return bool(struct.unpack_from("<H", data, pe + 22)[0] & 0x20)


def privacy_needles(work_dir):
    needles = set()
    for name in (getpass.getuser(), os.environ.get("USERNAME", ""), os.environ.get("USER", "")):
        if len(name) >= 3:
            needles.add(name.lower())
    host = platform.node().split(".")[0]
    if len(host) >= 3:
        needles.add(host.lower())
    needles.add(os.path.abspath(work_dir).lower())
    needles.add(os.path.abspath(work_dir).replace("\\", "/").lower())
    for fixed in (":\\", "/home/", "\\users\\", "/users/", "drive_c", "steamuser"):
        needles.add(fixed)
    return needles


def check_log_privacy(text, work_dir):
    low = text.lower()
    for needle in privacy_needles(work_dir):
        if needle in low:
            raise Failure("the log leaks private information (matched %r)" % needle)


def run(cmd, cwd, env, timeout=180):
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise Failure("timed out: the host process hung")
    return p.returncode, p.stdout.decode("utf-8", "replace")


def prepare(args, name, exe, ini_text):
    d = os.path.join(args.work_dir, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    shutil.copy2(os.path.join(args.bin_dir, exe), d)
    if exe != PASSIVE:
        for f in FIXTURES:
            shutil.copy2(os.path.join(args.bin_dir, f), d)
    shutil.copy2(args.dll, os.path.join(d, "version.dll"))
    if ini_text is not None:
        with open(os.path.join(d, INI), "w", newline="\r\n") as f:
            f.write(ini_text)
    return d


def launch(args, d, exe, exe_args, env):
    return run(args.emulator + [os.path.join(d, exe)] + exe_args, d, env)


def read_log(d):
    path = os.path.join(d, LOG)
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def name_hash(name):
    """The archives' file-name hash: CRC-64, polynomial 1Bh, reflected, of the lower-case path."""
    crc = 0
    for b in name.lower().encode("ascii"):
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xD800000000000000 if crc & 1 else crc >> 1
    return crc


def make_xbt(top_path, source_id, width, height, fourcc, pixels, seed):
    """A texture file: header, optional path of its top mip level file, DDS image."""
    if top_path is None:
        path = bytes(4)
    else:
        path = top_path.encode("ascii") + bytes(1)
        path += bytes(-len(path) % 4)
    header = struct.pack("<4sIIII", b"TBX", 0x68, 0x20 + len(path), 8 if top_path else 2, 0xFFFFFF00)
    header += bytes((source_id * 16 + i) & 0xFF for i in range(12)) + path
    dds = bytearray(128)
    dds[0:4] = b"DDS "
    struct.pack_into("<I", dds, 4, 124)
    struct.pack_into("<II", dds, 12, height, width)
    struct.pack_into("<I", dds, 28, 1)
    struct.pack_into("<II", dds, 76, 32, 4)
    dds[84:88] = fourcc
    return header + bytes(dds) + bytes((i * 13 + seed) & 0xFF for i in range(pixels))


def write_archive(folder, name, files):
    """Writes <name>.fat / <name>.dat holding {path: content}, everything stored."""
    data = bytearray(1)
    entries = []
    for path, content in files.items():
        entries.append((name_hash(path), len(data), len(content)))
        data += content + bytes(3)
    fat = struct.pack("<4sIIIII", b"2TAF", 9, 0x301, 0, 0, len(entries))
    for h, offset, size in sorted(entries):
        fat += struct.pack("<IIIII", h >> 32, h & 0xFFFFFFFF, 0, offset >> 2, ((offset & 3) << 30) | size)
    with open(os.path.join(folder, name + ".fat"), "wb") as f:
        f.write(fat)
    with open(os.path.join(folder, name + ".dat"), "wb") as f:
        f.write(data)


def setup_texture_archives(d):
    """Builds <d>/../data_win32 the way a game with a texture mod looks, and
    leaves two top mip level files of the "game" next to the host."""
    sep = chr(92)
    data = os.path.join(os.path.dirname(d), "data_win32")
    shutil.rmtree(data, ignore_errors=True)
    os.makedirs(os.path.join(data, "worlds", "main"))

    ground, ground_top_name = "tex" + sep + "ground_d.xbt", "tex" + sep + "ground_d_mip0.xbt"
    rock, rock_top_name = "tex" + sep + "rock_d.xbt", "tex" + sep + "rock_d_mip0.xbt"
    ground_top = make_xbt(None, 1, 128, 128, b"DXT1", 1500, 11)
    rock_base = make_xbt(rock_top_name, 2, 64, 64, b"DXT5", 700, 20)
    rock_top = make_xbt(None, 2, 128, 128, b"DXT5", 1500, 21)
    # The "mod": repaints the ground (same size, header copied from the
    # original) and carries the rock texture unchanged.
    write_archive(data, "patch", {
        ground: make_xbt(ground_top_name, 1, 64, 64, b"DXT1", 700, 99),
        rock: rock_base,
        "scripts" + sep + "note.lua": b"-- not a texture",
    })
    write_archive(os.path.join(data, "worlds", "main"), "main", {
        ground: make_xbt(ground_top_name, 1, 64, 64, b"DXT1", 700, 10),
        ground_top_name: ground_top,
        rock: rock_base,
        rock_top_name: rock_top,
    })
    # An archive without data, like the empty language archives the game ships.
    with open(os.path.join(data, "empty.fat"), "wb") as f:
        f.write(struct.pack("<4sIIIII", b"2TAF", 9, 0x301, 0, 0, 0))
    open(os.path.join(data, "empty.dat"), "wb").close()
    # What the engine has in its hands when it streams these two in.
    with open(os.path.join(d, "stale_top.xbt"), "wb") as f:
        f.write(ground_top)
    with open(os.path.join(d, "fresh_top.xbt"), "wb") as f:
        f.write(rock_top)


# Scenarios whose host sits in <work>/<name>/bin, next to a data folder, as
# the game sits in <game>/bin next to <game>/data_win32.
WITH_DATA = {"textures": setup_texture_archives, "textures-noscan": setup_texture_archives}


def run_scenario(args, name, env):
    exe, ini_text, exe_args, expect_log, must, must_not = SCENARIOS[name]
    if name in WITH_DATA:
        d = prepare(args, os.path.join(name, "bin"), exe, ini_text)
        WITH_DATA[name](d)
    else:
        d = prepare(args, name, exe, ini_text)
    code, out = launch(args, d, exe, exe_args, env)
    if code != 0:
        raise Failure("host exited with code %d\n%s" % (code, out.strip()))
    log = read_log(d)
    if expect_log and log is None:
        raise Failure("no log file was written\n%s" % out.strip())
    if not expect_log and log is not None:
        raise Failure("a log file was written although none is expected:\n%s" % log)
    if log is not None:
        for s in must:
            if s not in log:
                raise Failure("log does not contain %r\n--- log ---\n%s" % (s, log))
        for s in must_not:
            if s in log:
                raise Failure("log unexpectedly contains %r\n--- log ---\n%s" % (s, log))
        check_log_privacy(log, args.work_dir)
    leftovers = [f for f in os.listdir(d) if f.endswith(".bak") or f.endswith(".fc3pp-new")]
    if leftovers and name != "noini":
        raise Failure("unexpected files created: %s" % leftovers)
    return out


def run_laa(args, env):
    """Large Address Aware self-patching of the running executable."""
    d = prepare(args, "laa", HARNESS, ini(laa=1))
    exe = os.path.join(d, HARNESS)
    with open(exe, "rb") as f:
        original = f.read()
    if pe_is_laa(exe):
        raise Failure("the test host is already large-address-aware; cannot test the patch")

    code, out = launch(args, d, HARNESS, ["laa", "3"], env)
    if code != 0:
        raise Failure("first run failed (%d)\n%s" % (code, out))
    log = read_log(d) or ""
    if "enabled the Large Address Aware flag on fc3pp_harness.exe" not in log:
        raise Failure("first run did not report the LAA patch\n%s" % log)
    if not pe_is_laa(exe):
        raise Failure("executable on disk is not large-address-aware after the first run")
    backups = glob.glob(exe + ".fc3pp-*.bak")
    if len(backups) != 1:
        raise Failure("expected exactly one backup, found %d" % len(backups))
    with open(backups[0], "rb") as f:
        if f.read() != original:
            raise Failure("the backup is not identical to the original executable")
    with open(exe, "rb") as f:
        patched = f.read()
    diff = sum(1 for a, b in zip(original, patched) if a != b)
    if len(patched) != len(original) or not 1 <= diff <= 5:
        raise Failure("the patch changed %d bytes (expected the flag and the checksum only)" % diff)

    code, out = launch(args, d, HARNESS, ["laa", "3"], env)
    if code != 0:
        raise Failure("second run failed (%d)\n%s" % (code, out))
    log = read_log(d) or ""
    if "process is large-address-aware" not in log:
        raise Failure("second run does not run large-address-aware\n%s" % log)
    if len(glob.glob(exe + ".fc3pp-*.bak")) != 1:
        raise Failure("the second run created another backup")
    # The log of the first run survives the second start.
    previous = os.path.join(d, LOG.replace(".log", ".previous.log"))
    if not os.path.exists(previous):
        raise Failure("the log of the previous start was not kept")
    with open(previous, "rb") as f:
        if b"enabled the Large Address Aware flag" not in f.read():
            raise Failure("the kept log is not the one of the previous start")
    with open(exe, "rb") as f:
        if f.read() != patched:
            raise Failure("the second run modified the executable again")
    check_log_privacy(log, args.work_dir)
    return out


def run_audio(args, env):
    """XAudio2 keep-loaded guard and trace, as far as this machine has XAudio2 2.7."""
    d = prepare(args, "audio", HARNESS, ini(trace_audio=1))
    code, out = launch(args, d, HARNESS, ["audio"], env)
    if code != 0:
        raise Failure("host exited with code %d\n%s" % (code, out.strip()))
    log = read_log(d) or ""
    stage = -1
    for line in out.splitlines():
        if line.startswith("audio stage:"):
            stage = int(line.split(":")[1])
    if stage < 0:
        raise Failure("the harness did not report an audio stage\n%s" % out)
    expect = ["audio: trace enabled"]
    if stage & 1:
        expect += ["will stay loaded until the game exits", "audio: trace attached to XAudio2 engine #1"]
    else:
        expect += ["the game could not create XAudio2 2.7"]
    if stage & 4:
        expect += ["audio: output device 0:", "audio: mastering voice requested with 0 channels, 0 Hz",
                   "audio: mastering voice input is"]
    if stage & 8:
        expect += ["audio: first source voice with PCM, 1 channel, 22050 Hz, 16 bit",
                   "audio: 1 source voices created in 1 format"]
    if stage & 16:
        expect += ["audio: new output matrix, source voice, 1 -> ", "mono sources, highest level sent"]
    if stage & 32:
        expect += ["audio: a source voice was re-tuned to 16000 Hz for the first time",
                   "re-tuned to  16000 Hz       1 times"]
    if stage & 8 and not stage & 32:
        raise Failure("stage %d: SetSourceSampleRate failed on a fresh source voice (wrong slot?)" % stage)
    for s in expect:
        if s not in log:
            raise Failure("stage %d: log does not contain %r\n--- log ---\n%s" % (stage, s, log))
    check_log_privacy(log, args.work_dir)

    # Both features off: the import is left alone and nothing is said about audio.
    d = prepare(args, "audio-off", HARNESS, ini(keep_xaudio=0))
    code, out2 = launch(args, d, HARNESS, ["audio-off"], env)
    if code != 0:
        raise Failure("audio-off: host exited with code %d\n%s" % (code, out2.strip()))
    if "audio:" in (read_log(d) or ""):
        raise Failure("audio-off: the log mentions audio although both features are off")
    return out + "(XAudio2 stage %d) 0 failures" % stage


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin-dir", required=True)
    ap.add_argument("--dll", required=True)
    ap.add_argument("--work-dir")
    ap.add_argument("--emulator", default="", help="e.g. 'wine' (may include arguments)")
    ap.add_argument("--only", action="append")
    args = ap.parse_args()
    args.bin_dir = os.path.abspath(args.bin_dir)
    args.dll = os.path.abspath(args.dll)
    args.work_dir = os.path.abspath(args.work_dir or os.path.join(args.bin_dir, "..", "runtime-work"))
    args.emulator = args.emulator.split() if args.emulator.strip() else []

    env = dict(os.environ)
    if args.emulator:
        # Wine prefers its built-in version.dll unless told otherwise - exactly
        # the launch option the README tells Proton users to set. The dxgi
        # entry is for the test's fake dxgi.dll only.
        env["WINEDLLOVERRIDES"] = "version=n,b;dxgi=n,b"
        env.setdefault("WINEDEBUG", "-all")

    names = args.only or (list(SCENARIOS) + ["laa", "audio"])
    failed = 0
    for name in names:
        try:
            if name == "laa":
                out = run_laa(args, env)
            elif name == "audio":
                out = run_audio(args, env)
            else:
                out = run_scenario(args, name, env)
            summary = [line for line in out.splitlines() if "checks" in line or "failures" in line]
            print("[ OK ] %-18s %s" % (name, summary[-1].strip() if summary else ""))
        except Failure as e:
            failed += 1
            print("[FAIL] %-18s %s" % (name, e))
    print("%d scenario(s), %d failed" % (len(names), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
