# Contributing to Biper

This is a product fork of [MeshCore](https://github.com/meshcore-dev/MeshCore).
Before you open anything here, one question decides where it belongs.

## Does it belong here or upstream?

**Upstream, at [MeshCore](https://github.com/meshcore-dev/MeshCore):** the mesh,
routing, the companion protocol, board support, anything this fork does not
touch. Please do not send those here — a fix landed upstream reaches everybody,
and a fix landed here reaches one fork.

**Here:** the Biper layer — `src/helpers/biper/`, the web panel in `biper/app/`,
the `variants/biper_ap/` environments, the fonts, and the four marked
`BIPER_AP hook` blocks in `examples/companion_radio/main.cpp`. That is the whole
of what this repository adds; see the README for the measured diffstat.

## Ground rules

**Keep the upstream footprint small.** Upstream edits stay minimal and marked; see the README section “How this fork touches upstream”, in
four blocks that announce themselves. A change that spreads into more upstream
files needs a reason worth stating in the pull request, because every one of
them is a conflict at the next rebase.

**Say what you measured.** This project marks evidence, and the marks are load
bearing: a host gate passing is a statement about a host, never about a device.
If you did not put it on hardware, write that. "Should work" is not a status.

**Regenerate the assets.** `biper/app/gen-asset.py` after touching the panel,
`biper/app/gen-fonts.py` after touching the typography. CI compares both
generated headers against their sources and will fail the build otherwise.

**Both environments must build:** `pio run -e Biper_AP_C6L_spike -e
Biper_AP_C6L_wifi_only`. CI runs exactly that.

**Screen text is size-checked at compile time.** The OLED line holds ten
characters and a literal wider than that fails to compile, on purpose — a
screen that silently clips a word is worse than one that says nothing.

## Licences

Our code is MIT. The embedded font data is OFL-1.1 and cannot be relicensed —
see `fonts/README.md` before touching `BiperFontAsset.h`. Third-party libraries
keep their own terms.

## What this project will not take

Anything that makes the device quietly do something other than what its screen
says. That is the one line the whole fork is built around.
