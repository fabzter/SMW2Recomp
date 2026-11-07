#!/usr/bin/env bash
# Package the macOS build into a double-clickable .app bundle.
#
# Usage:  scripts/macos/package-app.sh [BUILD_DIR] [OUT_DIR]
#   BUILD_DIR  cmake build tree containing the executable (default: build-macos)
#   OUT_DIR    where the .app is written (default: dist)
#
# Layout produced (see scripts/macos/MACOS.md):
#   Yoshi's Island Recomp.app/Contents/MacOS/    executable + staged sidecars
#   Yoshi's Island Recomp.app/Contents/Resources/assets -> ../MacOS/assets
#   Yoshi's Island Recomp.app/Contents/Info.plist
#
# Everything the exe resolves "next to itself" (assets/, mods/, staged shaders)
# goes in Contents/MacOS. SDL_GetBasePath() points at Contents/Resources inside
# a bundle, so the launcher UI root is symlinked there — no payload copies.
# User state (rom.cfg, *.ini, saves/, save states, logs, the ROM itself) is
# NEVER bundled: the app starts clean and writes its own beside the exe.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-build-macos}"
OUT_DIR="${2:-dist}"
EXE="SuperMarioWorld2YoshisIslandSNESRecomp"
APP_NAME="Yoshi's Island Recomp"
BUNDLE_ID="com.fabzter.smw2recomp"
MIN_OS="12.0"

cd "$ROOT"
SRC="$BUILD_DIR"
[ -x "$SRC/$EXE" ] || { echo "package-app.sh: $SRC/$EXE not found — run scripts/macos/build-macos.sh first" >&2; exit 1; }

VERSION="$(tr -d '[:space:]' < VERSION 2>/dev/null || echo dev)"

APP="$OUT_DIR/$APP_NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

# Payload: the exe plus every sidecar it resolves next to itself, excluding
# per-machine/user state that must never ship inside a bundle.
( cd "$SRC" && find . -mindepth 1 -maxdepth 1 \
    ! -name 'rom.cfg' ! -name 'keybinds.ini' ! -name 'config.ini' \
    ! -name 'input.ini' ! -name 'update.ini' ! -name 'audio_interpolation.txt' \
    ! -name 'saves' ! -name 'save-backups' ! -name 'states' \
    ! -name '*.log' ! -name '*.sav' ! -name '*.srm' \
    ! -name '*.sfc' ! -name '*.smc' ! -name 'rom_probe.json' \
    ! -name 'CMakeFiles' ! -name 'cmake_install.cmake' ! -name 'CMakeCache.txt' \
    ! -name 'build.ninja' ! -name 'rules.ninja' ! -name '.ninja_*' \
    ! -name '*.o' ! -name '*.a' ! -name '*.d' \
    -exec ditto {} "$ROOT/$APP/Contents/MacOS/{}" \; )

# SDL_GetBasePath() returns Contents/Resources inside a bundle; the launcher
# resolves its art/fonts through it. Symlink rather than copy the payload.
for d in assets launcher_assets mods; do
  if [ -d "$APP/Contents/MacOS/$d" ]; then
    ln -sfn "../MacOS/$d" "$APP/Contents/Resources/$d"
  fi
done

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>         <string>$EXE</string>
  <key>CFBundleIdentifier</key>         <string>$BUNDLE_ID</string>
  <key>CFBundleName</key>               <string>$APP_NAME</string>
  <key>CFBundleDisplayName</key>        <string>$APP_NAME</string>
  <key>CFBundlePackageType</key>        <string>APPL</string>
  <key>CFBundleShortVersionString</key> <string>$VERSION</string>
  <key>CFBundleVersion</key>            <string>$VERSION</string>
  <key>LSMinimumSystemVersion</key>     <string>$MIN_OS</string>
  <key>NSHighResolutionCapable</key>    <true/>
</dict>
</plist>
EOF
plutil -lint "$APP/Contents/Info.plist" >/dev/null

# Ad-hoc sign the executable: enough for Apple Silicon to load it locally.
# Container sealing can fail on asset layouts codesign misreads — not fatal.
if ! codesign --force --sign - "$APP/Contents/MacOS/$EXE" 2>/dev/null; then
  echo "package-app.sh: ad-hoc codesign failed (binary may already carry a linker signature — usually fine)" >&2
fi
if ! codesign --force --deep --sign - "$APP" 2>/dev/null; then
  echo "package-app.sh: container sealing skipped (asset layout codesign can't seal — the signed executable still runs)" >&2
fi

echo "Built: $APP"
echo "Run:   open \"$APP\""
