# utterance

Pipe text in, fly through it in 3D. SDF font rendering with a free-look camera.

```
echo 'hello world' | utterance
utterance somefile.txt
utterance -f /path/to/font.ttf mycode.c
cat novel.txt | utterance
```

If no display is available, falls back to `cat`.

## Controls

| Key | Action |
|-----|--------|
| W/A/S/D | Move |
| Space / Ctrl | Up / Down |
| Shift | 5x speed |
| Click | Capture mouse (free-look) |
| Escape | Release mouse |
| Q | Quit |

## Building

Requires GLFW3 (built from source in `third_party/`).

```
make
```

## How it works

- Loads a TTF font via [stb_truetype](https://github.com/nothings/stb) and generates an SDF atlas
- ASCII glyphs (32-127) are pre-rendered at startup; everything else is lazily atlased on first use
- Text is laid out into a static VBO of textured quads, rendered with an SDF fragment shader
- Camera uses a perspective projection — text lives on the Z=0 plane, you fly around it

## Dependencies

- GLFW3 (vendored in `third_party/`)
- stb_truetype.h (vendored in `third_party/`)
- OpenGL 3.3 core profile
- X11 / Wayland
