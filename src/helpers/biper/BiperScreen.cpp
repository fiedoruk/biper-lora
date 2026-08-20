// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#if defined(BIPER_AP) && defined(BIPER_SCREEN)

#include "BiperScreen.h"
#include "BiperVersion.h"
#include <NodePrefs.h>
#include "BiperAp.h"
#include "BiperApInterface.h"
#include "BiperButton.h"
#include "BiperFeedback.h"
#include "BiperLogic.h"

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#define SSD1306_NO_SPLASH
#include <Adafruit_SSD1306.h>

// M5Stack Unit C6L: SSD1306 64x48 (0.66") sharing the LoRa SPI bus
// (SCK=20, MOSI=21), own control lines below (M5 pinmap). Two hard-won
// constraints (measured 17.08 + working reference TheRealHaoLiu/MeshCore
// branch main-m5stack-unit-c6l, MIT):
//  1. the display MUST use the radio's own SPIClass instance
//     (biper_radio_spi() from our env-scoped target copy) — a second
//     SPIClass on the same bus yields a dead panel;
//  2. 64x48 needs the alternative COM-pins config (0x12): the Adafruit
//     library lacks this geometry and defaults to sequential COM pins.
#ifndef BIPER_OLED_CS
#define BIPER_OLED_CS 6
#endif
#ifndef BIPER_OLED_DC
#define BIPER_OLED_DC 18
#endif
#ifndef BIPER_OLED_RST
#define BIPER_OLED_RST 15
#endif

// Panel size; every coordinate below is hand-placed for 64x48.
static const int BIPER_OLED_W = 64;
static const int BIPER_OLED_H = 48;

// Load-bearing: upstream setup() (radio_init on the same SPI bus) must finish
// before the display is brought up.
static const uint32_t BIPER_SCR_BOOT_DELAY_MS = 4000;
// Motion doctrine (owner, 18.08: "not like the 90s"): animated pages run at
// ~30 fps with CONTINUOUS, eased motion driven by millis() — no discrete
// 4-state loops. A 384-byte frame over shared SPI makes 30 fps free.
static const uint32_t BIPER_SCR_TICK_MS = 33;         // button poll + anim fps
// Pairing window for the BLE pin on the INFO page (see draw_info_page).
static const uint32_t BIPER_PIN_WINDOW_MS = 180000;   // three minutes from boot
// How long a button press lights the panel while ninja mode is on.
static const uint32_t BIPER_NINJA_WAKE_MS = 4000;
// Wipe gesture: the countdown becomes visible after BIPER_WIPE_FROM_MS and the
// wipe happens at BIPER_WIPE_MS. The visible warning is the whole
// anti-accident design — no confirmation dialog exists on a 64x48 screen with
// one button.
// 6000, nie 4000 (wlasciciel, 20.08, na zywych kostkach): gest hotspotu
// urosl do rownych 3 s i przy progu 4 s zostawala JEDNA sekunda ludzkiego
// luzu — "trzymam okolo trzech sekund" co drugi raz konczylo sie ekranem
// WYMAZ zamiast Wi-Fi. Martwa strefa 3-6 s: po odpaleniu gestu hotspotu
// jest czas spokojnie puscic, a zamiar wymazania i tak wymaga swiadomych
// dziesieciu sekund.
static const uint32_t BIPER_WIPE_FROM_MS = 6000;
static const uint32_t BIPER_WIPE_MS = 10000;
static const uint32_t BIPER_SCR_REFRESH_MS = 1000;    // static-page refresh
static const uint32_t BIPER_SCR_TOAST_MS = 800;       // mode-change confirmation

// SSD1306 contrast: barely visible in ninja mode, library default otherwise.
static const uint8_t BIPER_OLED_CONTRAST_NINJA = 0x01;
static const uint8_t BIPER_OLED_CONTRAST_NORMAL = 0xCF;

// Provided by variants/biper_ap/BiperC6LTarget.cpp (env-scoped target copy).
extern SPIClass* biper_radio_spi();

static Adafruit_SSD1306* scr = nullptr;

// Click cycle: WAVE -> SIEC -> RADIAL -> INFO -> (back to WAVE): two resting
// animations alternating with the two functional pages.
//
// A resting face is a STATE SCREEN, not a carousel of ornaments. State is
// carried by SHAPE and FIELD, never by colour — the panel has one bit.
//
// Division of labour: the animations are REST, the thing you look at. The mark
// (diamond + waves) is SIGNALLING, the thing you read — it lives only in the
// state faces, where it has to be recognised instantly rather than admired.
// A logo on a resting screen loses to a living composition, and rightly so:
// a logo is for recognition, not for watching.
//
// The inverted face left the rotation: at rest it spelled BIPER a second time
// in a row, and inverting the whole field is reserved for FACE_DOSZLO.
enum BiperPage {
  PAGE_WAVE, PAGE_NETWORK, PAGE_RADIAL, PAGE_INFO,
  PAGE_COUNT
};
static BiperPage biper_page = PAGE_WAVE;
static inline bool page_is_face() {
  return biper_page == PAGE_WAVE || biper_page == PAGE_RADIAL;
}

// State arrives from BiperApInterface — from the frames that pass through it
// anyway (CMD_SEND_TXT_MSG=2 / CMD_SEND_CHANNEL_TXT_MSG=3 towards the mesh,
// RESP_CODE_SENT=6 and PUSH_CODE_SEND_CONFIRMED=0x82 towards the phone).
// No new hooks in main.cpp, no reaching into the_mesh — we report only what we
// see passing through our own layer.
//
// THE HONESTY BOUNDARY: we see the traffic going through OUR bridge (the panel
// from the cube). A conversation held in the official app over BLE goes through
// a different interface and we will not see these states there — which is why
// every state expires to ZYJE by itself instead of staying on the screen as an
// out-of-date promise.
static volatile BiperFace biper_face = FACE_ZYJE;
static volatile uint32_t biper_face_since = 0;   // millis() of the last change

// Znaczniki potwierdzen, na ktore NAPRAWDE czekamy. Dotykane wylacznie z petli
// mesh (writeFrame -> biper_face_resp_sent/confirmed), wiec bez blokad.
static BiperPendingTags biper_tags;

void biper_face_set(BiperFace f) {
  // CZEKAM i DOSZLO nie wchodza juz ta droga — maja wlasne funkcje ponizej,
  // z dopasowaniem po znaczniku (audyt Codexa F-03).
  // Timestamp BEFORE state, and the order is load-bearing. These two writes are
  // not atomic and the screen task reads both: if the state landed first and the
  // task ran in between, it would judge a fresh state by a stale timestamp and
  // expire it instantly — the cube would show nothing at the exact moment the
  // message went out. Written this way the timestamp is never older than the
  // state it belongs to.
  biper_face_since = millis();
  biper_face = f;
}
// RESP_SENT dla DM-a (interfejs rozpoznal to po FIFO oczekiwanych RESP-ow —
// login/status/telemetria tu NIE trafiaja; kanal publiczny w ogole nie dostaje
// RESP_SENT i jego NADAJE po prostu wygasa do ZYJE). Rejestracja znacznika
// jest niezalezna od rysowanej twarzy: przy dwoch szybkich DM-ach drugi RESP
// przychodzi, gdy ekran stoi juz na CZEKAM, a jego potwierdzenie tez musi miec
// w co trafic (rewident, 20.08). Twarz to podglad, nie protokol.
void biper_face_resp_sent(const uint8_t* tag_or_null) {
  const bool nadaje = (biper_face == FACE_NADAJE);
  if (tag_or_null != nullptr) {
    biper_tags.add(tag_or_null, millis());
    if (nadaje) { biper_face_since = millis(); biper_face = FACE_CZEKAM; }
  } else if (nadaje) {
    // RESP bez znacznika: nie ma czego dopasowywac, wiec nie obiecujemy CZEKAM.
    biper_face_since = millis();
    biper_face = FACE_ZYJE;
  }
}

// PUSH_CONFIRMED: DOSZLO wylacznie za trafienie w zywy znacznik. Spoznione
// (po minucie), zdublowane albo cudze potwierdzenie nie zmienia ekranu —
// w produkcie, ktorego rdzeniem jest zaufanie do statusu, ekran nie ma prawa
// obiecac doreczenia, na ktore nikt nie czekal (audyt Codexa F-03/F-19).
void biper_face_confirmed(const uint8_t* tag_or_null) {
  if (tag_or_null == nullptr) return;
  if (!biper_tags.confirm(tag_or_null, millis())) return;
  biper_face_since = millis();
  biper_face = FACE_DOSZLO;
}

// Expiry times. Every state has an end — the screen must not claim CZEKAM for
// an hour, because after a minute that is no longer information, only decor.
static const uint32_t EXPIRE_NADAJE_MS = 4000;    // no RESP -> back to ZYJE
static const uint32_t EXPIRE_CZEKAM_MS = 60000;   // no 0x82 for a minute
static const uint32_t EXPIRE_DOSZLO_MS = 5000;    // the confirmation "settles"

static void biper_face_expire(uint32_t now) {
  const uint32_t since = now - biper_face_since;
  switch (biper_face) {
    case FACE_NADAJE: if (since > EXPIRE_NADAJE_MS) biper_face = FACE_ZYJE; break;
    case FACE_CZEKAM: if (since > EXPIRE_CZEKAM_MS) biper_face = FACE_ZYJE; break;
    case FACE_DOSZLO: if (since > EXPIRE_DOSZLO_MS) biper_face = FACE_ZYJE; break;
    default: break;
  }
}

// 64 px / 6 px per character = TEN characters per line at setTextSize(1).
// There is no wrapping (setTextWrap(false) below), so a longer string is CUT
// at the right edge — and silently, with no error at all. On 18.08 this ate the
// last letter of the radio mode indicator: "radio: SIEC" was displayed as
// "radio: SIE". Found only by a screenshot (site/scripts/zrzuty-ekranu.py),
// because nobody counts that in their head on a 64x48 screen.
//
// Hence this gate: a literal that is too long does not COMPILE. Use it for
// every fixed string drawn from the left edge.
template <size_t N> constexpr const char* screen_line(const char (&s)[N]) {
  static_assert(N - 1 <= 10, "label wider than the screen - it would be cut silently");
  return s;
}

static void draw_text(int x, int y, const char* text) {
  scr->setCursor(x, y);
  scr->print(text);
}

static int text_width(const char* text) {
  int16_t x1, y1;
  uint16_t w, h;
  scr->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return (int)w;
}

static void draw_centered(int y, const char* text) {
  draw_text((BIPER_OLED_W - text_width(text)) / 2, y, text);
}

static void draw_header(const char* title) {
  draw_text(0, 0, title);
  // Uptime since power-on, top-right: "21m", then "1h21" past the hour.
  char line[12];
  const uint32_t up_min = millis() / 60000UL;
  if (up_min < 60) {
    snprintf(line, sizeof(line), "%lum", (unsigned long)up_min);
  } else {
    snprintf(line, sizeof(line), "%luh%02lu", (unsigned long)(up_min / 60),
             (unsigned long)(up_min % 60));
  }
  draw_text(BIPER_OLED_W - text_width(line), 0, line);
  scr->drawFastHLine(0, 9, BIPER_OLED_W, SSD1306_WHITE);
}

// 4x4 dither threshold — the same one the SVG masks #r50 / #r37 apply on the
// website. The identity of density between panel and paper is checkable and
// deliberate.
static const uint8_t BAYER4[4][4] = {
  {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

// ---------------------------------------------------------------------------
// DECORATIVE FACES — owner's concept, iterated live on 17-18.08.
// WAVE / NEGATIVE / RADIAL: a full-screen pixel-art composition, no text,
// 30 fps with continuous motion. This is the RESTING look of the cube and it
// stays. A radio state (NADAJE/CZEKAM/DOSZLO/POMOC) only TAKES OVER the screen
// for the duration of the event and hands it back — the animations are the
// house, the state is a guest.
// ---------------------------------------------------------------------------
static const int8_t SIN8[256] = {
  0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,
    37,   40,   43,   46,   49,   51,   54,   57,   60,   63,   65,   68,
    71,   73,   76,   78,   81,   83,   85,   88,   90,   92,   94,   96,
    98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
   117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,
   126,  127,  127,  127,  127,  127,  127,  127,  126,  126,  126,  125,
   125,  124,  123,  122,  122,  121,  120,  118,  117,  116,  115,  113,
   112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
    90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,
    60,   57,   54,   51,   49,   46,   43,   40,   37,   34,   31,   28,
    25,   22,   19,   16,   12,    9,    6,    3,    0,   -3,   -6,   -9,
   -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
   -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,
   -81,  -83,  -85,  -88,  -90,  -92,  -94,  -96,  -98, -100, -102, -104,
  -106, -107, -109, -111, -112, -113, -115, -116, -117, -118, -120, -121,
  -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
  -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122,
  -122, -121, -120, -118, -117, -116, -115, -113, -112, -111, -109, -107,
  -106, -104, -102, -100,  -98,  -96,  -94,  -92,  -90,  -88,  -85,  -83,
   -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
   -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,
   -12,   -9,   -6,   -3
};

// Distance from the centre. This used to be a 48x64 table filled with `sqrtf` at
// startup — 3072 B of RAM held for the whole life of the device just to save one
// square root per pixel per frame. On the ESP32-C6 RAM is scarcer than cycles,
// and this is a 64x48 screen, not a renderer. We compute it on the fly, in
// integers.
static inline uint8_t radius(int x, int y) {
  const int dx = 2 * x - BIPER_OLED_W;   // doubled, so that the centre falls
  const int dy = 2 * y - BIPER_OLED_H;   // between pixels without fractions
  uint32_t sq = (uint32_t)(dx * dx + dy * dy);  // = (2r)^2, < 2^13 on this panel
  // Bit-by-bit integer square root. The earlier Newton loop could ping-pong
  // between two values (sq=8: 2 <-> 3) and never terminate — the screen task
  // would hang inside a frame. Caught by the site's pixel-parity gate, which
  // compiles this very function; the bitwise form terminates by construction.
  uint32_t res = 0, bit = 1u << 12;
  while (bit > sq) bit >>= 2;
  while (bit) {
    if (sq >= res + bit) { sq -= res + bit; res = (res >> 1) + bit; }
    else res >>= 1;
    bit >>= 2;
  }
  return (uint8_t)(res * 2);  // scaled like the old table: r * 4 = (2r) * 2
}

// Face 1/2: three drifting sine layers interfere; Bayer 4x4 dithers the
// gradient. `invert` renders the negative.
static void draw_wave_field(uint32_t t_ms, bool invert, uint16_t tempo = 12, int8_t threshold = 0) {
  const uint32_t t = t_ms / tempo;  // global drift speed
  for (int y = 0; y < BIPER_OLED_H; y++) {
    for (int x = 0; x < BIPER_OLED_W; x++) {
      int v = SIN8[(uint8_t)(x * 5 + t)] + SIN8[(uint8_t)(y * 7 - (t >> 1))] +
              SIN8[(uint8_t)((x + y) * 3 + (t >> 2))];
      // v in [-381..381] -> 16 levels vs the Bayer threshold
      bool on = (int)((v + 384) / 48) > (int)BAYER4[y & 3][x & 3] + threshold;
      if (on != invert) scr->drawPixel(x, y, SSD1306_WHITE);
    }
  }
}

// Face 3: rings born at the centre and travelling outward across the whole
// panel — the cube visibly "transmitting". Two ring layers for richness.
static void draw_radial_field(uint32_t t_ms, uint16_t tempo = 14) {
  const uint32_t t = t_ms / tempo;
  for (int y = 0; y < BIPER_OLED_H; y++) {
    for (int x = 0; x < BIPER_OLED_W; x++) {
      uint8_t r = radius(x, y);
      int v = SIN8[(uint8_t)(r * 2 - t)] + SIN8[(uint8_t)(r * 5 - t * 2)];
      if ((uint8_t)((v + 256) / 32) > BAYER4[y & 3][x & 3])
        scr->drawPixel(x, y, SSD1306_WHITE);
    }
  }
}

// Envelope glyph: the "a message is waiting" face. An outline, not a filled
// shape, because it is drawn over the moving wave field and a filled block
// would read as a hole in the animation.
static void envelope() {
  const int x0 = 13, y0 = 14, w = 38, h = 20;
  scr->drawRect(x0, y0, w, h + 1, SSD1306_WHITE);
  for (int i = 0; i < w / 2; i++) {
    const int yy = y0 + i * h * 2 / w;
    if (yy > y0 && yy < y0 + h) {
      scr->drawPixel(x0 + i, yy, SSD1306_WHITE);
      scr->drawPixel(x0 + w - 1 - i, yy, SSD1306_WHITE);
    }
  }
}
// ---------------------------------------------------------------------------
// A MESSAGE ON THE FIELD — owner's system (18.08)
// The three animations are the CARRIER, not an ornament next to one. State is
// read from two things at once: which animation is running and how fast, and
// which word is written into it. No pictograms — the word IS the icon. An
// earlier attempt (diamond, waves, two blocks) was rejected: pixels drawn one
// by one give an indicator, not an effect. A 4x7 face instead of 5x7, because
// with five columns the six-letter messages (CZEKAM, DOSZLO) do not fit in
// 64 px at scale 2 and run together into a blot.
// ---------------------------------------------------------------------------
static const uint8_t FONT_W = 4, FONT_H = 7;
// U, W, Y doszly 20.08: napis "WYMAZUJE" (jedyna nieodwracalna operacja!)
// rysowal sie jako " MAZ JE", bo brakujace glify spadaly na spacje, a bramka
// pikselowa C<->JS liczy zgodnosc generatorow, nie obecnosc liter (audyt).
static const char FONT_CHARS[] = "ABCDEIJKLMNOPRSZUWY ";
static const uint8_t FONT[][FONT_H] = {
  {0x6,0x9,0x9,0xF,0x9,0x9,0x9}, {0xE,0x9,0x9,0xE,0x9,0x9,0xE},
  {0x7,0x8,0x8,0x8,0x8,0x8,0x7}, {0xE,0x9,0x9,0x9,0x9,0x9,0xE},
  {0xF,0x8,0x8,0xE,0x8,0x8,0xF}, {0xE,0x4,0x4,0x4,0x4,0x4,0xE},
  {0x3,0x1,0x1,0x1,0x1,0x9,0x6}, {0x9,0xA,0xC,0x8,0xC,0xA,0x9},
  {0x8,0x8,0x8,0x8,0x8,0x8,0xF}, {0x9,0xF,0xF,0x9,0x9,0x9,0x9},
  {0x9,0xD,0xD,0xB,0xB,0x9,0x9}, {0x6,0x9,0x9,0x9,0x9,0x9,0x6},
  {0xE,0x9,0x9,0xE,0x8,0x8,0x8}, {0xE,0x9,0x9,0xE,0xA,0x9,0x9},
  {0x7,0x8,0x8,0x6,0x1,0x1,0xE}, {0xF,0x1,0x2,0x4,0x8,0x8,0xF},
  {0x9,0x9,0x9,0x9,0x9,0x9,0x6},  // U
  {0x9,0x9,0x9,0x9,0xF,0xF,0x9},  // W (odwrocone M)
  {0x9,0x9,0x9,0x6,0x6,0x6,0x6},  // Y (ramiona zbiegaja w dwie srodkowe)
  {0x0,0x0,0x0,0x0,0x0,0x0,0x0},  // space
};
static int glyph_index(char c) {
  for (int i = 0; FONT_CHARS[i]; i++) if (FONT_CHARS[i] == c) return i;
  return (int)(sizeof(FONT_CHARS) - 2);
}
static int label_width(const char* t, int scale, int gap) {
  return (int)strlen(t) * (FONT_W * scale + gap) - gap;
}
// The word has to be BIG: we drop the spacing first, and only then the scale.
static void fit_label(const char* t, int* scale, int* gap) {
  static const int W_[4][2] = {{2,3},{2,2},{2,1},{1,1}};
  for (int i = 0; i < 4; i++)
    if (label_width(t, W_[i][0], W_[i][1]) <= BIPER_OLED_W - 2) {
      *scale = W_[i][0]; *gap = W_[i][1]; return;
    }
  *scale = 1; *gap = 1;
}
// A 1 px halo in the opposite colour — without it the letters vanish where the
// field is dense.
static void label_on_field(const char* t, uint16_t val) {
  int scale, gap;
  fit_label(t, &scale, &gap);
  const int x0 = (BIPER_OLED_W - label_width(t, scale, gap)) / 2;
  const int y0 = (BIPER_OLED_H - FONT_H * scale) / 2;
  const uint16_t halo = (val == SSD1306_WHITE) ? SSD1306_BLACK : SSD1306_WHITE;
  for (int pass_idx = 0; pass_idx < 2; pass_idx++) {
    for (int gi = 0; t[gi]; gi++) {
      const uint8_t* g = FONT[glyph_index(t[gi])];
      const int gx = x0 + gi * (FONT_W * scale + gap);
      for (int r = 0; r < FONT_H; r++)
        for (int c = 0; c < FONT_W; c++) {
          if (!(g[r] & (0x8 >> c))) continue;
          for (int dy = 0; dy < scale; dy++)
            for (int dx = 0; dx < scale; dx++) {
              const int x = gx + c * scale + dx, y = y0 + r * scale + dy;
              if (pass_idx == 0) {
                for (int ox = -1; ox <= 1; ox++)
                  for (int oy = -1; oy <= 1; oy++)
                    if (x+ox >= 0 && x+ox < BIPER_OLED_W && y+oy >= 0 && y+oy < BIPER_OLED_H)
                      scr->drawPixel(x+ox, y+oy, halo);
              } else {
                scr->drawPixel(x, y, val);
              }
            }
        }
    }
  }
}

// The character of a state: which animation, how fast, which word.
// A smaller tempo = faster (a time divisor, as in the original WAVE face).
struct BiperCharacter { const char* word; uint8_t kind; uint16_t tempo; int8_t threshold; bool negative; };
enum { FIELD_WAVE = 0, FIELD_RADIAL = 1, FIELD_ROTOR = 2 };
static const BiperCharacter CHARACTER[] = {
  /* ZYJE   */ {"BIPER",  FIELD_WAVE,   12, 0, false},
  /* NADAJE */ {nullptr,  FIELD_ROTOR,  16, 0, false},
  /* CZEKAM */ {"CZEKAM", FIELD_WAVE,   34, 2, false},
  /* DOSZLO */ {"DOSZLO", FIELD_WAVE,   10, 0, true},
  /* POMOC  */ {"POMOC",  FIELD_RADIAL,  4, 0, false},
  /* CISZA  */ {"CISZA",  FIELD_WAVE,   70, 7, false},
};

// A seventh face added to `enum BiperFace` without a row in the table would read
// memory past its end — and not straight away, only on the first occurrence of
// that one state. The compiler is to catch this instead of the device.
static_assert(sizeof(CHARACTER) / sizeof(CHARACTER[0]) == (size_t)FACE_CISZA + 1,
              "CHARACTER[] needs exactly one row per face in enum BiperFace");

// Counter of unique nodes heard in the 15-minute window — from
// BiperApInterface, from the advert frames that pass through it anyway.
uint8_t biper_heard_15min();

// RESTING TEMPO = DENSITY OF THE NETWORK (owner's decision, 18.08).
// The field flows as alive as the network around it. Zero cubes heard is almost
// stillness; the more neighbours are audible, the faster it runs. One glance at
// the cube then says whether you are alone or in a dense area — without numbers
// and without going into INFO. A smaller value = faster (a time divisor).
// The scale is stretched where people actually live: 0-5 neighbours is nearly
// everyone's case, so the differences between steps are largest there. The top
// is 120% of the starting speed (tempo 12 = 100%, tempo 10 = 120%) and it
// saturates at 7+, because above that the eye reads no difference anyway.
static uint16_t resting_tempo() {
  static const uint16_t CURVE[] = {58, 42, 32, 24, 19, 15, 12, 10};
  const uint8_t n = biper_heard_15min();
  return CURVE[n < 8 ? n : 7];
}


// Three wave sources overlap so that the fringes read as a turning ROTOR — the
// owner saw it first and it is better than the intent it came out of (it was
// meant to draw the network as N sources). The motion has a centre and a
// direction, so it fits the NADAJE state exactly: something spins and pushes
// outward. The other prototypes (moire, droplet, interference of 1 and 6
// sources) were rejected.

// Cheap distance approximation without sqrt: max + 0.375*min. Error below 4%,
// invisible at 64x48 in one bit, and it saves a sqrt per pixel.
static inline int dist_int(int dx, int dy) {
  dx = dx < 0 ? -dx : dx; dy = dy < 0 ? -dy : dy;
  const int a = dx > dy ? dx : dy, b = dx > dy ? dy : dx;
  return a + (b * 3) / 8;
}

static void draw_rotor_field(uint32_t t_ms, uint16_t tempo) {
  static const int8_t ZX[3] = {32, 10, 54};
  static const int8_t ZY[3] = {24, 12, 36};
  const uint32_t t = t_ms / tempo;
  for (int y = 0; y < BIPER_OLED_H; y++)
    for (int x = 0; x < BIPER_OLED_W; x++) {
      int v = 0;
      for (int i = 0; i < 3; i++)
        v += SIN8[(uint8_t)(dist_int(x - ZX[i], y - ZY[i]) * 6 - t)];
      v = v * 3 / 5;
      if ((int)((v + 384) / 48) > (int)BAYER4[y & 3][x & 3])
        scr->drawPixel(x, y, SSD1306_WHITE);
    }
}

// Priority on the panel: a message waits -> the envelope; something happens on
// the radio -> that state's character for the duration of the event; calm ->
// the animation picked by clicking.
static void draw_face(BiperPage page) {
  const uint32_t t = millis();
  if (biper_msg_waiting()) { draw_wave_field(t, false); envelope(); return; }

  if (biper_face != FACE_ZYJE) {
    const BiperCharacter& ch = CHARACTER[(int)biper_face];
    if      (ch.kind == FIELD_ROTOR)  draw_rotor_field(t, ch.tempo);
    else if (ch.kind == FIELD_RADIAL) draw_radial_field(t, ch.tempo);
    else                              draw_wave_field(t, ch.negative, ch.tempo, ch.threshold);
    if (ch.word != nullptr)
      label_on_field(ch.word, ch.negative ? SSD1306_BLACK : SSD1306_WHITE);
    return;
  }

  // AT REST both animations carry BIPER. The word changes only when something
  // happens on the radio — the home screen is no place for a message.
  const uint16_t tempo = resting_tempo();
  if (page == PAGE_RADIAL) draw_radial_field(t, tempo);
  else                     draw_wave_field(t, false, tempo);
  label_on_field("BIPER", SSD1306_WHITE);
}

// This screen is read in ONE situation: a person holds their phone and copies
// the password. So the password is the hero — two groups of four digits,
// font 2x.
//
// 18.08, owner's correction: the seconds counter stood as text at y=41, and the
// second group of digits (font 2x from y=29) reaches y=44. Four lines of pixels
// ran over each other and the password digits became unreadable — 48 pixels of
// height have no room for 8+16+16+8. The window time therefore goes down to a
// bar at the bottom edge: with no unit, because "still plenty / nearly gone" is
// the whole content needed here. The guest counter (g:N) was removed — an
// abbreviation with no legend that nobody will read.
static void draw_network_page() {
  const BiperApState& st = biper_ap_get_state();

  if (!st.active) {
    draw_header("SIEC");
    draw_text(0, 13, screen_line("hotspot"));
    draw_text(0, 23, screen_line("3s = wlacz"));
    // The radio's working mode stands HERE, not only at the moment of
    // switching: a switch without an indicator is worse than no switch, because
    // the person does not know which state they left the cube in. This is the
    // only page with room for it. Apart from the hotspot, with a gap: it is a
    // different subject than the Wi-Fi above it.
    draw_text(0, 38, biper_forwarding() ? screen_line("radio SIEC") : screen_line("radio SAM"));
    return;
  }

  draw_centered(0, st.ssid);
  scr->drawFastHLine(0, 9, BIPER_OLED_W, SSD1306_WHITE);

  if (st.pass[0]) {
    char grp[6];
    scr->setTextSize(2);
    memcpy(grp, st.pass, 4); grp[4] = 0;
    draw_centered(12, grp);          // 12..27
    memcpy(grp, st.pass + 4, 4); grp[4] = 0;
    draw_centered(28, grp);          // 28..43
    scr->setTextSize(1);
  } else {
    draw_centered(20, "otwarta");
  }

  // Window bar: full at the start, shrinking to the left. The last minute turns
  // it into a dashed line across the full width — 60 seconds is only 6 px of
  // bar, so the bar itself would stop being visible exactly when it carries the
  // most. No blinking: the page refreshes at 1 Hz and the blink phase would
  // fall into the refresh rhythm.
  if (st.window_total_s > 0) {
    const int y = BIPER_OLED_H - 3;
    if (st.window_left_s <= 60) {
      for (int x = 0; x < BIPER_OLED_W; x += 2)
        scr->drawFastVLine(x, y, 3, SSD1306_WHITE);
    } else {
      int w = (int)(((uint32_t)st.window_left_s * (uint32_t)BIPER_OLED_W) /
                    st.window_total_s);
      if (w > BIPER_OLED_W) w = BIPER_OLED_W;
      if (w > 0) scr->fillRect(0, y, w, 3, SSD1306_WHITE);
    }
  }
}

// INFO is a FIELD page, not a developer page: it answers the questions asked of
// the cube when no phone is at hand — who is audible, and what it transmits on —
// plus the session BLE pin needed to pair the app.
// The library 5x7 font carries no Polish glyphs, which is why this page (and
// only this page) spells SLYSZE without its diacritic.
uint16_t biper_batt_mv();
uint8_t biper_heard_15min();

static void draw_info_page() {
  const BiperApState& st = biper_ap_get_state();
  char line[20];

  draw_header("INFO");

  // BAT is gone: measured on DEV 18.08 — biper_batt_mv() returns 0 mV, because
  // the C6L has neither a battery NOR a way to measure one (doc 29: "USB-C 5 V;
  // no battery"). The line showed a number that looked like a measurement and
  // was not one.
  snprintf(line, sizeof(line), "SLYSZE %u", (unsigned)biper_heard_15min());
  draw_text(0, 14, line);

#if defined(BLE_PIN_CODE)
  // The BLE pin is shown only in a PAIRING WINDOW: the first three minutes
  // after power-on. Outside it, the line reads BLE ------.
  //
  // It used to sit here permanently, three clicks away, always. Bonding in LE
  // Secure Connections is durable, so whoever picked the cube up off a table for
  // half a minute could pair their own phone and put it back. Nothing on the
  // cube would ever say so. A pairing pin exists for the moment of pairing;
  // afterwards it is only a key left in the lock.
  // Czlon "|| st.active" wylecial przy autostarcie (rewident, 20.08): okno
  // hotspotu jest odtad otwarte od 8. sekundy i potrafi zyc caly wieczor,
  // wiec "PIN takze przy otwartym oknie" znaczylo "PIN zawsze" — dokladnie
  // stan, ktory ten warunek mial zlikwidowac. Rollover-safe przez millis()
  // wprost: po ~49,7 dnia PIN blysnie na trzy minuty — koszt akceptowalny.
  const bool pairing_window = millis() < BIPER_PIN_WINDOW_MS;
  if (pairing_window) {
    snprintf(line, sizeof(line), "BLE %06lu", (unsigned long)st.ble_pin);
  } else {
    snprintf(line, sizeof(line), "BLE ------");
  }
  draw_text(0, 26, line);
#else
  // Wydanie wifi_only (decyzja wlasciciela 0.9.0, 20.08): BLE nie istnieje,
  // wiec i wiersz pinu nie istnieje. Nic go nie zastepuje — preset radia
  // zszedl z tej strony juz 19.08 (identyczny na kazdej kostce, odpowiadal
  // na pytanie, ktorego nikt nie zadaje), a pusty wiersz to nie brak tresci,
  // tylko mniej szumu.
  (void)st;
#endif

// The bottom line used to show the radio preset (869.6 SF8) — identical on
  // every cube by design, so it answered a question nobody asked. What the
  // owner actually needs on the device is WHICH SYSTEM RUNS HERE: with several
  // cubes and several releases a day, versions blur (owner, 19.08 evening).
  snprintf(line, sizeof(line), "OS %s", BIPER_LAYER_VERSION);
  draw_text(0, 38, line);
}

static void draw_page() {
  scr->clearDisplay();
  switch (biper_page) {
    case PAGE_WAVE:
    case PAGE_RADIAL: draw_face(biper_page); break;
    case PAGE_NETWORK: draw_network_page(); break;
    case PAGE_INFO:    draw_info_page(); break;
    default: break;
  }
  scr->display();
}

// Which pages carry motion (redrawn every tick, ~30 fps, instead of 1 Hz).
// Motion only where it CARRIES STATE: ZYJE breathes, NADAJE and POMOC travel.
// The static states (CZEKAM, DOSZLO, CISZA) refresh at 1 Hz — less TX over SPI
// and less current, and the shape carries the whole message anyway.
static bool page_is_animated() {
  if (!page_is_face() || biper_msg_waiting()) return false;
  if (biper_face == FACE_CZEKAM || biper_face == FACE_DOSZLO ||
      biper_face == FACE_CISZA) return false;   // static states: 1 Hz
  return true;                                   // ZYJE, NADAJE, POMOC: 30 fps
}

// Ninja mode (double click): no sound, no LED, screen at minimum brightness.
// Survival feature — night use, staying unnoticed. Shows a short toast so the
// user sees which way the switch went.
// Ninja mode TURNS THE PANEL OFF, it does not merely dim it. Contrast 0x01 still
// emits light, and in a dark room that is exactly the light the user asked us to
// remove — the mode exists to make the cube unnoticeable in a room, and a person
// who double-clicks in the dark is not asking for "dimmer".
//
// A single click wakes the panel for a few seconds so the state stays readable,
// then it goes dark again. Sound and LED stay off the whole time (BiperFeedback).
// What ninja does NOT do is stop the radio, and the website now says so.
static uint32_t ninja_wake_until = 0;

static void ninja_panel(bool on) {
  scr->ssd1306_command(on ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
}

static uint32_t toast_until = 0;         // 0 = brak toastu
static bool toast_clear_wake = false;
static bool toast_restore_ninja = false;

static void toggle_ninja_mode() {
  const bool on = !biper_ninja();
  biper_set_ninja(on);
  scr->ssd1306_command(SSD1306_SETCONTRAST);
  scr->ssd1306_command(on ? BIPER_OLED_CONTRAST_NINJA : BIPER_OLED_CONTRAST_NORMAL);
  ninja_panel(true);                       // the toast has to be readable
  scr->clearDisplay();
  scr->setTextSize(1);
  draw_centered(20, on ? "NINJA" : "NINJA OFF");
  scr->display();
  Serial.printf("[BIPER] ninja=%d\n", (int)on);
  // Toast jako STAN z deadlinem, nie vTaskDelay: 800 ms blokady zjadalo
  // klikniecia (oba zbocza miedzy pollingami) i mrozilo feedback (Kimi A-11).
  toast_until = millis() + BIPER_SCR_TOAST_MS;
  toast_clear_wake = true;
  toast_restore_ninja = on;
}

// Forwarding other people's packets: SIEC <-> SAM, by triple click.
// Both modes are equivalent and both named positively — this is not "normal"
// and "crippled", but two answers to the question whether the cube also works
// for the neighbours. SIEC is the default. The screen says which mode is on,
// because without that the switch would be worse than no switch: the person
// would not know which state they left the device in.
static void toggle_forwarding() {
  const bool forwarding = biper_forwarding_toggle();
  biper_feedback_gesture();
  ninja_panel(true);
  scr->clearDisplay();
  scr->setTextSize(1);
  draw_centered(12, forwarding ? "SIEC" : "SAM");
  // "SWOJE", nie "TYLKO SWOJE": 11 znakow x 6 px = 66 px > 64 px ekranu,
  // draw_centered ucina krawedzie bez zadnego bledu (audyt Kimi B-13.7).
  draw_centered(30, forwarding ? "PRZEKAZUJE" : "SWOJE");
  scr->display();
  toast_until = millis() + BIPER_SCR_TOAST_MS;   // patrz toggle_ninja_mode
  toast_restore_ninja = biper_ninja();
}

// Any button press in ninja mode lights the panel for BIPER_NINJA_WAKE_MS.
static void ninja_wake() {
  if (!biper_ninja()) return;
  ninja_wake_until = millis() + BIPER_NINJA_WAKE_MS;
  ninja_panel(true);
}

static bool wiping = false;

// ── WIPE ON DEMAND ──────────────────────────────────────────────────────────
// Holding the button ten seconds erases the identity, contacts and channels.
//
// Why: without Secure Boot and flash encryption a seized cube gives up
// EVERYTHING — the private key lies in SPIFFS as raw 32 bytes and comes off
// over a cable in a quarter of an hour, without a trace. Whoever has that key
// reads the owner's messages, transmits in their name and holds the channel
// secret of the whole group. A wipe does not fix that boundary, but it gives a
// person who SEES they are about to lose the cube something to press. Until
// 18.08 there was nothing to press.
//
// We inject the frame down the same path the panel uses (SPSC → the mesh loop),
// so no further hook appears in main.cpp — the wipe fits inside our layer.
static void wipe_everything() {
  static const uint8_t FRAME[] = {51, 'r', 'e', 's', 'e', 't'};  // CMD_FACTORY_RESET
  // The frame goes into the SPSC queue and the MESH loop erases and reboots us
  // asynchronously. Until 19.08 this function drew one static frame and the
  // screen task then fell back to normal pages for a moment before the reboot
  // hit — the owner read it as "it said wiping, then changed its mind". The
  // `wiping` latch keeps the wipe animation on screen until the lights go out.
  Serial.printf("[BIPER] wipe requested from button\n");
  // Ramka NAJPIERW, zatrzask POTEM: pelny ring RX odrzuca ramke po cichu,
  // a zatrzask `wiping` trzymalby ekran "WYMAZUJE" na zawsze przy
  // nietknietych danych (audyt Kimi A-12). Ring drenuje petla mesh co
  // przebieg, wiec kilka krotkich prob wystarcza za caly backoff.
  // Ring LOKALNY: przejecie sesji panelu w zlym momencie plukalo ring sesyjny
  // razem z ramka wymazania — biper_ap_forget() zdazyl skasowac nasze klucze,
  // a wlasciwy factory reset MeshCore nigdy nie ruszal i ekran wisial na
  // "WYMAZUJE" przy zywej tozsamosci (rewident, 20.08).
  bool sent = false;
  for (int i = 0; i < 8 && !sent; i++) {
    sent = biper_ap_interface()->onLocalCommand(FRAME, sizeof(FRAME));
    if (!sent) vTaskDelay(pdMS_TO_TICKS(25));
  }
  if (!sent) {
    // Bez ekranu (padniety OLED, F-02) odmowe slychac zamiast widac.
    if (scr != nullptr) {
      scr->clearDisplay();
      scr->setTextSize(1);
      draw_centered(14, "ZAJETE");
      draw_centered(26, "SPROBUJ ZNOWU");
      scr->display();
      toast_until = millis() + BIPER_SCR_TOAST_MS;
    } else {
      biper_feedback_click();
    }
    return;
  }
  wiping = true;
  // Przyjecie wymazania potwierdzamy takze dzwiekiem/LED — na kostce bez
  // dzialajacego ekranu to jedyny dowod, ze gest zadzialal (Codex F-02).
  biper_feedback_gesture();
  // MeshCore's factory reset knows nothing about our storage (the SIEC/SAM
  // bit, the fixed hotspot password) — we clear it ourselves, so a wiped cube
  // really does come up like new.
  biper_ap_forget();
}

// Draws the countdown; returns true when the wipe has to happen.
static bool wipe_countdown(uint32_t held_ms) {
  if (held_ms < BIPER_WIPE_FROM_MS) return false;
  if (held_ms >= BIPER_WIPE_MS) return true;
  const unsigned remaining = (BIPER_WIPE_MS - held_ms + 999) / 1000;
  char line[16];
  scr->clearDisplay();
  scr->setTextSize(1);
  draw_centered(6, "PUSC ABY");
  draw_centered(16, "ANULOWAC");
  snprintf(line, sizeof(line), "WYMAZ %u", remaining);
  draw_centered(32, line);
  scr->display();
  return false;
}

static bool screen_init() {
  SPIClass* spi = biper_radio_spi();
  if (spi == nullptr) {
    Serial.printf("[BIPER_SCR] no radio SPI, screen off\n");
    return false;
  }
  static Adafruit_SSD1306 display(BIPER_OLED_W, BIPER_OLED_H, spi,
                                  BIPER_OLED_DC, BIPER_OLED_RST, BIPER_OLED_CS);
  scr = &display;
  scr->setRotation(2);  // panel mounted upside-down (working reference)
  // periphBegin=false: the radio already began this SPI bus.
  if (!scr->begin(SSD1306_SWITCHCAPVCC, 0, true, false)) {
    Serial.printf("[BIPER_SCR] begin FAILED\n");
    scr = nullptr;
    return false;
  }
  // 64x48 fix: alternative COM pins, or the output is garbled/dark.
  scr->ssd1306_command(SSD1306_SETCOMPINS);
  scr->ssd1306_command(0x12);
  scr->setTextColor(SSD1306_WHITE);
  scr->setTextWrap(false);
  Serial.printf("[BIPER_SCR] init done, heap=%lu\n",
                (unsigned long)ESP.getFreeHeap());
  return true;
}

static void biper_screen_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(BIPER_SCR_BOOT_DELAY_MS));

  // Przycisk i feedback PRZED ekranem i niezaleznie od jego wyniku: padnieta
  // tasma OLED odbierala kostce cale HMI (gesty hotspotu, ninja, wymazania,
  // buzzer, LED), choc ekspander i buzzer to osobny sprzet, a mesh i hotspot
  // dzialaja dalej (audyt Kimi A-07).
  const bool button_ok = biper_button_init();
  biper_feedback_init();
  biper_feedback_boot();

  if (!screen_init()) {
    // Bez ekranu petla dalej obsluguje przycisk i feedback — tylko nie rysuje.
    // Gest 10 s dziala takze TUTAJ (audyt Codexa F-02): padnieta tasma OLED nie
    // moze odbierac czlowiekowi jedynej lokalnej drogi wyczyszczenia kostki.
    // Odliczania nie widac, wiec je SLYCHAC: klik co sekunde od 4. sekundy
    // trzymania, potem melodia gestu potwierdza przyjecie wymazania.
    uint32_t last_tick_s = 0;
    bool hold_toggled = false;
    for (;;) {
      if (button_ok && !wiping) {
        const BiperButtonEvent ev = biper_button_poll();
        const uint32_t held_ms = biper_button_held_ms();
        if (ev == BIPER_BTN_PRESS) hold_toggled = false;
        if (held_ms >= BIPER_WIPE_FROM_MS) {
          // Jak w petli z ekranem: dojscie do strefy WYMAZ cofa przelaczenie
          // okna odpalone w 3. sekundzie tego samego ucisku.
          if (hold_toggled) { hold_toggled = false; biper_ap_request_toggle(); }
          if (held_ms >= BIPER_WIPE_MS) {
            wipe_everything();
          } else if (held_ms / 1000 != last_tick_s) {
            last_tick_s = held_ms / 1000;
            biper_feedback_click();
          }
        } else {
          last_tick_s = 0;
          if (ev == BIPER_BTN_HOLD) { biper_feedback_gesture(); biper_ap_request_toggle(); hold_toggled = true; }
          else if (ev == BIPER_BTN_DOUBLE) biper_set_ninja(!biper_ninja());
          else if (ev == BIPER_BTN_TRIPLE) { biper_feedback_gesture(); biper_forwarding_toggle(); }
        }
      }
      biper_feedback_tick(biper_ap_get_state().active, millis());
      vTaskDelay(pdMS_TO_TICKS(BIPER_SCR_TICK_MS));
    }
  }
  // Boot goes straight into the living face — no intro animation (owner's call).

  uint32_t last_draw = 0;
  bool prev_active = false;
  bool prev_msg = false;
  uint8_t prev_guests = 0;
  // Czy 3-sekundowy gest hotspotu odpalil sie w BIEZACYM ucisku — do cofniecia,
  // gdy ten sam nieprzerwany ucisk dojedzie do strefy wymazania.
  bool hold_toggled = false;
  for (;;) {
    if (wiping) {
      // Erasing: the fastest rings the cube can draw (faster than POMOC),
      // carrying the word, until the factory reset reboots us.
      scr->clearDisplay();
      draw_radial_field(millis(), 2);
      label_on_field("WYMAZUJE", SSD1306_WHITE);
      scr->display();
      biper_feedback_tick(false, millis());   // melodia/LED zyja do konca
      vTaskDelay(pdMS_TO_TICKS(BIPER_SCR_TICK_MS));
      continue;
    }
    bool redraw_now = false;
    const uint32_t now = millis();

    // Ninja: the panel goes dark again once the wake window closes.
    if (biper_ninja() && ninja_wake_until != 0 && (int32_t)(now - ninja_wake_until) >= 0) {
      ninja_wake_until = 0;
      ninja_panel(false);
    }

    if (button_ok) {
      // POLL BEFORE YOU MEASURE HOW LONG IT IS HELD. Until 18.08 the order was
      // the other way round and the countdown branch ended in `continue` — so
      // from the fourth second on, the button was not polled ONCE.
      // `biper_btn_was_down` is cleared only in `biper_button_poll()`, so
      // RELEASING THE BUTTON WAS NEVER NOTICED: the counter kept rising and in
      // the tenth second the cube wiped itself, even though the person had let
      // go in the fifth.
      //
      // All that time the screen said PUSC ABY ANULOWAC, and the panel guide
      // promised the same. That was untrue about the one gesture in this device
      // that cannot be undone.
      const BiperButtonEvent ev = biper_button_poll();
      const uint32_t held_ms = biper_button_held_ms();
      if (held_ms >= BIPER_WIPE_FROM_MS) {
        ninja_panel(true);          // the countdown must show in ninja too
        // Ten sam nieprzerwany ucisk odpalil juz gest hotspotu w 3. sekundzie.
        // Kto doszedl do odliczania, nie chcial przelaczac okna — cofamy tamto
        // przelaczenie, wiec przerwane wymazanie zostawia kostke w stanie
        // sprzed calego gestu (wlasciciel, 20.08: "co drugi raz nie wlacza").
        if (hold_toggled) { hold_toggled = false; biper_ap_request_toggle(); }
        if (wipe_countdown(held_ms)) { wipe_everything(); continue; }
        // Tick feedbacku takze w odliczaniu: bez niego LED i melodia
        // zamieraly na cale 6 s trzymania przycisku (Kimi A-11).
        biper_feedback_tick(biper_ap_get_state().active, millis());
        vTaskDelay(pdMS_TO_TICKS(BIPER_SCR_TICK_MS));
        continue;                   // the countdown outranks drawing pages
      }
      if (ev != BIPER_BTN_NONE) ninja_wake();
      if (ev == BIPER_BTN_PRESS) {
        // A tick on EVERY press, before it is known how many there will be.
        // The person clicking then hears that the cube is counting and knows
        // when to add the third. The action itself comes after the window
        // closes, below.
        hold_toggled = false;       // nowy ucisk = czysta historia gestu
        biper_feedback_click();
      } else if (ev == BIPER_BTN_CLICK) {
        if (biper_msg_waiting()) {
          biper_msg_clear();  // first click acknowledges the bubble
        } else {
          biper_page = (BiperPage)((biper_page + 1) % PAGE_COUNT);
        }
        redraw_now = true;
      } else if (ev == BIPER_BTN_DOUBLE) {
        toggle_ninja_mode();
        redraw_now = true;
      } else if (ev == BIPER_BTN_TRIPLE) {
        toggle_forwarding();
        redraw_now = true;
      } else if (ev == BIPER_BTN_HOLD) {
        biper_feedback_gesture();
        biper_ap_request_toggle();
        hold_toggled = true;        // do cofniecia, gdyby ucisk doszedl do WYMAZ
        biper_page = PAGE_NETWORK;
        redraw_now = true;
      }
    }

    // Sound + light on AP state transitions and on a guest joining.
    const BiperApState& st = biper_ap_get_state();
    if (st.active && !prev_active) biper_feedback_ap_on();
    if (!st.active && prev_active) biper_feedback_ap_off();
    if (st.guests > prev_guests) biper_feedback_guest();
    const bool msg = biper_msg_waiting();
    if (msg && !prev_msg) { biper_feedback_msg(); redraw_now = true; }
    if (!msg && prev_msg) redraw_now = true;
    prev_msg = msg;
    prev_active = st.active;
    prev_guests = st.guests;
    biper_feedback_tick(st.active, now);

    biper_face_expire(now);

    // Toast trzyma ekran, ale NIE wstrzymuje przycisku ani feedbacku —
    // wszystko powyzej w tej iteracji juz sie wykonalo.
    if (toast_until) {
      if ((int32_t)(now - toast_until) < 0) {
        vTaskDelay(pdMS_TO_TICKS(BIPER_SCR_TICK_MS));
        continue;
      }
      toast_until = 0;
      if (toast_clear_wake) { toast_clear_wake = false; ninja_wake_until = 0; }
      if (toast_restore_ninja) { toast_restore_ninja = false; ninja_panel(false); }
      redraw_now = true;
    }

    // Animated pages redraw every tick (~30 fps); static ones at 1 Hz.
    if (redraw_now || page_is_animated() || now - last_draw >= BIPER_SCR_REFRESH_MS) {
      last_draw = now;
      draw_page();
    }
    vTaskDelay(pdMS_TO_TICKS(BIPER_SCR_TICK_MS));
  }
}

void biper_screen_start() {
  // Bez tego zadania nie ma ekranu, gestow ani feedbacku. Zawiedziona alokacja
  // ma zostawic slad w logu zamiast udawac, ze wszystko gra (Codex F-14);
  // mesh i mostek panelu dzialaja dalej bez nas.
  if (xTaskCreate(biper_screen_task, "biper_scr", 4096, NULL, 1, NULL) != pdPASS)
    Serial.printf("[BIPER_SCR] task create FAILED, heap=%lu\n",
                  (unsigned long)ESP.getFreeHeap());
}

#endif  // BIPER_AP && BIPER_SCREEN
