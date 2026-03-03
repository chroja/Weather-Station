
// ── Pinout — závisí na verzi desky (BOARD_VERSION z config.h) ─────────────────
//
// Přehled verzí:
//
// IO9 je na ESP32-C3 boot strapping pin (LOW při resetu = download mode).
// V v3.5/v3.6 je na IO9 fyzické FLASH tlačítko — pro NeoPixel nelze použít.
// V v4.1+ je IO9 sdíleno: boot strapping pin + SK6812 LED data linka.
//   Konflikt nevznikne: VSENSOR je při bootu vypnutý, SK6812 bez napájení je
//   v high-Z a data pin neovlivňuje; interní pull-up IO9 drží HIGH → normální boot.
// IO0 = PIN_ADC (baterie) ve všech verzích.
//
//   BOARD_VERSION │ I2C_PWR │ IO9 použití          │ Tlačítko │ NeoPixel
//   ──────────────┼─────────┼──────────────────────┼──────────┼─────────
//       35  v3.5  │  IO3    │  FLASH button        │  —       │  —
//       36  v3.6  │  IO3    │  FLASH button        │  —       │  —
//       41  v4.1+ │  IO4    │  boot + SK6812 data  │  IO5     │  IO9
//
// config.h musí být vložen před tímto souborem (definuje BOARD_VERSION).

// ── Piny společné pro všechny verze ──────────────────────────────────────────
#define PIN_SDA      19   // I2C data
#define PIN_SCL      18   // I2C hodiny
#define PIN_ADC       0   // ADC baterie (dělič napětí na desce)
#define PIN_1WIRE    10   // 1-Wire pro DS18B20 (pin na desce, zatím neosazeno)

// ── Piny závislé na verzi desky ───────────────────────────────────────────────

#if BOARD_VERSION == 41

// ── LaskaKit Meteo Mini v4.1+ ────────────────────────────────────────────────
// IO4 = EN uSUP/VSENSOR; IO5 = tlačítko; IO9 = SK6812 RGB LED
#define PIN_I2C_PWR   4   // EN uSUP / VSENSOR (IO4)
#define PIN_BTN       5   // IO5 — tlačítko (aktivní LOW, pull-up 10 kΩ na VSENSOR, 1 µF debounce)
#define PIN_LED       9   // IO9 — SK6812-EC20 RGB LED (napájení přes PIN_I2C_PWR)

#elif BOARD_VERSION >= 35

// ── LaskaKit Meteo Mini v3.5 / v3.6 ─────────────────────────────────────────
// IO3 = EN uSUP/VSENSOR; IO9 = FLASH pin (TS-1088R-02026) — nelze použít pro LED
#define PIN_I2C_PWR   3   // EN uSUP / VSENSOR (IO3)
// PIN_BTN  — nedefinováno (v3.5/v3.6 nemá uživatelské tlačítko na IO5)
// PIN_LED  — nedefinováno (IO9 je FLASH boot pin, nelze použít pro NeoPixel)
// HAS_BUTTON a HAS_NEOPIXEL se pro BOARD_VERSION < 41 nedefinují

#else
#error "Neznámá verze desky — nastav BOARD_VERSION v config.h (35 = v3.5/v3.6, 41 = v4.1)"
#endif
