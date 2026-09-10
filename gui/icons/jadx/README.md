# jadx node icons

These SVG files are copied without modification from
https://github.com/skylot/jadx/tree/bde91ffe977e65d79153188f4d3a7ef8820c605a/jadx-gui/src/main/resources/icons/nodes
(commit `bde91ffe977e65d79153188f4d3a7ef8820c605a`). Original copyright headers
are preserved. See the upstream Apache-2.0 LICENSE and upstream NOTICE in this
directory. NOTICE also describes other upstream components, which are not all
included here. Icons carrying JetBrains headers are copyright 2000–2021
JetBrains s.r.o.; see each SVG for its attribution.

`png/` contains rasterizations of these SVGs, generated with librsvg at 16, 32,
48 and 64 pixels. Run `python3 scripts/render_jadx_icons.py` from the repository
with `rsvg-convert` installed to regenerate them and the Qt resource manifest.
This preserves SVG masks and avoids a runtime SVG dependency on any platform.
The source SVGs and generated PNGs are distributed under the same license.

`gui/nodeicons.cpp` mirrors jadx's JClass, JField, MethodRenderHelper and
OverlayIcon selection/overlay rules. Class visibility is shown only for concrete
classes. Method visibility overrides constructor/abstract variants, and
synchronized methods use methodReference. Final and static overlays apply to
fields and methods only, in that order. Annotation types use the original green
@ icon; ordinary classes use C. Package nodes use the original gray package.

Toolbar assets prefixed `tool` come from the same commit’s `icons/ui` directory.
The SVG contents and copyright headers are unchanged.

Resource assets prefixed `res` are unchanged icons from the same upstream `icons/nodes` directory, selected by extension in `NodeIcons::resource`.

Navigation additions: `toolmainActivity` = `ui/home.svg`, `toolsync` = `ui/locate.svg`,
`toolpackages` = `ui/moduleGroup.svg`, from the same local jadx icon set.
