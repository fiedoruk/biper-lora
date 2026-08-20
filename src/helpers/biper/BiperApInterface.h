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
    uint8_t buf[MAX_FRAME_SIZE];
  };
  static const int QUEUE_SIZE = 4;  // same depth as upstream's TCP interface

  // A ring is empty when head == tail and full when advancing head would land
  // on tail, so one slot always stays unused.
  static uint8_t nextSlot(uint8_t index) {
    return (uint8_t)((index + 1) % QUEUE_SIZE);
  }

  bool _enabled = false;
  // Rings: single writer + single reader each, lock-free via volatile indices.
  Frame _rx[QUEUE_SIZE];
  volatile uint8_t _rx_head = 0, _rx_tail = 0;
  Frame _tx[QUEUE_SIZE];
  volatile uint8_t _tx_head = 0, _tx_tail = 0;

public:
  // BaseSerialInterface
  void enable() override { _enabled = true; }
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
  void drainTx();  // runs inside httpd context via httpd_queue_work
  void resetQueues();
  // Zeruje SAM ring TX — na progu przejecia sesji (patrz biper_ws_session_open).
  void dropTxQueue() { _tx_head = _tx_tail = 0; }
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

// New-message signal for the OLED faces. The interface reports isConnected()
// while enabled, so upstream sends the PUSH 0x83 "message waiting" tickle even
// with no phone attached; writeFrame observes it and raises this flag.
bool biper_msg_waiting();
void biper_msg_clear();
