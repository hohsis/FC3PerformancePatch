#!/usr/bin/env python3
"""Static checks on the built proxy DLL.

    python check_binary.py <version.dll> [--system-dll <path to the OS version.dll>]

* 32-bit x86 DLL with ASLR and DEP enabled
* export table: exactly the exports of the Windows version.dll, undecorated,
  with the same ordinals, none of them forwarded back to "version"
  (optionally compared against a real system version.dll)
* imports: only libraries that exist on every Windows and Wine installation
  (no Visual C++ redistributable, no version.dll)
* no local build path or user name leaked into the debug directory

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import struct
import sys

# The export table of version.dll on Windows 10/11 (name -> ordinal).
EXPECTED_EXPORTS = {
    "GetFileVersionInfoA": 1,
    "GetFileVersionInfoByHandle": 2,
    "GetFileVersionInfoExA": 3,
    "GetFileVersionInfoExW": 4,
    "GetFileVersionInfoSizeA": 5,
    "GetFileVersionInfoSizeExA": 6,
    "GetFileVersionInfoSizeExW": 7,
    "GetFileVersionInfoSizeW": 8,
    "GetFileVersionInfoW": 9,
    "VerFindFileA": 10,
    "VerFindFileW": 11,
    "VerInstallFileA": 12,
    "VerInstallFileW": 13,
    "VerLanguageNameA": 14,
    "VerLanguageNameW": 15,
    "VerQueryValueA": 16,
    "VerQueryValueW": 17,
}

# The three functions the game's engine modules import from VERSION.dll.
USED_BY_THE_GAME = ("GetFileVersionInfoA", "GetFileVersionInfoSizeA", "VerQueryValueA")

ALLOWED_IMPORTS = ("kernel32.dll", "winmm.dll", "user32.dll", "msvcrt.dll", "ucrtbase.dll")
ALLOWED_IMPORT_PREFIXES = ("api-ms-win-crt-", "api-ms-win-core-")


class PE:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        if d[:2] != b"MZ":
            raise ValueError("not a PE file")
        self.pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[self.pe:self.pe + 4] != b"PE\0\0":
            raise ValueError("not a PE file")
        self.machine, nsec = struct.unpack_from("<HH", d, self.pe + 4)
        optsz, self.characteristics = struct.unpack_from("<HH", d, self.pe + 20)
        opt = self.pe + 24
        self.magic = struct.unpack_from("<H", d, opt)[0]
        self.dll_characteristics = struct.unpack_from("<H", d, opt + 70)[0]
        self.dirs = [struct.unpack_from("<II", d, opt + 96 + 8 * i) for i in range(16)]
        self.sections = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name, vsize, va, rsize, raw = struct.unpack_from("<8sIIII", d, o)
            self.sections.append((name.rstrip(b"\0").decode("latin1"), va, max(vsize, rsize), raw))

    def off(self, rva):
        for _name, va, size, raw in self.sections:
            if va <= rva < va + size:
                return rva - va + raw
        raise ValueError("RVA 0x%x is outside every section" % rva)

    def cstr(self, rva):
        o = self.off(rva)
        return self.d[o:self.d.index(b"\0", o)].decode("latin1")

    def exports(self):
        """name -> (ordinal, forwarder or None)"""
        rva, size = self.dirs[0]
        if not rva:
            return {}
        o = self.off(rva)
        base, nfuncs, nnames, funcs_rva, names_rva, ords_rva = struct.unpack_from("<IIIIII", self.d, o + 16)
        out = {}
        for i in range(nnames):
            name = self.cstr(struct.unpack_from("<I", self.d, self.off(names_rva) + 4 * i)[0])
            index = struct.unpack_from("<H", self.d, self.off(ords_rva) + 2 * i)[0]
            func = struct.unpack_from("<I", self.d, self.off(funcs_rva) + 4 * index)[0]
            forwarder = self.cstr(func) if rva <= func < rva + size else None
            out[name] = (base + index, forwarder)
        self.export_count = nfuncs
        return out

    def imports(self):
        rva = self.dirs[1][0]
        out = []
        if not rva:
            return out
        o = self.off(rva)
        while True:
            _oft, _ts, _fc, name, first = struct.unpack_from("<IIIII", self.d, o)
            if not name and not first:
                break
            out.append(self.cstr(name))
            o += 20
        return out

    def debug_paths(self):
        """PDB paths recorded in CodeView debug entries."""
        rva, size = self.dirs[6]
        out = []
        if not rva:
            return out
        o = self.off(rva)
        for i in range(size // 28):
            dtype, dsize, _addr, ptr = struct.unpack_from("<IIII", self.d, o + 28 * i + 12)
            if dtype == 2 and dsize > 24 and self.d[ptr:ptr + 4] == b"RSDS":
                raw = self.d[ptr + 24:ptr + dsize]
                out.append(raw.split(b"\0")[0].decode("utf-8", "replace"))
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dll")
    ap.add_argument("--system-dll", help="a real version.dll to compare the export table against")
    args = ap.parse_args()

    failures = []
    checks = 0

    def check(cond, message):
        nonlocal checks
        checks += 1
        if not cond:
            failures.append(message)

    pe = PE(args.dll)
    check(pe.machine == 0x014C, "machine is 0x%04x, expected 0x014c (i386)" % pe.machine)
    check(pe.magic == 0x010B, "not a PE32 image")
    check(pe.characteristics & 0x2000, "not marked as a DLL")
    check(pe.dll_characteristics & 0x0040, "ASLR (DYNAMICBASE) is not enabled")
    check(pe.dll_characteristics & 0x0100, "DEP (NXCOMPAT) is not enabled")

    exports = pe.exports()
    for name, ordinal in EXPECTED_EXPORTS.items():
        check(name in exports, "export %s is missing" % name)
        if name in exports:
            check(exports[name][0] == ordinal, "export %s has ordinal %d, expected %d" % (name, exports[name][0], ordinal))
            check(exports[name][1] is None, "export %s is a forwarder string (%s); it must be real code" % (name, exports[name][1]))
    for name in exports:
        check(name in EXPECTED_EXPORTS, "unexpected export %s" % name)
        check("@" not in name and not name.startswith("_"), "export %s is decorated" % name)
    check(len(exports) == len(EXPECTED_EXPORTS), "%d named exports, expected %d" % (len(exports), len(EXPECTED_EXPORTS)))
    for name in USED_BY_THE_GAME:
        check(name in exports, "the game imports %s, which is not exported" % name)

    if args.system_dll:
        system = PE(args.system_dll).exports()
        for name, (ordinal, _fwd) in system.items():
            check(name in exports, "the system version.dll exports %s, the proxy does not" % name)
            if name in exports:
                check(exports[name][0] == ordinal,
                      "ordinal of %s differs from the system DLL (%d vs %d)" % (name, exports[name][0], ordinal))
        for name in exports:
            check(name in system, "the proxy exports %s, the system version.dll does not" % name)

    imports = [i.lower() for i in pe.imports()]
    for name in imports:
        check(name in ALLOWED_IMPORTS or name.startswith(ALLOWED_IMPORT_PREFIXES),
              "imports %s, which is not present on every system" % name)
    check("version.dll" not in imports, "imports version.dll (it would load itself)")
    check("kernel32.dll" in imports, "does not import kernel32.dll?")

    for path in pe.debug_paths():
        check("\\" not in path and "/" not in path and ":" not in path,
              "the debug directory contains a path: %r (only a bare file name is acceptable)" % path)

    if failures:
        for f in failures:
            print("  FAIL: " + f)
    print("%d checks, %d failures (%d exports, imports: %s)" % (checks, len(failures), len(exports), ", ".join(imports)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
