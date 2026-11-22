# nx-hbmenu for Switch 2

This is upstream nx-hbmenu for Switch 2, built with the Switch 2 devkit.
Rendering is switched from CPU to GPU, using Vulkan.

Programs not built with the Switch 2 devkit show a warning before they start.

## Improvements over upstream

- **Vectorized icons.** The built-in icons are SVG (`vectors/`), rasterized
  at the target resolution instead of scaled from fixed-size bitmaps, so they
  stay sharp at every size.
- **Native resolution, up to 4K at 60 fps.** The menu renders at the
  console's output resolution and refresh rate: 1080p handheld, and the TV's
  resolution docked, up to **3840 × 2160 at 60 fps**.

## Build

From the repository root, with the Switch 2 devkit installed in devkitPro (see
the devkit's `README.md`):

```sh
make -f switch2/Makefile
```

or with Docker (or Podman), in the devkit's image (`ounce-devkit`, see the devkit's
`README.md`):

```sh
docker build --output out .
```

The Makefile packs `romfs/assets.zip` (Python 3), compiles the shaders and
links `build/nro/nro/hbmenu.nro`; `docker build` writes it to
`out/hbmenu.nro`. `--build-arg DEVKIT_IMAGE=...` selects another devkit image.
`make -f switch2/Makefile DEFINES=-DHBMENU_PERF_OVERLAY` draws frame timings
on the screen. The NRO's icon is `switch2/icon.jpg`;
`make -f switch2/Makefile icon` makes it again from
`switch2/vectors/app_icon.svg`, with ImageMagick.

## Install

Copy `hbmenu.nro` to the root of the SD card. The Switch 2 loader starts
`sdmc:/hbmenu.nro` in the game process and returns to it after each
program.

## License

The menu is upstream's, under its ISC license (`LICENSE.md`).
`third_party/nanosvg` is under its zlib license.
