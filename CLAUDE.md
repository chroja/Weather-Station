# CLAUDE.md — poznámky pro AI asistenta

## Co je tento projekt
Firmware pro **LaskaKit DIY Mini Weather Station** (ESP32-C3).
`FW_experimental/` je aktivně vyvíjená verze. `FW/` je starší referenční verze (neupravovat).

---

## Struktura projektu

```
Weather_Station_Mini-main/
├── FW_experimental/
│   ├── FW_experimental.ino   ← aktivní firmware (upravovat zde)
│   ├── config.h              ← konstanty: SLEEP_SEC, BUFFER_MAX, DEBUG_LEVEL, ...
│   ├── secrets.h             ← privátní URL serverů (gitignore, uživatel spravuje)
│   ├── secrets.h.example     ← šablona pro secrets.h (committed)
│   └── README.md             ← popis experimentální verze
├── FW/                       ← starší verze (referenční, neupravovat)
├── SW/tmep/                  ← původní kód od LaskaKit (referenční, neupravovat)
├── 3D/                       ← 3D tisk krabičky/radiačního štítu
├── img/                      ← fotografie hardware
├── TODO.md                   ← plán dalšího vývoje
├── README.md                 ← popis projektu
└── CLAUDE.md                 ← tento soubor
```

---

## Aktuální hardware (venkovní jednotka)

| Komponenta | Účel | I2C adresa |
|---|---|---|
| ESP32-C3 (LaskaKit Meteo Mini) | MCU, WiFi | — |
| SHT40 | teplota, relativní vlhkost | 0x44 |
| BME280 | atmosferický tlak (jen tlak, vlhkost SAMPLING_NONE) | 0x77 |
| LTR390 | UV index, ambient light | 0x53 |
| LiPol 900mAh | napájení | — |
| Solární panel 5V/4W | dobíjení | — |

**DS3231 momentálně neosazeno** — timestampy se odhadují z NTP syncu + boot counteru.

**Piny:**
- SDA = GPIO 19, SCL = GPIO 18
- I2C napájení = GPIO 4 (PIN_I2C_PWR / uSUP EN; v3.5 = GPIO 3)
- ADC baterie = GPIO 0
- ADC koeficient: `ADC_TO_BATV = 1.7857` (kalibrováno: 2240 mV → 4.0 V)

---

## Klíčová architektonická rozhodnutí

### Deep sleep flow
Celá logika je v `setup()`, `loop()` je prázdný.
**Pořadí operací:**
1. Měření senzorů (timestamp může být 0 před prvním NTP syncem)
2. WiFi + NTP — probíhá **před** zápisem do bufferu
3. Oprava timestampu: `if (m.timestamp == 0 && lastNTPTime > 0) m.timestamp = lastNTPTime`
4. Buffer — měření s `timestamp == 0` se **zahodí** (WiFi selhala při prvním bootu)
5. Odesílání (jen při wifiConnected)
6. Deep sleep

Typický cyklus: ~3–10 s aktivní, 60 s spánek.

### Dvouúrovňový buffer

**RTC RAM** (přežije deep sleep, reset při výpadku napájení):
- `buffer[BUFFER_MAX]` = 112 slotů × 36 B = 4 032 B
- Tier 0 (1min): max 70 záznamů; 10× tier0 → 1× tier1
- Tier 1 (10min): max 42 záznamů; 6× tier1 → 1× NVS tier2
- Pokrytí: ~7 hodin (před kompakcí do NVS)

**NVS flash** (`Preferences`, přežije výpadek napájení, lazy load):
- `nvsLocalBuf[FLASH_MAX]` = 150 hodinových průměrů × 36 B = 5 400 B (statická globální proměnná)
- FIFO; po naplnění se zahazuje nejstarší záznam
- Pokrytí: 150 hodin (~6.25 dní)

**Detekce výpadku napájení:** `RTC_MAGIC = 0x4D455445` — při neshodě se RTC proměnné resetují.

### Per-server tracking
Každý ze tří serverů má vlastní offset v RTC (s1Sent/s2Sent/s3Sent) a NVS (nvs1Sent/nvs2Sent/nvs3Sent).
Záznamy se z bufferu odstraní, až je přijmou všechny tři servery.

### WiFi strategie
- Pokus o připojení každý boot
- Při prvním bootu (`wifiConfigured == false`) → WiFiManager portál (180 s timeout, AP=LaskaKitMeteo)
- Při dalších bootech → `WiFi.begin()` bez portálu (WIFI_TIMEOUT_SEC = 10 s)
- `wifiConfigured` flag v RTC RAM — oprava: `WiFi.SSID()` je prázdné po `WiFi.mode(WIFI_STA)`, takže se nespoléhá na SSID

### Timestampy
- `lastNTPTime` = Unix timestamp při posledním NTP syncu (RTC RAM)
- `lastNTPBoot` = bootCount při posledním NTP syncu (RTC RAM)
- `getCurrentTimestamp()` = `lastNTPTime + (bootCount - lastNTPBoot) * SLEEP_SEC`
- Drift: zlomky sekundy/cyklus, pro meteorologii dostačující

### TMEP.cz API — Batch CSV POST (HTTPS)
Tři servery, každý přijímá CSV tělo (jeden řádek = jedno měření):
- `serverName1` → S1: T;RH;P (teplota, vlhkost, tlak)
- `serverName2` → S2: ALS;UVS;absH (světlo, UV, absolutní vlhkost)
- `serverName3` → S3: batV;;

Formát řádku: `YYYY-MM-DD HH:MM:SS;G1;G2;G3;voltage;rssi`

HTTPS s `WiFiClientSecure.setInsecure()` (bez ověření certifikátu).

### secrets.h
URL serverů jsou v `secrets.h` (gitignorovaný). Šablona: `secrets.h.example`.
Přepínač `#define TEST_MODE` v `.ino` (před `#include "config.h"`) vybírá TEST nebo produkční blok.

---

## Plánované změny (viz TODO.md)

### Hotovo
- ✓ `analogReadMilliVolts()` pro přesnější napětí baterie
- ✓ SHT40 pro vlhkost (BME280 jen pro tlak)
- ✓ Dvouúrovňový buffer (RTC tier0+tier1, NVS tier2)
- ✓ Batch CSV POST místo GET
- ✓ WiFi+NTP před bufferem, zahazování timestamp=0 měření
- ✓ secrets.h oddělení od config.h

### Střednědobé (hardware)
- DS3231 RTC — přidat až bude k dispozici modul

### Dlouhodobé (architektura)
- Přechod na ESP-NOW místo přímého WiFi
- Indoor gateway (ESP32 + ESPHome) → MQTT → Home Assistant na RPi

---

## Známé problémy / omezení

1. **Timestamp drift** — bez DS3231 odhad z NTP + boot counter. Drift minuty/den, pro meteorologii OK.

2. **String fragmentace** — `buildCsvFrom()` staví CSV přes Arduino `String`. Pro stávající rozsah
   nevadí; pro budoucí ESP-NOW verzi bude pevný `char[]` packet.

3. **HTTPS setInsecure()** — certifikát serveru není ověřen (MitM riziko v nedůvěryhodné síti).

---

## Použité knihovny

| Knihovna | Verze | Poznámka |
|---|---|---|
| Adafruit SHT4x | >= 1.0 | SHT40 teplota + vlhkost |
| Adafruit BME280 | >= 2.2 | tlak |
| Adafruit LTR390 | >= 1.0 | UV + světlo |
| WiFiManager (tzapu) | >= 2.0 | konfigurační portál |
| Preferences | built-in | NVS flash buffer |
| WiFiClientSecure | built-in | HTTPS bez certifikátu |
| Arduino ESP32 core | >= 2.0 | `analogReadMilliVolts()` |

---

## Styl kódu v tomto projektu
- Jazyk komentářů: čeština
- Konstanty: `#define` s velkými písmeny
- Funkce: camelCase
- Proměnné: camelCase
- Struct členové: camelCase
- Formátování: 4 mezery indent
- Upravovat **pouze FW_experimental/**, pokud není explicitně řečeno jinak
