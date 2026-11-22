#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Writes the romfs's assets.zip, as upstream's Makefile makes it
(zip -rj assets), with switch2/vectors/<name>.svg in place of each
assets/<name>.bin that it replaces (switch2/vector_assets.c).

  pack_romfs.py ROMFS/assets.zip    (run from the repository root)
"""
import pathlib
import sys
import zipfile


def main() -> None:
    output = pathlib.Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    vectors = pathlib.Path("switch2/vectors")
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(pathlib.Path("assets").iterdir()):
            if not path.is_file():
                continue
            vector = vectors / (path.stem + ".svg")
            if path.suffix == ".bin" and vector.is_file():
                archive.write(vector, vector.name)
            else:
                archive.write(path, path.name)


if __name__ == "__main__":
    main()
