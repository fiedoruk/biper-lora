// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#pragma once

#include <helpers/BaseSerialInterface.h>  // also pulls in Arduino.h
#include <esp_http_server.h>

// Panel bridge: companion-protocol frames carried 1:1 as WebSocket BINARY
// messages (the WS layer does the framing, so no TCP-style length header).
// Single client at a time (panic UX: one phone). Data path:
//   ws handler (httpd task) -> SPSC rx ring -> checkRecvFrame (mesh loop)
//   writeFrame (mesh loop) -> SPSC tx ring -> httpd_queue_work -> ws send
class BiperApInterface : public BaseSerialInterface {
  struct Frame {
    uint16_t len;
    uint8_t gen;  // generacja sesji WS (TX): stara odpowiedz nie trafia do nowego telefonu
    uint8_t buf[MAX_FRAME_SIZE];
  };
  // 8, nie 4 (upstreamowa glebia TCP): sekwencja startowa panelu to SZESC
  // ramek pod rzad (APP_START, DEVICE_QUERY, SET_TIME, GET_BATT, GET_CONTACTS,
  // SYNC), a ring o glebi 4 ma uzyteczne TRZY sloty — nadmiar byl gubiony po
  // cichu, dopoki odpowiedz 0xB5 (F-05) nie zaczela tego uczciwie pokazywac:
  // Android widywal "KOSTKA ZAJETA" przy kazdym polaczeniu (biurko, 20.08).
  static const int QUEUE_SIZE = 8;

  // A ring is empty when head == tail and full when advancing head would land
  // on tail, so one slot always stays unused.
  static uint8_t nextSlot(uint8_t index) {
    return (uint8_t)((index + 1) % QUEUE_SIZE);
  }

  bool _enabled = false;
  // Rings. Kontrakt SPSC okazal sie fikcja (rewident, 20.08): do RX pisza
  // TRZY taski (httpd=panel, AP=adverty, ekran=WYMAZ), do indeksow TX siegal
  // takeover z httpd. Dlatego: (a) rozkazy LOKALNE maja WLASNY ring, ktorego
  // plukanie po takeover nie dotyka — wyplukanie zakolejkowanego WYMAZANIA
  // zostawialoby ekran "WYMAZUJE" przy nietknietych danych; (b) kazda operacja
  // na ringach idzie pod jednym mutexem (sekcje krotkie, C6 jednordzeniowy,
  // dziedziczenie priorytetow w FreeRTOS); (c) ramki TX niosa generacje sesji.
  void* _mtx = nullptr;  // SemaphoreHandle_t; tworzony w enable()
  Frame _rx[QUEUE_SIZE];
  volatile uint8_t _rx_head = 0, _rx_tail = 0;
  Frame _lo[4];  // rozkazy lokalne (WYMAZ, adverty) — nigdy nie plukane
  volatile uint8_t _lo_head = 0, _lo_tail = 0;
  volatile uint8_t _session_gen = 0;
  Frame _tx[QUEUE_SIZE];
  volatile uint8_t _tx_head = 0, _tx_tail = 0;
  // FIFO komend, ktore dostana RESP_CODE_SENT (2/26/27/39/52/57 wg MyMesh.cpp;
  // kanal 3 NIE dostaje). Bez tego RESP LOGIN-u zuzywal oczekiwanie DM-a
  // i potwierdzenie wlasciwej wiadomosci nie mialo w co trafic (weryfikacja
  // Codexa, 20.08). Pod tym samym mutexem co ringi.
  uint8_t _resp_fifo[8];
  volatile uint8_t _resp_head = 0, _resp_tail = 0;

public:
  // BaseSerialInterface
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _enabled; }
  bool isConnected() const override;
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;

  // Called from the httpd task (WS handler / close callback).
  // true = ramka przyjeta do ringu; false = pelny ring/oversize (odrzucona).
  // Panel ignoruje wynik (klient moze ponowic), ale WIPE z przycisku musi go
  // znac: cicho zgubiona ramka kasowania zostawialaby ekran 'WYMAZUJE' na
  // zawsze przy nietknietych danych (audyt Kimi A-12).
  bool onClientFrame(const uint8_t* payload, size_t len);
  // Rozkaz LOKALNY (WYMAZ z przycisku, adverty warstwy) — wlasny ring,
  // odporny na plukanie po przejeciu sesji panelu.
  bool onLocalCommand(const uint8_t* payload, size_t len);
  void drainTx();  // runs inside httpd context via httpd_queue_work
  void resetQueues();
  // Prog przejecia sesji: podbija generacje pod mutexem. Ramki RX i TX nosza
  // generacje z chwili powstania — stare komendy pomija konsument, stare
  // odpowiedzi gasna w drainTx. Zastepuje dawne plukanie do znacznika
  // (ktore i tak nie umialo cofnac komendy juz pobranej przez mesh) i nie
  // dotyka ringu lokalnego.
  void beginSession();
};

BiperApInterface* biper_ap_interface();

// WS session state lives in BiperApInterface.cpp; the httpd handlers in
// BiperAp.cpp drive it. One client at a time, so no id is needed.
void biper_ws_session_open(httpd_handle_t hd, int fd);
void biper_ws_session_close();
int biper_ws_fd_get();  // -1 when no client is connected

// Frame counters behind the [BIPER_WS] telemetry line.
uint32_t biper_ws_rx_count();
uint32_t biper_ws_tx_count();
// millis() ostatniej ramki OD klienta — okno AP odswieza sie tylko przy zywym
// panelu, nie przy samym skojarzeniu stacji (audyt Codexa F-04).
uint32_t biper_ws_last_rx_millis();
void biper_ws_note_rx();  // tylko handler WS; lokalne rozkazy NIE sa aktywnoscia

// New-message signal for the OLED faces. The interface reports isConnected()
// while enabled, so upstream sends the PUSH 0x83 "message waiting" tickle even
// with no phone attached; writeFrame observes it and raises this flag.
bool biper_msg_waiting();
void biper_msg_clear();
