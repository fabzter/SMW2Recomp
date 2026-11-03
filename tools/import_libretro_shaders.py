#!/usr/bin/env python3
"""Import a curated set of libretro GLSL presets into assets/shaders.

The launcher lists every .glsl/.glslp under assets/shaders except paths that
start with '_' or '.', so the libretro files go to assets/shaders/_libretro/
(only what each preset references) and a renamed copy of each preset, with
its paths rewritten, goes to assets/shaders/<Category>/<Name>.glslp.

Each preset below was loaded by this project's GLSL presenter and checked on
screen. ScaleFX, ScaleFX+rAA and xBRZ multipass compile but draw black here,
and CRT-Pi, CRT-Hyllian and CRT-Royale do not load, so they are left out.

Usage: python tools/import_libretro_shaders.py <libretro/glsl-shaders checkout>
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from pathlib import Path

PRESETS = [
    ("Scanlines", "Classic", "scanlines/scanline.glslp"),
    ("Scanlines", "Soft (sine)", "scanlines/scanlines-sine-abs.glslp"),
    ("Scanlines", "GDV mini", "scanlines/gdv-mini-scanlines.glslp"),
    ("Scanlines", "Resolution independent", "scanlines/res-independent-scanlines.glslp"),
    ("Scanlines", "Fract", "scanlines/scanline-fract.glslp"),
    ("Scanlines", "OSSC", "scanlines/ossc.glslp"),
    ("CRT", "zfast", "crt/zfast-crt.glslp"),
    ("CRT", "Easymode", "crt/crt-easymode.glslp"),
    ("CRT", "Lottes", "crt/crt-lottes.glslp"),
    ("CRT", "Geom (curved)", "crt/crt-geom.glslp"),
    ("CRT", "Aperture", "crt/crt-aperture.glslp"),
    ("Upscaler", "xBRZ", "xbrz/xbrz-freescale.glslp"),
    ("Upscaler", "Super-xBR", "xbr/super-xbr-3p-smoother.glslp"),
    ("Upscaler", "OmniScale", "omniscale/omniscale.glslp"),
    ("Upscaler", "HQ3x", "hqx/hq3x.glslp"),
]

KEY_VALUE = re.compile(r"^(\s*)([A-Za-z0-9_]+)(\s*=\s*)(.*?)\s*$")


def unquote(value: str) -> str:
    return value[1:-1] if len(value) >= 2 and value[0] == value[-1] == '"' else value


def import_preset(src_root: Path, out_root: Path, category: str, name: str, rel: str) -> None:
    preset = src_root / rel
    lines = preset.read_text(encoding="utf-8").splitlines()
    textures: set[str] = set()
    for line in lines:
        m = KEY_VALUE.match(line)
        if m and m.group(2) == "textures":
            textures = {t for t in unquote(m.group(4)).split(";") if t}

    dest_dir = out_root / category
    dest_dir.mkdir(parents=True, exist_ok=True)
    out_lines = []
    for line in lines:
        m = KEY_VALUE.match(line)
        key = m.group(2) if m else ""
        if m and (re.fullmatch(r"shader\d+", key) or key in textures):
            source = (preset.parent / unquote(m.group(4))).resolve()
            target = out_root / "_libretro" / source.relative_to(src_root.resolve())
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
            new_path = Path(os.path.relpath(target, dest_dir)).as_posix()
            line = f'{m.group(1)}{key}{m.group(3)}"{new_path}"'
        out_lines.append(line)
    header = f"# {category}: {name} -- libretro/glsl-shaders {rel}\n"
    (dest_dir / f"{name}.glslp").write_text(header + "\n".join(out_lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("glsl_shaders", type=Path, help="libretro/glsl-shaders checkout")
    parser.add_argument("--out", type=Path,
                        default=Path(__file__).resolve().parent.parent / "assets" / "shaders")
    args = parser.parse_args()
    for category in {c for c, _, _ in PRESETS} | {"_libretro"}:
        shutil.rmtree(args.out / category, ignore_errors=True)
    for category, name, rel in PRESETS:
        import_preset(args.glsl_shaders, args.out, category, name, rel)
    (args.out / "_libretro" / "README.md").write_text(
        "Files copied unmodified from https://github.com/libretro/glsl-shaders by\n"
        "tools/import_libretro_shaders.py. Each shader keeps its author's license\n"
        "in its header comment.\n", encoding="utf-8")
    print(f"imported {len(PRESETS)} presets into {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
