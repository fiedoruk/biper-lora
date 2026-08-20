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
static httpd_handle_t volatile biper_ws_hd = nullptr;  // pisany z httpd, czytany z mesh — volatile jak fd
static volatile int biper_ws_fd = -1;

int biper_ws_fd_get() { return biper_ws_fd; }

void biper_ws_session_open(httpd_handle_t hd, int fd) {
  // Prog przejecia sesji: nowa generacja TX (odpowiedzi dla STAREGO telefonu
  // gasna w drainTx zamiast leciec do nowego — audyt Kimi B-05.1, bez ruszania
  // indeksow producenta) + plukanie ringu WS do znacznika (audyt Codexa F-06);
  // rozkazy LOKALNE maja wlasny ring i takeover ich nie dotyka.
  biper_iface.beginSession();
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
// Ramki KLIENTA odrzucone przez pelny ring RX — po WYSLIJ bez reakcji to
// jedyny slad, ze rozkaz w ogole nie wszedl (audyt Kimi B-05.4).
static volatile uint32_t biper_rx_dropped = 0;
uint32_t biper_ws_rx_dropped() { return biper_rx_dropped; }
uint32_t biper_ws_rx_count() { return biper_rx_frames; }
uint32_t biper_ws_tx_count() { return biper_tx_frames; }

// Kiedy klient OSTATNIO odezwal sie po WS. Okno hotspotu odswieza sie tylko
// przy zywym panelu, nie przy samym skojarzeniu stacji Wi-Fi — zapamietany
// laptop w kieszeni nie moze trzymac AP w nieskonczonosc (audyt Codexa F-04).
// Panel podtrzymuje sie sam pulsem GET_BATT co minute.
static volatile uint32_t biper_ws_last_rx = 0;
uint32_t biper_ws_last_rx_millis() { return biper_ws_last_rx; }
// Wolane WYLACZNIE z handlera WS po ramce od klienta — patrz onClientFrame.
void biper_ws_note_rx() { biper_ws_last_rx = millis(); }

static volatile bool biper_msg_flag = false;
bool biper_msg_waiting() { return biper_msg_flag; }
void biper_msg_clear() { biper_msg_flag = false; }

void BiperApInterface::enable() {
  // Mutex ringow — patrz komentarz w naglowku (trzej producenci RX).
  if (_mtx == nullptr) _mtx = (void*)xSemaphoreCreateMutex();
  _enabled = true;
}

void BiperApInterface::disable() {
  _enabled = false;
  resetQueues();
}

void BiperApInterface::resetQueues() {
  if (_mtx) xSemaphoreTake((SemaphoreHandle_t)_mtx, portMAX_DELAY);
  // Ring LOKALNY zostaje NIETKNIETY: WYMAZ przyjety sekunde przed zamknieciem
  // okna musi sie wykonac — zerowanie go tutaj zostawialo ekran "WYMAZUJE"
  // przy zywej tozsamosci (weryfikacja Codexa, 20.08). Rozkazy lokalne nie
  // naleza do zadnej sesji, wiec zamkniecie okna nie ma nad nimi wladzy.
  _rx_head = _rx_tail = 0;
  _tx_head = _tx_tail = 0;
  _resp_head = _resp_tail = 0;
  // Sesja gasnie POD mutexem: writeFrame czyta fd/hd tez pod nim, wiec po
  // wyjsciu stad zaden queue_work nie poleci na uchwyt, ktory AP zaraz
  // zwolni w httpd_stop (weryfikacja Kimi, 20.08).
  biper_ws_session_close();
  if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
}

void BiperApInterface::beginSession() {
  if (_mtx) xSemaphoreTake((SemaphoreHandle_t)_mtx, portMAX_DELAY);
  _session_gen = (uint8_t)(_session_gen + 1);
  // Ring TX pelny ramek STAREJ generacji potrafil sie zaklinowac na zawsze:
  // writeFrame widzial busy i nie zlecal drainTx, a tylko drainTx umial go
  // oproznic (weryfikacja Kimi, 20.08). Prog nowej sesji zeruje TX — pod
  // mutexem to legalne, producent (mesh) tez pisze pod nim.
  _tx_head = _tx_tail = 0;
  if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
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
  // State for the cube's screen. Delivery truth is matched BY TAG: RESP_SENT
  // carries a 4-byte expected-ack marker (offset 2) and PUSH_CONFIRMED carries
  // the same marker (offset 1). A confirmation that matches no awaited tag —
  // late, duplicated or belonging to another interface — must not light DOSZLO
  // on the screen (audyt Codexa F-03/F-19; ten sam mechanizm co w panelu).
  else if (src[0] == PUSH_CONFIRMED) biper_face_confirmed(len >= 5 ? &src[1] : nullptr);
  else if (src[0] == RESP_SENT) {
    // Czyj to RESP? FIFO z onClientFrame mowi, ktora komenda go wywolala —
    // tylko DM (2) rejestruje znacznik doreczenia; login/status/telemetria
    // przechodza bez sladu na twarzy (weryfikacja Codexa, 20.08).
    uint8_t expected = 0;
    if (_mtx) xSemaphoreTake((SemaphoreHandle_t)_mtx, portMAX_DELAY);
    if (_resp_tail != _resp_head) {
      expected = _resp_fifo[_resp_tail];
      _resp_tail = (uint8_t)((_resp_tail + 1) % 8);
    }
    if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
    if (expected == CMD_SEND_DM) biper_face_resp_sent(len >= 6 ? &src[2] : nullptr);
  }
  // Odczyt fd/hd, wpis do ringu i queue_work pod JEDNYM przebiegiem mutexa:
  // zamkniecie okna czysci hd i dopiero POTEM zatrzymuje httpd (tez pod
  // mutexem), wiec nie zdazymy zawolac queue_work na uchwycie, ktory za
  // mikrosekunde bedzie zwolniony (TOCTOU — weryfikacja Kimi, 20.08).
  if (_mtx) xSemaphoreTake((SemaphoreHandle_t)_mtx, portMAX_DELAY);
  const httpd_handle_t hd = biper_ws_hd;
  if (biper_ws_fd < 0 || hd == nullptr) {
    if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
    return len;  // no client: observed, not deliverable
  }
  if (isWriteBusy()) {
    if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
    return 0;
  }
  Frame& f = _tx[_tx_head];
  f.len = (uint16_t)len;
  f.gen = _session_gen;  // odpowiedz nalezy do TEJ sesji; po takeover gasnie
  memcpy(f.buf, src, len);
  _tx_head = nextSlot(_tx_head);
  // Marshal the actual socket write into the httpd task context.
  const bool queued = httpd_queue_work(hd, [](void*) { biper_iface.drainTx(); }, nullptr) == ESP_OK;
  if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
  if (!queued) {
    biper_ws_session_close();  // pelna kolejka httpd: inaczej most milczy do restartu (Kimi B-05.3)
  }
  return len;
}

void BiperApInterface::drainTx() {
  // Pod mutexem: resetQueues z zadania AP zeruje indeksy TX i bez blokady
  // moglby to zrobic w polowie naszego przebiegu (weryfikacja Codexa, 20.08).
  if (_mtx) xSemaphoreTake((SemaphoreHandle_t)_mtx, portMAX_DELAY);
  while (_tx_tail != _tx_head) {
    Frame& f = _tx[_tx_tail];
    // Ramka z poprzedniej sesji gasnie tu, zamiast leciec do nowego telefonu.
    if (biper_ws_fd >= 0 && f.gen == _session_gen) {
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
  if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
}

bool BiperApInterface::onClientFrame(const uint8_t* payload, size_t len) {
  if (!_enabled || len == 0 || len > MAX_FRAME_SIZE) return false;
  // UWAGA: biper_ws_last_rx aktualizuje WYLACZNIE handler WS (BiperAp.cpp).
  // Gdy robil to ten wspolny punkt, wlasny beacon co 10 min odswiezal
  // "aktywnosc panelu" i martwe gniazdo trzymalo hotspot wiecznie —
  // F-04 obchodzil sam siebie (rewident, 20.08).
  if (_mtx) xSemaphoreTake((SemaphoreHandle_t)_mtx, portMAX_DELAY);
  const uint8_t next = nextSlot(_rx_head);
  if (next == _rx_tail) {
    biper_rx_dropped = biper_rx_dropped + 1;
    if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
    return false;  // ring full
  }
  // The user pressed SEND: this is the only moment when we know about a
  // transmission before the radio answers. A public channel gets NO RESP_SENT
  // at all (MyMesh), so NADAJE simply expires back to ZYJE on its own.
  if (payload[0] == CMD_SEND_DM || payload[0] == CMD_SEND_CHANNEL) {
    biper_face_set(FACE_NADAJE);
  }
  // FIFO oczekiwanych RESP_CODE_SENT: tylko te komendy go dostana (MyMesh);
  // dzieki temu RESP LOGIN-u nie zuzywa oczekiwania DM-a. Pelne FIFO wypycha
  // najstarszy wpis — desynchronizacja konczy sie brakiem CZEKAM, nie klamstwem.
  switch (payload[0]) {
    case 2: case 26: case 27: case 39: case 52: case 57: {
      const uint8_t rn = (uint8_t)((_resp_head + 1) % 8);
      if (rn == _resp_tail) _resp_tail = (uint8_t)((_resp_tail + 1) % 8);
      _resp_fifo[_resp_head] = payload[0];
      _resp_head = rn;
      break;
    }
    default: break;
  }
  Frame& f = _rx[_rx_head];
  f.len = (uint16_t)len;
  f.gen = _session_gen;  // komenda nalezy do TEJ sesji; po takeover jest pomijana
  memcpy(f.buf, payload, len);
  _rx_head = next;
  biper_rx_frames = biper_rx_frames + 1;
  if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
  // Ten return BYL nieobecny, a funkcja deklaruje bool — przy globalnym `-w`
  // kompilator milczal i wynik byl przypadkiem z rejestru (audyt Codexa F-01).
  // WIPE z przycisku dziala tylko dzieki temu, ze ta wartosc jest prawdziwa.
  return true;
}

bool BiperApInterface::onLocalCommand(const uint8_t* payload, size_t len) {
  if (!_enabled || len == 0 || len > MAX_FRAME_SIZE) return false;
  if (_mtx) xSemaphoreTake((SemaphoreHandle_t)_mtx, portMAX_DELAY);
  const uint8_t next = (uint8_t)((_lo_head + 1) % 4);
  if (next == _lo_tail) {
    if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
    return false;  // pelny ring lokalny — wolajacy decyduje o ponowieniu
  }
  Frame& f = _lo[_lo_head];
  f.len = (uint16_t)len;
  memcpy(f.buf, payload, len);
  _lo_head = next;
  if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
  return true;
}

size_t BiperApInterface::checkRecvFrame(uint8_t dest[]) {
  if (_mtx) xSemaphoreTake((SemaphoreHandle_t)_mtx, portMAX_DELAY);
  // Rozkazy LOKALNE (WYMAZ, adverty) przed sesyjnymi — i poza generacjami.
  if (_lo_tail != _lo_head) {
    Frame& f = _lo[_lo_tail];
    size_t len = f.len;
    memcpy(dest, f.buf, len);
    _lo_tail = (uint8_t)((_lo_tail + 1) % 4);
    if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
    return len;
  }
  // Komendy STAREJ sesji (gen sprzed takeover) sa pomijane w konsumpcji —
  // to zastapilo plukanie do znacznika: dziala takze wtedy, gdy stara ramka
  // i ramki nowego klienta leza w ringu przemieszane (audyt Codexa F-06 +
  // weryfikacja 20.08). Komendy juz POBRANEJ zaden mechanizm nie cofnie —
  // jej odpowiedz moze dotrzec do nowego telefonu tego samego wlasciciela;
  // udokumentowane ograniczenie.
  while (_rx_tail != _rx_head && _rx[_rx_tail].gen != _session_gen)
    _rx_tail = nextSlot(_rx_tail);
  if (_rx_tail == _rx_head) {
    if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
    return 0;
  }
  Frame& f = _rx[_rx_tail];
  size_t len = f.len;
  memcpy(dest, f.buf, len);
  _rx_tail = nextSlot(_rx_tail);
  if (_mtx) xSemaphoreGive((SemaphoreHandle_t)_mtx);
  return len;
}

#endif  // BIPER_AP
