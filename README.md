# utterance

`cat` prints. `less` scrolls. `ut` puts you inside it.

Pipe any text into a 3D space and walk through it. Markdown rendered with color. ANSI codes painted. Code blocks boxed. Images loaded. Links clickable. Keyboard cursor. **~730KB**. Links `libc` and `libm`. Nothing else.

```
echo 'hello world' | ut
man bash | ut
dmesg | ut
cat README.md | ut
ut -f /path/to/font.ttf somefile.c
ut -m raw_markdown.txt
```

Falls back to `cat` when there's no display. Invisible unless you have a render surface.

## What it does

Two-phase text layout: **prepare** (segment UTF-8 + cache glyph widths, once) then **layout** (greedy line-break on cached widths, pure arithmetic, re-runs for free). Dynamic reflow as you move through the text.

- **SDF font rendering** — Full BMP atlas (codepoints 32–65535), `fwidth()` antialiasing, crisp at any distance
- **ANSI color** — 8-color, 256-color, 24-bit RGB. Per-vertex color in the shader. `gcc 2>&1 | ut` just works
- **Markdown** — Headers, bold, italic, code spans, fenced code blocks with background boxes, links, blockquotes, lists, tables, horizontal rules. Auto-detected from `.md` extension or content
- **Images** — PNG/JPEG/GIF/BMP via stb_image, rendered as textured quads in the text flow. Remote URLs fetched with curl
- **Ctrl+click links** — Opens URLs in your browser
- **Text cursor** — Arrow keys, Page Up/Down, Home/End, Ctrl+Home/End. Blinking caret. Shift+arrow selection
- **CJK breaking** — Per-character line breaks for Chinese, Japanese, Korean, fullwidth forms
- **Soft-hyphen** — U+00AD breaks with visible hyphen at discretionary points
- **Streaming** — Non-blocking source reads. `tail -f | ut` works
- **Dynamic reflow** — Text re-wraps based on camera distance. Zoom in, columns narrow. Zoom out, they widen
- **Blink teleport** — Click to fly. Four post-processing effects. Bounce-back on miss

## Controls

| Key | Action |
|-----|--------|
| **W/A/S/D** | Move |
| **Space / Alt** | Up / Down |
| **Arrow keys** | Text cursor navigation |
| **Shift+Arrow** | Extend selection |
| **Page Up/Down** | Scroll by page |
| **Home/End** | Start/end of line (Ctrl: document) |
| **Ctrl+C** | Copy selection |
| **Ctrl+click** | Open link in browser |
| **Shift** | 5× speed |
| **+/-** | Speed multiplier |
| **Scroll wheel** | Vertical scroll |
| **Right-click** | Mouselook |
| **Left-click** | Blink teleport / deselect |
| **Click + drag** | Select text |
| **F** | Blink to crosshair |
| **\\** | Cycle blink effects |
| **Q / Esc** | Quit |

## Building

```
make
```

## Dependencies

- GLFW3 (vendored, static)
- stb_truetype.h (vendored)
- stb_image.h (vendored)
- OpenGL 3.3 core profile
- curl (runtime, for remote images only)

Runtime: `libc` + `libm`. Runs on a Raspberry Pi 3.

## Architecture

```
Source (file, pipe, streaming fd)
  → Segment (UTF-8 → words, spaces, breaks, ANSI, CJK, soft-hyphens)
  → Measure (sum glyph advances per segment, cached)
  → Line-break (greedy on cached widths, pure arithmetic)
  → Mesh (segments → GlyphInstances with per-vertex color)
  → Render (SDF shader + image quads + highlight quads)
```

Prepare runs once. Layout re-runs on wrap-width change at negligible cost. Streaming appends incrementally. The source buffer is never modified — `cat` fallback dumps raw bytes.

## Acknowledgments

The two-phase prepare/layout architecture is based on ideas from Cheng Lou's [pretext](https://github.com/chenglou/pretext) — a TypeScript library that proves text layout can be pure arithmetic over cached measurements. The segment-based analysis, greedy line-breaking on cached widths, and the separation of measurement from layout all trace back to that work. The research depth on CJK, Arabic, Thai, and emoji in pretext is remarkable and informed the internationalization approach here.

## License

All rights reserved.
