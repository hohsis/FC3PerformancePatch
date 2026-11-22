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
        "restore_fs": 1, "dpi": 1, "ghosting": 1, "keep_xaudio": 1, "trace_audio": 0,
    }
    values.update(kw)
    return (
        "[General]\nEnabled={enabled}\nLogLevel={log}\n"
        "[CPU]\nFixCoreEnumeration={fix}\nMode={mode}\nMaxCores={max_cores}\nUseSMT={smt}\n"
        "[Timing]\nTimerResolutionMs={timer}\nSleepZeroYield={sleep_yield}\n"
        "[Memory]\nApplyLargeAddressAware={laa}\n"
        "[Display]\nRestoreFullscreenOnFocus={restore_fs}\nHighDpiAware={dpi}\nDisableWindowGhosting={ghosting}\n"
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
                "trace: SetProcessAffinityMask", "ignored"],
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
                   ["FC3.dll:", "Mode=engine", "initialised"], ["ERROR"]),
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


def run_scenario(args, name, env):
    exe, ini_text, exe_args, expect_log, must, must_not = SCENARIOS[name]
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
        expect += ["audio: output matrix 1 -> "]
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
