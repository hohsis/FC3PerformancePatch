#!/usr/bin/env bash
# FC3PerformancePatch - uninstaller for Linux (Steam Proton, Steam Deck).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Removes the patch and restores every file it changed. Accepts the same
# --game-dir, --game, --prefix, --keep-config and --yes options as install.sh.
exec "$(cd "$(dirname "$0")" && pwd)/install.sh" --uninstall "$@"
