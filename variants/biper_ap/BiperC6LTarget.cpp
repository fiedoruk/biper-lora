// SPDX-License-Identifier: MIT
// Portions Copyright (c) 2025 Scott Powell / rippleradios.com — MIT.
// Copyright (c) 2026 Tomasz Fiedoruk (modifications).
//
// Twenty-nine of the thirty-five lines below come verbatim from upstream, which
// makes this a substantial portion of the Software in the sense of the MIT
// licence — so the upstream notice has to travel with the file. It did not
// until 18 Aug 2026, and until then this header claimed the whole file as ours.
//
// Env-scoped copy of variants/m5stack_unit_c6l/UnitC6LBoard.cpp (35 lines,
// upstream companion-v1.17.1). Our env excludes the upstream file and builds
// this one instead, for ONE reason: the display must share the radio's own
// SPIClass instance (a second SPIClass on the same bus produces no usable
// transfers — measured 17.08; the working C6L display reference passes the
// radio SPI and forbids a second begin). The only additions versus upstream
// are dropping `static` and the biper_radio_spi() accessor at the bottom.
// Rebase checklist: re-diff against the upstream file on every release.
#include <Arduino.h>
#include "target.h"

UnitC6LBoard board;

#if defined(P_LORA_SCLK)
  SPIClass spi(0);
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY, spi);
#else
  RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_1, P_LORA_RESET, P_LORA_BUSY);
#endif

WRAPPER_CLASS radio_driver(radio, board);

ESP32RTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
SensorManager sensors;

bool radio_init() {
  fallback_clock.begin();
  rtc_clock.begin(Wire);

#if defined(P_LORA_SCLK)
  spi.begin(P_LORA_SCLK, P_LORA_MISO, P_LORA_MOSI);
  return radio.std_init(&spi);
#else
  return radio.std_init();
#endif
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}

#if defined(P_LORA_SCLK)
SPIClass* biper_radio_spi() { return &spi; }
#else
SPIClass* biper_radio_spi() { return nullptr; }
#endif

// Battery voltage for the cube's screen. This file is OUR env-scoped copy of
// the target, so reaching for the global `board` is not reaching into upstream
// and adds no hook — exactly like biper_radio_spi() above.
uint16_t biper_batt_mv() { return board.getBattMilliVolts(); }
