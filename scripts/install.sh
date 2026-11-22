#!/usr/bin/env bash
# FC3PerformancePatch - installer / uninstaller for Linux (Steam Proton, Steam Deck).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The patch itself is two files that can simply be copied into the game's "bin"
# folder, plus one Steam launch option. This script does the copying for you:
#
#   * finds the game in your Steam libraries (native, Flatpak and Snap Steam)
#   * copies version.dll and FC3PerformancePatch.ini into <game>/bin
#   * enables the Large Address Aware flag on the game executables
#   * prints the launch option Proton needs (or sets it with --set-launch-option)
#
# It never touches DXVK: Proton already runs the game on its own DXVK.
# Every file that is modified is first copied to "<name>.fc3pp-<UTC date>-<time>.bak".
# Running the script twice changes nothing; --uninstall puts everything back.

set -u

PATCH_NAME="FC3PerformancePatch"
DLL_NAME="version.dll"
INI_NAME="$PATCH_NAME.ini"
LOG_NAME="$PATCH_NAME.log"
MANIFEST_NAME="$PATCH_NAME.install.txt"
BACKUP_INFIX=".fc3pp-"
LAUNCH_TOKEN='WINEDLLOVERRIDES="version=n,b"'
LAUNCH_OPTION="$LAUNCH_TOKEN %command%"

# id|display name|Steam app id|profile folder|executables
GAMES="fc3|Far Cry 3|220240|Far Cry 3|farcry3.exe farcry3_d3d11.exe
blooddragon|Far Cry 3 Blood Dragon|233270|Far Cry 3 Blood Dragon|fc3_blooddragon.exe fc3_blooddragon_d3d11.exe"

GAME_DIR=""
GAME_FILTER="auto"
PREFIX_DIR=""
SOURCE_DIR=""
NO_LAA=0
MT_RENDERING=0
SET_LAUNCH_OPTION=0
RESET_CONFIG=0
UNINSTALL=0
KEEP_CONFIG=0
UNATTENDED=0

usage() {
    cat <<'EOF'
Usage: install.sh [options]

  --game-dir DIR          game folder (the one that contains "bin"); skips auto-detection
  --game NAME             auto (default), fc3 or blooddragon
  --prefix DIR            the game's Wine prefix ("pfx" folder) if it is not found automatically
  --no-laa                do not modify the game executables
  --enable-mt-rendering   set D3D11MultithreadedRendering="1" in GamerProfile.xml
                          (reasonable on Proton, which always uses DXVK; off by default)
  --set-launch-option     write the required launch option into Steam's configuration
                          (Steam must be closed); by default it is only printed
  --reset-config          replace an existing FC3PerformancePatch.ini with the default
  --uninstall             remove the patch and restore every backup
  --keep-config           with --uninstall: keep FC3PerformancePatch.ini
  -y, --yes               never ask questions
  -h, --help              show this help
EOF
}

say()  { printf '  %s\n' "$*"; }
note() { printf '  note: %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --game-dir) [ $# -ge 2 ] || die "--game-dir needs a folder"; GAME_DIR=$2; shift ;;
        --game) [ $# -ge 2 ] || die "--game needs a name"; GAME_FILTER=$2; shift ;;
        --prefix) [ $# -ge 2 ] || die "--prefix needs a folder"; PREFIX_DIR=$2; shift ;;
        --source-dir) [ $# -ge 2 ] || die "--source-dir needs a folder"; SOURCE_DIR=$2; shift ;;
        --no-laa) NO_LAA=1 ;;
        --enable-mt-rendering) MT_RENDERING=1 ;;
        --set-launch-option) SET_LAUNCH_OPTION=1 ;;
        --reset-config) RESET_CONFIG=1 ;;
        --uninstall) UNINSTALL=1 ;;
        --keep-config) KEEP_CONFIG=1 ;;
        -y|--yes) UNATTENDED=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

case "$GAME_FILTER" in
    auto|fc3|blooddragon) ;;
    *) die "--game must be auto, fc3 or blooddragon" ;;
esac

if [ -z "$SOURCE_DIR" ]; then
    SOURCE_DIR=$(cd "$(dirname "$0")" && pwd)
fi

# ----------------------------------------------------------------------------- helpers

stamp() { date -u +%Y%m%d-%H%M%S; }

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

same_file() { [ "$(sha256 "$1")" = "$(sha256 "$2")" ]; }

# Copies FILE to FILE.fc3pp-<stamp>.bak and prints the backup's name.
make_backup() {
    local file=$1 s backup n=1
    s=$(stamp)
    backup="$file$BACKUP_INFIX$s.bak"
    while [ -e "$backup" ]; do
        backup="$file$BACKUP_INFIX$s-$n.bak"
        n=$((n + 1))
    done
    cp -p "$file" "$backup" || die "could not create a backup of $(basename "$file")"
    basename "$backup"
}

# Prints the backups of FILE, oldest first, one per line.
list_backups() {
    local file=$1 f
    for f in "$file$BACKUP_INFIX"*.bak; do
        [ -e "$f" ] && printf '%s\n' "$f"
    done | LC_ALL=C sort
}

oldest_backup() { list_backups "$1" | head -n 1; }

remove_backups() {
    local f
    list_backups "$1" | while IFS= read -r f; do rm -f "$f"; done
}

is_our_dll() { [ -f "$1" ] && LC_ALL=C grep -a -q "$PATCH_NAME" "$1"; }

# ----------------------------------------------------------------------------- PE: Large Address Aware

# Little-endian unsigned integer of NBYTES at OFFSET.
rd_le() {
    local file=$1 offset=$2 nbytes=$3 value=0 mult=1 b
    for b in $(od -An -v -tu1 -j"$offset" -N"$nbytes" "$file"); do
        value=$((value + b * mult))
        mult=$((mult * 256))
    done
    printf '%s' "$value"
}

# Writes VALUE as NBYTES little-endian bytes at OFFSET.
wr_le() {
    local file=$1 offset=$2 nbytes=$3 value=$4 i=0 esc=""
    while [ "$i" -lt "$nbytes" ]; do
        esc="$esc\\$(printf '%03o' $((value % 256)))"
        value=$((value / 256))
        i=$((i + 1))
    done
    # shellcheck disable=SC2059
    printf "$esc" | dd of="$file" bs=1 seek="$offset" conv=notrunc 2>/dev/null
}

# Offset of the PE signature if FILE is a 32-bit x86 executable; fails otherwise.
pe_offset() {
    local file=$1 size pe
    size=$(wc -c <"$file" | tr -d ' ')
    [ "$size" -ge 256 ] || return 1
    [ "$(rd_le "$file" 0 2)" = "23117" ] || return 1               # "MZ"
    pe=$(rd_le "$file" 60 4)
    [ "$pe" -gt 0 ] && [ $((pe + 96)) -le "$size" ] || return 1
    [ "$(rd_le "$file" "$pe" 4)" = "17744" ] || return 1           # "PE\0\0"
    [ "$(rd_le "$file" $((pe + 4)) 2)" = "332" ] || return 1       # machine: i386
    [ "$(rd_le "$file" $((pe + 24)) 2)" = "267" ] || return 1      # PE32
    printf '%s' "$pe"
}

# Standard PE checksum; the stored checksum field (at CSUM_OFFSET) counts as zero.
pe_checksum() {
    local file=$1 csum_offset=$2
    od -An -v -tu1 "$file" | awk -v c="$csum_offset" '
        { for (k = 1; k <= NF; k++) {
              if (i % 2 == 0) { lo = $k }
              else {
                  off = i - 1
                  if (off != c && off != c + 2) {
                      sum += lo + 256 * $k
                      sum = (sum % 65536) + int(sum / 65536)
                  }
              }
              i++
          } }
        END {
            if (i % 2 == 1) { sum += lo; sum = (sum % 65536) + int(sum / 65536) }
            sum = (sum % 65536) + int(sum / 65536)
            printf "%d", sum + i
        }'
}

# Prints "patched", "already" or "not-pe".
set_laa() {
    local file=$1 pe flags csum
    pe=$(pe_offset "$file") || { printf 'not-pe'; return; }
    flags=$(rd_le "$file" $((pe + 22)) 2)
    if [ $((flags & 32)) -ne 0 ]; then
        printf 'already'
        return
    fi
    make_backup "$file" >/dev/null
    wr_le "$file" $((pe + 22)) 2 $((flags | 32))
    csum=$(rd_le "$file" $((pe + 88)) 4)
    if [ "$csum" -ne 0 ]; then
        wr_le "$file" $((pe + 88)) 4 "$(pe_checksum "$file" $((pe + 88)))"
    fi
    printf 'patched'
}

# True when CURRENT and ORIGINAL differ only in the LAA flag byte and the checksum.
only_laa_difference() {
    local current=$1 original=$2 pe diffs off
    [ "$(wc -c <"$current" | tr -d ' ')" = "$(wc -c <"$original" | tr -d ' ')" ] || return 1
    pe=$(pe_offset "$original") || return 1
    # cmp -l prints 1-based offsets of differing bytes
    diffs=$(cmp -l "$current" "$original" 2>/dev/null | awk '{print $1 - 1}')
    for off in $diffs; do
        if [ "$off" -ne $((pe + 22)) ] && { [ "$off" -lt $((pe + 88)) ] || [ "$off" -gt $((pe + 91)) ]; }; then
            return 1
        fi
    done
    return 0
}

# ----------------------------------------------------------------------------- finding Steam and the game

steam_roots() {
    local candidates r
    if [ -n "${FC3PP_STEAM_ROOTS:-}" ]; then
        candidates=$(printf '%s' "$FC3PP_STEAM_ROOTS" | tr ':' '\n')
    else
        candidates="$HOME/.steam/steam
$HOME/.steam/root
$HOME/.local/share/Steam
$HOME/.steam/debian-installation
$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam
$HOME/.var/app/com.valvesoftware.Steam/data/Steam
$HOME/snap/steam/common/.local/share/Steam"
    fi
    printf '%s\n' "$candidates" | while IFS= read -r r; do
        [ -n "$r" ] && [ -d "$r/steamapps" ] && (cd "$r" && pwd -P)
    done | awk '!seen[$0]++'
}

# Every Steam library: the roots plus each "path" in libraryfolders.vdf.
steam_libraries() {
    local root vdf
    steam_roots | while IFS= read -r root; do
        printf '%s\n' "$root"
        for vdf in "$root/steamapps/libraryfolders.vdf" "$root/config/libraryfolders.vdf"; do
            [ -f "$vdf" ] && sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$vdf" |
                sed 's/\\\\/\//g'
        done
    done | while IFS= read -r lib; do
        [ -d "$lib/steamapps" ] && (cd "$lib" && pwd -P)
    done | awk '!seen[$0]++'
}

has_game_exe() {
    local root=$1 exes=$2 e
    for e in $exes; do
        [ -f "$root/bin/$e" ] && return 0
    done
    return 1
}

find_game_root() {
    local name=$1 exes=$2 lib
    steam_libraries | while IFS= read -r lib; do
        if has_game_exe "$lib/steamapps/common/$name" "$exes"; then
            printf '%s\n' "$lib/steamapps/common/$name"
            break
        fi
    done
}

find_prefix() {
    local appid=$1 lib
    if [ -n "$PREFIX_DIR" ]; then
        printf '%s\n' "$PREFIX_DIR"
        return
    fi
    steam_libraries | while IFS= read -r lib; do
        if [ -d "$lib/steamapps/compatdata/$appid/pfx" ]; then
            printf '%s\n' "$lib/steamapps/compatdata/$appid/pfx"
            break
        fi
    done
}

find_profile() {
    local prefix=$1 folder=$2 docs
    [ -n "$prefix" ] || return 0
    for docs in "Documents" "My Documents"; do
        if [ -f "$prefix/drive_c/users/steamuser/$docs/My Games/$folder/GamerProfile.xml" ]; then
            printf '%s\n' "$prefix/drive_c/users/steamuser/$docs/My Games/$folder/GamerProfile.xml"
            return
        fi
    done
}

steam_is_running() {
    [ "${FC3PP_TEST_STEAM_RUNNING:-}" = "1" ] && return 0
    [ "${FC3PP_TEST_STEAM_RUNNING:-}" = "0" ] && return 1
    pgrep -x steam >/dev/null 2>&1
}

game_is_running() {
    local exes=$1 e
    [ -n "${FC3PP_TEST_STEAM_RUNNING:-}" ] && return 1
    for e in $exes; do
        pgrep -f "$e" >/dev/null 2>&1 && return 0
    done
    return 1
}

# ----------------------------------------------------------------------------- manifest (key=value lines)

manifest_get() {
    [ -f "$1" ] || return 0
    sed -n "s/^$2=//p" "$1" | tail -n 1
}

manifest_set() {
    local file=$1 key=$2 value=$3
    if [ -f "$file" ]; then
        grep -v "^$key=" "$file" >"$file.tmp" || true
        mv "$file.tmp" "$file"
    fi
    printf '%s=%s\n' "$key" "$value" >>"$file"
}

# ----------------------------------------------------------------------------- Steam launch option

# Rewrites the LaunchOptions of APPID in a localconfig.vdf read from stdin.
# MODE is "add" or "remove". Prints the new file; the last line is a status
# marker "#fc3pp:<status>" (set|unchanged|conflict|noapp|removed).
launch_option_filter() {
    awk -v appid="$1" -v mode="$2" -v token='WINEDLLOVERRIDES=\\"version=n,b\\"' '
        function indent(n,   s) { s = ""; while (n-- > 0) s = s "\t"; return s }
        function in_app() {
            return depth >= 2 && tolower(path[depth - 1]) == "apps" && path[depth] == appid && tolower(path[depth - 2]) == "steam"
        }
        BEGIN { depth = 0; status = "noapp"; seen = 0 }
        {
            line = $0
            stripped = line
            gsub(/^[ \t]+|[ \t\r]+$/, "", stripped)
            if (stripped == "{") {
                depth++
                path[depth] = pending
                if (in_app()) { status = "unchanged"; seen = 0 }
                print line
                next
            }
            if (stripped == "}") {
                if (in_app() && !seen && mode == "add") {
                    print indent(depth + 1) "\"LaunchOptions\"\t\t\"" token " %command%\""
                    status = "set"
                }
                depth--
                print line
                next
            }
            if (in_app() && tolower(stripped) ~ /^"launchoptions"[ \t]/) {
                seen = 1
                value = stripped
                sub(/^"[^"]*"[ \t]+"/, "", value)
                sub(/"$/, "", value)
                if (mode == "add") {
                    if (index(value, token) > 0) {
                        status = "unchanged"
                    } else if (index(value, "WINEDLLOVERRIDES") > 0) {
                        status = "conflict"
                    } else {
                        if (value == "") value = token " %command%"
                        else if (index(value, "%command%") > 0) value = token " " value
                        else value = token " %command% " value
                        status = "set"
                    }
                } else {
                    if (index(value, token) > 0) {
                        if (value == token " %command%") value = ""
                        else { sub(/WINEDLLOVERRIDES=\\"version=n,b\\" /, "", value) }
                        status = "removed"
                    }
                }
                lead = line
                sub(/".*$/, "", lead)
                print lead "\"LaunchOptions\"\t\t\"" value "\""
                next
            }
            if (stripped ~ /^"[^"]*"$/) pending = substr(stripped, 2, length(stripped) - 2)
            print line
        }
        END { print "#fc3pp:" status }'
}

# Applies launch_option_filter to every localconfig.vdf. Prints a summary line.
edit_launch_option() {
    local appid=$1 mode=$2 root cfg status any=0 result="noapp"
    while IFS= read -r root; do
        for cfg in "$root"/userdata/*/config/localconfig.vdf; do
            [ -f "$cfg" ] || continue
            any=1
            launch_option_filter "$appid" "$mode" <"$cfg" >"$cfg.fc3pp-new" || { rm -f "$cfg.fc3pp-new"; continue; }
            status=$(tail -n 1 "$cfg.fc3pp-new" | sed -n 's/^#fc3pp://p')
            if [ "$status" = "set" ] || [ "$status" = "removed" ]; then
                make_backup "$cfg" >/dev/null
                sed '$d' "$cfg.fc3pp-new" >"$cfg.fc3pp-out" && cat "$cfg.fc3pp-out" >"$cfg"
                rm -f "$cfg.fc3pp-out"
                result=$status
            elif [ "$result" = "noapp" ] && [ -n "$status" ]; then
                result=$status
            fi
            rm -f "$cfg.fc3pp-new"
        done
    done <<EOF
$(steam_roots)
EOF
    [ "$any" = "1" ] || result="noconfig"
    printf '%s' "$result"
}

print_launch_option() {
    local name=$1
    printf '\n'
    say "Required once in Steam: right-click \"$name\" > Properties > General > Launch Options:"
    printf '\n      %s\n\n' "$LAUNCH_OPTION"
    say "(Wine uses its own version.dll unless told otherwise; without this line the patch is not loaded.)"
}

# ----------------------------------------------------------------------------- install

install_game() {
    local name=$1 appid=$2 profile_folder=$3 exes=$4 root=$5
    local bin="$root/bin" manifest prefix profile e result old backup status
    manifest="$bin/$MANIFEST_NAME"

    printf '\n%s\n' "$name"
    game_is_running "$exes" && die "$name is running. Close the game and run this again."

    # --- version.dll
    if [ -f "$bin/$DLL_NAME" ]; then
        if same_file "$bin/$DLL_NAME" "$SOURCE_DIR/$DLL_NAME"; then
            say "$DLL_NAME is already up to date"
        elif is_our_dll "$bin/$DLL_NAME"; then
            cp "$SOURCE_DIR/$DLL_NAME" "$bin/$DLL_NAME" || die "could not write $DLL_NAME"
            say "$DLL_NAME updated"
        else
            backup=$(make_backup "$bin/$DLL_NAME")
            cp "$SOURCE_DIR/$DLL_NAME" "$bin/$DLL_NAME" || die "could not write $DLL_NAME"
            note "another mod's $DLL_NAME was in the way; it was saved as $backup and will be restored on uninstall"
        fi
    else
        cp "$SOURCE_DIR/$DLL_NAME" "$bin/$DLL_NAME" || die "could not write to $bin (is it writable?)"
        say "$DLL_NAME installed"
    fi

    # --- INI
    if [ ! -f "$bin/$INI_NAME" ]; then
        cp "$SOURCE_DIR/$INI_NAME" "$bin/$INI_NAME" || die "could not write $INI_NAME"
        say "$INI_NAME installed"
    elif [ "$RESET_CONFIG" = "1" ] && ! same_file "$bin/$INI_NAME" "$SOURCE_DIR/$INI_NAME"; then
        backup=$(make_backup "$bin/$INI_NAME")
        cp "$SOURCE_DIR/$INI_NAME" "$bin/$INI_NAME"
        say "$INI_NAME reset to defaults (your version: $backup)"
    else
        say "$INI_NAME kept (your settings are untouched)"
    fi

    # --- Large Address Aware
    if [ "$NO_LAA" = "1" ]; then
        say "Large Address Aware: skipped (--no-laa)"
    else
        for e in $exes; do
            [ -f "$bin/$e" ] || continue
            result=$(set_laa "$bin/$e")
            case "$result" in
                patched) say "$e : Large Address Aware enabled (original kept as a backup)" ;;
                already) say "$e : already Large Address Aware" ;;
                *) note "$e is not a 32-bit executable; left alone" ;;
            esac
        done
    fi

    # --- GamerProfile.xml (inside the Wine prefix)
    prefix=$(find_prefix "$appid")
    profile=$(find_profile "$prefix" "$profile_folder")
    if [ "$MT_RENDERING" = "1" ]; then
        if [ -n "$profile" ]; then
            old=$(sed -n 's/.*[[:space:]]D3D11MultithreadedRendering="\([^"]*\)".*/\1/p' "$profile" | head -n 1)
            if [ -z "$old" ]; then
                note "GamerProfile.xml has no D3D11MultithreadedRendering setting; left alone"
            elif [ "$old" = "1" ]; then
                say "GamerProfile.xml: D3D11MultithreadedRendering is already 1"
            else
                make_backup "$profile" >/dev/null
                sed 's/\([[:space:]]D3D11MultithreadedRendering="\)[^"]*"/\11"/' "$profile" >"$profile.fc3pp-new" &&
                    cat "$profile.fc3pp-new" >"$profile"
                rm -f "$profile.fc3pp-new"
                [ -n "$(manifest_get "$manifest" profile_original)" ] || manifest_set "$manifest" profile_original "$old"
                say "GamerProfile.xml: D3D11MultithreadedRendering set to 1 (backup kept)"
            fi
        else
            note "GamerProfile.xml not found (start the game once first); multithreaded rendering was not changed"
        fi
    else
        say "GamerProfile.xml: not modified"
    fi

    # --- launch option
    if [ "$SET_LAUNCH_OPTION" = "1" ]; then
        if steam_is_running; then
            note "Steam is running, so its configuration cannot be changed safely. Close Steam and run this again with --set-launch-option, or set it by hand:"
            print_launch_option "$name"
        else
            status=$(edit_launch_option "$appid" add)
            case "$status" in
                set) say "Steam launch option set: $LAUNCH_OPTION" ;;
                unchanged) say "Steam launch option is already set" ;;
                conflict)
                    note "the game already has its own WINEDLLOVERRIDES launch option; add version=n,b to it by hand:"
                    print_launch_option "$name" ;;
                *)
                    note "Steam has no settings for this game yet (start it once), so the launch option could not be written:"
                    print_launch_option "$name" ;;
            esac
        fi
    else
        print_launch_option "$name"
    fi

    manifest_set "$manifest" patch_version "1.0.0"
    say "$name: done"
}

# ----------------------------------------------------------------------------- uninstall

# Prints restored|unchanged|kept|none.
restore_oldest_backup() {
    local file=$1 always=${2:-0} oldest
    oldest=$(oldest_backup "$file")
    [ -n "$oldest" ] || { printf 'none'; return; }
    if [ ! -f "$file" ]; then
        cp -p "$oldest" "$file"
        remove_backups "$file"
        printf 'restored'
    elif same_file "$file" "$oldest"; then
        remove_backups "$file"
        printf 'unchanged'
    elif [ "$always" = "1" ] || only_laa_difference "$file" "$oldest"; then
        cat "$oldest" >"$file"
        remove_backups "$file"
        printf 'restored'
    else
        printf 'kept'
    fi
}

uninstall_game() {
    local name=$1 appid=$2 profile_folder=$3 exes=$4 root=$5
    local bin="$root/bin" manifest prefix profile e did=0 original current status
    manifest="$bin/$MANIFEST_NAME"

    printf '\n%s\n' "$name"
    game_is_running "$exes" && die "$name is running. Close the game and run this again."

    if is_our_dll "$bin/$DLL_NAME"; then
        rm -f "$bin/$DLL_NAME"
        say "$DLL_NAME removed"
        did=1
    fi
    if [ "$(restore_oldest_backup "$bin/$DLL_NAME" 1)" = "restored" ]; then
        say "the previous $DLL_NAME was restored"
        did=1
    fi

    for e in $exes; do
        case "$(restore_oldest_backup "$bin/$e")" in
            restored) say "$e restored to the original"; did=1 ;;
            unchanged) say "$e was already original; backup removed"; did=1 ;;
            kept) note "$e has changed since it was patched (game update?); it was left alone and its backups were kept" ;;
        esac
    done

    # GamerProfile.xml: undo only our own change, keep whatever the user changed since.
    original=$(manifest_get "$manifest" profile_original)
    prefix=$(find_prefix "$appid")
    profile=$(find_profile "$prefix" "$profile_folder")
    if [ -n "$original" ] && [ -n "$profile" ]; then
        current=$(sed -n 's/.*[[:space:]]D3D11MultithreadedRendering="\([^"]*\)".*/\1/p' "$profile" | head -n 1)
        if [ "$current" = "1" ]; then
            sed "s/\\([[:space:]]D3D11MultithreadedRendering=\"\\)[^\"]*\"/\\1$original\"/" "$profile" >"$profile.fc3pp-new" &&
                cat "$profile.fc3pp-new" >"$profile"
            rm -f "$profile.fc3pp-new"
            say "GamerProfile.xml: D3D11MultithreadedRendering set back to $original"
        fi
        remove_backups "$profile"
        did=1
    fi

    # Launch option: remove ours wherever it is found (only possible while Steam is closed).
    if steam_is_running; then
        note "Steam is running: if you set the launch option, remove \"$LAUNCH_TOKEN\" from it by hand"
    else
        status=$(edit_launch_option "$appid" remove)
        if [ "$status" = "removed" ]; then
            say "Steam launch option removed"
            did=1
        fi
    fi

    if [ -f "$bin/$INI_NAME" ]; then
        if [ "$KEEP_CONFIG" = "1" ]; then
            say "$INI_NAME kept (--keep-config)"
        else
            rm -f "$bin/$INI_NAME"
            remove_backups "$bin/$INI_NAME"
            say "$INI_NAME removed"
            did=1
        fi
    fi
    for e in "$LOG_NAME" "$MANIFEST_NAME"; do
        if [ -f "$bin/$e" ]; then
            rm -f "$bin/$e"
            did=1
        fi
    done
    rm -f "$bin"/*.fc3pp-new 2>/dev/null

    if [ "$did" = "1" ]; then
        say "$name: the patch was removed"
    else
        say "nothing to remove"
    fi
}

# ----------------------------------------------------------------------------- main

if [ "$UNINSTALL" = "1" ]; then
    printf '%s uninstaller\n' "$PATCH_NAME"
else
    printf '%s installer\n' "$PATCH_NAME"
    for f in "$DLL_NAME" "$INI_NAME"; do
        [ -f "$SOURCE_DIR/$f" ] || die "$f was not found next to this script. Download the release package (not the source code), or build it first."
    done
fi

if [ -n "$GAME_DIR" ]; then
    GAME_DIR=${GAME_DIR%/}
    [ "$(basename "$GAME_DIR")" = "bin" ] && GAME_DIR=$(dirname "$GAME_DIR")
fi

found=0
while IFS='|' read -r id name appid profile_folder exes; do
    [ -n "$id" ] || continue
    if [ "$GAME_FILTER" != "auto" ] && [ "$GAME_FILTER" != "$id" ]; then
        continue
    fi
    if [ -n "$GAME_DIR" ]; then
        has_game_exe "$GAME_DIR" "$exes" || continue
        root=$GAME_DIR
    else
        root=$(find_game_root "$name" "$exes")
        [ -n "$root" ] || continue
    fi
    found=$((found + 1))
    if [ "$UNINSTALL" = "1" ]; then
        uninstall_game "$name" "$appid" "$profile_folder" "$exes" "$root" </dev/null
    else
        install_game "$name" "$appid" "$profile_folder" "$exes" "$root" </dev/null
    fi
done <<EOF
$GAMES
EOF

if [ "$found" = "0" ]; then
    if [ -n "$GAME_DIR" ]; then
        die "no supported game executable was found under '$GAME_DIR/bin'"
    fi
    die "the game was not found in any Steam library. Pass --game-dir \"<folder that contains bin>\"."
fi

printf '\nFinished.\n'
if [ "$UNINSTALL" != "1" ]; then
    printf 'The patch writes its log to <game>/bin/%s. To remove everything again, run uninstall.sh.\n' "$LOG_NAME"
fi
exit 0
