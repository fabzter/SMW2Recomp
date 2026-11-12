#!/usr/bin/env bash
# Debug Play runs with the host's working directory anchored to build-debug/,
# not the repository root. Seed the build tree once from the installed app;
# existing local config, keybinds, ROM path, and saves are never overwritten.
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$repo_root/build-debug"
binary="$build_dir/SuperMarioWorld2YoshisIslandSNESRecomp"
user_app_dir="$HOME/Applications/Yoshi's Island Recomp.app/Contents/MacOS"

if [[ ! -x "$binary" ]]; then
    printf 'Debug Play: missing debug binary; run scripts/macos/build-debug.sh first\n' >&2
    exit 1
fi

copy_file_if_absent() {
    local source="$1" destination="$2"
    if [[ ! -e "$destination" && ! -L "$destination" && -f "$source" ]]; then
        cp -p "$source" "$destination"
    fi
}

copy_file_if_absent "$user_app_dir/config.ini" "$build_dir/config.ini"
copy_file_if_absent "$user_app_dir/keybinds.ini" "$build_dir/keybinds.ini"
copy_file_if_absent "$user_app_dir/rom.cfg" "$build_dir/rom.cfg"

if [[ ! -e "$build_dir/saves" && ! -L "$build_dir/saves" && -d "$user_app_dir/saves" ]]; then
    cp -R "$user_app_dir/saves" "$build_dir/saves"
fi

export SNESRECOMP_ARTIFACT_DIR="$repo_root/artifacts"
export SNESRECOMP_INPUT_RECORD=1
export SNESRECOMP_WS_BG_MARGINS_DEBUG=1
exec "$binary"
