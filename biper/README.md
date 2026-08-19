# biper/

The product half of the fork that is not C++.

| path | what it is |
|---|---|
| `app/` | the panel the cube serves from its own flash: `biper-app.html` is the single source, `gen-asset.py` gzips it into `src/helpers/biper/BiperAppAsset.h`, `gen-fonts.py` regenerates the font subsets. CI diffs the generated header against this source on every push. |
| `obrazy/` | screen renders used by the README — drawn by compiling the firmware's own drawing code against a stub display, not mock-ups |
| `WIKI-C6L.md` | measured hardware facts for the M5Stack Unit C6L, each with a MEASURED/REFERENCE/UNVERIFIED status |
| `release.sh` | builds the merged image (bootloader + partitions + app, offset 0x0) that a web installer flashes, and prints its SHA-256 |
| `releases/` | local output of `release.sh`; git-ignored on purpose — released files are published with their hashes, not committed |

After editing `app/biper-app.html`, run `python3 biper/app/gen-asset.py` — the
build does not regenerate the asset for you, and CI fails on a mismatch.
