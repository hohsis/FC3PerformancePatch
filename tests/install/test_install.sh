#!/usr/bin/env bash
# Tests for scripts/install.sh and scripts/uninstall.sh.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything happens inside a temporary folder: HOME is redirected to a fake
# home directory that contains fake Steam installations (native and Flatpak
# layout), a second Steam library, Wine prefixes and Steam user configuration.
#
#   tests/install/test_install.sh [path/to/version.dll]

set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
SCRIPTS="$REPO/scripts"
PATCH_DLL=${1:-}
CHECKS=0
FAILURES=0
CURRENT=""

PYTHON=""
for candidate in python3 python; do
    if "$candidate" -c "import sys" >/dev/null 2>&1; then
        PYTHON=$candidate
        break
    fi
done

check() { # check <condition exit status> <message>
    CHECKS=$((CHECKS + 1))
    if [ "$1" -ne 0 ]; then
        FAILURES=$((FAILURES + 1))
        printf '  FAIL [%s] %s\n' "$CURRENT" "$2"
    fi
}

contains() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

SANDBOX=$(mktemp -d 2>/dev/null || mktemp -d -t fc3pp)
trap 'rm -rf "$SANDBOX"' EXIT
REAL_HOME=$HOME
export HOME="$SANDBOX/home"
export FC3PP_TEST_STEAM_RUNNING=0

STEAM="$HOME/.local/share/Steam"
FLATPAK="$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
LIB2="$SANDBOX/second library"
FC3="$LIB2/steamapps/common/Far Cry 3"
BD="$FLATPAK/steamapps/common/Far Cry 3 Blood Dragon"
PFX="$LIB2/steamapps/compatdata/220240/pfx"
PROFILE_DIR="$PFX/drive_c/users/steamuser/Documents/My Games/Far Cry 3"
PROFILE="$PROFILE_DIR/GamerProfile.xml"
FAKE_ACCOUNT=1 # a made-up Steam account folder
LOCALCONFIG="$STEAM/userdata/$FAKE_ACCOUNT/config/localconfig.vdf"
SOURCE="$SANDBOX/package"

# --- helpers to build fixtures ---------------------------------------------------

poke() { # poke <file> <offset> <hex bytes>
    local file=$1 offset=$2 hex=$3 esc="" i
    for ((i = 0; i < ${#hex}; i += 2)); do esc+="\\x${hex:i:2}"; done
    printf '%b' "$esc" | dd of="$file" bs=1 seek="$offset" conv=notrunc 2>/dev/null
}

# Reference PE checksum, independent of the installer's awk implementation.
py_checksum() {
    "$PYTHON" - "$1" <<'PY'
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())
pe = struct.unpack_from('<I', d, 0x3C)[0]
field = pe + 88
stored = struct.unpack_from('<I', d, field)[0]
d[field:field + 4] = b'\0\0\0\0'
if len(d) % 2:
    d.append(0)
total = 0
for (w,) in struct.iter_unpack('<H', bytes(d)):
    total += w
    total = (total & 0xFFFF) + (total >> 16)
total = (total & 0xFFFF) + (total >> 16)
size = len(open(sys.argv[1], 'rb').read())
print(stored, total + size)
PY
}

checksum_valid() {
    [ -n "$PYTHON" ] || return 0
    local out
    out=$(py_checksum "$1")
    [ "${out%% *}" = "${out##* }" ]
}

is_laa() {
    local pe flags
    pe=$(od -An -tu1 -j60 -N1 "$1" | tr -d ' ')
    flags=$(od -An -tu1 -j$((pe + 22)) -N1 "$1" | tr -d ' ')
    [ $((flags & 32)) -ne 0 ]
}

make_exe() { # make_exe <path> <seed>: minimal PE32 with a valid checksum
    local file=$1 seed=$2
    head -c 1536 /dev/zero >"$file"
    poke "$file" 0 "4d5a"
    poke "$file" 60 "80000000"
    poke "$file" 128 "50450000" # PE\0\0
    poke "$file" 132 "4c01"     # i386
    poke "$file" 134 "0100"
    poke "$file" 148 "e000"
    poke "$file" 150 "0201"     # characteristics 0x0102, no LAA
    poke "$file" 152 "0b01"     # PE32
    poke "$file" 180 "00004000"
    poke "$file" 600 "$(printf '%02x%02x%02x%02x' "$seed" $((seed * 3 % 256)) 171 205)"
    if [ -n "$PYTHON" ]; then
        local sum
        sum=$(py_checksum "$file")
        sum=${sum##* }
        poke "$file" 216 "$(printf '%02x%02x%02x%02x' $((sum & 255)) $((sum >> 8 & 255)) $((sum >> 16 & 255)) $((sum >> 24 & 255)))"
    fi
}

write_profile() {
    printf '<GamerProfile>\r\n\t<RenderProfile MSAALevel="0" UseD3D11="1" D3D11MultithreadedRendering="0" VSync="0" />\r\n</GamerProfile>\r\n' >"$PROFILE"
}

write_localconfig() { # write_localconfig <LaunchOptions line or empty>
    local extra=$1
    {
        printf '"UserLocalConfigStore"\n{\n\t"Software"\n\t{\n\t\t"Valve"\n\t\t{\n\t\t\t"Steam"\n\t\t\t{\n'
        printf '\t\t\t\t"apps"\n\t\t\t\t{\n'
        printf '\t\t\t\t\t"440"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LaunchOptions"\t\t"-novid"\n\t\t\t\t\t}\n'
        printf '\t\t\t\t\t"220240"\n\t\t\t\t\t{\n\t\t\t\t\t\t"LastPlayed"\t\t"1700000000"\n'
        [ -n "$extra" ] && printf '\t\t\t\t\t\t"LaunchOptions"\t\t"%s"\n' "$extra"
        printf '\t\t\t\t\t}\n\t\t\t\t}\n\t\t\t}\n\t\t}\n\t}\n'
        printf '\t"friends"\n\t{\n\t\t"220240"\n\t\t{\n\t\t\t"name"\t\t"not an app block"\n\t\t}\n\t}\n}\n'
    } >"$LOCALCONFIG"
}

reset_sandbox() {
    rm -rf "$HOME" "$LIB2" "$SOURCE"
    mkdir -p "$STEAM/steamapps" "$FLATPAK/steamapps" "$FC3/bin" "$BD/bin" "$PROFILE_DIR" \
        "$(dirname "$LOCALCONFIG")" "$SOURCE" "$HOME/.steam"
    ln -s "$STEAM" "$HOME/.steam/steam" 2>/dev/null || true
    {
        printf '"libraryfolders"\n{\n\t"0"\n\t{\n\t\t"path"\t\t"%s"\n\t}\n' "$STEAM"
        printf '\t"1"\n\t{\n\t\t"path"\t\t"%s"\n\t\t"apps"\n\t\t{\n\t\t\t"220240"\t\t"1"\n\t\t}\n\t}\n}\n' "$LIB2"
    } >"$STEAM/steamapps/libraryfolders.vdf"
    make_exe "$FC3/bin/farcry3.exe" 1
    make_exe "$FC3/bin/farcry3_d3d11.exe" 2
    make_exe "$BD/bin/fc3_blooddragon.exe" 3
    make_exe "$BD/bin/fc3_blooddragon_d3d11.exe" 4
    printf 'engine' >"$FC3/bin/FC3.dll"
    write_profile
    write_localconfig ""
    if [ -n "$PATCH_DLL" ]; then
        cp "$PATCH_DLL" "$SOURCE/version.dll"
    else
        printf 'MZ stand-in for the FC3PerformancePatch proxy DLL' >"$SOURCE/version.dll"
    fi
    cp "$REPO/config/FC3PerformancePatch.ini" "$SOURCE/"
    SNAPSHOT=$(snapshot)
}

# Relative path + hash of every file outside the package folder.
snapshot() {
    (cd "$SANDBOX" && find . -type f ! -path './package/*' -print0 | LC_ALL=C sort -z | xargs -0 sha256sum 2>/dev/null)
}

snapshot_diff() { # prints the differing paths
    diff <(printf '%s\n' "$1") <(printf '%s\n' "$2") | sed -n 's/^[<>] [0-9a-f]*  //p' | LC_ALL=C sort -u | tr '\n' ' '
}

install() { OUT=$(bash "$SCRIPTS/install.sh" --source-dir "$SOURCE" --yes "$@" 2>&1); RC=$?; }
uninstall() { OUT=$(bash "$SCRIPTS/uninstall.sh" --yes "$@" 2>&1); RC=$?; }

backup_count() { # backup_count <file>
    local n=0 f
    for f in "$1".fc3pp-*.bak; do [ -e "$f" ] && n=$((n + 1)); done
    printf '%s' "$n"
}

launch_value() { # LaunchOptions of app 220240
    awk '/"220240"/ { inapp = 1 } inapp && /"LaunchOptions"/ { sub(/^[^"]*"LaunchOptions"[ \t]*"/, ""); sub(/"$/, ""); print; exit } inapp && /^\t\t\t\t\t}/ { exit }' "$LOCALCONFIG"
}

EXPECTED_OPTION='WINEDLLOVERRIDES=\"version=n,b\" %command%'

# =================================================================================
CURRENT="install: auto-detection (second library + Flatpak Steam)"
reset_sandbox
install
check "$RC" "installer failed: $OUT"
contains "$OUT" "Far Cry 3 Blood Dragon"; check $? "Blood Dragon in the Flatpak Steam folder was not found"
for bin in "$FC3/bin" "$BD/bin"; do
    [ -f "$bin/version.dll" ]; check $? "version.dll missing in $bin"
    [ -f "$bin/FC3PerformancePatch.ini" ]; check $? "INI missing in $bin"
    cmp -s "$bin/version.dll" "$SOURCE/version.dll"; check $? "installed DLL differs from the package"
done
for exe in "$FC3/bin/farcry3.exe" "$FC3/bin/farcry3_d3d11.exe" "$BD/bin/fc3_blooddragon.exe" "$BD/bin/fc3_blooddragon_d3d11.exe"; do
    is_laa "$exe"; check $? "$(basename "$exe") is not Large Address Aware"
    checksum_valid "$exe"; check $? "$(basename "$exe") has an invalid PE checksum after patching"
    [ "$(backup_count "$exe")" = "1" ]; check $? "$(basename "$exe") has $(backup_count "$exe") backups, expected 1"
    for b in "$exe".fc3pp-*.bak; do
        is_laa "$b"; [ $? -ne 0 ]; check $? "the backup of $(basename "$exe") is not the original"
        case "$(basename "$b")" in *.fc3pp-[0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9].bak) true ;; *) false ;; esac
        check $? "backup name $(basename "$b") is not timestamped"
    done
done
contains "$OUT" 'WINEDLLOVERRIDES="version=n,b" %command%'; check $? "the launch option was not printed"
[ "$(backup_count "$PROFILE")" = "0" ]; check $? "GamerProfile.xml was backed up although it was not modified"
[ -z "$(launch_value)" ]; check $? "Steam's configuration was changed without --set-launch-option"
[ "$(backup_count "$LOCALCONFIG")" = "0" ]; check $? "localconfig.vdf was touched without --set-launch-option"
# Proton ships its own DXVK: the installer must never add Direct3D DLLs.
for f in d3d9.dll d3d11.dll dxgi.dll d3d10core.dll; do
    [ ! -e "$FC3/bin/$f" ]; check $? "$f was created in the game folder"
done
grep -q -i "dxvk" "$SCRIPTS/install.sh" && ! grep -q -E '(cp|curl|wget|tar)[^#]*dxvk' "$SCRIPTS/install.sh"
check $? "install.sh contains code that deploys DXVK"

CURRENT="install: idempotent"
BEFORE=$(snapshot)
install
check "$RC" "second install failed: $OUT"
AFTER=$(snapshot)
[ "$BEFORE" = "$AFTER" ]; check $? "a second install changed: $(snapshot_diff "$BEFORE" "$AFTER")"
contains "$OUT" "already Large Address Aware"; check $? "second run did not report the executables as already patched"

CURRENT="uninstall: byte-identical restore"
uninstall
check "$RC" "uninstaller failed: $OUT"
AFTER=$(snapshot)
[ "$SNAPSHOT" = "$AFTER" ]; check $? "uninstall did not restore: $(snapshot_diff "$SNAPSHOT" "$AFTER")"

CURRENT="uninstall: idempotent"
uninstall
check "$RC" "second uninstall failed: $OUT"
contains "$OUT" "nothing to remove"; check $? "second uninstall did not say that nothing was left"
[ "$SNAPSHOT" = "$(snapshot)" ]; check $? "second uninstall changed things"

# =================================================================================
CURRENT="another mod's version.dll"
reset_sandbox
printf 'some other mod loader, definitely not ours' >"$FC3/bin/version.dll"
SNAPSHOT=$(snapshot)
install --game fc3
check "$RC" "install failed: $OUT"
contains "$OUT" "another mod"; check $? "the displaced DLL was not mentioned"
[ "$(backup_count "$FC3/bin/version.dll")" = "1" ]; check $? "the other mod's DLL was not backed up"
[ ! -e "$BD/bin/version.dll" ]; check $? "--game fc3 also installed into Blood Dragon"
uninstall --game fc3
[ "$SNAPSHOT" = "$(snapshot)" ]; check $? "the other mod's DLL was not restored: $(snapshot_diff "$SNAPSHOT" "$(snapshot)")"

# =================================================================================
CURRENT="configuration is preserved"
reset_sandbox
install --game fc3
printf '; my own tweak\n' >>"$FC3/bin/FC3PerformancePatch.ini"
MINE=$(sha256sum "$FC3/bin/FC3PerformancePatch.ini")
install --game fc3
[ "$MINE" = "$(sha256sum "$FC3/bin/FC3PerformancePatch.ini")" ]; check $? "a re-install overwrote the user's INI"
install --game fc3 --reset-config
cmp -s "$FC3/bin/FC3PerformancePatch.ini" "$SOURCE/FC3PerformancePatch.ini"; check $? "--reset-config did not restore the default INI"
[ "$(backup_count "$FC3/bin/FC3PerformancePatch.ini")" = "1" ]; check $? "--reset-config did not back up the user's INI"
uninstall --game fc3 --keep-config
[ -f "$FC3/bin/FC3PerformancePatch.ini" ]; check $? "--keep-config removed the INI"
[ ! -e "$FC3/bin/version.dll" ]; check $? "--keep-config left the DLL behind"
uninstall --game fc3
[ "$SNAPSHOT" = "$(snapshot)" ]; check $? "leftovers after uninstall: $(snapshot_diff "$SNAPSHOT" "$(snapshot)")"

# =================================================================================
CURRENT="explicit --game-dir"
reset_sandbox
install --game-dir "$FC3/bin"
check "$RC" "--game-dir <bin> failed: $OUT"
[ -f "$FC3/bin/version.dll" ] && [ ! -e "$BD/bin/version.dll" ]; check $? "--game-dir did not install into exactly that game"
uninstall --game-dir "$FC3"
[ "$SNAPSHOT" = "$(snapshot)" ]; check $? "uninstall with --game-dir left changes"
install --game-dir "$SANDBOX/no such folder"
[ "$RC" -ne 0 ]; check $? "a wrong --game-dir did not fail"
install --game fc3 --no-laa
is_laa "$FC3/bin/farcry3.exe"; [ $? -ne 0 ]; check $? "--no-laa still patched the executable"
uninstall
install --bogus-option
[ "$RC" -ne 0 ]; check $? "an unknown option was accepted"

CURRENT="game not found"
reset_sandbox
rm -rf "$LIB2" "$BD"
install
[ "$RC" -ne 0 ] && contains "$OUT" "--game-dir"; check $? "a missing game did not produce a helpful error"

CURRENT="package without the DLL"
reset_sandbox
rm -f "$SOURCE/version.dll"
install
[ "$RC" -ne 0 ] && contains "$OUT" "release package"; check $? "a source checkout without version.dll did not explain itself"

# =================================================================================
CURRENT="backup made by the DLL itself (drag-and-drop install)"
reset_sandbox
cp "$FC3/bin/farcry3_d3d11.exe" "$FC3/bin/farcry3_d3d11.exe.fc3pp-20200101-000000.bak"
poke "$FC3/bin/farcry3_d3d11.exe" 150 "22" # what the DLL does at first launch
cp "$SOURCE/version.dll" "$SOURCE/FC3PerformancePatch.ini" "$FC3/bin/"
printf 'log' >"$FC3/bin/FC3PerformancePatch.log"
uninstall --game fc3
check "$RC" "uninstall failed: $OUT"
[ "$SNAPSHOT" = "$(snapshot)" ]; check $? "a drag-and-drop install was not cleaned up: $(snapshot_diff "$SNAPSHOT" "$(snapshot)")"

CURRENT="game updated after patching"
reset_sandbox
install --game fc3
make_exe "$FC3/bin/farcry3.exe" 99
UPDATED=$(sha256sum "$FC3/bin/farcry3.exe")
uninstall --game fc3
check "$RC" "uninstall failed: $OUT"
[ "$UPDATED" = "$(sha256sum "$FC3/bin/farcry3.exe")" ]; check $? "uninstall overwrote an updated executable with an old backup"
contains "$OUT" "has changed since"; check $? "the kept executable was not mentioned"
is_laa "$FC3/bin/farcry3_d3d11.exe"; [ $? -ne 0 ]; check $? "the untouched executable was not restored"

# =================================================================================
CURRENT="--enable-mt-rendering"
reset_sandbox
install --game fc3 --enable-mt-rendering
check "$RC" "install failed: $OUT"
grep -q 'D3D11MultithreadedRendering="1"' "$PROFILE"; check $? "the setting was not changed inside the Wine prefix"
[ "$(backup_count "$PROFILE")" = "1" ]; check $? "GamerProfile.xml was modified without a backup"
grep -q $'\r' "$PROFILE"; check $? "the file's CRLF line endings were lost"
BEFORE=$(snapshot)
install --game fc3 --enable-mt-rendering
[ "$BEFORE" = "$(snapshot)" ]; check $? "re-install is not idempotent: $(snapshot_diff "$BEFORE" "$(snapshot)")"
sed 's/VSync="0"/VSync="1"/' "$PROFILE" >"$PROFILE.tmp" && mv "$PROFILE.tmp" "$PROFILE" # the user changes a setting
uninstall --game fc3
grep -q 'VSync="1"' "$PROFILE"; check $? "uninstall threw away a setting the user changed after installing"
grep -q 'D3D11MultithreadedRendering="0"' "$PROFILE"; check $? "uninstall did not undo its own change"
[ "$(backup_count "$PROFILE")" = "0" ]; check $? "GamerProfile.xml backups were left behind"

reset_sandbox
install --game fc3 --enable-mt-rendering
uninstall --game fc3
[ "$SNAPSHOT" = "$(snapshot)" ]; check $? "install+uninstall with --enable-mt-rendering is not byte-identical: $(snapshot_diff "$SNAPSHOT" "$(snapshot)")"

# =================================================================================
CURRENT="--set-launch-option: no option yet"
reset_sandbox
install --game fc3 --set-launch-option
check "$RC" "install failed: $OUT"
[ "$(launch_value)" = "$EXPECTED_OPTION" ]; check $? "launch option is '$(launch_value)'"
grep -q '"LaunchOptions"		"-novid"' "$LOCALCONFIG"; check $? "another game's launch option was changed"
grep -q '"name"		"not an app block"' "$LOCALCONFIG"; check $? "an unrelated block with the same id was modified"
[ "$(backup_count "$LOCALCONFIG")" = "1" ]; check $? "localconfig.vdf was modified without a backup"
BEFORE=$(snapshot)
install --game fc3 --set-launch-option
[ "$BEFORE" = "$(snapshot)" ]; check $? "setting the launch option twice changed things"
uninstall --game fc3
[ -z "$(launch_value)" ]; check $? "uninstall left the launch option behind: '$(launch_value)'"

CURRENT="--set-launch-option: existing arguments"
reset_sandbox
write_localconfig "-skipintro -offline"
install --game fc3 --set-launch-option
[ "$(launch_value)" = "$EXPECTED_OPTION -skipintro -offline" ]; check $? "launch option is '$(launch_value)'"
uninstall --game fc3
[ "$(launch_value)" = "%command% -skipintro -offline" ]; check $? "after uninstall the option is '$(launch_value)'"

CURRENT="--set-launch-option: existing %command% wrapper"
reset_sandbox
write_localconfig "gamemoderun %command% -skipintro"
install --game fc3 --set-launch-option
[ "$(launch_value)" = 'WINEDLLOVERRIDES=\"version=n,b\" gamemoderun %command% -skipintro' ]; check $? "launch option is '$(launch_value)'"
uninstall --game fc3
[ "$(launch_value)" = "gamemoderun %command% -skipintro" ]; check $? "after uninstall the option is '$(launch_value)'"

CURRENT="--set-launch-option: user already has WINEDLLOVERRIDES"
reset_sandbox
write_localconfig 'WINEDLLOVERRIDES=\"xaudio2_7=n,b\" %command%'
BEFORE=$(sha256sum "$LOCALCONFIG")
install --game fc3 --set-launch-option
check "$RC" "install failed: $OUT"
[ "$BEFORE" = "$(sha256sum "$LOCALCONFIG")" ]; check $? "an existing WINEDLLOVERRIDES option was rewritten"
contains "$OUT" "by hand"; check $? "the user was not told to merge the option manually"

CURRENT="--set-launch-option: Steam running / game never started"
reset_sandbox
BEFORE=$(sha256sum "$LOCALCONFIG")
FC3PP_TEST_STEAM_RUNNING=1 install --game fc3 --set-launch-option
check "$RC" "install failed: $OUT"
[ "$BEFORE" = "$(sha256sum "$LOCALCONFIG")" ]; check $? "Steam's configuration was edited while Steam was running"
contains "$OUT" "Steam is running"; check $? "no explanation was given"
install --game blooddragon --set-launch-option
check "$RC" "install failed: $OUT"
contains "$OUT" "start it once"; check $? "a game without Steam settings was not explained"
[ "$BEFORE" = "$(sha256sum "$LOCALCONFIG")" ]; check $? "localconfig.vdf was changed for a game that has no block in it"

# =================================================================================
CURRENT="shell syntax"
bash -n "$SCRIPTS/install.sh"; check $? "install.sh has a syntax error"
bash -n "$SCRIPTS/uninstall.sh"; check $? "uninstall.sh has a syntax error"
if command -v shellcheck >/dev/null 2>&1; then
    shellcheck -S warning "$SCRIPTS/install.sh" "$SCRIPTS/uninstall.sh" >/dev/null 2>&1
    check $? "shellcheck reports warnings: $(shellcheck -S warning "$SCRIPTS/install.sh" "$SCRIPTS/uninstall.sh" 2>&1 | head -n 20)"
fi

export HOME=$REAL_HOME
printf '%s checks, %s failures%s\n' "$CHECKS" "$FAILURES" "$([ -n "$PYTHON" ] || printf ' (PE checksum cross-check skipped: no python)')"
[ "$FAILURES" -eq 0 ]
