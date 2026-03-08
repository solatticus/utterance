# utterance

Your text viewer has two dimensions. That's cute.

Pipe anything in. Walk through it. **598KB**, links `libc` and `libm`. That's it. No Electron. No "frameworks." No npm install that downloads a small country. Just text, in space, and you're inside it.

```
echo 'hello world' | utterance
dmesg | ut
cat war_and_peace.txt | ut
ut -f /path/to/your/fancy/font.ttf somefile.c
```

Falls back to `cat` when there's no display. Because unlike your app, this one actually works everywhere.

## What it does

SDF font rendering. Full BMP atlas (codepoints 32–65535). OpenGL 3.3 core. FPS camera. You're inside the text. You fly through it. That's the whole thing, and it's more than most text editors manage.

## Controls

| Key | Action |
|-----|--------|
| **W/A/S/D** | Move (yes, like a real application) |
| **Space / Ctrl** | Up / Down |
| **Shift** | 5× speed, for when you have places to be |
| **+/-** | Speed multiplier (stacks) |
| **Right-click** | Mouselook |
| **F / Left-click** | Blink — raycast teleport to where you're looking |
| **\\** | Cycle blink effects (vignette → tunnel → chromatic → scanline → off) |
| **Q / Esc** | Quit |

## Blink

Aim the crosshair at text. Press F. You fly there. If there's text at the destination, you stop. If there isn't, you bounce back — because even the teleport has taste.

Four post-processing effects, toggled with `\`:
- **Vignette** — the screen literally blinks. Snap shut, hold black, smooth open. The metaphor writes itself.
- **Tunnel** — radial speed lines. Demoscene called, they want royalties.
- **Chromatic aberration** — RGB splits at the edges. Space had a bad day.
- **Scanline wipe** — CRT resync sweep with phosphor tint. For the nostalgic and the correct.

## Building

```
make
```

That's the whole build system. One `Makefile`. If your project needs more than that, it's not a project, it's a lifestyle.

## Dependencies

- GLFW3 (vendored, static)
- stb_truetype.h (vendored)
- OpenGL 3.3 core profile
- A C compiler that isn't embarrassed to exist

Runtime: `libc` + `libm`. Runs on a Raspberry Pi 3. Runs on your 2011 ThinkPad. Runs on that GPU you forgot you had. Your Electron app could never.

## Technical

- C11, hand-rolled GL loader (~30 functions, no GLEW, no GLAD, no committee)
- SDF rendering with `fwidth()` antialiasing — crisp at any distance, any angle
- 4096×4096 SDF atlas, lazy glyph rendering (only atlas what you see)
- FBO post-processing pipeline for blink effects (one uber-shader, zero overhead when idle)
- UTF-8 decoding, perspective + orthographic rendering, zero runtime allocations in the render loop

589K → 598K after adding four post-processing effects and an FBO pipeline. Your `node_modules` is crying.

## Philosophy

`cat` prints. `less` scrolls. `ut` puts you in it.

## License

All rights reserved.
