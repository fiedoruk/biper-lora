// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#pragma once

// CZYSTA logika prawdy doreczen — zero Arduino, zero FreeRTOS, zero sprzetu.
// Ten plik kompiluje sie takze na hoscie i jest jedynym kawalkiem warstwy
// Biper objetym testem natywnym (test/test_biper_faces). Wszystko, co dotyka
// ekranu, socketow albo zegara, zostaje w BiperScreen.cpp / BiperApInterface.cpp
// i podaje tu czas jako parametr.

#include <stdint.h>
#include <string.h>

// Tablica OCZEKIWANYCH potwierdzen. RESP_SENT niesie 4-bajtowy znacznik
// (firmware: "app needs to match this to RESP_CODE_SENT.tag"), PUSH_CONFIRMED
// niesie ten sam znacznik. DOSZLO wolno pokazac wylacznie, gdy potwierdzenie
// trafia w znacznik, na ktory naprawde czekamy — spoznione, zdublowane albo
// cudze potwierdzenie ma zostac zignorowane (audyt Codexa F-03/F-19).
class BiperPendingTags {
public:
  static const int MAX_TAGS = 8;
  // Rowna EXPIRE_CZEKAM_MS: po minucie ekran i tak wraca do ZYJE, wiec
  // potwierdzenie starsze niz minuta nie ma juz czego domykac.
  static const uint32_t TAG_TTL_MS = 60000;

  // Rejestruje znacznik. Pelna tablica wypycha NAJSTARSZY wpis — to on
  // pierwszy straci waznosc, a gubic wolno tylko na korzysc nowszych wysylek.
  void add(const uint8_t tag[4], uint32_t now) {
    sweep(now);
    int slot = -1;
    for (int i = 0; i < MAX_TAGS; i++)
      if (!slots_[i].used) { slot = i; break; }
    if (slot < 0) {
      slot = 0;
      for (int i = 1; i < MAX_TAGS; i++)
        if (slots_[i].ts < slots_[slot].ts) slot = i;
    }
    memcpy(slots_[slot].tag, tag, 4);
    slots_[slot].ts = now;
    slots_[slot].used = true;
  }

  // true = trafienie w zywy znacznik (wpis znika, DOSZLO jest zasluzone).
  bool confirm(const uint8_t tag[4], uint32_t now) {
    sweep(now);
    for (int i = 0; i < MAX_TAGS; i++)
      if (slots_[i].used && memcmp(slots_[i].tag, tag, 4) == 0) {
        slots_[i].used = false;
        return true;
      }
    return false;
  }

  int count(uint32_t now) {
    sweep(now);
    int n = 0;
    for (int i = 0; i < MAX_TAGS; i++)
      if (slots_[i].used) n++;
    return n;
  }

  void clear() {
    for (int i = 0; i < MAX_TAGS; i++) slots_[i].used = false;
  }

private:
  struct Slot { uint8_t tag[4]; uint32_t ts; bool used; };
  Slot slots_[MAX_TAGS] = {};
  void sweep(uint32_t now) {
    for (int i = 0; i < MAX_TAGS; i++)
      if (slots_[i].used && now - slots_[i].ts > TAG_TTL_MS) slots_[i].used = false;
  }
};

// Czy okno hotspotu wolno odswiezyc? Sama skojarzona stacja to NIE obecnosc
// czlowieka: zapamietany laptop w zasiegu trzymalby AP w nieskonczonosc
// (audyt Codexa F-04). Okno podtrzymuje tylko ZYWY panel — otwarte gniazdo WS,
// ktore odezwalo sie nie dawniej niz activity_ms temu (panel pulsuje co minute).
inline bool biper_window_keepalive(uint8_t guests, int ws_fd, uint32_t now,
                                   uint32_t last_rx_ms, uint32_t activity_ms) {
  if (guests == 0) return false;
  if (ws_fd < 0) return false;
  if (last_rx_ms == 0) return false;          // klient nigdy sie nie odezwal
  return now - last_rx_ms < activity_ms;
}
