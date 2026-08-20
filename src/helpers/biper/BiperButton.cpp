// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#if defined(BIPER_AP) && defined(BIPER_SCREEN)

#include "BiperButton.h"

#include <Arduino.h>
#include <Wire.h>

// PI4IOE5V6408 registers (values after the working C6L reference,
// TheRealHaoLiu/MeshCore branch main-m5stack-unit-c6l, MIT).
static const uint8_t BIPER_EXP_ADDRS[] = {0x43, 0x44};
static const uint8_t BIPER_EXP_REG_IO_DIR = 0x03;
static const uint8_t BIPER_EXP_REG_OUT_SET = 0x05;
static const uint8_t BIPER_EXP_REG_OUT_H_IM = 0x07;
static const uint8_t BIPER_EXP_REG_IN_DEF = 0x09;
static const uint8_t BIPER_EXP_REG_PULL_EN = 0x0B;
static const uint8_t BIPER_EXP_REG_PULL_SEL = 0x0D;
static const uint8_t BIPER_EXP_REG_INPUT = 0x0F;
static const uint8_t BIPER_BTN_BIT = 0;  // SYS_KEY1 = expander P0, ACTIVE LOW

// The expander hangs on its own bus (M5 pinmap: SDA=G10, SCL=G8, INT=G3).
// Upstream's Wire runs on PIN_BOARD_SDA/SCL=16/17, which on this board is
// provably empty (full scan at boot), so re-routing Wire here breaks nothing:
// the RTC autodiscovery has already fallen back and no sensor sits on 16/17.
static const int BIPER_I2C_SDA = 10;
static const int BIPER_I2C_SCL = 8;

static const uint32_t BIPER_BTN_CLICK_MAX_MS = 600;
static const uint32_t BIPER_BTN_HOLD_MS = 3000  /* F-01 (wlasciciel, 21.08): rowne 3 s, jak uczy ekran, strona i instrukcja — koniec sprzecznosci 2,5 vs 3 */;
// Second press starting within this window after a release = double click;
// a third one inside the next window = triple. Cost: a single click gains this
// much latency, because it has to wait the window out.
//
// 500 ms, not 350. Half a second is the default double-click speed on Windows,
// macOS and in browsers — the tempo human hands are trained to. At 350 ms the
// third click did not fit inside the window and the cube ran ninja mode off the
// second one (owner's report from the cube, 18.08).
static const uint32_t BIPER_BTN_DOUBLE_MS = 500;

static uint8_t biper_exp_addr = 0;
static bool biper_btn_was_down = false;
static bool biper_btn_hold_sent = false;
static uint32_t biper_btn_down_since = 0;
// How many short presses have arrived inside the current window. Replaces the
// old boolean: with three gestures on one button we have to COUNT, and none of
// them may fire before the window closes — otherwise a double click would always
// announce a single one first.
static uint8_t biper_btn_clicks = 0;
static uint32_t biper_btn_released_at = 0;

static bool read_input_reg(uint8_t* value) {
  Wire.beginTransmission(biper_exp_addr);
  Wire.write(BIPER_EXP_REG_INPUT);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)biper_exp_addr, 1) != 1) return false;
  *value = (uint8_t)Wire.read();
  return true;
}

static bool write_reg(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(biper_exp_addr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

// Configure the expander so the button (P0) is readable and the RF pins
// (P5=LNA_EN, P6=ANT_SW, P7=SX_NRST) are actively driven high. The radio is
// already RUNNING when we get here, so unlike the reference we skip the chip
// reset and write the output STATES before the directions — the P7 line must
// never dip (a dip resets the SX1262 mid-operation).
static bool expander_config() {
  bool ok = true;
  ok &= write_reg(BIPER_EXP_REG_OUT_SET, 0b11100000);   // P5..P7 high first
  ok &= write_reg(BIPER_EXP_REG_IO_DIR, 0b11100000);    // P5..P7 outputs
  ok &= write_reg(BIPER_EXP_REG_OUT_H_IM, 0b00011100);  // P2..P4 stay Hi-Z
  ok &= write_reg(BIPER_EXP_REG_PULL_SEL, 0b11100011);  // pull-ups
  ok &= write_reg(BIPER_EXP_REG_PULL_EN, 0b11100011);   // on P0,P1,P5..P7
  ok &= write_reg(BIPER_EXP_REG_IN_DEF, 0b00000011);
  return ok;
}

// Appends every responding address as " 0xNN" (6 bytes incl. terminator).
static const size_t BIPER_SCAN_ENTRY_LEN = 6;

static size_t scan_bus(char* out, size_t out_size) {
  size_t used = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0 && used + BIPER_SCAN_ENTRY_LEN < out_size) {
      used += snprintf(out + used, out_size - used, " 0x%02X", addr);
    }
  }
  return used;
}

bool biper_button_init() {
  // Address-probe scans only, no register touched yet: upstream's Wire sits on
  // 16/17 (provably empty here), the expander on the C6L bus 10/8.
  char found[64] = "";
  size_t used = scan_bus(found, sizeof(found));
  Serial.printf("[BIPER_BTN] i2c scan(16/17):%s\n", used ? found : " (empty)");
  if (used == 0) {
    Wire.end();
    Wire.begin(BIPER_I2C_SDA, BIPER_I2C_SCL);
    used = scan_bus(found, sizeof(found));
    Serial.printf("[BIPER_BTN] i2c scan(10/8):%s\n", used ? found : " (empty)");
  }

  for (uint8_t addr : BIPER_EXP_ADDRS) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      biper_exp_addr = addr;
      break;
    }
  }
  if (biper_exp_addr == 0) {
    Serial.printf("[BIPER_BTN] expander not found, button off\n");
    return false;
  }
  if (!expander_config()) {
    Serial.printf("[BIPER_BTN] expander 0x%02X: config failed, button off\n",
                  biper_exp_addr);
    biper_exp_addr = 0;
    return false;
  }
  uint8_t input = 0;
  if (!read_input_reg(&input)) {
    Serial.printf("[BIPER_BTN] expander 0x%02X: input read failed, button off\n",
                  biper_exp_addr);
    biper_exp_addr = 0;
    return false;
  }
  Serial.printf("[BIPER_BTN] expander 0x%02X configured, input=0x%02X\n",
                biper_exp_addr, input);
  return true;
}

BiperButtonEvent biper_button_poll() {
  if (biper_exp_addr == 0) return BIPER_BTN_NONE;
  uint8_t input = 0;
  if (!read_input_reg(&input)) return BIPER_BTN_NONE;

  const bool down = !((input >> BIPER_BTN_BIT) & 1);  // active low
  const uint32_t now = millis();

  if (down) {
    if (!biper_btn_was_down) {  // edge: pressed
      biper_btn_was_down = true;
      biper_btn_hold_sent = false;
      biper_btn_down_since = now;
      return BIPER_BTN_PRESS;     // heard at once; what it means comes later

    } else if (!biper_btn_hold_sent &&
               now - biper_btn_down_since >= BIPER_BTN_HOLD_MS) {
      biper_btn_hold_sent = true;
      Serial.printf("[BIPER_BTN] hold\n");
      return BIPER_BTN_HOLD;
    }
  } else {
    if (biper_btn_was_down) {  // edge: released — a short press opens the window
      biper_btn_was_down = false;
      if (!biper_btn_hold_sent && now - biper_btn_down_since <= BIPER_BTN_CLICK_MAX_MS) {
        if (biper_btn_clicks < 3) biper_btn_clicks++;
        biper_btn_released_at = now;
      }
    } else if (biper_btn_clicks > 0 &&
               now - biper_btn_released_at > BIPER_BTN_DOUBLE_MS) {
      const uint8_t count = biper_btn_clicks;
      biper_btn_clicks = 0;
      Serial.printf("[BIPER_BTN] clicks=%u\n", (unsigned)count);
      if (count == 1) return BIPER_BTN_CLICK;
      if (count == 2) return BIPER_BTN_DOUBLE;
      return BIPER_BTN_TRIPLE;
    }
  }
  return BIPER_BTN_NONE;
}

uint32_t biper_button_held_ms() {
  if (!biper_btn_was_down) return 0;
  const uint32_t held = millis() - biper_btn_down_since;
  return held == 0 ? 1 : held;   // 0 is reserved for "not held"
}

#endif  // BIPER_AP && BIPER_SCREEN
