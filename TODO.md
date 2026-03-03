# TODO — LaskaKit Meteo Mini

## Hotovo
- [x] Refaktoring původního kódu (globální proměnné → lokální, čistší struktura)
- [x] SHT40 pro teplotu + vlhkost (BME280 jen pro tlak)
- [x] `analogReadMilliVolts()` pro přesnější napětí baterie
- [x] Rychlé WiFi připojení bez blokujícího portálu při výpadku (timeout 10 s)
- [x] NTP → RTC RAM timestamp (bez DS3231), odhad času z boot counteru
- [x] Oprava timestamp=0 po prvním NTP syncu
- [x] Dvouúrovňový RTC buffer (tier0 1min/70 slotů + tier1 10min/42 slotů, pokrytí ~7 h)
- [x] NVS flash buffer (tier2 hodinové průměry, 150 slotů, pokrytí ~6 dní, přežije výpadek napájení)
- [x] Batch CSV POST přes HTTPS (místo HTTP GET s parametrem `t=`)
- [x] Per-server tracking (s1Sent/s2Sent/s3Sent — každý server ví co přijal)
- [x] secrets.h oddělení od config.h (URL serverů nejsou v gitu)
- [x] Debug levely 0–3, automatická detekce USB připojení
- [x] ESP32 interní teplota PCB (`temperatureRead()`) v měření a výpisu
- [x] `adjSent` oprava v `doCompact()` a `flashAppend()` — per-server offsety se nyní správně
  posouvají po každém memmove; přidán unit test `TEST_ADJSENT` (4 scénáře)
- [x] **Server 3 jako diagnostické centrum** — G1=PCB teplota, G2=počet výpadků napájení (NVS),
  G3=délka předchozího cyklu v s; voltage sloupec = baterie
- [x] **Minimum sleep 30 s** — při překročení SLEEP_SEC se spí min 30 s (ochrana před rychlou smyčkou)
- [x] **CLEAR_BUFFERS** — jednorázový compile-time přepínač; po nahrání zavolá `clearAllBuffers()` která
  resetuje RTC buffer a zapíše `cnt=0` do NVS flash (smaže i hodinové průměry); po použití zakomentovat
- [x] **HTTP_STALE_RESPONSE automatický skip** — při HTTP 400 s odpovědí obsahující řetězec z `config.h`
  se nejstarší záznam přeskočí (`sent++`) a retry proběhne ve stejném bootu; takhle se buffer postupně
  vyčistí bez zásahu uživatele (každý server zpracovává staré záznamy nezávisle)

---

## Firmware — opravy chyb
- [x] **HTTP timeout** — `http.setTimeout(HTTP_TIMEOUT_SEC * 1000)` před POST; hodnota konfigurovatelná
  v `config.h` jako `HTTP_TIMEOUT_SEC` (výchozí 8 s)

---

## Firmware — vylepšení

- [ ] **Adaptivní sleep podle napětí baterie** — nízká baterie → delší spánek (méně WiFi pokusů)
- [ ] **Snížení CPU frekvence při měření** — `setCpuFrequencyMhz(80)` před senzory, úspora energie
- [ ] **Kalibrace `pcbTemp`** — přidat `PCB_TEMP_OFFSET` do `config.h` (senzor má ±3–5°C odchylku)
- [ ] **Validace naměřených hodnot** — odmítnout fyzikálně nesmyslná data před zápisem do bufferu
  (T < -60°C nebo > 85°C, RH > 105 %)

---

## Firmware — diagnostika

- [x] **Počítadlo výpadků napájení v NVS** — viz Hotovo → `powerLossCnt` (klíč `ploss`)
- [ ] **`esp_reset_reason()`** — zobrazit v záhlaví příčinu wakeup
  (deep sleep timer vs. watchdog reset vs. výpadek napájení)

---

## Hardware — plánované doplnění

- [ ] **DS3231 RTC** — přidat až bude k dispozici modul
  - přesnější timestampy (bez driftu boot counteru)
  - čas zachován i po výpadku napájení (CR2032 baterie)
- [ ] **Button na IO5** — dlouhý stisk = reset WiFi credentials (`wifiConfigured = false`)
- [ ] **DS18B20** — pin připraven na desce, až bude modul po ruce

---

## Velká změna architektury — ESP-NOW + indoor gateway

### Fáze 1 — Venkovní jednotka: přechod na ESP-NOW
- [ ] Odstranit WiFi připojení k routeru z venkovní jednotky
- [ ] Implementovat ESP-NOW vysílač
  - sestavit packet struct se všemi naměřenými hodnotami
  - `esp_now_send()` trvá ~1–5 ms → výrazná úspora baterie vs. WiFi
  - uložit MAC adresu gateway do `config.h`
- [ ] Zachovat RTC RAM buffer jako zálohu při nedostupnosti gateway

### Fáze 2 — Indoor gateway jednotka
- [ ] Postavit gateway: ESP32 (libovolný, napájení ze zásuvky)
  - přijímá ESP-NOW packety z venkovní jednotky
  - připojuje se k domácí WiFi
  - přeposílá data do Home Assistant
- [ ] Zvolit integraci do HA:
  - **ESPHome** (doporučeno) — konfigurace v YAML, native HA integrace
  - nebo MQTT broker na RPi

### Fáze 3 — Home Assistant
- [ ] Přidat entity v HA (teplota, vlhkost, tlak, UV, světlo, baterie)
- [ ] Dashboard / Lovelace karta
- [ ] Automations / alerting (nízká baterie, extrémní hodnoty)

---

## Ostatní nápady
- [ ] Solární panel — změřit a zalogovat efektivitu nabíjení
- [ ] OTA update přes WiFi (zejména pokud bude stanice venku bez fyzického přístupu)
- [ ] Zvážit OTA update přes indoor gateway (ESP-NOW OTA nebo BLE)
