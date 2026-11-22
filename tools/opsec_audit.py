#!/usr/bin/env python3
"""OPSEC / privacy audit for this repository.

Looks for personal or machine-specific information that must never reach a
public repository:

  * local paths that reveal a user name (Windows profile folders, Unix home
    folders) and other absolute drive paths
  * e-mail addresses (except GitHub no-reply addresses and reserved example
    domains)
  * Steam account identifiers
  * private network addresses and user@host targets
  * access tokens and private keys
  * the current user name, host name and e-mail addresses of this machine
    (read from the environment at run time, never stored in the repository)
  * extra terms listed one per line in a local, git-ignored ".opsec-denylist"

What is scanned:

  --tree            every file git would add (tracked + untracked, not ignored)
  --staged          the staged version of every staged file      (pre-commit)
  --history         every blob of every commit, plus commit metadata (pre-push)
  --binaries DIR    printable strings (ASCII and UTF-16) of the binaries in DIR
  --paths P [P..]   the given files or folders, whatever git thinks of them
  --check-identity  the git identity that would be used for a commit
  --self-test       verify that the detectors still detect

With no options it runs --tree --check-identity. Exit status 1 = findings.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import argparse
import getpass
import os
import platform
import re
import subprocess
import sys

MAX_FILE_SIZE = 8 * 1024 * 1024
BINARY_EXTENSIONS = {".dll", ".exe", ".pdb", ".lib", ".obj", ".o", ".a", ".exp", ".zip", ".7z", ".gz", ".xz"}

# Addresses that are fine to publish.
ALLOWED_EMAIL = re.compile(
    r"(@users\.noreply\.github\.com|noreply@anthropic\.com|noreply@github\.com|"
    r"@example\.(com|org|net)|@localhost)$",
    re.I,
)
# Drive paths that say nothing about the person who wrote them.
ALLOWED_DRIVE_PATH = re.compile(
    r"^[A-Za-z]:[\\/]+(Program Files( \(x86\))?|Windows|Games|SteamLibrary|Path[\\/]+To|\.\.\.|<)",
    re.I,
)
ALLOWED_HOME = {"deck", "user", "you", "username", "steamuser", "runner", "name"}
ALLOWED_IP = re.compile(r"^(127\.|0\.0\.0\.0$|255\.|10\.0\.0\.0$|192\.168\.0\.0$)")


def rule(name, pattern, flags=0, accept=None):
    return name, re.compile(pattern, flags), accept


def _home_ok(m):
    return m.group(1).lower() in ALLOWED_HOME or m.group(1).startswith(("$", "<", "{", "%"))


RULES = [
    rule("windows-profile-path", r"[A-Za-z]:[\\/]+Users[\\/]+([^\\/\s\"'<>|*?:]+)", re.I,
         lambda m: m.group(1).lower() in ALLOWED_HOME or m.group(1).startswith(("<", "%", "$", "{"))),
    # The allow-list is matched against the rest of the line, because allowed
    # folders such as "Program Files (x86)" contain spaces.
    rule("drive-path", r"(?<![A-Za-z0-9_])[A-Za-z]:\\(?!\\)[^\s\"'<>|*?]{2,}", 0,
         lambda m: bool(ALLOWED_DRIVE_PATH.match(m.string[m.start():]))),
    rule("unix-home-path", r"/home/([A-Za-z0-9_.$<{%-]+)", 0, _home_ok),
    rule("macos-home-path", r"/Users/([A-Za-z0-9_.$<{%-]+)", 0, _home_ok),
    rule("email-address", r"[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,}", 0,
         lambda m: bool(ALLOWED_EMAIL.search(m.group(0)))),
    rule("steam-id64", r"(?<!\d)7656119\d{10}(?!\d)"),
    rule("steam-id-legacy", r"STEAM_[0-5]:[01]:\d{3,}"),
    rule("steam-id3", r"\[U:1:\d{3,}\]"),
    rule("steam-userdata-path", r"userdata[\\/]+\d{4,}"),
    rule("private-ip-address",
         r"(?<![\d.])(?:10\.\d{1,3}\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3}|"
         r"172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3})(?![\d.])", 0,
         lambda m: bool(ALLOWED_IP.match(m.group(0)))),
    rule("user-at-host", r"[A-Za-z0-9_-]+@\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}"),
    rule("github-token", r"gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}"),
    rule("private-key", r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP )?PRIVATE KEY"),
    rule("aws-access-key", r"(?<![A-Z0-9])AKIA[0-9A-Z]{16}(?![A-Z0-9])"),
]


BINARY_RULES = [
    ("windows-profile-path", re.compile(r"[A-Za-z]:[\\/]+Users[\\/]+[A-Za-z0-9_.-]{2,}", re.I)),
    ("drive-path", re.compile(r"(?<![A-Za-z0-9_])[A-Za-z]:\\[A-Za-z0-9_ .()-]{3,}\\[A-Za-z0-9_ .()-]{2,}")),
    ("unix-home-path", re.compile(r"/home/[a-z_][a-z0-9_.-]{1,}/")),
    ("macos-home-path", re.compile(r"/Users/[A-Za-z0-9_.-]{2,}/")),
    ("private-key", re.compile(r"-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP )?PRIVATE KEY")),
]


def git(*args, binary=False, check=True):
    p = subprocess.run(["git"] + list(args), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if check and p.returncode != 0:
        raise RuntimeError("git %s failed: %s" % (" ".join(args), p.stderr.decode("utf-8", "replace").strip()))
    return p.stdout if binary else p.stdout.decode("utf-8", "replace")


def repo_root():
    try:
        return git("rev-parse", "--show-toplevel").strip()
    except Exception:
        return os.getcwd()


def local_terms(root):
    """Personal terms of this machine. Read at run time; never written anywhere."""
    terms = {}

    def add(value, why, min_len=3):
        value = (value or "").strip()
        if len(value) >= min_len:
            terms.setdefault(value.lower(), why)

    try:
        add(getpass.getuser(), "local user name")
    except Exception:
        pass
    for var in ("USERNAME", "USER", "LOGNAME"):
        add(os.environ.get(var), "local user name")
    add(platform.node().split(".")[0], "host name")
    add(os.environ.get("COMPUTERNAME"), "host name")
    add(os.environ.get("USERDOMAIN"), "host/domain name")
    for scope in ("--global", "--system"):
        try:
            email = git("config", scope, "user.email", check=False).strip()
        except Exception:
            email = ""
        if email and not ALLOWED_EMAIL.search(email):
            add(email, "private e-mail address", 5)
    deny = os.path.join(root, ".opsec-denylist")
    if os.path.exists(deny):
        with open(deny, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#"):
                    add(line, "listed in .opsec-denylist", 2)
    # Generic account names that carry no information.
    for generic in ("user", "admin", "administrator", "root", "runner", "runneradmin", "deck", "build",
                    "github", "localhost", "workgroup", "desktop", "ubuntu", "vsts", "docker"):
        terms.pop(generic, None)
    return terms


def term_regex(terms):
    if not terms:
        return None
    parts = []
    for t in sorted(terms, key=len, reverse=True):
        esc = re.escape(t)
        # Short alphanumeric terms only count as whole words, so a three-letter
        # user name does not match inside unrelated identifiers.
        parts.append(r"(?<![A-Za-z0-9])%s(?![A-Za-z0-9])" % esc if re.fullmatch(r"[A-Za-z0-9_-]+", t) else esc)
    return re.compile("|".join(parts), re.I)


class Auditor:
    def __init__(self, root, quiet=False):
        self.root = root
        self.terms = local_terms(root)
        self.term_re = term_regex(self.terms)
        self.findings = []
        self.scanned = 0
        self.quiet = quiet

    def report(self, where, line_no, name, excerpt):
        self.findings.append((where, line_no, name))
        if not self.quiet:
            excerpt = excerpt.strip()
            if len(excerpt) > 120:
                excerpt = excerpt[:117] + "..."
            loc = "%s:%d" % (where, line_no) if line_no else where
            print("  %-22s %s\n      %s" % (name, loc, excerpt))

    def scan_text(self, where, text):
        self.scanned += 1
        for line_no, line in enumerate(text.splitlines(), 1):
            if "opsec-audit: allow" in line:
                continue
            for name, rx, accept in RULES:
                for m in rx.finditer(line):
                    if accept and accept(m):
                        continue
                    self.report(where, line_no, name, line)
                    break
            if self.term_re:
                m = self.term_re.search(line)
                if m:
                    self.report(where, line_no, self.terms.get(m.group(0).lower(), "local term"), line)

    def scan_bytes(self, where, data):
        if len(data) > MAX_FILE_SIZE:
            self.report(where, 0, "oversized-file", "%d bytes: too large to be source; refusing to trust it" % len(data))
            return
        if b"\0" in data[:8192]:
            self.scan_binary(where, data)
            return
        self.scan_text(where, data.decode("utf-8", "replace"))

    def scan_binary(self, where, data):
        """Printable ASCII and UTF-16LE strings of a binary."""
        self.scanned += 1
        strings = [m.group(0).decode("ascii") for m in re.finditer(rb"[\x20-\x7E]{5,}", data)]
        strings += [m.group(0).decode("utf-16-le", "replace")
                    for m in re.finditer(rb"(?:[\x20-\x7E]\x00){5,}", data)]
        root_variants = {self.root.lower(), self.root.replace("/", "\\").lower(), self.root.replace("\\", "/").lower()}
        for s in strings:
            low = s.lower()
            if any(r and r in low for r in root_variants):
                self.report(where, 0, "build-path-in-binary", s)
                continue
            if re.search(r"[\\/][^\\/]+\.pdb$", s, re.I) and re.search(r"[\\/]", s):
                self.report(where, 0, "pdb-path-in-binary", s)
                continue
            # Random machine-code bytes can look like a drive letter followed by
            # a backslash, so in binaries a path only counts when it has two
            # real folder names.
            for name, rx in BINARY_RULES:
                m = rx.search(s)
                if m and not ALLOWED_DRIVE_PATH.match(s[m.start():]):
                    self.report(where, 0, name + " (binary)", s)
                    break
            else:
                if self.term_re and self.term_re.search(s):
                    self.report(where, 0, "local term (binary)", s)

    # ---- sources -----------------------------------------------------------

    def tree(self):
        files = git("ls-files", "-co", "--exclude-standard", "-z").split("\0")
        for rel in filter(None, files):
            path = os.path.join(self.root, rel)
            if os.path.isdir(path) or not os.path.exists(path):
                continue
            self.scan_name(rel)
            with open(path, "rb") as f:
                self.scan_bytes(rel, f.read(MAX_FILE_SIZE + 1))

    def staged(self):
        files = git("diff", "--cached", "--name-only", "--diff-filter=ACMR", "-z").split("\0")
        for rel in filter(None, files):
            self.scan_name(rel)
            self.scan_bytes(rel + " (staged)", git("show", ":" + rel, binary=True))

    def scan_name(self, rel):
        ext = os.path.splitext(rel)[1].lower()
        if ext in BINARY_EXTENSIONS:
            self.report(rel, 0, "binary-in-repository", "compiled or archive files must not be committed")
        self.scan_text(rel + " (file name)", rel.replace("\\", "/"))

    def history(self):
        commits = git("rev-list", "--all").split()
        if not commits:
            return
        seen = set()
        for commit in commits:
            meta = git("show", "-s", "--format=%an%n%ae%n%cn%n%ce%n%B", commit)
            lines = meta.splitlines()
            for email in (lines[1], lines[3]) if len(lines) >= 4 else ():
                if not ALLOWED_EMAIL.search(email):
                    self.report("commit " + commit[:10], 0, "commit-identity", "author/committer e-mail: " + email)
            self.scan_text("commit " + commit[:10] + " (message/identity)", meta)
            for entry in git("ls-tree", "-r", "-z", commit).split("\0"):
                if not entry:
                    continue
                info, rel = entry.split("\t", 1)
                blob = info.split()[2]
                if blob in seen or info.split()[1] != "blob":
                    continue
                seen.add(blob)
                self.scan_name(rel)
                self.scan_bytes("%s @ %s" % (rel, commit[:10]), git("cat-file", "blob", blob, binary=True))

    def binaries(self, directory):
        for base, _dirs, names in os.walk(directory):
            for n in names:
                if os.path.splitext(n)[1].lower() in (".dll", ".exe"):
                    path = os.path.join(base, n)
                    with open(path, "rb") as f:
                        self.scan_binary(os.path.relpath(path, directory), f.read())

    def paths(self, paths):
        for p in paths:
            if os.path.isdir(p):
                for base, _dirs, names in os.walk(p):
                    for n in names:
                        full = os.path.join(base, n)
                        with open(full, "rb") as f:
                            self.scan_bytes(full, f.read(MAX_FILE_SIZE + 1))
            else:
                with open(p, "rb") as f:
                    self.scan_bytes(p, f.read(MAX_FILE_SIZE + 1))

    def identity(self):
        email = git("config", "user.email", check=False).strip()
        name = git("config", "user.name", check=False).strip()
        if not email:
            self.report("git config", 0, "commit-identity", "user.email is not set for this repository")
        elif not ALLOWED_EMAIL.search(email):
            self.report("git config", 0, "commit-identity",
                        "user.email is not a no-reply address; set one with: "
                        "git config user.email <id>+<login>@users.noreply.github.com")
        if self.term_re and name and self.term_re.search(name):
            self.report("git config", 0, "commit-identity", "user.name matches a local account or host name")


def self_test():
    """Feeds synthetic samples (assembled here so this file stays clean) through
    the detectors and checks both the hits and the deliberate non-hits."""
    bs = "\\"
    a = Auditor(os.getcwd(), quiet=True)
    a.terms = {"zzlocaluser": "local user name", "zzhost-pc": "host name"}
    a.term_re = term_regex(a.terms)
    positives = {
        "windows-profile-path": "C:" + bs + "Users" + bs + "SomePerson" + bs + "Documents",
        "drive-path": "Q:" + bs + "Projects" + bs + "secret-thing" + bs + "build",
        "unix-home-path": "/ho" + "me/someperson/.steam",
        "macos-home-path": "/Us" + "ers/someperson/Library",
        "email-address": "contact some.person" + "@" + "privatemail.io for details",
        "steam-id64": "id 7656119" + "8012345678 here",
        "steam-id-legacy": "STEAM" + "_0:1:12345678",
        "steam-id3": "[U:" + "1:123456789]",
        "steam-userdata-path": "userdata" + "/123456789/config",
        "private-ip-address": "ssh to 192." + "168.44.17 now",
        "user-at-host": "someone" + "@" + "203.0.113.9",
        "github-token": "gh" + "p_" + "A" * 36,
        "private-key": "-----BEGIN OPENSSH " + "PRIVATE KEY-----",
        "local user name": "built by zzlocaluser on a Tuesday",
        "host name": "host ZZHOST-PC responded",
    }
    negatives = [
        "C:" + bs + "Program Files (x86)" + bs + "Steam" + bs + "steamapps",
        "/ho" + "me/deck/.local/share/Steam",
        "%USERPROFILE%" + bs + "Documents" + bs + "My Games",
        "1234567+someone" + "@" + "users.noreply.github.com",
        "mail me at person" + "@" + "example.com",
        "listen on 127.0.0.1 port 80",
        "printf(\"%s:" + bs + "n\")",
        "the zzlocaluserish word and azzhost-pc2 do not count",
        "version 10.0.19045 build",
    ]
    ok = True
    for expected, sample in positives.items():
        a.findings = []
        a.scan_text("sample", sample)
        names = [f[2] for f in a.findings]
        if expected not in names:
            print("self-test FAILED: %r did not trigger %s (got %s)" % (sample, expected, names))
            ok = False
    for sample in negatives:
        a.findings = []
        a.scan_text("sample", sample)
        if a.findings:
            print("self-test FAILED: false positive %s on %r" % ([f[2] for f in a.findings], sample))
            ok = False
    # binaries: an embedded profile path in ASCII and in UTF-16
    a.findings = []
    blob = b"\x00\x01" + ("C:" + bs + "Users" + bs + "SomePerson" + bs + "x.pdb").encode("ascii") + b"\x00\x02"
    blob += ("D:" + bs + "Users" + bs + "Other" + bs + "src").encode("utf-16-le") + b"\x00\x00"
    a.scan_binary("blob", blob)
    if len(a.findings) < 2:
        print("self-test FAILED: binary string scan found %d of 2 planted paths" % len(a.findings))
        ok = False
    print("self-test %s (%d positive and %d negative samples)" % ("passed" if ok else "FAILED", len(positives),
                                                                   len(negatives)))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tree", action="store_true")
    ap.add_argument("--staged", action="store_true")
    ap.add_argument("--history", action="store_true")
    ap.add_argument("--binaries", metavar="DIR", action="append", default=[])
    ap.add_argument("--paths", nargs="+", default=[])
    ap.add_argument("--check-identity", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return 0 if self_test() else 1
    if not (args.tree or args.staged or args.history or args.binaries or args.paths or args.check_identity):
        args.tree = args.check_identity = True

    root = repo_root()
    os.chdir(root)
    auditor = Auditor(root)
    print("OPSEC audit (%d local term%s loaded from this machine's environment)" % (
        len(auditor.terms), "" if len(auditor.terms) == 1 else "s"))
    if args.check_identity:
        auditor.identity()
    if args.staged:
        auditor.staged()
    if args.tree:
        auditor.tree()
    if args.history:
        auditor.history()
    for d in args.binaries:
        auditor.binaries(d)
    if args.paths:
        auditor.paths(args.paths)

    if auditor.findings:
        print("FAILED: %d finding%s in %d scanned item%s" % (
            len(auditor.findings), "" if len(auditor.findings) == 1 else "s", auditor.scanned,
            "" if auditor.scanned == 1 else "s"))
        return 1
    print("passed: %d item%s scanned, nothing found" % (auditor.scanned, "" if auditor.scanned == 1 else "s"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
