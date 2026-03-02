# TODO — LaskaKit Meteo Mini

## Hotovo
- [x] Refaktoring původního kódu (globální proměnné → lokální, čistší struktura)
- [x] Offline buffer v RTC RAM (50 měření, přežije deep sleep)
- [x] Rychlé WiFi připojení bez blokujícího portálu při výpadku (timeout 10 s)
- [x] Hromadné odeslání bufferu na TMEP.cz s parametrem `t=`
- [x] SHT40 pro teplotu + vlhkost (BME280 jen pro tlak)
- [x] `analogReadMilliVolts()` pro přesnější napětí baterie
- [x] NTP → RTC RAM timestamp (bez DS3231)
- [x] Oprava timestamp=0 po prvním NTP syncu

---

## Hardware — plánované doplnění

- [ ] **DS3231 RTC** — přidat až bude k dispozici modul
  - přesnější timestampy (bez driftu boot counteru)
  - čas zachován i po výpadku napájení (CR2032 baterie)

- [ ] **Světlo — LTR390 zůstává**
  - Hodnoty jsou fyzikálně správné (difuzní záření za oblačnosti
    vs. stín za slunečného dne je reálný jev, ne chyba senzoru)

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
- [ ] Přejít z HTTP na HTTPS pro TMEP.cz (až bude flash prostor)
- [ ] Solární panel — změřit a zalogovat efektivitu nabíjení
- [ ] Zvážit OTA update přes indoor gateway (ESP-NOW OTA nebo BLE)
