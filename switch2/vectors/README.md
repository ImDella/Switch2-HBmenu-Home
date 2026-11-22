# Vector icons

hbmenu's built-in icons as SVG, under the nx-hbmenu ISC licence in each
file's header. Design sources: <https://github.com/jaames/nx-hbmenu-mockup>.

The Switch 2 build ships these instead of the `assets/*.bin` bitmaps with
the same names, and draws them at the screen's resolution
(`switch2/vector_assets.c`). `app_icon.svg` is the source of the NRO's 256x256 JPEG
icon, `switch2/icon.jpg` (regenerate it with `make -f switch2/Makefile
icon`, which needs ImageMagick), since the NRO format requires a JPEG. `button_a_*` and `button_b_*` replace the unused
`assets_unused/button_*` bitmaps and are not shipped: hbmenu draws button
glyphs from the system font.

Changed from the designer's files for nanosvg: the gradients in
`folder_icon.svg` and `app_icon.svg` give their endpoints as percentages
(`y2="100%"`). nanosvg reads a bare `y2="1"` as one user unit rather than
the whole shape, which drew the folder front in a flat colour.

nanosvg does not support `clipPath`, so these files must not use one:
`eth_none_icon.svg` draws the cable's gap under the slash as a flattened
path.
