# Changelog — the Biper layer

What this fork adds on top of MeshCore, release by release. Upstream's own
history lives in the [MeshCore repository](https://github.com/meshcore-dev/MeshCore).
Dates use ISO 8601. Nothing below claims device evidence it does not have —
see the README section "What is measured and what is not".

## Unreleased

Nothing yet.

## v0.8.22 — 2026-08-20 — the adversarial pass: local commands get their own lane

A second adversarial review of the day's changes (independent AI reviewer,
prompted to break the autostart interactions) confirmed a deeper truth: the
"SPSC" inbound ring was a fiction. Three tasks were producing into it — the
panel's WebSocket handler, the AP task's adverts, and the screen task's
button-wipe — and the day's new machinery made that visible in three ways.

- **Local commands ride their own ring.** The button wipe and the layer's
  adverts no longer share the session ring with panel frames: a phone
  takeover can never flush a queued FACTORY_RESET (the screen said WYMAZUJE,
  the identity survived), and one task can no longer corrupt another's
  enqueue. All ring operations now run under one mutex (single-core C6,
  priority inheritance, sections of a few microseconds).
- **A cube's own beacon no longer counts as "panel activity".**
  `biper_ws_last_rx` was refreshed by every enqueued frame — including the
  presence beacon every ten minutes, which is shorter than the 3 + 10 minute
  idle budget, so a dead socket kept the hotspot alive forever and F-04
  defeated itself. Only the WebSocket handler notes activity now.
- **TX frames carry a session generation.** Replies produced for the old
  phone die in `drainTx` instead of reaching the new one, and nothing
  touches the producer's ring index from the consumer's task any more.
- **The second of two quick DMs keeps its delivery marker.** Tag
  registration is decoupled from the displayed face: the face is a preview,
  not a protocol guard.
- **The BLE PIN window is three minutes from power-on again.** "…or while
  the hotspot window is open" made sense with manual windows; with autostart
  it meant "always".
- Hardening: advert timers advance only after a successful enqueue (a
  rejected boot advert retries next tick instead of waiting an hour); the
  presence period gets a fresh random component each cycle on top of the
  per-cube MAC offset; deadline comparisons are rollover-safe; a failed
  0xB5 "busy" reply closes the dying session instead of pretending the
  panel was warned.

Known and accepted for now (tracked): the release build still enables BLE
alongside the panel, and upstream's companion state is single-client — a
phone on the official app and the panel used at the same time can interleave
responses. The owner's call on shipping the Wi-Fi-only variant is pending.

## v0.8.21 — 2026-08-20 — the autostart aftermath: four live-desk regressions, same day

v0.8.20's autostart armed a landmine that had been lying in the code since the
beginning: opening the hotspot window is a BLOCKING call inside the AP task's
loop, and the advert machinery lived only in the idle part of that loop. With
the window now open from second eight — and held open by a live panel — a cube
never announced itself at all, and a pair showed SŁYSZĘ 0 on both screens
within fifteen minutes. Found on the owner's desk, fixed and re-verified on
hardware the same afternoon.

- **Adverts tick in both loops (the root fix).** Boot advert, presence beacon
  and advert-reply moved into a shared `biper_advert_tick()` called from the
  idle loop AND from inside the window loop. Proof on hardware: with the
  window open since second nine, the boot advert fired at 45.5 s.
- **Anti-lockstep jitter, derived from the eFuse MAC.** Two cubes powered on
  in the same second (a family after a blackout) transmitted their boot
  adverts at the same millisecond — and their fixed 45 s / 10 min schedules
  kept them colliding forever. A per-cube constant offset (0–20 s boot,
  0–60 s beacon, 0–7 s advert-reply) breaks the symmetry deterministically.
  Measured: simultaneous boot now yields adverts 11.8 s apart.
- **Session takeover no longer eats the new client's first frame.** The
  v0.8.20 RX flush discarded everything queued at the next mesh pass — a fast
  phone's `APP_START` regularly arrived before that pass and vanished; iOS
  hung on ŁĄCZĘ forever. The flush now walks only up to a marker recorded at
  the takeover moment; frames that arrive after it stay.
- **The inbound ring holds the panel's connect burst.** The panel opens with
  six back-to-back frames; a depth-4 ring has three usable slots, so the
  overflow was always dropped silently — v0.8.20's honest "busy" reply
  (F-05) merely made it visible on every Android connect. Depth is now 8.

## v0.8.20 — 2026-08-20 — the second external audit: state truth and recovery

A second independent full-tree audit (Codex) confirmed nine P1 findings —
every one verified against this tree before a line changed. All six logic
findings land here; the audit's own words for the theme: the cube must never
claim more than it knows. Two owner decisions from the live desk test land
with it:

- **The hotspot window opens by itself at power-on** (owner decision, 20.08,
  reversing the 18.08 "panic ritual" doctrine). The panel is the cube's only
  interface — without the window you cannot connect at all, and manually
  arming the core path before every use proved to be friction, not safety.
  The lab flag `BIPER_AP_AUTOSTART` became production behaviour and is gone.
  Privacy and power keep their guard: without a live panel the window still
  closes after 10 minutes, and the 3-second hold reopens it.
- **The wipe countdown arms at 6 s, not 4 s** (owner-reported live
  regression). v0.8.19 grew the hotspot hold to exactly 3 s but left the
  wipe threshold at 4 s — one second of human margin, and "hold about three
  seconds" ended on the WYMAŻ screen every other try. The dead zone is now
  3–6 s, and an uninterrupted press that does reach the countdown UNDOES the
  window toggle it fired at second three — an aborted wipe leaves the cube
  exactly as it was before the whole gesture.

- **Delivery truth is now matched by tag (F-03/F-19).** `RESP_SENT` carries a
  4-byte marker of the expected confirmation; the screen registers it and
  `DOSZLO` lights up only when `PUSH_CONFIRMED` hits a live marker. A late
  (60 s+), duplicated or foreign confirmation no longer changes the screen.
  The matching logic is pure C++ (`BiperLogic.h`) with 11 host tests.
- **A missing `return true` in the frame-accept path (F-01).** The function
  returned garbage from a register; the button-wipe path worked by accident.
  The global `-w` that silenced it is now stripped from the Biper
  environments and a missing return is a compile error (`-Werror=return-type`).
- **The 10-second wipe gesture works without a working OLED (F-02).** The
  fallback loop only knew HOLD/DOUBLE/TRIPLE; a dead display stole the one
  local way to erase the cube. The countdown is now audible (a click every
  second, a melody confirms acceptance).
- **A remembered station no longer holds the hotspot open forever (F-04).**
  The window is held by a LIVE panel — an open WebSocket that spoke within
  the last 3 minutes; the panel pulses once a minute. A phone that fell
  asleep stops holding the window; a person at the panel still keeps it open
  all night (the 19.08 owner decision stands in spirit).
- **A full inbound queue answers instead of losing commands silently
  (F-05).** The WebSocket handler now tells the panel "busy" (0xB5) and the
  panel says it out loud. No blind auto-retry: the reply does not identify
  which frame was dropped, and re-sending a message that DID enter would
  duplicate it.
- **Phone takeover flushes the old session's pending commands (F-06).** Only
  the TX ring was cleared; a command queued by the previous phone could
  execute on the new session's watch. The RX ring is now flushed from the
  consumer side on takeover.
- Hardening from the same audit: the stored hotspot password must be exactly
  8 characters (F-12); a failed URI registration aborts the window start
  instead of serving a half-dead panel (F-13); task-creation failures are
  logged instead of vanishing (F-14); changing the Wi-Fi word mid-window no
  longer relabels the live network — the new name is answered as
  next-window and the OLED keeps showing the network that is actually on
  air (F-11).
- CI grows three gates (F-07/F-15): stock C6L BLE/USB builds (Biper touches
  shared files — TCXO 3.0 V among them), host tests
  (`native_biper_faces` + `native_kiss_modem`), and a release-flag gate that
  refuses lab flags (`BIPER_AP_OPEN`, `BIPER_DEBUG_SECRETS`,
  `BIPER_RNG_SAMPLES`, `BIPER_AP_AUTOSTART`) in shipped environments.
- Correction to the v0.8.19 notes below: the shipped filesystem partition is
  **1 MB**, not 7.9 MB — SPIFFS above ~2 MB formatted for minutes after the
  first erase and the boot looked hung, so the partition was shrunk before
  the release was cut. The remaining flash stays free for the future.

## v0.8.19 — 2026-08-21 — four owner decisions land: the law, the promise, the future

- **A hard legal airtime limit (owner decision F-03).** The airtime budget
  factor becomes 9.0 in the Biper variant — at most 10% duty, the ETSI limit
  for the 869.4–869.65 MHz sub-band — enforced in the getter, so neither
  stored preferences nor the CLI can raise it. The MeshCore default (1.0,
  i.e. 50%) would let a rescue device break the law exactly when it
  transmits the most. Real traffic (an SOS every minute ≈ 1.7%) fits with
  a wide margin. Recorded in specs/regions/pl-eu868.v1.json.
- **The 3-second gesture is now three seconds (F-01).** The screen, the site
  and the manual always said "hold for 3 s"; the code fired at 2.5. One
  constant, zero copy edits, and more head-room after a triple click.
- **Coding Rate stays 4/5 (F-02)** — community-compatible and cheaper on
  airtime; 4/8 will be judged against the E-02 range measurement, not
  guessed. The site already tells the truth.
- **16 MB and an OTA-ready partition table (F-04).** The stock board
  definition pretended 4 MB and huge_app burned OTA away forever; the cube
  physically carries 16 MB. New board manifest + two 4 MB app slots +
  a 1 MB filesystem (corrected 2026-08-20; this entry previously said
  7.9 MB, but SPIFFS that large formatted for minutes and the shipped
  table was already cut to 1 MB). The OTA update mechanism itself is a future vector —
  this table merely stops forbidding it. **Installing this version requires
  a full erase** (the web installer does one anyway): the partition map
  moved, so identities and contacts start fresh.

## v0.8.18 — 2026-08-20 — the hardening pass: an external audit, verified line by line

An independent 36-area audit (Kimi K3) of the whole fork, executed with a
falsifier on every claim — three findings were REJECTED against evidence
(0x8A really does mean a new contact on this tree; production already gzips;
the portal sitemap already carries our routes) and the rest verified in code
before touching anything. What went in:

- **TCXO fed 3.0 V, as the vendor schematic demands.** The radio oscillator
  (X1G0041310042) is powered from the SX1262's DIO3 pin and the M5Stack
  schematic annotates it "VDD: 3.0V"; we compiled 1.8 V. The desk pair never
  noticed — range measurement before/after is still open (E-02).
- **"WYMAZUJE" finally has all its letters.** The 4×7 screen font lacked
  U, W and Y, so the one irreversible operation showed " MAZ JE". Glyphs
  added on both sides of the C↔JS pixel gate, and the gate now compares the
  font TABLES literally — frame comparison alone could never catch glyphs
  no animation state uses.
- **Messages received before the panel's first visit survive.** The frame
  format was chosen at QUEUE time by the client's declared version, which is
  zero until a client connects — so early messages were queued in the old
  dialect the panel silently drops. The Biper build now defaults to V3.
- **Frame length gates** on the paths that read past the declared length:
  channel-send, contact responses, login responses, TRACE packets, advert
  parsing, transport codes, full-record contact updates. All minimal fork
  insertions, marked BIPER, candidates for upstream PRs.
- **Compile-time geometry guard.** Losing `-D MAX_CONTACTS=100` once compiled
  one class with two memory layouts (the 19.08 pair failure). The guard sits
  in BaseChatMesh.h — the header whose default of 32 is the wrong one — so a
  lost flag now fails the build with a message instead of failing the pair.
  CI also gained a VERSION↔header gate and wider build triggers.
- **HMI never dies with the screen**: button and feedback initialize before
  and independently of the OLED. Mode toasts became non-blocking states —
  an 800 ms vTaskDelay used to eat clicks and freeze the LED. The wipe frame
  is retried and reported instead of silently dropped on a full ring; a
  failed AP start closes the window instead of spinning ten empty minutes.
- **UTF-8-safe truncation** at all three text cut points (the helper existed,
  nothing called it); WS rejects fragmented frames and shortens the receive
  timeout; a session takeover clears the TX ring (the new phone no longer
  receives frames queued for the old one); dropped client frames are counted.
- Hygiene: dead code out, stale comments fixed, the "TYLKO SWOJE" toast no
  longer overflows 64 px, the LED is written only on change, release.sh now
  writes a tracked `.sha256.txt` manifest next to each (untracked) binary,
  and internal audit dossiers are ignored so they can never reach the public
  mirror again. Panel component v0.15 (import limit now matches the
  transport's 174-byte reality instead of promising 250).

## v0.8.17 — 2026-08-20 — the message carries proof of its journey

The close of the UX vector. Panel component v0.14:

- **Proof of the journey.** Every incoming private message now says how it
  travelled — "direct" or "via N nodes" — and a delivered message carries
  the same proof next to its DOSZŁO stamp. This is the one line an ordinary
  messenger has no way to say.
- **Radio in human words.** "SNR 6.5" becomes "heard clearly (6.5)" — the
  word leads, the number stays in brackets for those who read it. Same
  thresholds everywhere, from lived SF8 practice.
- **A contact card above the conversation.** The empty upper two-thirds of
  a private thread now answers the only questions anyone looks for there:
  AUDIBILITY and PATH. Freshness stays in the top bar — no duplication.
- **The channel stops promising.** A public-channel message showed a waiting
  ring forever, because the ring's confirmation does not exist on a shared
  channel — and the DOM element it should have updated was never tracked.
  Now: NADANO, plainly, and an error still marks NOT SENT.
- **Navigation decision (owner-delegated):** six tabs stay at every width —
  merging tabs per screen size would put two different product maps into
  one manual and one set of films. Below 360 px only the typography tightens.
- Bubbles at 78 % width with the author-side corner cut; the CONTACTS icon
  is two people instead of accidental scissors; audibility and path speak
  in neutral ink — green stays reserved for the link and DOSZŁO; the mock
  rotates its replies so screenshots stop looking like duplicate-message bugs.

## v0.8.16 — 2026-08-20 — the panel keeps answering three questions: who hears me, is my cube linked, where is the alarm

An external UX review of panel screenshots (fresh reviewer, no build context)
found that the interface stopped answering the three questions that matter
in an emergency. Panel component v0.13:

- **"Who am I talking to" never disappears.** The recipient lived only in the
  input placeholder, which vanishes at the first typed letter — exactly when
  someone returning from a private chat would broadcast their position to the
  whole network. A permanent capsule next to the input now says
  DO WSZYSTKICH / DO: <name>, with a hot outline for private threads.
- **The alarm is reachable from every screen.** A persistent SOS shortcut sits
  in the header, and its twin in the conversation bar (the header is hidden
  there). The HELP tab is renamed ALARM (EN: SOS) and its icon is a warning
  triangle — it used to share its shape with the brand mark, so the alarm tab
  read as "about this app".
- **Green means one thing: the link to YOUR cube.** The conversation header
  showed the contact's last-heard time in the same green, same corner where
  the channel screen shows POŁĄCZONO — two levels of trust in one slot. The
  link LED now travels into the conversation bar and last-heard turns neutral.
- **Glove-sized quick replies, honest edges.** Quick-reply chips grow to
  48 px and lose the orange transmit glow they inherited by accident (\"U mnie
  OK\" looked like an alarm action). The bottom navigation gains side margins
  — on a 320 px screen the first tab started 4 px from the edge and six
  targets touched sides.
- **The preview mock finally shows delivery states.** Its ACK carried no tag,
  so DOSZŁO could never appear in screenshots — the reviewer reasonably
  mistook an unmatchable mock for a missing feature.

## v0.8.15 — 2026-08-20 — SLYSZE stops lying between adverts, and the word pool doubles

- **The "SLYSZE N" counter and the advert cadence finally agree.** The
  counter shows neighbours heard in the last 15 minutes, but after the boot
  advert the next transmission came a full hour later — so a healthy,
  actively chatting pair spent 45 minutes of every hour truthfully
  displaying zero. Two fixes, both sides of the same coin: a zero-hop
  presence beacon every 10 minutes (repeaters do not rebroadcast it, so the
  wider mesh hears nothing) keeps live neighbours inside the window, and a
  received direct message now refreshes its sender in the counter — a
  conversation is the strongest possible proof the other cube is alive.
- **The station word pool grows from 149 Polish to 294 words.** An English
  block joins the Polish one — same rules: 3–4 letters, sayable out loud,
  nothing rude in either language, and nothing that sounds like an existing
  Polish entry (CRAB~KRAB, THOR~TOR and friends were dropped). Same-batch
  name collisions fall below half a percent. Note: the update may redraw a
  cube's DERIVED word (the modulo changed); a word set by the owner in the
  panel is stored in NVS and never touched.

## v0.8.14 — 2026-08-20 — messages reach the phone, passwords stop lying to the eye

- **The panel finally understands the cube's message frames.** The panel
  never declared its protocol version, so the cube answered with the old
  frame codes while the panel's parser only understood the new ones — every
  message was fetched and silently dropped (the preview mock spoke the new
  codes, which hid the bug until a live pair). The panel now declares
  version 3 on connect, and additionally drains the queue at startup, so
  messages that arrived while the phone slept appear the moment the panel
  opens. Panel component v0.11.
- **The password alphabet loses its look-alike twins.** S/5, G/6, B/8 and
  Z/2 render near-identically on the 5×7 screen font; the owner typed a
  password with S and G off the OLED and the phone refused it while the
  access point itself was fine. Both twins of every pair are gone
  (23 symbols, 36.2 bits), and a stored password containing dropped
  characters is redrawn on boot, so the screen always shows something
  typeable.

## v0.8.13 — 2026-08-20 — the pair completes both ways, and cubes carry their word as a name

- **The advert-reply escapes the shared rate limit.** The boot advert almost
  always fired within the last ten minutes, so the shared limit silently
  suppressed the one transmission that completes the pair — observed as
  "cube A sees B, B does not see A". The reply now has its own one-minute
  gap; storms stay impossible.
- **A factory-named cube introduces itself by its word.** Contact lists full
  of "047DCB4B" read like a debugger, not a family tool. When the node name
  is still factory (empty or eight hex digits), it now defaults to the
  cube's word — the same one the Wi-Fi carries (KIER, ELF…) — so adverts,
  KONTAKTY and messages all speak one name. A name typed in USTAW always
  wins and is never touched.

## v0.8.12 — 2026-08-19 — a contact across any distance

- **SETTINGS gains "contact at a distance".** SHOW MY CODE exports the
  cube's signed advert as a pasteable `BIPER1:` code — send it by SMS,
  e-mail, any messenger. ADD CONTACT FROM CODE verifies the signature on
  the cube and adds the person to KONTAKTY; forged or damaged codes are
  rejected. Exchange codes both ways and a message will arrive whenever a
  chain of network nodes exists between you. The built-in guide explains
  the flow. Bench-verified: export 111 B, import OK, contact listed,
  garbage rejected. Panel component v0.10.

## v0.8.11 — 2026-08-19 — the pair finally sees each other: one class, one memory layout

- **The contact list works.** The RAM diet of 18 Aug used `-U MAX_CONTACTS`
  before its own `-D` — and PlatformIO moves every `-U` to the END of the
  compiler command, erasing BOTH defines. Each source file then fell back to
  its own header default: `MyMesh.h` said 100 contacts, `BaseChatMesh.h`
  said 32. One class, two array sizes, two different addresses for every
  field behind that array — the radio path stored contacts where the
  panel's enumeration never looked. Proven with address probes on the
  owner's pair; fixed by overriding with a later `-D` alone. Bench-verified
  end to end afterwards: both contact lists filled, direct messages
  delivered and acknowledged both ways.
- **A neighbour parked in the transient zone is promoted.** A record created
  by the anonymous path (e.g. an SOS) sits in slots the contact list never
  shows; an advert used to only refresh it there, forever. Hearing an advert
  for such a record now clears the transient slot and adds a real contact.

## v0.8.10 — 2026-08-19 — discovery no longer hinges on one 45-second window

- **A cube answers a new neighbour with its own advert.** Discovery used to
  depend on a single advert 45 seconds after boot — flash two cubes one
  after the other and the first one's only announcement flies while the
  second is still in the bootloader; the pair then never finds itself
  (owner's evening test; verified end-to-end on the bench over a USB
  companion probe: protocol, clock, contact sync and radio RX all healthy,
  contacts empty purely for lack of a second advert). Now hearing a NEW
  node triggers our own flood advert after three seconds, so one advert in
  either direction completes the pair.
- **A periodic re-advert every hour** heals any missed window and keeps
  neighbours' "last heard" honest. Both mechanisms share a ten-minute rate
  limit; SAM stays silent, as always.

## v0.8.9 — 2026-08-19 — the panel link recovers by itself, the clock becomes real

- **The newest phone takes over the panel.** The bridge used to refuse a
  second client — principled on paper, and in practice a phone with a
  suspended tab left a half-dead socket that bricked the panel for every
  next phone, while SŁYSZY showed the radio was fine (owner's two-phone
  test). Physical presence on the cube's Wi-Fi is the auth; the person
  opening the panel now outranks a socket nobody is looking at.
- **The panel sets the cube's clock on every connect.** The cube has no
  clock battery and woke up in epoch zero, so contacts showed absurd
  "last heard" ages. The firmware accepts only a time not earlier than its
  own, so the clock can never be pushed back. Panel component v0.9.

## v0.8.8 — 2026-08-19 — the cube says which system it runs

- **The INFO page shows the OS version.** The bottom line used to show the
  radio preset — identical on every cube by design, so it answered a question
  nobody asked. With several cubes and several releases a day, the owner
  needs the device itself to say which system runs on it.
- **The panel reports the cube's version, not its own file's.** The footer
  reads the OS version live from the cube; and the cube now serves its pages
  with `Cache-Control: no-cache`, because a phone happily kept a panel cached
  from an older firmware and showed a stale version forever.

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
