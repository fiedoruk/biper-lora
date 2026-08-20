// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#pragma once

#include <stdint.h>

// Sound + light feedback for the C6L: buzzer on G11, WS2812 on G2.
// Everything here is called from ONE task (the screen task) — the tiny
// melody player and the LED breathing are advanced by biper_feedback_tick().
//
// Signal language (documented in WIKI-C6L and the user guides):
//   boot     — ascending three-tone + white LED pulse ("I am ready")
//   click    — short tick ("I hear you")
//   gesture  — double tick (hold accepted)
//   AP on    — two rising tones, LED breathes blue while the hotspot runs
//   AP off   — two falling tones
//   guest    — cheerful double beep + cyan flash (someone joined — in panic
//              this is the moment that matters)
//   message  — two rising tones + magenta flash (a message waits on the cube)
//   idle     — soft green heartbeat every ~3 s. Says only that the cube is
//              powered and its screen task runs; it knows nothing about the mesh.

void biper_feedback_init();
void biper_feedback_boot();
void biper_feedback_click();
void biper_feedback_gesture();
void biper_feedback_ap_on();
void biper_feedback_ap_off();
void biper_feedback_guest();
void biper_feedback_msg();  // a message is waiting on the cube

// Advance melodies + LED animation. Call every screen tick (~33 ms, BIPER_SCR_TICK_MS).
void biper_feedback_tick(bool ap_active, uint32_t now);

// Ninja mode (double click): no sound, no LED at all; the screen dims
// separately in BiperScreen. Survival feature: night / staying unnoticed.
void biper_set_ninja(bool on);
bool biper_ninja();
