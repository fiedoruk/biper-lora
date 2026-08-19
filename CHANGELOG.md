# Changelog — the Biper layer

What this fork adds on top of MeshCore, release by release. Upstream's own
history lives in the [MeshCore repository](https://github.com/meshcore-dev/MeshCore).
Dates use ISO 8601. Nothing below claims device evidence it does not have —
see the README section "What is measured and what is not".

## Unreleased

Nothing yet.

## v0.7 — 2026-08-19 — honest wipe feedback, English internals

- **The wipe no longer flickers back to normal pages.** From the moment the
  ten-second countdown completes, the screen holds a dedicated full-screen
  animation — the fastest rings the cube can draw, carrying WYMAZUJE — until
  the factory reset reboots the device. Previously the erase ran
  asynchronously and the normal screen returned for a moment mid-wipe, which
  read as "it said wiping, then changed its mind" (owner-reported, 19 Aug).
- **A screen-task hang that never reached a cube.** The integer square root
  behind the radial rings (POMOC, the radial face — and now the wipe screen)
  could ping-pong between two values and spin forever on some pixels. The
  site's pixel-parity gate compiles the firmware's own drawing code and hung
  exactly where a cube would have; replaced with a bitwise square root that
  terminates by construction, ported 1:1 to the site's emulator.
- The layer's comments, log messages and identifiers are now English ahead of
  opening the repository (verified change-free: the code is identical modulo
  the rename map). The cube's on-screen vocabulary (SIEC, SAM, WYMAZ …) and
  the stored NVS keys stay Polish on purpose — one is the device's locked
  vocabulary, the other would silently reset the relay choice on already
  flashed cubes.
- The product version now lives in `biper/VERSION`, decoupled from the
  panel's own component version.
- CI re-checks the numbers the README claims (diffstat vs the MeshCore base
  tag, panel asset size) on every push.

## v0.6 — 2026-08-19 — first public build

Served by the web installer at [esp32ai.me/biper](https://esp32ai.me/biper/)
as a merged image built from commit `0e4503b` (SHA-256 published next to the
installer and in the site's release manifest).

- **Panel from the cube's flash**: help, shared channel, neighbours with
  private messages, settings; contact backup and restore kept in the phone's
  browser storage; Polish and English; light and dark.
- **Screen language**: six animated states on the 64 × 48 OLED; the resting
  field carries BIPER and its speed follows local mesh density.
- **One button, five gestures**: click cycles pages, double click is ninja
  mode (dark and silent — the radio keeps transmitting and the README says
  so), triple click toggles the relay mode, three seconds opens the hotspot,
  ten seconds wipes the cube with a visible countdown from the fourth second.
- **Relay switch that survives restarts**: SIEC (relays other people's
  traffic, the default) and SAM (transmits only yours), stored in NVS.
- **Hotspot custody**: a fresh eight-character Wi-Fi password for every
  window, shown on the cube's screen; a ten-minute window; origin guard on
  the WebSocket bridge; security headers on everything the cube serves.
- **Identity custody**: private-key export and import compiled out; the BLE
  pairing code is per-session and shown on the INFO screen.
- Measured on the bench: both environments compile (flash 71.1 %, RAM
  32.9 %), the radio entropy source is alive across five boots, the panel is
  served gzipped. Not measured on this build: a two-device pair, range,
  power draw.

## 2026-08-17 … 2026-08-18 — internal milestones, never released

In order: Wi-Fi hotspot spike on the stock companion; the screen; sound and
light; ninja mode; the RAM diet (34 → 115 kB free by trimming stock buffer
sizes); the captive portal; the panel–radio WebSocket bridge speaking the
companion protocol; the panel itself; hardening (session BLE code, Wi-Fi
password, single client). Each step was closed with a measurement on one
development cube and lives in this repository's history and gates.
