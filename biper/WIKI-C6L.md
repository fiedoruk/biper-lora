# WIKI: M5Stack Unit C6L — the single source of hardware truth for Biper

Rule: an entry lands here ONLY with a status. `MEASURED` (on our DEV cube),
`REFERENCE` (working code from TheRealHaoLiu/MeshCore, branch
`main-m5stack-unit-c6l`, MIT), `DOCUMENTATION` (docs.m5stack.com — misleading
after summarizing; trust only the pinmap tables), `UNVERIFIED`.
Lesson of 17 Aug 2026: two flashes went to wrong pins — I guessed from summaries.

## Core

- SoC: ESP32-C6 (RISC-V, 1 HP core, WiFi 6 + BLE + 802.15.4, NO PSRAM,
  512 kB RAM, 4 MB flash) — MEASURED (build/boot);
- LoRa radio: SX1262, on SPI; USB-C = USB-Serial-JTAG (D+ G13, D- G12).

## Pin map (MEASURED + REFERENCE + DOCUMENTATION tables — in agreement)

| what | pins | status |
|---|---|---|
| SPI shared by LoRa+OLED | SCK=G20, MOSI=G21, MISO=G22 | MEASURED (radio+display work) |
| SX1262 | NSS=G23, BUSY=G19, DIO1(IRQ)=G7, RESET: via expander P7 | MEASURED |
| OLED SSD1306 64×48 (0.66", SPI!) | CS=G6, DC=G18, RST=G15 | REFERENCE+MEASURED |
| internal I2C (expander) | SDA=G10, SCL=G8, INT=G3 | MEASURED (scan) |
| PI4IOE5V6408 expander | address 0x43 | MEASURED |
| buzzer | G11 | REFERENCE (upstream OK) |
| RGB LED WS2812C | G2 | REFERENCE (`P_LORA_TX_NEOPIXEL_LED=2`) |
| Grove HY2.0-4P | G5 (yellow), G4 (white), 5V, GND | DOCUMENTATION |

## PI4IOE5V6408 expander @0x43 (REFERENCE + MEASURED)

| pin | function | notes |
|---|---|---|
| P0 | SYS_KEY1 button | **ACTIVE LOW**, needs a pull-up from the register |
| P1 | input (unused) | pull-up as on P0 |
| P2–P4 | not connected | leave Hi-Z |
| P5 | SX_LNA_EN | output HIGH = LNA on |
| P6 | SX_ANT_SW | output HIGH = antenna path |
| P7 | SX_NRST (radio reset!) | output HIGH; **a dip = SX1262 reset** |

Registers: 0x01 chip-reset, 0x03 direction (1=out), 0x05 output state,
0x07 Hi-Z, 0x09 default-in, 0x0B pull-enable, 0x0D pull-select (1=up),
0x0F input state, 0x11 INT mask, 0x13 INT status.
Reference init: DIR=0b11100000, H_IM=0b00011100, PULL_SEL/EN=0b11100011,
IN_DEF=0b00000011, OUT: P7→1, then P6,P5→1. **Ours (init after the radio
starts): OUT_SET=0b11100000 first, THEN the directions — no dip on P7;
no chip-reset.** Without this configuration: the button is unreadable (P0
with no pull-up sits at 0) — MEASURED 17 Aug 2026.

## Display: hard conditions for it to work (MEASURED 17 Aug 2026 + REFERENCE)

1. **It must use the RADIO's SPIClass instance** (ours: `biper_radio_spi()` from
   the env-scoped copy of the target). A second SPIClass object on the same bus
   = a dead panel, even on the 0xA5 (all-on) command.
2. Adafruit_SSD1306: `begin(SSD1306_SWITCHCAPVCC, 0, true, false)` — the last
   parameter false = do not call spi.begin() a second time.
3. **64×48 requires COM-pins 0x12** (SETCOMPINS) — the library does not know
   this geometry and gives sequential → garbage/dark.
4. `setRotation(2)` — the panel is mounted upside down.
5. U8g2 with an HW-SPI constructor did NOT work on this board (point 1).

## Bugs in the upstream C6L variant (for a PR; our env avoids them)

- `P_LORA_TX_LED=15` — that is the display RST line, not an LED (ours `-U`);
- `PIN_BOARD_SDA/SCL=16/17` — an empty bus (the real one: 10/8);
- `SX126X_DIO3_TCXO_VOLTAGE=1.8` vs reference 3.0 — UNVERIFIED, a possible
  impact on sensitivity/range (a candidate for a field measurement!);
- upstream does not configure the expander → LNA/ANT_SW float on pull-ups —
  UNVERIFIED whether it costs range (consistent with the owner's observation
  "weak without a repeater"); the reference sets P5/P6 HIGH explicitly.

## Capability vectors of the model (inventory: in use / lying fallow)

| resource | our state | idea |
|---|---|---|
| OLED 64×48 | YES: v0.2: logo, animations, pages, torch | logo bitmap from .afpub |
| button (expander P0) | YES: click=pages, 2×click=ninja, 3 s=hotspot | 10 s=settings reset? |
| SoftAP WiFi 6 + captive | YES: 10 min window | K4: WS bridge |
| BLE companion | YES: stock | — |
| repeat/forward | YES: doctrine ON from boot | — |
| **buzzer G11** | YES: v0.3: event melodies (see the signal language) | POMOC sent/received after K4/K5 |
| **WS2812 G2** | YES: v0.3: heartbeat/breathing/flashes (see the signal language) | POMOC red after K4/K5; `P_LORA_TX_NEOPIXEL_LED` on TX |
| **expander INT G3** | NONE (polling 40 ms) | button on an interrupt = less I2C |
| **Grove G4/G5** | NONE | hardware-serial companion → Core2/CoreS3 as the kit's large screen (planned) |
| **dual-OTA switch** | NONE (we have huge_app without OTA) | trick from the reference: 2 firmwares on min_spiffs + the button held at startup switches the partition — NOT for us until WiFi fits in 1.9 MB |
| 802.15.4 (Thread/Zigbee) | NONE | distant future, do not touch |
| GPS on Grove | NONE | upstream has GPS_RX/TX — position in SOS without a phone; needs a module |

## Workshop

- DEV port: `/dev/cu.usbmodem2101`; the console requires in the env
  `ARDUINO_USB_MODE=1 ARDUINO_USB_CDC_ON_BOOT=1` (otherwise Serial → UART0);
- opening the port with pyserial + toggling RTS = reset (catching the boot);
- python with pyserial: use the environment PlatformIO sits in (`pio system info`
  shows its interpreter) — pyserial ships together with it;
- the first `pio run` can fail on `package-postinstall.py` — a retry passes;
- `pio ... | tail` masks the exit code;
- `[BIPER] reset_reason=` on every boot: 1=power-on, 3=sw, 4=panic, 9=BROWNOUT,
  11=USB(reset from the listener). Puzzle of 17 Aug 2026: a one-off reset ~10 s
  after a cold plug-in — suspected brownout at the first RF calibration (later
  boots from the calibration cache = OK). The next event will give the proof;
- NOTE: the heap after AP start dropped to ~26 kB when a phone holds BLE
  (earlier 34 kB with no connection) — a RAM diet before K4 is MANDATORY.

## Biper's signal language (v0.3 — buzzer G11 + WS2812 G2; for the guides!)

| event | sound | light |
|---|---|---|
| system start | 3 rising tones | white pulse |
| mesh is alive (idle) | — | green "heartbeat" every ~3 s |
| button click | short tick | — |
| 3 s gesture accepted | double tick | — |
| **double click = NINJA** | silence (no-sound mode) | everything off; display at minimum brightness |
| hotspot ON | 2 rising tones | blue "breathing" for the whole window |
| hotspot off | 2 falling tones | back to the heartbeat |
| **a guest joined the network** | cheerful beep-beep | cyan flash |
| (K4/K5) POMOC sent/received | reserved | reserved (red) |

Display: pages by click → STATUS (floating activity bar at the bottom) → SIEC
(a pulsing mark when the AP is active) → FALE (pure logo animation — demo/viral)
→ INFO (version, name, RAM) → LATARKA. Page names are the literal strings the
64x48 screen prints, so they stay in Polish: SIEC = network, FALE = waves,
LATARKA = torch. Animated pages ~12 fps, static ones 1 Hz.
The LED is deliberately dimmed (status, not a torch). Non-blocking melodies
(our own mini-player in BiperFeedback.cpp).
