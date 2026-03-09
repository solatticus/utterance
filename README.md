# utterance

`cat` prints. `less` scrolls. `ut` puts you inside it.

Pipe any text into a 3D space and walk through it. SDF font rendering, FPS camera, blink teleport with post-processing, text selection with copy. **625KB**. Links `libc` and `libm`. Nothing else. Runs on a Raspberry Pi 3, a 2011 ThinkPad, whatever you forgot in that drawer. Falls back to `cat` when there's no display.

```
echo 'hello world' | ut
man bash | ut
dmesg | ut
journalctl -b | ut
cat war_and_peace.txt | ut
ut -f /path/to/font.ttf somefile.c
```

## What it does

SDF font rendering. Full BMP atlas (codepoints 32–65535). OpenGL 3.3 core. FPS camera. You're inside the text. You fly through it. Select it. Copy it. That's the whole thing, and it's more than most text editors manage.

## Controls

| Key | Action |
|-----|--------|
| **W/A/S/D** | Move (yes, like a real application) |
| **Space / Alt** | Up / Down |
| **Shift** | 5× speed, for when you have places to be |
| **+/-** | Speed multiplier (stacks) |
| **Scroll wheel** | Scroll vertically |
| **Right-click** | Mouselook |
| **Left-click** | Blink — raycast teleport to where you click |
| **Click + drag** | Select text |
| **Ctrl+C** | Copy selection to clipboard |
| **F** | Blink — raycast teleport to crosshair |
| **\\** | Cycle blink effects (vignette → tunnel → chromatic → scanline → off) |
| **Q / Esc** | Quit |

## Blink

Click on text or press F. You fly there. The camera auto-aims toward the hit point and lands at a comfortable reading distance (~25 lines visible). If there's text at the destination, you stop and the word glows briefly. If there isn't, you bounce back — because even the teleport has taste.

Four post-processing effects, toggled with `\`:
- **Vignette** — the screen literally blinks. Snap shut, hold black, smooth open. The metaphor writes itself.
- **Tunnel** — radial speed lines. Demoscene called, they want royalties.
- **Chromatic aberration** — RGB splits at the edges. Space had a bad day.
- **Scanline wipe** — CRT resync sweep with phosphor tint. For the nostalgic and the correct.

## Selection

Click and drag to select text. Blue highlight behind the selected glyphs, one rect per line. Ctrl+C copies the selection (including whitespace) to your system clipboard. Click anywhere to clear.

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
- Camera unproject for mouse-to-world raycasting (blink targeting, text selection)
- UTF-8 decoding, perspective + orthographic rendering, zero runtime allocations in the render loop

625K with four post-processing effects, an FBO pipeline, lazy glyph atlas, animated blink, word highlight, and text selection. Your `node_modules` is crying.

## Philosophy

`cat` prints. `less` scrolls. `ut` puts you in it.

The approach here — C11, no frameworks, hand-rolled everything, minimal deps, runs anywhere — is the foundation for how we build terminal software in the collective. oscar.exe (the AI terminal client) follows the same principles: direct fd access, hand-built ANSI, dirty rectangle rendering, UTF-8 by hand. Utterance proved you don't need libraries to put text on a screen. You need a file descriptor and an opinion.

## License

All rights reserved.
