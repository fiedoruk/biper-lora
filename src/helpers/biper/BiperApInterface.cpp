// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#ifdef BIPER_AP

#include "BiperApInterface.h"
#include "BiperScreen.h"
#include "BiperAp.h"

#include <Arduino.h>
#include <esp_http_server.h>

// Companion protocol codes — verified in examples/companion_radio/MyMesh.cpp
// (lines 7, 8, 77, 114, 115, 122). Here they are only READ; we interpret nothing
// beyond the first byte, so an upstream change will at worst switch off the
// state preview, never break the transport.
static const uint8_t CMD_SEND_DM        = 2;     // CMD_SEND_TXT_MSG
static const uint8_t CMD_SEND_CHANNEL   = 3;     // CMD_SEND_CHANNEL_TXT_MSG
static const uint8_t RESP_SENT          = 6;     // RESP_CODE_SENT
static const uint8_t RESP_DM_OLD        = 7;     // RESP_CODE_CONTACT_MSG_RECV: [7][pub6]...
static const uint8_t RESP_DM_V3         = 16;    // ..._V3: [16][snr][r1][r2][pub6]...
static const uint8_t PUSH_CONFIRMED     = 0x82;  // PUSH_CODE_SEND_CONFIRMED
static const uint8_t PUSH_MSG_WAITING   = 0x83;  // PUSH_CODE_MSG_WAITING
static const uint8_t PUSH_ADVERT        = 0x80;  // PUSH_CODE_ADVERT: [0x80][pub_key]
static const uint8_t PUSH_NEW_ADVERT    = 0x8A;  // PUSH_CODE_NEW_ADVERT: [0x8A][pub_key][...]

// Who is audible. An advert carries a public key, so we count NODES, not
// frames — one neighbour transmitting twelve times is still one neighbour.
// A 15-minute window, the same as in the panel, so both surfaces tell the same
// truth.
static const uint32_t HEARD_WINDOW_MS = 15UL * 60UL * 1000UL;
static const uint8_t MAX_HEARD = 12;
struct HeardNode { uint8_t pfx[4]; uint32_t when; };
static HeardNode heard[MAX_HEARD];

static void note_advert(const uint8_t* pub) {
  const uint32_t now = millis();
  int free_slot = -1;
  for (int i = 0; i < MAX_HEARD; i++) {
    if (heard[i].when && memcmp(heard[i].pfx, pub, 4) == 0) {
      heard[i].when = now; return;                  // known: just refresh
    }
    if (free_slot < 0 && (heard[i].when == 0 ||
                      now - heard[i].when > HEARD_WINDOW_MS)) free_slot = i;
  }
  // All 12 slots are fresh: drop the OLDEST one. Overwriting slot 0 instead
  // looked harmless and was not: in a dense neighbourhood slot 0 was recycled
  // over and over while the other eleven froze, so "SLYSZE N" under-counted —
  // and that same counter drives the resting animation tempo, which then slowed
  // down in the densest network instead of speeding up. The one place the
  // indicator was supposed to mean something.
  if (free_slot < 0) {
    free_slot = 0;
    for (int i = 1; i < MAX_HEARD; i++)
      if (heard[i].when < heard[free_slot].when) free_slot = i;
  }
  memcpy(heard[free_slot].pfx, pub, 4);
  heard[free_slot].when = now;
}

uint8_t biper_heard_15min() {
  const uint32_t now = millis();
  uint8_t n = 0;
  for (int i = 0; i < MAX_HEARD; i++)
    if (heard[i].when && now - heard[i].when <= HEARD_WINDOW_MS) n++;
  return n;
}

static BiperApInterface biper_iface;
BiperApInterface* biper_ap_interface() { return &biper_iface; }

// Session state for the single WS client. Opened/closed from the httpd task
// by the handlers in BiperAp.cpp, read from the mesh loop — hence volatile fd.
static httpd_handle_t biper_ws_hd = nullptr;
static volatile int biper_ws_fd = -1;

int biper_ws_fd_get() { return biper_ws_fd; }

void biper_ws_session_open(httpd_handle_t hd, int fd) {
  biper_ws_hd = hd;
  biper_ws_fd = fd;
  Serial.printf("[BIPER_WS] client connected fd=%d\n", fd);
}

void biper_ws_session_close() {
  if (biper_ws_fd >= 0) Serial.printf("[BIPER_WS] client disconnected\n");
  // The socket and the server handle go out TOGETHER. Earlier only the socket
  // disappeared and `biper_ws_hd` stayed — the one thing keeping us from using a
  // stale handle was the order of the two lines below (`fd < 0` checked first).
  // That kind of protection vanishes with the first reordering nobody connects
  // to this place.
  biper_ws_fd = -1;
  biper_ws_hd = nullptr;
}

// Telemetry only; the AP window prints them on the [BIPER_WS] line.
static volatile uint32_t biper_rx_frames = 0;
static volatile uint32_t biper_tx_frames = 0;
uint32_t biper_ws_rx_count() { return biper_rx_frames; }
uint32_t biper_ws_tx_count() { return biper_tx_frames; }

static volatile bool biper_msg_flag = false;
bool biper_msg_waiting() { return biper_msg_flag; }
void biper_msg_clear() { biper_msg_flag = false; }

void BiperApInterface::disable() {
  _enabled = false;
  resetQueues();
}

void BiperApInterface::resetQueues() {
  _rx_head = _rx_tail = 0;
  _tx_head = _tx_tail = 0;
  biper_ws_session_close();
}

// Reported to the mesh, not the socket state: while enabled we always listen,
// so upstream keeps sending pushes (0x83) that light the OLED bubble even
// with no phone attached. Real client presence is biper_ws_fd.
bool BiperApInterface::isConnected() const { return _enabled; }

bool BiperApInterface::isWriteBusy() const {
  return nextSlot(_tx_head) == _tx_tail;  // tx ring full
}

size_t BiperApInterface::writeFrame(const uint8_t src[], size_t len) {
  if (!_enabled || len == 0 || len > MAX_FRAME_SIZE) return 0;
  if (src[0] == PUSH_MSG_WAITING) biper_msg_flag = true;
  // Both advert pushes carry the public key at offset 1. A node heard for the
  // FIRST time arrives as 0x8A, never 0x80 — counting only 0x80 meant the
  // counter missed the exact moment two fresh cubes met: each discovered the
  // other and both screens kept saying SLYSZE 0.
  else if ((src[0] == PUSH_ADVERT || src[0] == PUSH_NEW_ADVERT) && len >= 5) {
    note_advert(&src[1]);
    // A NEWLY discovered neighbour has us in range but almost certainly does
    // not have US yet — its one boot advert may have flown while we were
    // still flashing. Ask the AP task to answer with our own advert, so a
    // single advert in either direction completes the pair.
    if (src[0] == PUSH_NEW_ADVERT) biper_advert_reply_request();
  }
  // A direct message IS hearing its sender. The counter used to listen only to
  // adverts, so two cubes in mid-conversation could still show SLYSZE 0 — the
  // one moment the owner is SURE the neighbour is alive. Channel frames (8/17)
  // carry no public key and stay out; sync of an old offline backlog may
  // refresh a stale sender once, which self-corrects within one window.
  else if (src[0] == RESP_DM_OLD && len >= 7) note_advert(&src[1]);
  else if (src[0] == RESP_DM_V3 && len >= 10) note_advert(&src[4]);
  // State for the cube's screen. The guard (is this a consequence of OUR
  // transmission, and does a confirmation exist at all for this kind of send)
  // sits in biper_face_set() — here we only report what we saw in the frame.
  else if (src[0] == PUSH_CONFIRMED) biper_face_set(FACE_DOSZLO);
  else if (src[0] == RESP_SENT) biper_face_set(FACE_CZEKAM);
  if (biper_ws_fd < 0 || biper_ws_hd == nullptr) return len;  // no client: observed, not deliverable
  if (isWriteBusy()) return 0;
  Frame& f = _tx[_tx_head];
  f.len = (uint16_t)len;
  memcpy(f.buf, src, len);
  _tx_head = nextSlot(_tx_head);
  // Marshal the actual socket write into the httpd task context.
  httpd_queue_work(biper_ws_hd, [](void*) { biper_iface.drainTx(); }, nullptr);
  return len;
}

void BiperApInterface::drainTx() {
  while (_tx_tail != _tx_head) {
    Frame& f = _tx[_tx_tail];
    if (biper_ws_fd >= 0) {
      httpd_ws_frame_t ws = {};
      ws.type = HTTPD_WS_TYPE_BINARY;
      ws.payload = f.buf;
      ws.len = f.len;
      if (httpd_ws_send_frame_async(biper_ws_hd, biper_ws_fd, &ws) == ESP_OK) {
        biper_tx_frames = biper_tx_frames + 1;
      } else {
        biper_ws_session_close();
      }
    }
    _tx_tail = nextSlot(_tx_tail);
  }
}

void BiperApInterface::onClientFrame(const uint8_t* payload, size_t len) {
  if (!_enabled || len == 0 || len > MAX_FRAME_SIZE) return;
  const uint8_t next = nextSlot(_rx_head);
  if (next == _rx_tail) return;  // ring full: drop (client can resend)
  // The user pressed SEND: this is the only moment when we know about a
  // transmission before the radio answers. A public channel gets no delivery
  // confirmation, so after RESP_SENT it goes back to ZYJE instead of getting
  // stuck on CZEKAM.
  if (payload[0] == CMD_SEND_DM || payload[0] == CMD_SEND_CHANNEL) {
    biper_face_sent(payload[0] == CMD_SEND_DM);
    biper_face_set(FACE_NADAJE);
  }
  Frame& f = _rx[_rx_head];
  f.len = (uint16_t)len;
  memcpy(f.buf, payload, len);
  _rx_head = next;
  biper_rx_frames = biper_rx_frames + 1;
}

size_t BiperApInterface::checkRecvFrame(uint8_t dest[]) {
  if (_rx_tail == _rx_head) return 0;
  Frame& f = _rx[_rx_tail];
  size_t len = f.len;
  memcpy(dest, f.buf, len);
  _rx_tail = nextSlot(_rx_tail);
  return len;
}

#endif  // BIPER_AP
