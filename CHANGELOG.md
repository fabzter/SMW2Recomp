# Changelog

## 0.1.1 — 2026-11-05

World 1-7 (Touch Fuzzy, Get Dizzy) is drawn right. Its stage uses BG mode 2,
where a table in BG3 sets the scroll of each column of the stage layer, and
the renderer read that table one row off: the stage was put together from the
wrong rows of its map. Once that was fixed, the stage shook while Yoshi walked,
one frame forward and one back: the game uploads the next frame's table at the
bottom of the screen, and the renderer drew the whole frame with it. Video
memory, palettes and sprites written while the picture is drawn now reach only
the lines below the write, as on the console. Saves and save states from 0.1.0
keep working.

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
