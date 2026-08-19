# Fonts embedded in the firmware

These six subsetted `.woff2` files are compiled into `src/helpers/biper/BiperFontAsset.h`
by `biper/app/gen-fonts.py` and served by the device itself at `/f/*.woff2` when the
panel is open. The firmware therefore **redistributes** them, which is why the licence
and the copyright notices below have to travel with the code.

All three families are licensed under the **SIL Open Font License 1.1** — full text in
[`OFL-1.1.txt`](OFL-1.1.txt).

- **Atkinson Hyperlegible Next** — Copyright 2020-2024 The Atkinson Hyperlegible Next Project Authors
- **Atkinson Hyperlegible Mono** — Copyright 2020-2024 The Atkinson Hyperlegible Mono Project Authors
- **Archivo Black** — Copyright 2017 The Archivo Black Project Authors

OFL 1.1 is explicit that the fonts and any derivatives cannot be released under a
different licence. The MIT licence of this repository covers our own code; it does not
cover these files, and it does not cover the font data inside the generated header.

To regenerate the header after changing a subset:

    python3 biper/app/gen-fonts.py            # reads ./fonts by default
    python3 biper/app/gen-fonts.py /some/dir  # or an explicit directory
