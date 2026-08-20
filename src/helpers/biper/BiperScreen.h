// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#pragma once

#include <stdint.h>

// C6L OLED status screen. SSD1306 64x48 over the shared LoRa SPI bus, driven
// in an own low-rate FreeRTOS task. Layout is hand-placed for exactly 64x48 —
// pixel-perfect owner requirement.
//
// Visual system "one bit, printed" (18.08.2026): the cube's screen is not a
// carousel of ornaments but a STATE SCREEN. It draws the same mark as the site
// logo and the panel header, and state is carried by SHAPE and FIELD — never by
// colour alone, because the panel has one bit and some readers do not tell
// colours apart.

void biper_screen_start();

// Six states of the ladder. The names stay POLISH on purpose: they are the six
// words the cube actually prints on its screen, and the vocabulary is LOCKED
// SHUT — the same six words hold on the cube, in the panel and on the website.
// Translating the identifier would break that one-to-one mapping.
enum BiperFace {
  FACE_ZYJE,     // "alive": running, nothing to do  -> frame swap, slow wave
  FACE_NADAJE,   // "transmitting": went on the air  -> motion + edge bar
  FACE_CZEKAM,   // "waiting": no confirmation       -> open shape + 50% dither
  FACE_DOSZLO,   // "delivered": radio confirmed     -> closed shape, solid
  FACE_POMOC,    // "help": alarm                    -> the WHOLE field inverted
  FACE_CISZA     // "silence": no link / asleep      -> 37% dither, never blank
};

// Reports a state to the screen. Called from BiperApInterface on frames that
// pass through it anyway — no further hook in main.cpp and no reaching into
// the_mesh.
// Safe from any task in the sense that matters here, but NOT a lock-free
// guarantee: the setter reads two volatiles before writing two more, so it can
// race the screen task's own expiry logic. A torn read costs one wrong frame at
// ~30 fps, after which the screen re-converges to FACE_ZYJE. Do not extend this
// pattern to anything a person could act on.
// C4: a counter, not a single bool. A direct message gets a delivery
// confirmation, a shared channel does not — and with two quick sends one shared
// bool described a message that was NO LONGER the one in flight. The cube said
// DOSZLO about something it was still waiting for, or hung a full minute on
// CZEKAM after a channel message that will never be confirmed at all.
// So we count sends awaiting confirmation instead of remembering the last one.
// F-03 (audyt Codexa): DOSZLO dodatkowo dopasowywane PO ZNACZNIKU — RESP_SENT
// rejestruje 4-bajtowy tag oczekiwanego potwierdzenia, PUSH_CONFIRMED musi
// w niego trafic. Spoznione albo cudze potwierdzenie nie zmienia ekranu.
#if defined(BIPER_AP) && defined(BIPER_SCREEN)
void biper_face_set(BiperFace f);
void biper_face_resp_sent(const uint8_t* tag_or_null);   // RESP_SENT z ramki
void biper_face_confirmed(const uint8_t* tag_or_null);   // PUSH_CONFIRMED z ramki
#else
static inline void biper_face_set(BiperFace) {}
static inline void biper_face_resp_sent(const uint8_t*) {}
static inline void biper_face_confirmed(const uint8_t*) {}
#endif
