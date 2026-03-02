# FW_experimental — Experimentální firmware pro LaskaKit Meteo Mini

Rozšířená verze produkčního firmwaru (`FW/weather_mini.ino`) s podporou
dvouúrovňového offline bufferu (RTC RAM + NVS flash), per-server odesílání
a víceúrovňového debugování.

Firmware zvládne výpadek WiFi až **~7 dní** bez ztráty dat:
- **RTC RAM** (přežije deep sleep): minutová a desetiminutová data, ~7 hodin
- **NVS flash** (přežije výpadek napájení): hodinové průměry, ~6 dní

Po obnovení spojení se vše odešle na TMEP.cz jako batch — nejdřív nejstarší
hodinové záznamy z NVS, pak aktuální data z RTC.

---

## Obsah

1. [Požadavky](#1-požadavky)
2. [Nastavení Arduino IDE](#2-nastavení-arduino-ide)
3. [Nastavení TMEP.cz](#3-nastavení-tmepCz)
4. [Konfigurace firmwaru](#4-konfigurace-firmwaru)
5. [Nahrání firmwaru](#5-nahrání-firmwaru)
6. [První spuštění — nastavení WiFi](#6-první-spuštění--nastavení-wifi)
7. [Ověření funkčnosti](#7-ověření-funkčnosti)
8. [Kalibrace baterie](#8-kalibrace-baterie)
9. [Přechod na produkci](#9-přechod-na-produkci)
10. [Dvouúrovňový buffer — jak funguje](#10-dvouúrovňový-buffer--jak-funguje)
11. [Per-server odesílání](#11-per-server-odesílání)
12. [Debug výstup](#12-debug-výstup)
13. [Architektura firmwaru](#13-architektura-firmwaru)
14. [Řešení problémů](#14-řešení-problémů)
15. [Hardware a zapojení](#15-hardware-a-zapojení)
16. [Referenční tabulky](#16-referenční-tabulky)

---

## 1. Požadavky

### Hardware
- LaskaKit DIY Mini Weather Station (ESP32-C3)
- SHT40 (teplota + vlhkost), BME280 (tlak), LTR390 (UV + světlo)
- LiPol baterie 900 mAh + solární panel 5V/4W
- USB-C kabel pro nahrání firmwaru

### Software
- [Arduino IDE 2.x](https://www.arduino.cc/en/software)
- ESP32 board support package
- Knihovny (viz sekce 2)

### Účty / služby
- Účet na [tmep.cz](https://tmep.cz) — potřebuješ **3 senzory** (jeden pro každou skupinu dat)

---

## 2. Nastavení Arduino IDE

### 2.1 Přidat ESP32 board support

1. Otevři Arduino IDE → **File → Preferences**
2. Do pole **Additional boards manager URLs** přidej:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Otevři **Tools → Board → Boards Manager**
4. Vyhledej `esp32` od Espressif Systems a nainstaluj verzi **≥ 2.0**

### 2.2 Vybrat správnou desku

**Tools → Board → ESP32 Arduino → ESP32-C3 Dev Module**

Nastavení:
| Parametr | Hodnota |
|---|---|
| Board | ESP32-C3 Dev Module |
| USB CDC On Boot | Enabled |
| Flash Mode | QIO |
| Flash Size | 4MB |
| Partition Scheme | Default 4MB with spiffs |
| Upload Speed | 921600 |

> **USB CDC On Boot = Enabled** je důležité — zajistí funkčnost `Serial` přes USB-C bez UART převodníku.

### 2.3 Nainstalovat knihovny

Otevři **Tools → Manage Libraries** a nainstaluj:

| Knihovna | Autor | Verze |
|---|---|---|
| Adafruit SHT4x Library | Adafruit | ≥ 1.0 |
| Adafruit BME280 Library | Adafruit | ≥ 2.2 |
| Adafruit LTR390 Library | Adafruit | ≥ 1.0 |
| Adafruit Unified Sensor | Adafruit | ≥ 1.1 |
| WiFiManager | tzapu | ≥ 2.0 |

---

## 3. Nastavení TMEP.cz

Firmware odesílá data na **tři samostatné senzory** na TMEP.cz.
Každý senzor přijímá jinou skupinu dat.

### 3.1 Vytvořit tři senzory

Na [tmep.cz](https://tmep.cz) vytvoř tři nové senzory:

| Senzor | Doporučený název | Kanály |
|---|---|---|
| S1 | Meteo — teplota/vlhkost/tlak | G1=teplota (°C), G2=rel. vlhkost (%), G3=tlak (hPa) |
| S2 | Meteo — světlo/UV/abs.vlhkost | G1=světlo ALS, G2=UV index, G3=abs. vlhkost (g/m³) |
| S3 | Meteo — baterie | G1=napětí baterie (V) |

### 3.2 Zjistit URL senzorů

Po vytvoření každého senzoru najdeš na jeho stránce URL ve tvaru:
```
https://xxxxxx-yyyyyy.tmep.cz
```

Tuto URL (bez lomítka na konci) zkopíruj do `secrets.h` — viz sekce 4.2.

### 3.3 Nastavit kanály senzorů na TMEP.cz

V nastavení každého senzoru nastav popisky kanálů:

**S1:**
- Kanál 1 (G1): `Teplota` — jednotka `°C`
- Kanál 2 (G2): `Vlhkost` — jednotka `%`
- Kanál 3 (G3): `Tlak` — jednotka `hPa`
- Kanál 4 (voltage): `Baterie` — jednotka `V`

**S2:**
- Kanál 1 (G1): `Světlo ALS`
- Kanál 2 (G2): `UV index`
- Kanál 3 (G3): `Abs. vlhkost` — jednotka `g/m³`

**S3:**
- Kanál 1 (G1): `Baterie` — jednotka `V`

---

## 4. Konfigurace firmwaru

### 4.1 Otevřít projekt

Otevři `FW_experimental.ino` v Arduino IDE — automaticky se otevřou i `config.h` a `secrets.h`.

### 4.2 Vytvořit `secrets.h` s URL serverů

URL senzorů jsou uloženy v souboru `secrets.h`, který **není součástí repozitáře**
(je v `.gitignore`) — tvá soukromá tmep.cz URL se tak nedostane na GitHub.

```bash
# Ve složce FW_experimental/:
cp secrets.h.example secrets.h
```

Pak otevři `secrets.h` a vyplň:

```cpp
#ifdef TEST_MODE

const char serverName1[] = "https://xxxxxx-yyyyyy.tmep.cz";  // test: teplota, vlhkost, tlak
const char serverName2[] = "https://xxxxxx-yyyyyy.tmep.cz";  // test: světlo, UV, abs. vlhkost
const char serverName3[] = "https://xxxxxx-yyyyyy.tmep.cz";  // test: baterie

#else

const char serverName1[] = "https://xxxxxx-yyyyyy.tmep.cz";  // teplota, vlhkost, tlak
const char serverName2[] = "https://xxxxxx-yyyyyy.tmep.cz";  // světlo, UV, abs. vlhkost
const char serverName3[] = "https://xxxxxx-yyyyyy.tmep.cz";  // baterie

#endif
```

> **Pozor:** Bez `secrets.h` firmware nepůjde zkompilovat — soubor musí existovat.
> `secrets.h.example` slouží pouze jako šablona, nekopíruj tam URL přímo.

### 4.3 Přepínače v `FW_experimental.ino`

Na začátku souboru jsou dva přepínače:

```cpp
#define TEST_MODE
#define TEST_SEND
```

| Přepínač | Zapnutý | Vypnutý |
|---|---|---|
| `TEST_MODE` | Použijí se testovací URL z config.h | Použijí se produkční URL |
| `TEST_SEND` | Odešle jen jeden pevný CSV řádek (diagnostika spojení) | Normální provoz |

> Pro první testování nech `TEST_MODE` zapnutý. Pro produkci zakomentuj: `// #define TEST_MODE`

### 4.4 Ostatní parametry `config.h`

| Konstanta | Výchozí | Doporučení pro produkci | Popis |
|---|---|---|---|
| `SLEEP_SEC` | `15` | `60` | Perioda měření v sekundách |
| `WIFI_TIMEOUT_SEC` | `10` | `10` | Max. čekání na WiFi |
| `DEBUG_LEVEL` | `3` | `2` | Výchozí debug level (s USB kabelem) |
| `TIMEZONE` | `CET-1CEST,...` | dle umístění | POSIX TZ string |

> **SLEEP_SEC = 15** je jen pro testování — baterie by nevydržela. Pro venkovní provoz nastav **60** (1 měření/min).

---

## 5. Nahrání firmwaru

1. Připoj desku USB-C kabelem k počítači
2. Vyber správný port: **Tools → Port → COMx** (Windows) nebo `/dev/ttyACM0` (Linux/Mac)
3. Klikni na **Upload** (šipka doprava)
4. Počkej na `Done uploading`

> Pokud nahrání selže s chybou `Failed to connect`, poddrž tlačítko **BOOT** na desce a zkus znovu.

---

## 6. První spuštění — nastavení WiFi

Po prvním nahrání (nebo po resetu RTC RAM) se spustí **WiFiManager konfigurační portál**.

### Postup

1. Na telefonu nebo počítači otevři seznam WiFi sítí
2. Připoj se na síť **`LaskaKitMeteo`** (heslo: `meteostation`)
3. Automaticky se otevře konfigurační stránka (nebo přejdi na `192.168.4.1`)
4. Klikni na **Configure WiFi**
5. Vyber svou WiFi síť a zadej heslo
6. Klikni na **Save** — deska se restartuje a připojí

> Portál je aktivní **180 sekund**. Pokud se nikdo nepřipojí, deska přejde do deep sleep a zkusí to příští boot.

> WiFi přihlašovací údaje jsou uloženy trvalá flash paměť — po výpadku napájení se obnoví automaticky.

---

## 7. Ověření funkčnosti

### 7.1 Serial monitor

Otevři **Tools → Serial Monitor**, nastav **115200 baud**.

Po bootu uvidíš (při `DEBUG_LEVEL=2`):

```
╔══════════════════════════════════════╗
║ LaskaKit Meteo [EXPERIMENT+TEST]     ║
║  Boot #1                             ║
║  RTC:   0/112   NVS:   0/150         ║
║  Čas:  nesynchronizováno             ║
╚══════════════════════════════════════╝

[Konfigurace] TESTOVACÍ (TEST_MODE)
  S1 → xxxxxx-xxxxxx.tmep.cz
  S2 → yyyyyy-yyyyyy.tmep.cz
  S3 → zzzzzz-zzzzzz.tmep.cz

[Senzory]
  SHT40:OK  BME280:OK  LTR390:OK

[Měření]
  Baterie: 2240 mV → 4.000 V
  SHT40:  T=22.45°C  RH=58.32%  AH=11.40 g/m³
  BME280: P=1003.21 hPa
  LTR390: ALS=1024.00  UVS=0.32

[WiFi]
  MojeWifi  RSSI:-58  IP:192.168.1.105

[NTP]
  NTP..... OK: 2026-03-01 14:23:05

[Buffer]
  RTC: 1/112  [1×1m  0×10m]
  NVS: 0/150  (hodinové průměry)
  Sent RTC: S1:0 S2:0 S3:0
  Sent NVS: S1:0 S2:0 S3:0

[Odesílání]
  [NVS hodinové průměry] 0 záznamů — nic k odeslání
  [RTC] Fronta: 1/112  [1×1m 0×10m]  čeká: 1
  [S1 teplota/vlhkost/tlak] 1 řádků → https://k6cpfr-bhnx8n.tmep.cz
    → HTTP 200 OK  (834 ms)
  [S2 světlo/UV/abs.vlhkost] 1 řádků → https://...
    → HTTP 200 OK  (712 ms)
  [S3 baterie] 1 řádků → https://...
    → HTTP 200 OK  (598 ms)
  Uvolněno 1 odeslaných slotů, zbývá 0

[Spánek] běh:6823 ms → spánek:8177 ms
```

### 7.2 Co zkontrolovat

| Co vidíš | Co to znamená |
|---|---|
| `SHT40:OK  BME280:OK  LTR390:OK` | Všechny senzory fungují |
| `SHT40:CHYBA` | Zkontroluj pájení I2C sběrnice (SDA=GPIO19, SCL=GPIO18) |
| `WiFi OK` + IP adresa | WiFi připojeno |
| `WiFi TIMEOUT` | Špatné heslo nebo slabý signál |
| `HTTP 200 OK` | Data úspěšně odeslána na TMEP |
| `HTTP 400` | Špatný formát URL nebo dat — zkontroluj URL v config.h |
| `HTTP -1` | Nelze se připojit — zkontroluj URL (https://, bez lomítka na konci) |

### 7.3 Ověřit data na TMEP.cz

Přejdi na stránku svého senzoru na tmep.cz — první naměřená hodnota by se měla objevit do minuty od odeslání.

---

## 8. Kalibrace baterie

ADC na ESP32-C3 není přesné — před nasazením změř skutečné napětí baterie voltmetrem.

### Postup kalibrace

1. Připoj plně nabitou baterii
2. V Serial monitoru najdi řádek:
   ```
   Baterie: 2240 mV → 4.000 V
   ```
3. Změř skutečné napětí baterie voltmetrem (např. 3.87 V)
4. Spočítej nový koeficient:
   ```
   ADC_TO_BATV = skutečné_napětí_V × 1000 / ADC_hodnota_mV
   ```
   Příklad: `3870 / 2240 = 1.7277`
5. Uprav v `FW_experimental.ino`:
   ```cpp
   #define ADC_TO_BATV  1.7277f
   ```

> Výchozí hodnota `1.7857` byla kalibrována na konkrétním exempláři desky (2240 mV ADC = 4.000 V). Každá deska se mírně liší.

---

## 9. Přechod na produkci

Až firmware funguje správně s testovacími URL, přejdi na produkci:

### 9.1 Změny v `FW_experimental.ino`

```cpp
// Zakomentuj TEST_MODE:
// #define TEST_MODE
```

### 9.2 Změny v `config.h`

```cpp
#define SLEEP_SEC   60   // 1 měření za minutu místo 15s
#define DEBUG_LEVEL  1   // jen chyby + souhrn (šetří výpočetní čas)
```

### 9.3 Reset WiFi konfigurace (volitelné)

Pokud chceš smazat uloženou WiFi síť (např. při přesunu na jiné místo):
- Přidej do `setup()` před `connectWiFi()` řádek:
  ```cpp
  WiFiManager wm; wm.resetSettings();
  ```
- Nahraj, spustí se portál, nastav novou síť
- Odstraň řádek a nahraj znovu

---

## 10. Dvouúrovňový buffer — jak funguje

Při výpadku WiFi se záznamy průběžně průměrují do hrubších vrstev a přesouvají
mezi dvěma pamětmi — RTC RAM a NVS flash. Díky tomu se starší data neztrácí
ani při vybití baterie, jen se snižuje jejich rozlišení.

### Paměťové úrovně

| Paměť | Přežije deep sleep | Přežije výpadek napájení | Obsah |
|---|---|---|---|
| **RTC RAM** | ✓ | ✗ | Vrstva 0 (1 min) + Vrstva 1 (10 min) |
| **NVS flash** | ✓ | ✓ | Vrstva 2 (1 hod) — hodinové průměry |

### Schéma kompakce

```
Vrstva 0 (1min, RTC)    →  Vrstva 1 (10min, RTC)  :  10 nejstarších → 1 průměr
Vrstva 1 (10min, RTC)   →  Vrstva 2 (1h, NVS)     :   6 nejstarších → 1 průměr (flashAppend)
Vrstva 2 (1h, NVS)      →  zahazuje (FIFO)         :  při dosažení 150 hodinových záznamů
```

Kompakce se spouští automaticky po každém měření funkcí `tieredCompact()`.

### Kapacita při SLEEP_SEC=60

```
vrstva │ paměť │ granularita │ slotů │ min. pokrytí  │ výpočet
───────┼───────┼─────────────┼───────┼───────────────┼──────────────────
  0    │  RTC  │    1 min    │   70  │    1 hodina   │ (70−10)×1min
  1    │  RTC  │   10 min    │   42  │    6 hodin    │ (42−6)×10min
  2    │  NVS  │    1 hod    │  150  │  150 hodin    │ FIFO (nejstarší ven)
───────┼───────┼─────────────┼───────┼───────────────┼──────────────────
       │       │             │  262  │   ~7 dní      │ (7h RTC + 6.25d NVS)
```

*Minimální pokrytí RTC = co zbyde po kompakci. NVS je FIFO — při naplnění se
zahazuje nejstarší hodinový záznam.*

### Příklad: 3 dny bez WiFi

```
Boot 1–60:    RTC plní vrstvu0 (1min záznamy)
Boot 70:      vrstva0 dosáhla 70 → kompakce 10×1min → 1×10min (vrstva1 v RTC)
...
Boot 480:     vrstva1 dosáhla 42 → flashAppend: 6×10min → 1×1h → uloží do NVS
              RTC se zmenší, NVS roste o 1 hodinový průměr za hodinu
...
Po 3 dnech:   NVS: 72 hodinových průměrů  RTC: ~7 hodin aktuálních dat
```

### Chování po obnovení WiFi

Po obnovení spojení se odešle:
1. **sendNvsBuffered()** — nejdřív nejstarší hodinové záznamy z NVS (od nejstaršího)
2. **sendAllBuffered()** — pak aktuální minutová/desetiminutová data z RTC

Každý buffer (NVS i RTC) má per-server offsety → žádné duplicity ani při výpadku jednoho serveru.

Buffer/NVS se uvolní až po HTTP 200 od **všech tří serverů**.

### Paměť

```
RTC RAM:  112 slotů × 36 B = 4 032 B  (limit: 8 192 B)
NVS flash: 150 slotů × 36 B = 5 400 B  (limit Preferences namespace: 508 KB)
NVS zápisy: ~1×/hodinu = ~8 760/rok → opotřebení zanedbatelné (výdrž 11+ let)
```

RTC RAM přežije deep sleep, ale **ne výpadek napájení**.
NVS flash přežije obojí — hodinové průměry zůstanou i po vybití baterie.

### Měření bez platného timestampu

Při prvním bootu nebo po výpadku napájení je `lastNTPTime = 0`.
Firmware se nejdřív pokusí o WiFi + NTP sync a **až pak uloží měření** do bufferu.
Pokud WiFi selže a timestamp zůstane 0, **měření se zahodí** — data bez časového
kontextu by v TMEP time-series databázi způsobovala zkreslení (TMEP by jim přiřadil
čas odeslání, ne čas měření).

---

## 11. Per-server odesílání

Firmware odesílá na tři nezávislé TMEP.cz servery, každý s jinou sadou dat.
Každý server má vlastní offset — sleduje kolik záznamů přijal.

### Datové kanály

| Server | G1 | G2 | G3 | Vždy |
|---|---|---|---|---|
| S1 | teplota (°C) | rel. vlhkost (%) | tlak (hPa) | baterie (V), RSSI |
| S2 | světlo ALS | UV index | abs. vlhkost (g/m³) | baterie (V), RSSI |
| S3 | baterie (V) | — | — | baterie (V), RSSI |

### Odolnost vůči výpadku jednoho serveru

```
Boot 1:
  S1 → HTTP 200  (s1Sent = 5)
  S2 → HTTP 500  (s2Sent = 0)   ← selhání
  S3 → HTTP 200  (s3Sent = 5)
  compact = min(5, 0, 5) = 0   → buffer se NEUVOLNÍ

Boot 2:
  S1 → odešle jen nový záznam [5]  → bez duplicit
  S2 → odešle záznamy [0–5]        → retry + nový
  S3 → odešle jen nový záznam [5]  → bez duplicit
  compact = min(6, 6, 6) = 6  → buffer uvolněn
```

---

## 12. Debug výstup

### Připojení

Otevři Serial Monitor v Arduino IDE: **Tools → Serial Monitor → 115200 baud**

### Automatická detekce USB

```cpp
debugLevel = Serial ? DEBUG_LEVEL : 0;
```

- **USB kabel připojen** → použije se `DEBUG_LEVEL` z `config.h`
- **USB kabel odpojen** → `debugLevel = 0` (ticho, šetří čas a energii)

### Úrovně

| Level | Co uvidíš |
|---|---|
| `0` | Nic (produkce bez kabelu) |
| `1` | Záhlaví bootu, WiFi status, HTTP výsledky, chyby senzorů |
| `2` | Vše z level 1 + hodnoty senzorů, buffer složení, fronta |
| `3` | Vše z level 2 + celá těla CSV odesílaná na server |

> Pro ladění použij `DEBUG_LEVEL 3`, pro běžný monitoring `DEBUG_LEVEL 2`, pro produkci `DEBUG_LEVEL 1`.

### Vysvětlení výstupu záhlaví

```
╔══════════════════════════════════════╗
║ LaskaKit Meteo [EXPERIMENT+TEST]     ║  ← TEST_MODE je zapnutý
║  Boot #42                            ║  ← 42. probuzení od posledního výpadku
║  RTC:  87/112   NVS:  23/150         ║  ← RTC buffer + NVS flash
║  Čas:  2026-03-01 14:23:05           ║  ← odhadnutý lokální čas
╚══════════════════════════════════════╝
```

Pokud nastane výpadek napájení, přibyde řádek:
```
║  [!] VÝPADEK NAPÁJENÍ — RTC resetován ║
```

### Vysvětlení výstupu bufferu

```
RTC: 87/112  [60×1m  27×10m]     ← minutové + desetiminutové záznamy v RTC RAM
NVS: 23/150  (hodinové průměry)  ← hodinové průměry v NVS flash
Sent RTC: S1:60 S2:60 S3:60      ← per-server offsety pro RTC buffer
Sent NVS: S1:20 S2:20 S3:20      ← per-server offsety pro NVS buffer
```

---

## 13. Architektura firmwaru

### Deep sleep flow

```
wakeup
   │
   ├─ WiFi OFF, ADC input, Serial init
   ├─ delay(100) → USB CDC enumerace
   ├─ debugLevel = Serial ? DEBUG_LEVEL : 0
   ├─ I2C power ON, Wire.begin
   ├─ Detekce výpadku napájení (rtcMagic)
   │   └─ výpadek: reset RTC proměnných, načti nvsCount z NVS
   ├─ bootCount++, TZ nastavit
   ├─ [Záhlaví] Boot #, RTC count, NVS count, čas
   │
   ├─ [Senzory] sht4.begin(), bme.begin(), ltr.begin()
   │
   ├─ [Měření]
   │   ├─ readBatVoltage() — analogReadMilliVolts × ADC_TO_BATV
   │   ├─ m.timestamp = getCurrentTimestamp()  ← může být 0 (před NTP)
   │   ├─ readSHT40()      — T, RH, AH (abs. vlhkost dopočítána)
   │   ├─ readBME280()     — P (forced mode)
   │   └─ readLTR390()     — UVS + ALS (2× 500ms delay)
   │
   ├─ [WiFi + NTP]  ← PŘED uložením do bufferu (aby timestamp byl platný)
   │   ├─ první boot → WiFiManager portál (180s timeout)
   │   ├─ ostatní   → WiFi.begin() (WIFI_TIMEOUT_SEC × 10 pokusů)
   │   ├─ [NTP]  syncNTP() → aktualizuje lastNTPTime
   │   └─ if (m.timestamp==0 && lastNTPTime>0) m.timestamp = lastNTPTime
   │
   ├─ [Buffer]
   │   ├─ if (m.timestamp == 0) → ZAHODIT ← WiFi selhala při prvním bootu,
   │   │                                     data bez času jsou pro TMEP bezcenná
   │   ├─ buffer[bufferCount++] = m  (tier=0)
   │   └─ tieredCompact()
   │       ├─ vrstva0 ≥ TIER0_MAX(70) → 10×tier0 → 1×tier1 (v RTC)
   │       └─ vrstva1 ≥ TIER1_MAX(42) → 6×tier1 → 1×tier2 → flashAppend() (do NVS)
   │
   ├─ [Odesílání]  (jen při wifiConnected)
   │   ├─ sendNvsBuffered()  ← nejdřív nejstarší hodinová data z NVS flash
   │   │   └─ per-server offsety: nvs1Sent, nvs2Sent, nvs3Sent
   │   └─ sendAllBuffered()  ← pak aktuální minutová/10min data z RTC
   │       ├─ per-server offsety: s1Sent, s2Sent, s3Sent
   │       └─ uvolni záznamy přijaté všemi třemi (memmove + dekrementuj Sent)
   │
   ├─ WiFi.disconnect()
   ├─ I2C power OFF
   └─ deep sleep na max(1s, SLEEP_SEC×1000 - elapsed_ms)
```

Veškerá logika je v `setup()`. `loop()` je prázdný — po `esp_deep_sleep_start()` se nikdy nedostane.

### RTC RAM — co přežije deep sleep

```cpp
RTC_DATA_ATTR uint32_t    rtcMagic;        // detekce výpadku napájení
RTC_DATA_ATTR uint8_t     bufferCount;     // počet záznamů v RTC bufferu
RTC_DATA_ATTR uint32_t    bootCount;       // čítač probuzení
RTC_DATA_ATTR uint32_t    lastNTPTime;     // Unix timestamp posledního NTP syncu
RTC_DATA_ATTR uint32_t    lastNTPBoot;     // bootCount při posledním NTP syncu
RTC_DATA_ATTR bool        wifiConfigured;  // příznak prvního WiFi nastavení
RTC_DATA_ATTR uint8_t     s1Sent, s2Sent, s3Sent;         // per-server offsety (RTC)
RTC_DATA_ATTR uint8_t     nvs1Sent, nvs2Sent, nvs3Sent;   // per-server offsety (NVS)
RTC_DATA_ATTR uint8_t     nvsCount;        // cache počtu NVS záznamů
RTC_DATA_ATTR uint8_t     s1Fail, s2Fail, s3Fail;
RTC_DATA_ATTR int16_t     s1LastCode, s2LastCode, s3LastCode;
RTC_DATA_ATTR Measurement buffer[BUFFER_MAX];  // 112 × 36 B = 4 032 B
```

### NVS flash — co přežije výpadek napájení

Hodinové průměry (tier2) jsou uloženy v `Preferences` namespace `"meteo"`:

```
klíč "cnt"  → uint8_t  — počet uložených záznamů
klíč "buf"  → bytes    — nvsCount × sizeof(Measurement)
```

Data se načítají **lazy** — jen při `flashAppend()` nebo `sendNvsBuffered()`.
Zápis probíhá ~1× za hodinu (při vzniku nového hodinového průměru).

### Timestampy bez RTC

Bez externího RTC se čas odhaduje:
```
getCurrentTimestamp() = lastNTPTime + (bootCount − lastNTPBoot) × SLEEP_SEC
```

Drift je zlomky sekundy na cyklus — pro meteorologii dostačující.
Pokud `lastNTPTime == 0` (před prvním NTP syncem), vrací 0.
Měření s `timestamp == 0` se **nezapíše do bufferu** — viz sekce 10.

### CSV formát odesílaný na TMEP

```
YYYY-MM-DD HH:MM:SS;G1;G2;G3;voltage;rssi
2026-03-01 14:23:05;22.45;58.32;1003.21;4.000;-58
2026-03-01 14:24:05;22.47;58.28;1003.19;3.998;-58
```

Každý řádek = jedno měření. Batch = všechny záznamy od posledního úspěšného odeslání.

---

## 14. Řešení problémů

### Senzor hlásí CHYBA

| Problém | Možná příčina | Řešení |
|---|---|---|
| `SHT40:CHYBA` | Špatná I2C adresa nebo zapojení | Zkontroluj pájení SDA(GPIO19)/SCL(GPIO18) |
| `BME280:CHYBA` | I2C adresa 0x76 místo 0x77 | Změn `BME280_ADDR` na `0x76` |
| `LTR390:CHYBA` | Nedostatečné napájení | Zkontroluj GPIO4 (I2C power) |

### WiFi se stále připojuje přes portál

Příznak `wifiConfigured` se uložil jako `false` — může nastat po výpadku napájení.
Portál se spustí jen jednou, pak si zapamatuje. Nastav WiFi znovu přes portál.

### HTTP 400 Bad Request

- Zkontroluj URL v `config.h` — nesmí mít `/index.php` ani jiné přípony
- URL musí začínat `https://` a nesmí mít lomítko na konci: `https://xxxxxx-yyyyyy.tmep.cz`
- Ověř, že senzor na TMEP.cz existuje a je aktivní

### HTTP -1 (connection failed)

- Zkontroluj, že URL začíná `https://` (ne `http://`)
- TMEP batch endpoint vyžaduje HTTPS — HTTP vrací 400

### Buffer se nemaže ani po úspěšném odeslání

Jeden ze tří serverů pravděpodobně nevrátil HTTP 200 — zkontroluj řádek:
```
Selhání: S1=0×(200) S2=3×(500) S3=0×(200)
```
Buffer se uvolní až po potvrzení od všech tří.

### Timestamp je "nesynchronizováno"

Před prvním NTP syncem nebo po výpadku baterie. Normální stav.
Po úspěšném WiFi připojení se synchronizuje automaticky.

> **Důležité:** Při prvním bootu nebo po výpadku napájení se firmware nejdřív
> pokusí o WiFi + NTP, a teprve pak uloží měření do bufferu. Pokud WiFi selže
> a timestamp zůstane 0, **měření se zahodí** — viz sekce 10. Přístroj tak
> nezahltí TMEP záznamy bez smysluplného časového kontextu.

### Buffer stoupá, ale NVS zůstává 0

NVS se plní až po kompakci vrstva1→vrstva2, která nastane po **42 desetiminutových
záznamech** (= ~7 hodin měření). Předtím NVS zůstává prázdné — to je normální.

### Po výpadku napájení zmizela stará data

RTC RAM (minutová + desetiminutová data) se při výpadku napájení vynuluje.
Data v NVS flash (hodinové průměry) přežijí a odešlou se při příštím WiFi připojení.
Zobrazí se hlášení `[!] VÝPADEK NAPÁJENÍ — RTC resetován` v záhlaví.

### Deska nejde nahrát

- Drž tlačítko **BOOT** a stiskni **RESET** → pustit BOOT
- Zkontroluj, že je zvolen správný COM port
- Zkontroluj nastavení **USB CDC On Boot = Enabled**

---

## 15. Hardware a zapojení

### Komponenty

| Komponenta | Účel | I2C adresa |
|---|---|---|
| ESP32-C3 (LaskaKit Meteo Mini) | MCU, WiFi | — |
| SHT40 | teplota, relativní vlhkost | 0x44 |
| BME280 | atmosferický tlak | 0x77 |
| LTR390 | UV index, světlo (ALS) | 0x53 |
| LiPol 900 mAh | napájení | — |
| Solární panel 5V/4W | dobíjení přes TP4056 | — |

### Piny

| Signál | GPIO | Poznámka |
|---|---|---|
| SDA | 19 | I2C data |
| SCL | 18 | I2C hodiny |
| I2C napájení | 4 | uSUP EN (v4.1); pro v3.5 použij GPIO3 |
| ADC baterie | 0 | Dělič napětí na desce |

> **Pozor na verzi desky:** LaskaKit Meteo Mini v4.1 má `PIN_I2C_PWR = 4`, starší v3.5 má `PIN_I2C_PWR = 3`. Zkontroluj a uprav v `FW_experimental.ino` pokud potřeba.

### Spotřeba

| Stav | Proud |
|---|---|
| Deep sleep | ~10–15 µA |
| Aktivní měření | ~20–40 mA |
| WiFi přenos | ~80–150 mA |
| Typický průměr (60s cyklus) | ~0.3–0.5 mA |

Při kapacitě baterie 900 mAh a solárním panelu 5V/4W vydrží jednotka bez přímého slunce přibližně **75–125 dní** (závisí na WiFi dostupnosti a teplotě).

---

## 16. Referenční tabulky

### Paměťová mapa

**RTC RAM** (přežije deep sleep):
```
Proměnné (metadata):     ~60 B
Buffer (112 × 36 B):   4 032 B
───────────────────────────────
Celkem:                4 092 B
Limit ESP32-C3 RTC:    8 192 B
Volné:                 4 100 B
```

**NVS flash** (přežije výpadek napájení):
```
nvsLocalBuf (150 × 36 B):  5 400 B   ← statická globální proměnná, načtena lazy
NVS klíče: "cnt" + "buf"              ← namespace "meteo" v Preferences
Zápisy: ~1×/hodinu → výdrž 11+ let
```

### Struktura záznamu `Measurement` (36 B)

| Pole | Typ | Velikost | Popis |
|---|---|---|---|
| `timestamp` | `uint32_t` | 4 B | Unix UTC (0 = neznámý) |
| `tier` | `uint8_t` | 1 B | Vrstva: 0=1min, 1=10min, 2=1h |
| `_pad[3]` | `uint8_t[3]` | 3 B | Zarovnání na 4 B |
| `temperature` | `float` | 4 B | °C (-100 = nedostupný) |
| `relHumidity` | `float` | 4 B | % RH (-1 = nedostupný) |
| `pressure` | `float` | 4 B | hPa (-1 = nedostupný) |
| `absHumidity` | `float` | 4 B | g/m³ (-1 = nedostupný) |
| `als` | `float` | 4 B | Světlo (-1 = nedostupný) |
| `uvs` | `float` | 4 B | UV index (-1 = nedostupný) |
| `batVoltage` | `float` | 4 B | V |

### Použité knihovny

| Knihovna | Verze | Zdroj |
|---|---|---|
| Adafruit SHT4x | ≥ 1.0 | Library Manager |
| Adafruit BME280 | ≥ 2.2 | Library Manager |
| Adafruit LTR390 | ≥ 1.0 | Library Manager |
| Adafruit Unified Sensor | ≥ 1.1 | Library Manager (závislost) |
| WiFiManager (tzapu) | ≥ 2.0 | Library Manager |
| Arduino ESP32 core | ≥ 2.0 | Boards Manager |
