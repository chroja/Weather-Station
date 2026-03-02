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

---

## Firmware — opravy chyb

- [ ] **`adjSent` v `tieredCompact()` / `doCompact()`** — po kompakci tier0→tier1 se s1Sent/s2Sent/s3Sent
  neaktualizují; při částečném odeslání + offline provozu může dojít ke ztrátě záznamu nebo
  opakovanému odeslání. Stejný problém i ve `flashAppend()`.
- [ ] **HTTP timeout** — HTTPClient používá výchozí timeout ESP32, který může přesáhnout SLEEP_SEC;
  při slabém signálu hrozí zaseknutí bootu. Nastavit explicitní timeout (např. 8 s).

---

## Firmware — vylepšení

- [ ] **Adaptivní sleep podle napětí baterie** — nízká baterie → delší spánek (méně WiFi pokusů)
- [ ] **Snížení CPU frekvence při měření** — `setCpuFrequencyMhz(80)` před senzory, úspora energie
- [ ] **Kalibrace `pcbTemp`** — přidat `PCB_TEMP_OFFSET` do `config.h` (senzor má ±3–5°C odchylku)
- [ ] **Validace naměřených hodnot** — odmítnout fyzikálně nesmyslná data před zápisem do bufferu
  (T < -60°C nebo > 85°C, RH > 105 %)

---

## Firmware — diagnostika

- [ ] **Počítadlo výpadků napájení v NVS** — kolikrát nastal `powerLoss` od prvního spuštění
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
