// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#pragma once

#include <stdint.h>

// Biper-AP layer (lane B). Additive-only: the code lives entirely in
// src/helpers/biper/ and is reached from examples/companion_radio/main.cpp
// through FOUR marked #ifdef BIPER_AP blocks — the include, the RNG, the BLE
// pin, and setup — and through nothing else. With BIPER_AP undefined the build
// is bit-for-bit upstream.
// SoftAP + captive DNS + the cube's page (an snprintf template with live
// device numbers), run in own FreeRTOS tasks so upstream loop() stays
// untouched.

class NodePrefs;
class MultiSerialInterface;

// Called once from the main.cpp hook, after the_mesh.begin() has loaded prefs
// and after startInterface(). Registers the panel WS bridge with the manager.
void biper_ap_setup(NodePrefs* prefs, MultiSerialInterface* manager);

// Toggle the AP window (button hold): starts it when idle, closes it early
// when active. Safe to call from any task.
void biper_ap_request_toggle();

// Shared layer state, read by the screen task. Single writer, so no lock:
// biper_ap_setup() fills ssid before any task starts, the AP task owns the
// window fields, biper_ble_pin() fills ble_pin during upstream setup().
// Written in the AP task, read in the screen task.
//
// The scalars get `volatile`, because without it a compiler with LTO is free to
// keep them in a register and the screen stops seeing a change that has already
// happened.
//
// The strings DELIBERATELY do not. `volatile char[]` gives no atomicity — and
// atomicity, not write visibility, is the problem here — while it does break
// every string function (`strlen`, `snprintf`, `WiFi.softAP`), so it would be
// paid for with casts that strip the protection anyway. What guards against
// showing half a password is the ORDER: `active` goes to `true` only AFTER
// `ssid` and `pass` are written, and to `false` BEFORE they are cleared when the
// window closes. The screen draws the password only while `active`, so it never
// reads the field mid-write.
struct BiperApState {
  volatile bool active;
  char ssid[16];
  char pass[12];   // fresh 8-character WPA2 password per window, alphabet
                   // without look-alike glyphs, shown on the OLED. Alphabet
                   // and entropy are stated once, at ALF in BiperAp.cpp.
  volatile uint32_t ble_pin;
  volatile uint8_t guests;
  volatile uint16_t window_left_s;
  volatile uint16_t window_total_s;  // window length, so the screen can size the bar
};
const BiperApState& biper_ap_get_state();

// Called from the marked BLE hook in main.cpp. Factory 123456 -> a random
// 6-digit session pin (shown on the OLED INFO page; bonded phones are
// unaffected). A user-configured pin from the app is respected as-is.
uint32_t biper_ble_pin(uint32_t upstream_pin);

// Forwarding other people's packets. Two EQUIVALENT modes, switched by a triple
// click; SIEC is the default, because a network of cubes that do not forward
// has a range of one hop. (SIEC / SAM are the words shown on the cube and in the
// panel: "network" / "on its own".) `biper_forwarding_toggle` returns the state
// AFTER the change.
bool biper_forwarding();
bool biper_forwarding_toggle();
// Clears the remembered choice — called by the factory-reset wipe.
void biper_forwarding_forget();
