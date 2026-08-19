// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#pragma once

#include <stdint.h>

// C6L user button (SYS_KEY1) sits on P0 of the PI4IOE5V6408 I2C expander
// (own bus: SDA=G10, SCL=G8) and is ACTIVE LOW. The same expander drives the
// LoRa LNA enable (P5), antenna switch (P6) and the SX1262 reset line (P7),
// so init writes the config registers exactly once, in an order that keeps
// those three pins driven high throughout (see expander_config in the .cpp).
// Polling itself only reads the input-status register.

enum BiperButtonEvent {
  BIPER_BTN_NONE,
  BIPER_BTN_PRESS,   // button went down — feedback only, no action
  BIPER_BTN_CLICK,   // short press (fires after the double-click window)
  BIPER_BTN_DOUBLE,  // two short presses within the window
  BIPER_BTN_TRIPLE,  // three short presses within the window
  BIPER_BTN_HOLD
};

// Finds the expander (0x43/0x44) and configures it; false = no expander,
// feature off.
bool biper_button_init();

// Call every few tens of ms from one task; returns at most one event per call.
// CLICK, DOUBLE and TRIPLE are all reported once the multi-click window has
// expired — a press cannot be classified before it is known whether another one
// follows. HOLD fires as soon as the press passes the hold threshold.
//
// PRESS fires on the DOWN edge and means nothing more than „the button went
// down". It exists so that every press is heard AT ONCE, before it is known
// how many there will be. Without it a person trying three clicks gets no
// signal that the second one registered — and that was the first thing the
// owner reported after trying it on the cube (18.08: "you can't click 3x").
BiperButtonEvent biper_button_poll();

// How long the button has been held down, in milliseconds; 0 when it is up.
// Used by the wipe gesture, which needs to watch a press CONTINUE rather than
// react to a single event — and to draw a countdown while it does, so nobody
// wipes a cube by leaning on it.
uint32_t biper_button_held_ms();
