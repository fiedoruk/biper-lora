#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

// interface manager
#include <helpers/MultiSerialInterface.h>
MultiSerialInterface interface_manager;

// include bluetooth interface
#if defined(BLE_PIN_CODE)
  #ifdef ESP32
    // include esp32 bluetooth interface
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #elif defined(NRF52_PLATFORM)
    // include nrf52 bluetooth interface
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #else
    #error "SerialBLEInterface is not defined for this platform"
  #endif
#endif

// include wifi interface
#ifdef WIFI_SSID
  #ifndef TCP_PORT
    #define TCP_PORT 5000
  #endif
  #ifdef ESP32
    // include esp32 wifi interface
    #include <helpers/esp32/SerialWifiInterface.h>
    SerialWifiInterface wifi_interface;
  #else
    #error "SerialWifiInterface is not defined for this platform"
  #endif
#endif

// include usb interface
#if defined(ENABLE_USB_INTERFACE)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface usb_serial_interface;
#endif

// include ethernet interface
#if defined(ETHERNET_ENABLED)
  #include <helpers/ethernet/EthernetInterface.h>
  ETHERNET_CLASS ethernet_interface;
#endif

// include hardware serial interface
#if defined(SERIAL_RX)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface hardware_serial_interface;
  HardwareSerial companion_serial(1);
#endif

// ---- BIPER_AP additive hook (all code in src/helpers/biper/) ----
#ifdef BIPER_AP
  #include <helpers/biper/BiperAp.h>
#endif
// ---- end BIPER_AP hook ----

// platform file system
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
    #if defined(EXTRAFS)
      #include <CustomLFS.h>
      CustomLFS ExtraFS(0xD4000, 0x19000, 128);
      DataStore store(InternalFS, ExtraFS, rtc_clock);
    #else
      DataStore store(InternalFS, rtc_clock);
    #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &interface_manager);
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

/* WIFI RECONNECT TRACKERS */
#if defined(ESP32) && defined(WIFI_SSID)
  bool wifi_needs_reconnect = false;
  unsigned long last_wifi_reconnect_attempt = 0;
#endif

void setup() {
  Serial.begin(115200);
  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

// ---- BIPER_AP hook: give the hardware random generator back ----
// `fast_rng.begin()` is `randomSeed(seed)`, and the Arduino core for ESP32
// treats that as an order: "stop using the hardware RNG". From this line on,
// EVERY `random()` call in the whole firmware drops to newlib's `rand()` —
// a generator with 31 bits of state, seeded with a 31-bit value from radio
// noise. (cores/esp32/WMath.cpp: `s_useRandomHW = false` in `randomSeed`.)
//
// What it breaks: `StdRNG::random()` no longer mixes anything but `random()`,
// and the mesh takes from it the uniqueness blob of the packet hash
// (Mesh.cpp:477, BaseChatMesh.cpp:667) and the sixth byte of an ACK. Keys,
// secrets and nonces travel a separate path (ed25519 + SHA-256), so the privacy
// of conversations does not depend on this — but predictable blobs make traffic
// correlation easier, and that is exactly the property this product promises
// to protect.
//
// The identity is created through RadioNoiseListener, that is
// `randomByte() ^ random()` on EACH of the 32 seed bytes, so 31 bits do not
// bound it. What bounds it is the quality of `randomByte()` — and that is the
// only thing here that cannot be settled without the board.
//
// The fix is one line: we give `esp_random()` back. The seed above becomes
// meaningless, but we do not touch that line — it is upstream code.
#if defined(BIPER_AP) && defined(ESP32)
  useRealRandomGenerator(true);
  Serial.printf("[BIPER] rng=hw (esp_random restored after randomSeed)\n");

  // A measurement of the source the identity key stands on. The key seed is
  // `randomByte() ^ random()` on each of the 32 bytes; `random()` contributes
  // 31 bits to the whole sequence, so the entropy of the key is decided by
  // `randomByte()` — the eight lowest bits of the SX1262 RSSI register in
  // receive mode. If on THIS board that source is dead (antenna switch wired
  // wrong, LNA without power), the bytes will be constant or repeatable and the
  // key can be predicted. There is no other way to establish this — the board
  // or nothing.
  //
  // Three numbers are enough for a verdict: how many DIFFERENT values in 32
  // samples (random: ~28), how many bits set out of 256 (random: ~128) and
  // whether there is no long run of the same value. This is NOT the key seed —
  // a separate draw, for reading only.
  //
  // We sample through `getRngSeed()`, because `_radio` is private upstream,
  // while that method is public and assembles its number out of four
  // `randomByte()` calls (RadioLib PhysicalLayer::random). Eight calls =
  // 32 bytes from the same source, without adding a single line to someone
  // else's class.
  {
    uint8_t samples[32];
    for (int i = 0; i < 8; i++) {
      const uint32_t v = radio_driver.getRngSeed();
      samples[i*4+0] = (uint8_t)(v >> 24); samples[i*4+1] = (uint8_t)(v >> 16);
      samples[i*4+2] = (uint8_t)(v >> 8);  samples[i*4+3] = (uint8_t)v;
    }
    uint32_t bits = 0, in_a_row = 0;
    bool seen[256] = {false};
    uint16_t distinct = 0;
    for (int i = 0; i < 32; i++) {
      for (uint8_t b = samples[i]; b; b >>= 1) bits += (b & 1);
      if (!seen[samples[i]]) { seen[samples[i]] = true; distinct++; }
      if (i && samples[i] == samples[i - 1]) in_a_row++;
    }
    Serial.printf("[BIPER] rng_radio distinct=%u/32 bits=%lu/256 in_a_row=%lu\n",
                  (unsigned)distinct, (unsigned long)bits, (unsigned long)in_a_row);
    // The three numbers above are the VERDICT and stay always — no cost at all,
    // and without them a dead entropy source would only surface in an audit.
    // The raw bytes are another matter: they help only when the verdict is bad,
    // and on every boot of a public release they would look like a leak. Hence
    // the flag.
#ifdef BIPER_RNG_SAMPLES
    Serial.printf("[BIPER] rng_radio samples=");
    for (int i = 0; i < 32; i++) Serial.printf("%02X", samples[i]);
    Serial.printf("\n");
#endif
  }
#endif
// ---- end BIPER_AP hook ----

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#else
  #error "need to define filesystem"
#endif

// add bluetooth interface
#if defined(BLE_PIN_CODE)
// ---- BIPER_AP hook: session BLE pin (factory 123456 -> random, on the OLED) ----
#ifdef BIPER_AP
  bluetooth_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, biper_ble_pin(the_mesh.getBLEPin()));
#else
  bluetooth_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
#endif
// ---- end BIPER_AP hook ----
  interface_manager.addInterface(InterfaceType::Bluetooth, &bluetooth_interface);
#endif

// add wifi interface
#ifdef WIFI_SSID
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  WiFi.setAutoReconnect(true);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
          WIFI_DEBUG_PRINTLN("WiFi disconnected. Flagging for reconnect...");
          wifi_needs_reconnect = true;
      } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
          WIFI_DEBUG_PRINTLN("WiFi connected successfully!");
          wifi_needs_reconnect = false;
      }
  });

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  wifi_interface.begin(TCP_PORT);
  interface_manager.addInterface(InterfaceType::WiFi, &wifi_interface);
#endif

// add usb interface
#if defined(ENABLE_USB_INTERFACE)
  usb_serial_interface.begin(Serial);
  interface_manager.addInterface(InterfaceType::USB, &usb_serial_interface);
#endif

// add ethernet interface
#if defined(ETHERNET_ENABLED)
  ethernet_interface.begin();
  interface_manager.addInterface(InterfaceType::Ethernet, &ethernet_interface);
#endif

// add hardware serial interface
#if defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  hardware_serial_interface.begin(companion_serial);
  interface_manager.addInterface(InterfaceType::HardwareSerial, &hardware_serial_interface);
#endif

  the_mesh.startInterface(interface_manager);
  sensors.begin();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
#endif

// ---- BIPER_AP additive hook ----
#ifdef BIPER_AP
  biper_ap_setup(the_mesh.getNodePrefs(), &interface_manager);   // own FreeRTOS tasks; no loop() hook
#endif
// ---- end BIPER_AP hook ----

  board.onBootComplete();
}

void loop() {
  the_mesh.loop();
  interface_manager.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif

  if (!the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#endif
  }

#if defined(ESP32) && defined(WIFI_SSID)
  // Safely attempt to reconnect every 10 seconds if flagged
  if (wifi_needs_reconnect && (millis() - last_wifi_reconnect_attempt > 10000)) {
    WIFI_DEBUG_PRINTLN("Attempting manual WiFi reconnect...");
    WiFi.disconnect();
    WiFi.reconnect();
    last_wifi_reconnect_attempt = millis();
  }
#endif
}
