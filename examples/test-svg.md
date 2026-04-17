# SVG Test

Utterance renders SVG shapes as rasterized textures while routing `<text>`
elements through its native SDF pipeline, so labels stay vector-crisp at
any zoom. Below is the Cincinnati–Columbus fraud investigation network —
shapes on one plane, text in native glyphs on top of them.

![Network graph](/home/Iz/fraud/visual/network.svg)

And a timeline view, for the same corpus:

![Timeline](/home/Iz/fraud/visual/timeline.svg)

Scroll, zoom, blink-teleport. Text in the diagram should stay sharp.
