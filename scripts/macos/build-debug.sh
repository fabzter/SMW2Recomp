#!/usr/bin/env bash
set -euo pipefail
# This builder assumes the integrated local/macos baseline. Fresh upstream
# checkouts must run build-macos.sh first; committed local history may have
# superseded the bootstrap patch stack. Only that all-inapplicable case warns
# and continues below; partial stacks remain fatal.

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/../.." && pwd)"
cd "$repo_root"

die() {
    printf 'build-debug: ERROR: %s\n' "$*" >&2
    exit 1
}

[[ "$(uname -s)" == Darwin ]] || die "this script builds the native macOS target only"
for tool in git cmake ninja python3; do
    command -v "$tool" >/dev/null 2>&1 || die "required command not found: $tool"
done

# The ROM path can be supplied as --rom PATH or via SNESRECOMP_ROM. If both
# are present, the explicit argument wins, as it does in tools/regen.sh.
# ${arr[@]+"${arr[@]}"} is the bash-3.2-safe expansion for a maybe-empty array
# under set -u.
regen_args=()
if (($#)); then
    [[ $# -eq 2 && "$1" == "--rom" ]] || die "usage: $0 [--rom /path/to/Yoshi's Island (USA).sfc]"
    regen_args=(--rom "$2")
fi

echo "[1/4] Initializing pinned submodules"
git submodule update --init --recursive

# verify_stack_applied <module> <patch>...
# Patches stack on shared files, so an interior patch's context lines may no
# longer match what apply --reverse expects. Peeling in reverse apply order
# always works: the topmost patch reverses cleanly, then the next. Detect it
# on a scratch copy of the tree (no .git needed -- git apply is file-only).
verify_stack_applied() {
    local module="$1"; shift
    local scratch
    scratch="$(mktemp -d)" || return 1
    cp -a "$module/." "$scratch/" 2>/dev/null || { rm -rf "$scratch"; return 1; }
    rm -f "$scratch/.git"
    local pending=("$@") next=() p found rc
    (   cd "$scratch" || exit 1
        while ((${#pending[@]})); do
            found=
            for p in "${pending[@]}"; do
                if git apply --reverse --check "$p" 2>/dev/null; then
                    git apply --reverse "$p" 2>/dev/null || exit 1
                    found="$p"
                    break
                fi
            done
            [[ -n "$found" ]] || exit 1
            next=()
            for p in "${pending[@]}"; do
                [[ "$p" == "$found" ]] || next+=("$p")
            done
            if ((${#next[@]})); then pending=("${next[@]}"); else pending=(); fi
        done
    )
    rc=$?
    rm -rf "$scratch"
    return "$rc"
}

# ensure_patches <module> <patch>...  -- apply a stack, in listed order.
ensure_patches() {
    local module="$1"; shift
    local pending=() p
    for p in "$@"; do
        [[ -f "$p" ]] || die "required patch is missing: ${p#"$repo_root"/}"
        if git -C "$module" apply --check "$p" 2>/dev/null; then
            pending+=("$p")
        fi
    done
    if ((${#pending[@]} == 0)); then
        # Nothing applies forward: either the full stack is already applied
        # or committed local/macos history has superseded every patch.
        if verify_stack_applied "$module" "$@"; then
            echo "  $module: all $# patches already applied"
            return 0
        fi
        echo "  $module: patch stack superseded by committed local/macos history; continuing"
        return 0
    fi
    if ((${#pending[@]} != $#)); then
        die "$module: only ${#pending[@]}/$# patches apply forward -- the submodule is in an inconsistent state; inspect it and refresh patches/"
    fi
    for p in "${pending[@]}"; do
        git -C "$module" apply "$p" || die "failed to apply ${p#"$repo_root"/}"
        echo "  applied ${p#"$repo_root"/}"
    done
}

echo "[2/4] Applying framework patches"
ensure_patches snesrecomp \
    "$repo_root/patches/snesrecomp-audio-interpolation.patch" \
    "$repo_root/patches/snesrecomp-glsl-passprev-and-vao.patch" \
    "$repo_root/patches/snesrecomp-mosaic-frame-model.patch" \
    "$repo_root/patches/snesrecomp-nmi-idle-loop-wai.patch" \
    "$repo_root/patches/snesrecomp-nmi-wai-in-handler.patch" \
    "$repo_root/patches/snesrecomp-raster-oam-and-hdma.patch" \
    "$repo_root/patches/snesrecomp-raster-vram-journal.patch" \
    "$repo_root/patches/snesrecomp-resume-state-restart.patch" \
    "$repo_root/patches/snesrecomp-shader-forces-opengl.patch"
ensure_patches recomp-ui \
    "$repo_root/patches/recomp-ui-host-toggle.patch"

echo "[3/4] Regenerating game sources"
bash tools/regen.sh ${regen_args[@]+"${regen_args[@]}"}

echo "[4/4] Configuring and building arm64 macOS debug-capture binary"
# Keep the release optimization and LTO settings identical to build-macos.
# This build differs by enabling the optional video recorder; it is not -O0.
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=TRUE \
    -DSNESRECOMP_SDL3_FETCH=ON \
    -DCMAKE_DISABLE_FIND_PACKAGE_Freetype=TRUE \
    -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DSNESRECOMP_INTERP_READ_IMPL=FUSED \
    -DSNESRECOMP_SUPERFX_IDLE_IMPL=NATIVE \
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -mcpu=native" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -mcpu=native" \
    -DSNESRECOMP_ENABLE_VIDEO_CAPTURE=ON
cmake --build build-debug --parallel

echo "Build complete: $repo_root/build-debug/SuperMarioWorld2YoshisIslandSNESRecomp"
