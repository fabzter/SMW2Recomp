# Changelog

## 0.1.0 — 2026-11-03

First release: a setup pack that recompiles the game from your own ROM. Boots
through the Nintendo logo, the opening story and the title screen into the file
select and the levels, at full speed with the Super FX graphics drawn; the
in-game save is kept in `build\saves\save.srm`. A fresh install starts with no
shader (the plain picture); scanline, CRT and upscaler presets (ScaleFX,
NNEDI3) are in Settings → Shader. Settings during play (Ctrl+L or Select+L3);
audio interpolation (I key: Gaussian, Cubic, Sinc); update check on start that
backs up your saves before installing; asks for the dedicated graphics card on
laptops with two; console-free Windows build.

Renderer fixes: Yoshi's sprite is drawn whole while he moves (the renderer had
mixed two of his animation frames, showing extra heads and stray body parts),
and mosaic effects are drawn.
