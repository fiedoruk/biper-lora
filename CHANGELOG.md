# Changelog — the Biper layer

What this fork adds on top of MeshCore, release by release. Upstream's own
history lives in the [MeshCore repository](https://github.com/meshcore-dev/MeshCore).
Dates use ISO 8601. Nothing below claims device evidence it does not have —
see the README section "What is measured and what is not".

## Unreleased

Nothing yet.

## v0.8.7 — 2026-08-19 — the Wi-Fi word can be yours

- **SETTINGS now has a "cube's Wi-Fi" field.** Two to four plain letters
  after "Biper-" — `Biper-ELF` instead of the drawn word, applied from the
  next hotspot window so the current session is not dropped. An empty field
  returns to the drawn word; a wipe clears the choice too. The word is
  validated to A-Z on the cube before it can reach an SSID or the OLED.
  Panel component v0.8.

## v0.8.6 — 2026-08-19 — the contacts are called contacts

- **The panel tab WĘZŁY is now KONTAKTY (NODES → CONTACTS).** The owner —
  the person who knows this product best — could not find where to write to
  someone, because the neighbour list carried an engineer's word. A label
  the author cannot find has failed the only test it has. The built-in guide
  follows; internal identifiers stay unchanged, so nothing scripted against
  the panel breaks. Panel component v0.7.

## v0.8.5 — 2026-08-19 — a SIEC cube introduces itself on the air

- **Two fresh cubes now see each other without a phone.** A cube in SIEC
  mode sends one self-advert about 45 seconds after power-on — the same
  frame the panel's ROZGLOS button sends. The delay lets the mesh settle
  and keeps a cube powered on before its antenna is attached from
  transmitting immediately. SAM stays silent: whoever chose not to relay
  also chose not to be announced, and ROZGLOS remains the manual path for
  both modes.

## v0.8.4 — 2026-08-19 — four seats at the hotspot

- **A remembered device can no longer lock the owner out.** The hotspot
  allowed exactly one client, so a laptop auto-joining with a remembered
  password silently took the only seat — and the phone typing the CORRECT
  password off the OLED was refused with what Apple renders as a
  wrong-password error (owner's pair test, 19 Aug). The access point now
  seats four; "one person's terminal" stays enforced where it belongs, at
  the panel bridge, which accepts a single client.

## v0.8.3 — 2026-08-19 — the hotspot stops fighting the person using it

- **One password per cube, remembered by the phone.** The password used to
  rotate with every hotspot window, so every phone kept a stale one and every
  session began with retyping eight characters off a tiny screen. It is now
  drawn once, stored in the cube, and shown on the OLED whenever the window
  is open — the phone remembers it and joins by itself. Physical control
  stays the gate (only the button opens the window) and a wipe erases the
  password with everything else.
- **The window no longer closes mid-session.** The ten-minute countdown runs
  only while nobody is connected; as long as a phone is on the cube's Wi-Fi,
  the network stays up. A window nobody joined still closes after the same
  ten minutes.

## v0.8.2 — 2026-08-19 — the hotspot introduces itself with a word

- **`Biper-SOWA` instead of `Biper-3F2A`.** Owner decision: hex digits are
  easy to confuse between two cubes and impossible to say out loud. The cube
  now draws a 3–4-letter Polish noun (~150-word list, ASCII only, sized so
  the full name fits the 64-px OLED) from the same all-bits fold of the efuse
  identifier that v0.8.1 introduced. With ~150 words two cubes of one batch
  can still draw the same name (<1%); if that ever bites, the fix is a second
  word, not digits. v0.8.1 fixed the collision but never reached the public
  installer — this release supersedes it.

## v0.8.1 — 2026-08-19 — every cube gets its own hotspot name

- **Two cubes no longer announce the same Wi-Fi network.** The hotspot name
  took bits 32–47 of the chip's efuse identifier — and on the ESP32-C6 that
  identifier is an EUI-64, whose constant `FF:FE` filler plus a batch octet
  sit exactly in that slice. Every cube of a production batch therefore
  called itself the same `Biper-15FE` (measured on the owner's pair, 19 Aug),
  and a phone joining "the" hotspot could land on either device. The name now
  XOR-folds all 64 bits, so any octet that differs between units reaches the
  name. Existing cubes will show a new hotspot name once — the name is
  derived, not stored.
- **The boot banner stops lying about its version.** v0.8 printed
  "Biper-AP layer v0.7" — the banner was a forgotten literal. The version now
  lives in one header and `release.sh` refuses to build an image whose banner
  disagrees with `biper/VERSION`.

## v0.8 — 2026-08-19 — the iPhone reaches the panel, the screen admits who it hears

- **iOS no longer answers "bad host" on the way to the panel.** The cube served
  its welcome page directly to Apple's captive probe, so the sign-in sheet kept
  browsing as `captive.apple.com` — and the panel's own origin guard then
  (correctly) refused every click. The probe now answers with the same redirect
  the Android and Windows probes get, the sheet lands on `http://192.168.4.1/`,
  and the guard, the links and the WebSocket all agree (owner-reported on an
  iPhone, 19 Aug).
- **SLYSZE counts a node the first time it is heard.** A newly discovered
  neighbour arrives as a different push code (`0x8A`) than a re-advert
  (`0x80`); only the latter was counted, so two factory-fresh cubes could
  discover each other while both screens still said `SLYSZE 0`.
- **The screen stays informed after the hotspot window closes.** An earlier
  audit fix disabled the whole mesh bridge together with the Wi-Fi window;
  from then on adverts went uncounted and a button wipe was silently dropped.
  The bridge now listens for the device's whole life and the window only
  opens and closes the socket. Incoming messages were never at risk — the
  offline queue is unconditional and pushes are only a doorbell.

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
