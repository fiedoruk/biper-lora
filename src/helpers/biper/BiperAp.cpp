// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
#ifdef BIPER_AP

#include "BiperAp.h"
#include "BiperApInterface.h"
#include "BiperScreen.h"
#include "BiperVersion.h"

#include <NodePrefs.h>
#include <helpers/MultiSerialInterface.h>

#include <Arduino.h>
#include <esp_system.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_http_server.h>
#include <unistd.h>

// B1-lite: AP is a panic ritual, not an always-on service. The AP task is a
// persistent state machine: idle until a toggle request (button hold, or boot
// when BIPER_AP_AUTOSTART is set for bench testing), then a 10-minute window;
// a second toggle closes the window early.
//
// HTTP+WS run on esp_http_server (IDF, Apache-2.0, own task) — one port 80
// for the pages, the captive probes and the panel WebSocket bridge.

#ifndef BIPER_AP_WINDOW_MS
#define BIPER_AP_WINDOW_MS (10UL * 60UL * 1000UL)
#endif

// Load-bearing: let upstream setup() finish (mesh, BLE, radio) before the WiFi
// stack is touched.
static const uint32_t BIPER_AP_BOOT_DELAY_MS = 8000;
// Heap telemetry cadence (the bench harness greps these lines).
static const uint32_t BIPER_AP_HEAP_LOG_MS = 5000;
// Only DNS is polled now (httpd runs its own task).
static const uint32_t BIPER_AP_POLL_MS = 5;
// How often the idle state machine checks for a toggle request.
static const uint32_t BIPER_AP_IDLE_POLL_MS = 50;
// Probe handlers cannot read biper_ap_window()'s local softAPIP();
// 192.168.4.1 is the ESP32 SoftAP default.
static const char BIPER_AP_PORTAL_URL[] = "http://192.168.4.1/";
static const char BIPER_AP_IP_STR[] = "192.168.4.1";

// Forward declarations: both functions are used above their definitions.
static bool from_the_cube(httpd_req_t* req);
static void security_headers(httpd_req_t* req);

static DNSServer biper_dns;
static httpd_handle_t biper_httpd = nullptr;
static BiperApState biper_state;
static volatile bool biper_toggle_req = false;

const BiperApState& biper_ap_get_state() { return biper_state; }

void biper_ap_request_toggle() { biper_toggle_req = true; }

uint32_t biper_ble_pin(uint32_t upstream_pin) {
  if (upstream_pin == 123456) {  // factory default -> random session pin
    biper_state.ble_pin = 100000 + (esp_random() % 900000);
    // The pin is NOT logged. Same reason as the Wi-Fi password: the leak channel
    // is a pasted `pio device monitor` in a bug report, not someone with a cable.
    // It belongs on the OLED, where only the person holding the cube reads it.
#ifdef BIPER_DEBUG_SECRETS
    Serial.printf("[BIPER] ble_pin=%06lu (session)\n",
                  (unsigned long)biper_state.ble_pin);
#else
    Serial.printf("[BIPER] ble_pin=(session, shown on screen)\n");
#endif
  } else {  // user-configured in the app: respect it (and still show it)
    biper_state.ble_pin = upstream_pin;
    Serial.printf("[BIPER] ble_pin=user\n");
  }
  return biper_state.ble_pin;
}

// ---------------------------------------------------------------------------
// Pages. Civic Relay after the design-QC round; owner directives: light/dark
// theme (auto via prefers-color-scheme + manual no-JS checkbox toggle) and
// PL + EN versions (no JA in firmware). Placeholders: ssid, uptime h,
// uptime min, guests.
// ---------------------------------------------------------------------------

// WARNING — THESE PAGE STRINGS ARE snprintf() FORMAT STRINGS (see page_get).
// Every literal percent sign in the CSS below is therefore DOUBLED (%%). Add a
// plain `width:50%` here and the renderer will read an argument that was never
// passed: garbage on the page at best, a read past the stack frame at worst.
// This was undocumented until 18 Aug 2026 and is the easiest way to break the
// panel while "only editing some CSS".
#define BIPER_CSS_HEAD \
  "<meta charset=\"utf-8\">" \
  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" \
  "<title>Biper</title><style>" \
  ":root{--bg:#f3efe5;--ink:#1a1714;--mut:#6b675e;--mut2:#5f584c;" \
  "--card:#fff;--edge:#e2ddd0;--wave:#4737d4}" \
  "@media(prefers-color-scheme:dark){:root{--bg:#16141f;--ink:#f0ede4;" \
  "--mut:#a49e92;--mut2:#b0a998;--card:#211e2c;--edge:#37324a;--wave:#8d80ff}}" \
  "body:has(#t:checked){--bg:#16141f;--ink:#f0ede4;--mut:#a49e92;" \
  "--mut2:#b0a998;--card:#211e2c;--edge:#37324a;--wave:#8d80ff}" \
  "@media(prefers-color-scheme:dark){body:has(#t:checked){--bg:#f3efe5;" \
  "--ink:#1a1714;--mut:#6b675e;--mut2:#5f584c;--card:#fff;--edge:#e2ddd0;" \
  "--wave:#4737d4}}" \
  "*{box-sizing:border-box}body{font-family:-apple-system,system-ui,Segoe UI," \
  "sans-serif;background:var(--bg);color:var(--ink);margin:0;padding:16px 20px 28px}" \
  "main{max-width:26rem;margin:0 auto;text-align:center}" \
  ".bar{display:flex;justify-content:space-between;max-width:26rem;" \
  "margin:0 auto 12px;font-size:14px}" \
  ".bar a{color:var(--wave);text-decoration:none;letter-spacing:.1em}" \
  ".bar label{cursor:pointer;color:var(--wave)}" \
  "svg{width:40px;height:40px;display:block;margin:0 auto 10px}" \
  "svg path{stroke:var(--wave)}" \
  ".wm{font-weight:700;font-size:20px;letter-spacing:.28em}" \
  ".tag{color:var(--wave);font-size:.8rem;letter-spacing:.14em;" \
  "text-transform:uppercase;margin:6px 0 0}" \
  "h1{font-size:28px;line-height:1.15;margin:22px 0 6px}" \
  ".dot{width:10px;height:10px;border-radius:50%%;background:#ff5d3a;" \
  "display:inline-block;vertical-align:.08em;margin-right:8px}" \
  ".ev{font:500 13px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;" \
  "letter-spacing:.02em;color:var(--wave);margin:0 0 6px}" \
  "p{color:var(--mut);font-size:15px;line-height:1.5;margin:6px 0}" \
  ".card{background:var(--card);border:1px solid var(--edge);" \
  "border-radius:14px;padding:16px;margin-top:22px;text-align:left}" \
  ".card b{display:block;margin-bottom:6px;color:var(--ink)}" \
  ".card p{margin:0;font-size:.9rem;line-height:1.45}" \
  ".alt{font-size:14px;color:var(--mut2);border-top:1px dotted var(--wave);" \
  "padding-top:14px;margin-top:40px;text-align:left}" \
  "footer{margin-top:22px;color:var(--mut2)}" \
  "footer .u{font-size:14px}footer .w{font-size:13px}" \
  ".go{display:block;background:#ff5d3a;color:#fff;text-decoration:none;" \
  "border-radius:10px;font-weight:700;letter-spacing:.08em;text-align:center;" \
  "padding:14px;margin-top:12px;box-shadow:0 4px 18px rgba(255,93,58,.35)}" \
  "</style>"

#define BIPER_SVG_MARK \
  "<svg viewBox=\"0 0 512 512\" aria-hidden=\"true\">" \
  "<rect x=\"84\" y=\"216\" width=\"80\" height=\"80\" " \
  "transform=\"rotate(45 124 256)\" fill=\"#ff5d3a\"/>" \
  "<path d=\"M194 171c80 45 80 125 0 170M244 102c168 78 168 230 0 308\" " \
  "fill=\"none\" stroke-width=\"42\" stroke-linecap=\"square\"/></svg>"

static const char BIPER_PAGE_PL[] PROGMEM =
  "<!doctype html><html lang=\"pl\"><head>" BIPER_CSS_HEAD "</head><body>"
  "<input type=\"checkbox\" id=\"t\" hidden>"
  "<div class=\"bar\"><a href=\"/en\" hreflang=\"en\">EN</a>"
  "<label for=\"t\" title=\"jasny/ciemny\">&#9681;</label></div><main>"
  BIPER_SVG_MARK
  "<div class=\"wm\">BIPER</div>"
  "<p class=\"tag\">siec awaryjna &middot; dziala bez internetu</p>"
  "<h1><span class=\"dot\"></span>Polaczono z kostka.</h1>"
  "<p class=\"ev\">%s &middot; DZIALA %luh %lum &middot; GOSCIE %u</p>"
  "<p>Ta strona przyszla prosto z kostki. Nadaje w sieci mesh "
  "i wzmacnia zasieg wszystkich wokol.</p>"
  "<div class=\"card\"><b>Panel awaryjny.</b>"
  "<p>POMOC jednym przyciskiem, kanal ratunkowy, ustawienia.</p>"
  "<a class=\"go\" href=\"/app\">OTWORZ PANEL</a></div>"
  "<div class=\"alt\" lang=\"en\">English version: tap EN in the corner.</div>"
  "<footer><div class=\"u\">esp32ai.me/biper</div>"
  "<div class=\"w\">gdy wroci internet</div></footer></main></body></html>";

static const char BIPER_PAGE_EN[] PROGMEM =
  "<!doctype html><html lang=\"en\"><head>" BIPER_CSS_HEAD "</head><body>"
  "<input type=\"checkbox\" id=\"t\" hidden>"
  "<div class=\"bar\"><a href=\"/\" hreflang=\"pl\">PL</a>"
  "<label for=\"t\" title=\"light/dark\">&#9681;</label></div><main>"
  BIPER_SVG_MARK
  "<div class=\"wm\">BIPER</div>"
  "<p class=\"tag\">emergency network &middot; works without internet</p>"
  "<h1><span class=\"dot\"></span>Connected to the cube.</h1>"
  "<p class=\"ev\">%s &middot; UP %luh %lum &middot; GUESTS %u</p>"
  "<p>This page came straight from the cube. It is relaying on the mesh "
  "and extending everyone&rsquo;s range.</p>"
  "<div class=\"card\"><b>Emergency panel.</b>"
  "<p>One-button HELP, the emergency channel, settings.</p>"
  "<a class=\"go\" href=\"/app\">OPEN PANEL</a></div>"
  "<div class=\"alt\" lang=\"pl\">Wersja polska: PL w rogu strony.</div>"
  "<footer><div class=\"u\">esp32ai.me/biper</div>"
  "<div class=\"w\">when the internet is back</div></footer></main></body></html>";

// Rendered page (~3.3 kB). Static, not a local: it is filled by page_get() in
// the httpd task, whose stack has no room for 4 kB. esp_http_server serves
// from a single task, so one shared buffer needs no lock.
static char biper_page_buf[4096];

// ---------------------------------------------------------------------------
// httpd handlers (run in the httpd task)
// ---------------------------------------------------------------------------

// page_get serves PL by default; routes wanting English pass this as user_ctx.
static void* const BIPER_LANG_EN = (void*)1;

static esp_err_t page_get(httpd_req_t* req) {
  security_headers(req);
  const char* tmpl = (req->user_ctx == BIPER_LANG_EN) ? BIPER_PAGE_EN : BIPER_PAGE_PL;
  const uint32_t up_min = millis() / 60000UL;
  // snprintf returns the length it WOULD have written — not the one it wrote.
  // Today's headroom is 656 B out of 4096; one longer sentence on the page and
  // the browser gets HTML cut in the middle of a tag, with status 200 and no
  // trace in the log. We do not guess where it would break: a 500 is more honest
  // than half a page.
  const int needed = snprintf(biper_page_buf, sizeof(biper_page_buf), tmpl,
           biper_state.ssid, (unsigned long)(up_min / 60),
           (unsigned long)(up_min % 60), (unsigned)biper_state.guests);
  if (needed < 0 || (size_t)needed >= sizeof(biper_page_buf)) {
    Serial.printf("[BIPER_AP] page does not fit the buffer: %d of %u B\n",
                  needed, (unsigned)sizeof(biper_page_buf));
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "page buffer too small");
  }
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  return httpd_resp_send(req, biper_page_buf, HTTPD_RESP_USE_STRLEN);
}

// Radio mode for the panel. Why a separate route when the WS bridge exists: the
// bridge carries MeshCore's companion protocol, and adding a field of ours
// would mean changing someone else's protocol. Five characters of text are
// enough here.
//
// Why at all: the SIEC screen shows the mode only while the hotspot is OFF —
// with it on, the whole 64x48 is taken by the Wi-Fi password. So at exactly the
// moment the person is looking at their phone, the cube has no way to say which
// mode it is in. The panel is then the only place where it shows.
uint8_t biper_heard_15min();
uint16_t biper_batt_mv();

// The cube's FULL state for the panel in the phone.
//
// Earlier this route carried NOTHING BUT the radio mode, so a person who opened
// the panel had no way to learn anything about the device in their hand: how
// much of the hotspot window is left, whether the cube runs off a cell or a
// cable, how long it has been up. The companion protocol does not carry those —
// only the Biper layer knows them and only it can report them.
//
// Everything here is MEASURED. Voltage is emitted only when the board really
// measures it: on the C6L `biper_batt_mv()` returns 0, because there is no cell —
// and then the field does not lie with a zero, it is simply absent, and the panel
// says "from a cable".
static esp_err_t state_get(httpd_req_t* req) {
  if (!from_the_cube(req)) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "not from the cube");
  security_headers(req);
  const uint16_t mv = biper_batt_mv();
  char buf[288];
  // The JSON keys are Polish on purpose: they are the panel's wire contract,
  // matched literally by biper/app/biper-app.html. Renaming one here breaks the
  // panel. (tryb=mode, slyszy=hears, praca_s=uptime_s, okno_s=window_s,
  // goscie=guests, pamiec=free_heap.)
  int n = snprintf(buf, sizeof(buf),
      "{\"tryb\":\"%s\",\"slyszy\":%u,\"praca_s\":%lu,\"hotspot_s\":%u,"
      "\"okno_s\":%u,\"goscie\":%u,\"ssid\":\"%s\",\"pamiec\":%lu%s",
      biper_forwarding() ? "SIEC" : "SAM",
      (unsigned)biper_heard_15min(),
      (unsigned long)(millis() / 1000UL),
      (unsigned)biper_state.window_left_s,
      (unsigned)biper_state.window_total_s,
      (unsigned)biper_state.guests,
      biper_state.ssid,
      (unsigned long)ESP.getFreeHeap(),
      mv > 0 ? "" : "}");
  if (n > 0 && (size_t)n < sizeof(buf) && mv > 0) {
    n += snprintf(buf + n, sizeof(buf) - n, ",\"mv\":%u}", (unsigned)mv);
  }
  if (n < 0 || (size_t)n >= sizeof(buf)) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "state buffer too small");
  }
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

#include "BiperAppAsset.h"   // the panel, generated from biper/app/
#include "BiperFontAsset.h"  // system typography, generated from site subsets

// Security headers. Until 18.08 the cube set NOTHING BUT Content-Type. CSP
// closes what is left after the origin guard: loading anything from outside,
// embedding the panel in someone else's frame, and the consequences of any
// escaping hole. 'unsafe-inline' is necessary here — the whole panel is one
// file with the style and the script inlined, because that is what an asset in
// flash has to look like.
static void security_headers(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'self'; script-src 'self' 'unsafe-inline'; "
                     "style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
                     "font-src 'self'; connect-src 'self' ws://192.168.4.1; "
                     "frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
}

static esp_err_t app_get(httpd_req_t* req) {
  if (!from_the_cube(req)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad host");
  security_headers(req);
  // The asset is stored gzipped (see biper/app/gen-asset.py), so the browser
  // does the decompression. 62 kB over the cube's own WiFi, on every visit,
  // became 23 kB — the single cheapest improvement available here, and the ESP32
  // spends no cycles on it because the compression happened at build time.
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, (const char*)BIPER_APP_HTML, BIPER_APP_HTML_LEN);
}

// Panel typography. The fonts go as SEPARATE assets, not base64 in the HTML:
// base64 inflates them by 33%, and this way the browser caches them after the
// first visit. Without it the panel would speak system-ui — a face we put on
// the burned list ourselves — while the site and the cube speak Atkinson and
// Archivo Black.
uint16_t biper_batt_mv();

static esp_err_t font_get(httpd_req_t* req) {
  for (size_t i = 0; i < BIPER_FONTS_N; i++) {
    if (strcmp(req->uri, BIPER_FONTS[i].path) == 0) {
      httpd_resp_set_type(req, "font/woff2");
      httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=31536000, immutable");
      return httpd_resp_send(req, (const char*)BIPER_FONTS[i].data,
                             BIPER_FONTS[i].len);
    }
  }
  httpd_resp_set_status(req, "404 Not Found");
  return httpd_resp_send(req, nullptr, 0);
}

// Captive probes: a redirect makes Android/Windows pop the sign-in sheet.
static esp_err_t redirect_get(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", BIPER_AP_PORTAL_URL);
  return httpd_resp_send(req, nullptr, 0);
}

// ── ORIGIN GUARD ────────────────────────────────────────────────────────────
// Behind this bridge stands the FULL companion protocol: private-key export,
// the contact book, the channel secret, factory reset, transmitting in the
// owner's name. Upstream assumes the transport is paired BLE or a cable, so the
// transport itself is proof of identity. Here the transport is a WEB PAGE.
//
// Two things add up to the hole: (1) WebSockets are not subject to the
// same-origin policy, so any page open in the background of the phone can open
// ws://192.168.4.1/ws; (2) a captive portal answers with the cube's address for
// EVERY name, so such a tab keeps its own origin and aims at the cube. That is:
// a person connects to the cube to send a call for help, and at that same
// moment a page from the background can pull out their private key.
//
// We check both headers. Origin is sometimes absent (a client other than a
// browser — our own panel always sends it), but when it IS there it must point
// at the cube. Host must point at the cube always: it cuts off rebinding
// through wildcard DNS.
//
// Header must EQUAL one of two accepted values. It used to be
// `strstr(buf, "192.168.4.1")`, which the function name already denied: a
// substring test lets `192.168.4.1.evil.com` through, and that is a valid host
// name somebody can point at us with a short-TTL DNS record. The guard exists
// precisely to stop that, so it has to compare, not search.
static bool header_matches(httpd_req_t* req, const char* field, const char* a,
                          const char* b, bool required) {
  const size_t len = httpd_req_get_hdr_value_len(req, field);
  if (len == 0) return !required;          // header absent
  if (len >= 64) return false;             // absurdly long: reject
  char buf[64];
  if (httpd_req_get_hdr_value_str(req, field, buf, sizeof(buf)) != ESP_OK) return false;
  return strcmp(buf, a) == 0 || (b != nullptr && strcmp(buf, b) == 0);
}

static bool from_the_cube(httpd_req_t* req) {
  // Host: 192.168.4.1 (port optional). Origin: http://192.168.4.1 when present.
  if (!header_matches(req, "Host", "192.168.4.1", "192.168.4.1:80", true)) return false;
  if (!header_matches(req, "Origin", "http://192.168.4.1", "http://192.168.4.1:80",
                     false)) return false;
  return true;
}

static esp_err_t ws_handler(httpd_req_t* req) {
  if (req->method == HTTP_GET) {  // WS handshake done by httpd
    if (!from_the_cube(req)) {
      Serial.printf("[BIPER_WS] handshake refused: foreign origin/host\n");
      return ESP_FAIL;
    }
    const int fd = httpd_req_to_sockfd(req);
    if (biper_ws_fd_get() >= 0 && biper_ws_fd_get() != fd) {
      // One phone = one terminal: refuse a second client instead of letting
      // the LRU purge silently evict the first one.
      Serial.printf("[BIPER_WS] second client refused\n");
      return ESP_FAIL;
    }
    biper_ws_session_open(req->handle, fd);
    return ESP_OK;
  }
  static uint8_t ws_buf[MAX_FRAME_SIZE];
  httpd_ws_frame_t f = {};
  // Two-step read: max_len 0 fills type/len only, then we supply the buffer.
  if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK) return ESP_FAIL;
  if (f.len > MAX_FRAME_SIZE) return ESP_FAIL;  // oversize: drop connection
  if (f.len > 0) {
    f.payload = ws_buf;
    if (httpd_ws_recv_frame(req, &f, f.len) != ESP_OK) return ESP_FAIL;
  }
  if (f.type == HTTPD_WS_TYPE_BINARY && f.len > 0) {
    biper_ap_interface()->onClientFrame(ws_buf, f.len);
  } else if (f.type == HTTPD_WS_TYPE_CLOSE) {
    biper_ws_session_close();
  }
  return ESP_OK;
}

static void on_sock_close(httpd_handle_t hd, int sockfd) {
  if (sockfd == biper_ws_fd_get()) biper_ws_session_close();
  close(sockfd);
}

static bool biper_httpd_start() {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.max_uri_handlers = 14;
  cfg.uri_match_fn = httpd_uri_match_wildcard;
  cfg.close_fn = on_sock_close;
  cfg.lru_purge_enable = true;
  if (httpd_start(&biper_httpd, &cfg) != ESP_OK) return false;

  // Fields in order: uri, method, handler, user_ctx, is_websocket,
  // handle_ws_control_frames, supported_subprotocol. Matching follows
  // registration order, so the wildcard must stay last.
  static const httpd_uri_t uris[] = {
      {"/", HTTP_GET, page_get, nullptr, false, false, nullptr},
      {"/en", HTTP_GET, page_get, BIPER_LANG_EN, false, false, nullptr},
      {"/stan", HTTP_GET, state_get, nullptr, false, false, nullptr},
      {"/app", HTTP_GET, app_get, nullptr, false, false, nullptr},
      {"/ws", HTTP_GET, ws_handler, nullptr, true, false, nullptr},
      {"/f/*", HTTP_GET, font_get, nullptr, false, false, nullptr},
      {"/generate_204", HTTP_GET, redirect_get, nullptr, false, false, nullptr},
      {"/gen_204", HTTP_GET, redirect_get, nullptr, false, false, nullptr},
      // iOS probe: REDIRECT, never the page itself. Serving the page directly
      // kept the sign-in sheet on http://captive.apple.com, so every relative
      // link (/app) carried Host: captive.apple.com — and the origin guard
      // answered "bad host" to the very person the page was inviting in
      // (measured on an iPhone, 19.08). After the 302 the sheet lands on
      // http://192.168.4.1/ and the guard and the links agree.
      {"/hotspot-detect.html", HTTP_GET, redirect_get, nullptr, false, false, nullptr},
      {"/ncsi.txt", HTTP_GET, redirect_get, nullptr, false, false, nullptr},
      {"/connecttest.txt", HTTP_GET, redirect_get, nullptr, false, false, nullptr},
      {"/*", HTTP_GET, page_get, nullptr, false, false, nullptr},  // catch-all
  };
  for (auto& u : uris) httpd_register_uri_handler(biper_httpd, &u);
  return true;
}

static void biper_httpd_stop() {
  if (biper_httpd != nullptr) {
    httpd_stop(biper_httpd);  // also fires on_sock_close for open sockets
    biper_httpd = nullptr;
  }
}

static Preferences biper_nvs;
// The stored key stays "siec" ("network"): it is on flash in every cube already
// shipped, and renaming it would silently reset their choice on the next update.
static const char* NVS_NAMESPACE = "biper";
static const char* NVS_FORWARD = "siec";
static const char* NVS_PASS = "haslo";  // the cube's fixed hotspot password

// ---------------------------------------------------------------------------
// AP window state machine (own task)
// ---------------------------------------------------------------------------

static void biper_ap_window() {
  const uint32_t heap_before = ESP.getFreeHeap();
  const uint32_t t_start = millis();

  WiFi.mode(WIFI_AP);
#ifdef BIPER_AP_OPEN
  // Escape hatch: an open "beacon" network (pre-v0.7 behavior).
  const bool ap_ok = WiFi.softAP(biper_state.ssid, nullptr, 1, 0, 1);
  biper_state.pass[0] = 0;
#else
  // Owner decision (19.08, revising his own 18.08 call): the password is FIXED
  // per cube — drawn once, kept in the cube's own storage, shown on the OLED
  // whenever the window is open. The per-window rotation defended recorded
  // traffic, but its daily cost was paid by the owner's family: every phone
  // remembered a stale password, every new window meant retyping eight
  // characters off a tiny screen (owner's two-cube test, 19.08). Physical
  // control remains the gate — only the button opens the window — and a wipe
  // erases the password with the rest of the cube.
  // max_connection=1: the cube is ONE person's terminal.
  // Alphabet without 0/O/1/I/L: the password is copied off a 64x48 screen, so
  // look-alike glyphs would cost more than the two bits they bring.
  // Eight characters from a 31-symbol alphabet = 39.6 bits. That figure is
  // stated HERE and nowhere else; BiperAp.h points here rather than repeat it.
  if (biper_nvs.getString(NVS_PASS, biper_state.pass, sizeof(biper_state.pass)) == 0 ||
      biper_state.pass[0] == 0) {
    static const char ALF[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    for (size_t i = 0; i + 1 < sizeof(biper_state.pass) && i < 8; i++)
      biper_state.pass[i] = ALF[esp_random() % (sizeof(ALF) - 1)];
    biper_state.pass[8] = 0;
    biper_nvs.putString(NVS_PASS, biper_state.pass);
    Serial.printf("[BIPER_AP] pass drawn once, stored\n");
  }
  const bool ap_ok = WiFi.softAP(biper_state.ssid, biper_state.pass, 1, 0, 1);
#endif
  IPAddress ip = WiFi.softAPIP();
  biper_dns.start(53, "*", ip);
  const bool web_ok = biper_httpd_start();

  const uint32_t t_up = millis();
  biper_state.active = ap_ok && web_ok;
  biper_state.window_total_s = (uint16_t)(BIPER_AP_WINDOW_MS / 1000UL);
  Serial.printf("[BIPER_AP] ap=%s ok=%d ip=%s time_to_ap_ms=%lu heap_before=%lu heap_after=%lu\n",
                biper_state.ssid, (int)biper_state.active, ip.toString().c_str(),
                (unsigned long)(t_up - t_start),
                (unsigned long)heap_before, (unsigned long)ESP.getFreeHeap());
  // The password is NOT logged. Not because of someone with a cable — whoever
  // has USB can reflash the cube anyway — but because of the pasted log: a
  // person in trouble copies `pio device monitor` into a bug report and
  // publishes a live WPA2 key next to its SSID. The password belongs on the
  // OLED, where only the person holding the cube can read it.
  Serial.printf("[BIPER_AP] pass=%s max_conn=1\n",
                biper_state.pass[0] ? "(set, shown on screen)" : "(open)");

  uint32_t last_log = 0;
  // The countdown runs only while the cube is ALONE (owner decision 19.08):
  // a session at the panel is never cut mid-use. Every tick with a guest
  // present pushes `idle_since` forward, so the ten minutes start counting
  // the moment the last phone leaves — and a window nobody joined still
  // closes after the same ten minutes as before.
  uint32_t idle_since = t_up;
  for (;;) {
    const uint32_t now = millis();
    const uint32_t elapsed = now - t_up;
    if (biper_toggle_req) {  // second gesture: close the window early
      biper_toggle_req = false;
      Serial.printf("[BIPER_AP] toggle: closing early\n");
      break;
    }

    biper_dns.processNextRequest();

    const uint8_t guests = (uint8_t)WiFi.softAPgetStationNum();
    biper_state.guests = guests;
    if (guests > 0) idle_since = now;
    if (now - idle_since >= BIPER_AP_WINDOW_MS) break;
    biper_state.window_left_s = (uint16_t)((BIPER_AP_WINDOW_MS - (now - idle_since)) / 1000UL);

    if (now - last_log > BIPER_AP_HEAP_LOG_MS) {
      last_log = now;
      // batt: measured 0 mV on the C6L (18 Aug 2026) — the board has no battery and
  // no battery sense (doc 29: USB-C 5 V only). Kept on the telemetry line as a
  // regression tripwire: any non-zero value means the target copy changed under us.
      Serial.printf("[BIPER_AP] t=%lus heap=%lu min=%lu sta=%d batt=%umV\n",
                    (unsigned long)(elapsed / 1000),
                    (unsigned long)ESP.getFreeHeap(),
                    (unsigned long)ESP.getMinFreeHeap(),
                    (int)guests, (unsigned)biper_batt_mv());
      if (biper_ap_interface()->isConnected()) {
        Serial.printf("[BIPER_WS] rx=%lu tx=%lu\n",
                      (unsigned long)biper_ws_rx_count(),
                      (unsigned long)biper_ws_tx_count());
      }
    }
    vTaskDelay(pdMS_TO_TICKS(BIPER_AP_POLL_MS));
  }

  // B1-lite window over: tear down cleanly, mesh keeps running.
  // `active` goes out FIRST and that order is load-bearing: the screen draws the
  // password only while the window is active, so from this line on nobody reads
  // it any more and it can be cleared safely. The reverse order would show half
  // of a string being zeroed.
  biper_state.active = false;
  biper_state.guests = 0;
  biper_state.window_left_s = 0;
  biper_state.window_total_s = 0;
  // A closed window has no reason to keep the password in RAM for the rest of
  // the device's life; the next window reads it back from storage.
  memset(biper_state.pass, 0, sizeof(biper_state.pass));
  // The bridge STAYS enabled — only the socket dies with the window. The C3
  // audit fix put a disable() here and that was a regression: after the first
  // window closed the mesh saw no connected client, stopped pushing, and the
  // OLED went blind — SLYSZY froze and a button wipe was silently dropped,
  // the exact two failures the boot-time enable() exists to prevent. Incoming
  // messages are safe either way (the offline queue is unconditional; pushes
  // are only a doorbell), so an always-listening bridge costs nothing.
  biper_ap_interface()->resetQueues();
  biper_httpd_stop();
  biper_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.printf("[BIPER_AP] window closed, ap off, heap=%lu\n",
                (unsigned long)ESP.getFreeHeap());
}

static void biper_ap_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(BIPER_AP_BOOT_DELAY_MS));
  for (;;) {
    if (biper_toggle_req) {
      biper_toggle_req = false;
      biper_ap_window();
    }
    vTaskDelay(pdMS_TO_TICKS(BIPER_AP_IDLE_POLL_MS));
  }
}

// The prefs pointer already reaches us in setup — we keep it so the screen can
// show the radio preset without any new hook.
static NodePrefs* biper_prefs_ref = nullptr;
const NodePrefs* biper_prefs() { return biper_prefs_ref; }

// ── THE SIEC/SAM CHOICE SURVIVES A RESTART ───────────────────────────────────
// The cube has no battery, so a "restart" is a power bank pulled out, a knocked
// cable or a brownout — something that happens by itself, with no decision from
// a person. A switch that quietly returns to the other mode after such an event
// is worse than no switch at all: the owner is convinced they left the cube in
// SAM, and it is forwarding. Until the evening of 18.08 that was exactly the
// case — setRepeatEn(true) in setup overwrote the choice on every start.
//
// We write ONE BIT to NVS, not to MeshCore's prefs: those would have to be
// persisted through CommonCLI::savePrefs, that is, through ANOTHER hook in
// upstream. Four marked blocks in one file of someone else's is our whole
// intrusion and it is to stay that way — every further one is paid for at every
// rebase onto a new MeshCore.

// The forwarding switch. Returns the state AFTER the change; false if no prefs.
bool biper_forwarding_toggle() {
  if (biper_prefs_ref == nullptr) return false;
  const bool now_on = !biper_prefs_ref->isRepeatEn();
  biper_prefs_ref->setRepeatEn(now_on);
  biper_nvs.putBool(NVS_FORWARD, now_on);
  Serial.printf("[BIPER] repeat=%s\n", now_on ? "on" : "off");
  return now_on;
}

// A wipe to factory state has to take OUR whole storage with it — otherwise
// the "new" cube comes up in the previous owner's mode and, worse, with the
// previous owner's hotspot password still on its screen.
void biper_ap_forget() {
  biper_nvs.remove(NVS_FORWARD);
  biper_nvs.remove(NVS_PASS);
  Serial.printf("[BIPER] repeat+pass=forgotten (factory)\n");
}
bool biper_forwarding() {
  return biper_prefs_ref != nullptr && biper_prefs_ref->isRepeatEn();
}

void biper_ap_setup(NodePrefs* prefs, MultiSerialInterface* manager) {
  biper_prefs_ref = prefs;
  // Permanent diagnostic: why did we boot? (Hunting a one-off reset seen
  // ~10 s after a cold plug on 17.08 — suspicion: brownout during first
  // RF calibration when WiFi comes up. ESP_RST_BROWNOUT would confirm.)
  Serial.printf("[BIPER] reset_reason=%d\n", (int)esp_reset_reason());
#ifdef BIPER_AP_AUTOSTART
  const bool autostart = true;
#else
  const bool autostart = false;
#endif

  // Biper doctrine (owner, 17.08.2026): a cube assumes NO infrastructure around,
  // so packet forwarding is ON from boot. RAM-only, re-applied every boot.
  //
  // Since 18 Aug 2026 the person holding the cube can also turn it off, with a
  // triple click (see BiperScreen). The two modes are DELIBERATELY EQUIVALENT,
  // not "normal" and "crippled": SIEC carries other people's messages onward,
  // SAM transmits only your own. Forwarding stays the default because a network
  // of cubes that do not forward has a range of one hop and barely works.
  //
  // Why the switch has to exist at all: without it the cube transmits whenever
  // anyone nearby floods a packet — so somebody else, not its owner, decides
  // when it goes on the air. That is a fair trade in a storm and a bad one when
  // the owner has a reason to be quiet. It also costs power, and the cube runs
  // off a power bank.
  //
  // SIEC from the factory; after that whatever the person chose holds — see
  // biper_forwarding_toggle() above.
  // Storage opens unconditionally: the fixed hotspot password lives here too,
  // and it must survive even a build where prefs never arrived.
  biper_nvs.begin(NVS_NAMESPACE, false);
  if (prefs != nullptr) {
    const bool forwarding = biper_nvs.getBool(NVS_FORWARD, true);
    prefs->setRepeatEn(forwarding);
    Serial.printf("[BIPER] repeat=%s (remembered)\n", forwarding ? "on" : "off");
  }

  // Register the WS bridge with the interface manager. Registration is
  // slot-only, so doing it after startInterface() is safe; enable/disable is
  // driven by the AP window.
  if (manager != nullptr) {
    const bool added = manager->addInterface(InterfaceType::WiFi,
                                             biper_ap_interface());
    Serial.printf("[BIPER_WS] bridge registered=%d\n", (int)added);
    // We enable AT ONCE, not only inside the hotspot window. Upstream calls
    // `serial.enable()` inside `startInterface()` (MyMesh.cpp) and we register
    // AFTER it, so that moment passed us by. The effect was twofold and silent
    // both times: `onClientFrame()` returned on its first line, so a wipe from
    // the button was quietly thrown away on a cube whose hotspot had never been
    // opened — the screen said WYMAZUJE and erased nothing. And `writeFrame()`
    // returned before it counted an advert, so INFO showed SLYSZE 0 in a dense
    // neighbourhood and the resting animation said "you are alone".
    // From now on the AP window drives only the socket, not whether the bridge
    // exists.
    biper_ap_interface()->enable();
  }

  // The cube introduces itself with a WORD, not hex (owner decision, 19 Aug:
  // digits are easy to confuse between two cubes and impossible to say out
  // loud — "join Biper-SOWA" works, "Biper-3F2A" does not). The word comes
  // from folding ALL the efuse bits: the old code printed bits 32-47, and on
  // the C6 the efuse identifier is an EUI-64 whose constant FF:FE filler plus
  // a batch octet sit exactly in that slice — the owner's whole pair announced
  // the same Biper-15FE hotspot. An XOR fold lets any differing octet reach
  // the name, wherever the unique bytes live.
  //
  // Words are 3-4 letters for a reason: 64 px of OLED show ten 6-px glyphs,
  // so "Biper-" plus four letters is the longest SSID the screen can carry
  // without touching the drawing code (and its pixel-parity gate). With ~150
  // words two cubes of one batch can still draw the same name (<1%); if that
  // ever bites, the fix is a second word, not digits.
  static const char* const NAME_WORDS[] = {
      "SOWA", "WILK", "RYBA", "FOKA", "KRAB", "KRET", "KURA", "KOZA", "PUMA",
      "LAMA", "ORKA", "KIWI", "DROP", "IBIS", "MEWA", "KRUK", "MYSZ", "LIS",
      "KOT",  "OSA",  "RAK",  "SUM",  "EMU",  "LEW",  "PAW",  "BYK",  "TUR",
      "KUC",  "GIL",  "KOS",  "KUNA", "MORS", "KARP", "KRYL", "ARA",  "GNU",
      "REN",  "SZOP", "DODO", "MOA",  "KEA",  "LIN",  "SOLA", "BOA",  "LORI",
      "NEON", "KLON", "LIPA", "BUK",  "GRAB", "CIS",  "BEZ",  "MAK",  "LEN",
      "IRYS", "FIGA", "CEDR", "MECH", "KORA", "OWOC", "TORF", "KRA",  "SAD",
      "WOSK", "FALA", "LAWA", "RAFA", "JAR",  "WODA", "ROSA", "GRAD", "LUNA",
      "MARS", "KLIF", "STEP", "LAS",  "GAJ",  "POLE", "STAW", "ATOL", "URAN",
      "DOM",  "DACH", "OKNO", "MOST", "STER", "LINA", "KNOT", "FLET", "KOSZ",
      "KULA", "NORA", "MAPA", "LUPA", "GAMA", "NUTA", "RYTM", "ECHO", "ROMB",
      "AGAT", "OPAL", "MIKA", "CYNA", "CYNK", "SMOK", "GNOM", "ELF",  "GRYF",
      "PORT", "MOLO", "FORT", "PROM", "TAMA", "BOJA", "GROT", "TRAP", "KIL",
      "RUFA", "REJA", "SER",  "JUTA", "SAGA", "RUNO", "HAK",  "PION", "KLIN",
      "DRUT", "NIT",  "MISA", "SITO", "TACA", "BUT",  "PAS",  "SZAL", "TOGA",
      "DAR",  "CZAR", "SEN",  "TON",  "KOC",  "KARO", "KIER", "PIK",  "GOL",
      "KORT", "TOR",  "META", "RAMA", "PLED",
  };
  const uint64_t mac = ESP.getEfuseMac();
  const uint16_t fold = (uint16_t)(mac ^ (mac >> 16) ^ (mac >> 32) ^ (mac >> 48));
  snprintf(biper_state.ssid, sizeof(biper_state.ssid), "Biper-%s",
           NAME_WORDS[fold % (sizeof(NAME_WORDS) / sizeof(NAME_WORDS[0]))]);
#ifdef BIPER_SCREEN
  biper_screen_start();
#endif
  Serial.printf("[BIPER] Biper-AP layer v" BIPER_LAYER_VERSION ", ssid=%s autostart=%d\n",
                biper_state.ssid, (int)autostart);
  if (autostart) biper_toggle_req = true;
  xTaskCreate(biper_ap_task, "biper_ap", 12288, nullptr, 1, nullptr);
}

#endif  // BIPER_AP
