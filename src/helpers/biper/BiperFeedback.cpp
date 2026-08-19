// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#if defined(BIPER_AP) && defined(BIPER_SCREEN)

#include "BiperFeedback.h"

#include <Arduino.h>

#ifndef BIPER_PIN_BUZZER
#define BIPER_PIN_BUZZER 11
#endif
#ifndef BIPER_PIN_RGB
#define BIPER_PIN_RGB 2
#endif

// LED kept dim on purpose: a status light, not a torch.
static const uint8_t BIPER_LED_DIM = 20;
// Idle heartbeat: one short green blink per period. It reports that the cube
// is powered and ticking — NOT that the mesh is reachable.
static const uint32_t BIPER_LED_HEARTBEAT_MS = 3000;
static const uint32_t BIPER_LED_HEARTBEAT_ON_MS = 90;
// Dwell per step of the AP breathing ramp (8 steps -> ~1.3 s cycle).
static const uint32_t BIPER_LED_BREATH_STEP_MS = 160;

// --- tiny non-blocking melody player (single melody at a time) ---

struct ToneStep {
  uint16_t freq;  // 0 = rest
  uint16_t ms;
};

static const ToneStep MELODY_BOOT[] = {{900, 90}, {1350, 90}, {1800, 140}, {0, 0}};
static const ToneStep MELODY_CLICK[] = {{2200, 18}, {0, 0}};
static const ToneStep MELODY_GESTURE[] = {{1200, 50}, {0, 30}, {1600, 60}, {0, 0}};
static const ToneStep MELODY_AP_ON[] = {{1000, 110}, {0, 40}, {1500, 140}, {0, 0}};
static const ToneStep MELODY_AP_OFF[] = {{1500, 110}, {0, 40}, {1000, 140}, {0, 0}};
static const ToneStep MELODY_GUEST[] = {{1800, 70}, {0, 50}, {1800, 70}, {0, 0}};
static const ToneStep MELODY_MSG[] = {{1400, 90}, {0, 60}, {1900, 130}, {0, 0}};

static const ToneStep* melody = nullptr;
static uint32_t melody_step_until = 0;
static bool ninja = false;

static void play(const ToneStep* m) {
  if (ninja) return;
  melody = m;
  melody_step_until = 0;  // advance on next tick
}

static void melody_tick(uint32_t now) {
  if (melody == nullptr) return;
  // Difference, not absolute comparison: millis() wraps after 49.7 days, and a
  // cube left running as a repeater WILL reach that. With `now < deadline` the
  // wrap makes the condition true for the next 49.7 days — the buzzer would go
  // silent and the LED dark for good, while screen and button kept working.
  if ((int32_t)(now - melody_step_until) < 0) return;
  if (melody->ms == 0) {  // end marker
    noTone(BIPER_PIN_BUZZER);
    melody = nullptr;
    return;
  }
  if (melody->freq > 0) {
    tone(BIPER_PIN_BUZZER, melody->freq, melody->ms);
  } else {
    noTone(BIPER_PIN_BUZZER);
  }
  melody_step_until = now + melody->ms;
  melody++;
}

// --- LED ---

static void led(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(BIPER_PIN_RGB, r, g, b);
}

// One-shot flash requested by an event; overrides ambient for its duration.
static uint32_t flash_until = 0;

static void flash(uint8_t r, uint8_t g, uint8_t b, uint16_t ms) {
  // Ninja mode has to be checked HERE, not only in play(). Until 18 Aug 2026 it
  // was not, and the bug was as bad as it gets for this feature: a guest or a
  // waiting message lit the LED, and biper_feedback_tick() then returned at its
  // own ninja guard without ever clearing it — so the light stayed on until the
  // user left ninja. Silence was kept, darkness was not, in the one mode whose
  // only job is darkness. Whoever double-clicked in order not to be seen was
  // being shown.
  if (ninja) return;
  led(r, g, b);
  flash_until = millis() + ms;
}

// --- public API ---

void biper_feedback_init() {
  pinMode(BIPER_PIN_BUZZER, OUTPUT);
  led(0, 0, 0);
}

void biper_feedback_boot() {
  play(MELODY_BOOT);
  flash(BIPER_LED_DIM, BIPER_LED_DIM, BIPER_LED_DIM, 400);
}

void biper_feedback_click() { play(MELODY_CLICK); }

void biper_feedback_gesture() { play(MELODY_GESTURE); }

void biper_feedback_ap_on() { play(MELODY_AP_ON); }

void biper_feedback_ap_off() { play(MELODY_AP_OFF); }

void biper_feedback_guest() {
  play(MELODY_GUEST);
  flash(0, BIPER_LED_DIM, BIPER_LED_DIM, 500);
}

void biper_feedback_msg() {
  play(MELODY_MSG);
  flash(BIPER_LED_DIM, 0, BIPER_LED_DIM, 900);  // magenta: a message waits
}

void biper_set_ninja(bool on) {
  ninja = on;
  if (on) {
    noTone(BIPER_PIN_BUZZER);
    melody = nullptr;
    flash_until = 0;
    led(0, 0, 0);
  }
}

bool biper_ninja() { return ninja; }

void biper_feedback_tick(bool ap_active, uint32_t now) {
  if (ninja) return;  // dark and silent, LED already off
  melody_tick(now);

  if ((int32_t)(now - flash_until) < 0) return;  // event flash owns the LED now

  if (ap_active) {
    // Blue breathing while the hotspot runs (triangle ramp).
    static const uint8_t breath[] = {2, 5, 9, 14, 20, 14, 9, 5};
    led(0, 0, breath[(now / BIPER_LED_BREATH_STEP_MS) % sizeof(breath)]);
  } else {
    const bool beat = (now % BIPER_LED_HEARTBEAT_MS) < BIPER_LED_HEARTBEAT_ON_MS;
    led(0, beat ? BIPER_LED_DIM : 0, 0);
  }
}

#endif  // BIPER_AP && BIPER_SCREEN
