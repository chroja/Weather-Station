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
- [x] **Kalibrace `pcbTemp`** — `PCB_TEMP_OFFSET` v `config.h`, přičte se k `temperatureRead()`
- [x] **Validace naměřených hodnot** — T mimo −60…85°C nebo RH > 105 % → uložit jako nedostupné

---

## Firmware — diagnostika

- [x] **Počítadlo výpadků napájení v NVS** — viz Hotovo → `powerLossCnt` (klíč `ploss`)
- [x] **`esp_reset_reason()`** — zobrazit v záhlaví příčinu resetu
  (power-on / sleep / restart / brownout / panic / watchdog); lambda v setup(), řádek Boot #

---

## Hardware — plánované doplnění

- [x] **Button + NeoPixel (v4.1+)** — boot tlačítko IO5 s SK6812 RGBW LED na IO9; zóny 0–10s určují
  akci (modrá=reset WiFi / cyan=smaž buffery / bílá=factory bez WiFi / červená=factory úplný);
  potvrzovací blikání 5s s možností zrušení opětovným stiskem; aktivováno přes `BOARD_VERSION >= 41`
- [ ] **DS18B20** — deska je připravena pro osazení čidla (PIN_1WIRE = IO10), čidlo momentálně neosazeno

---

## Firmware — SHT40 refresh / detekce zaseknutého čidla

SHT40 nabízí vestavěný ohřívač (heater), který lze použít k vysušení kondenzátu na čidle
a zároveň jako základní test funkčnosti — správně fungující čidlo by po krátkém ohřevu
mělo vykázat měřitelný nárůst teploty a pokles relativní vlhkosti.

### Co řešit před implementací

- [ ] **Spouštěcí podmínka** — kdy refresh spustit?
  - pravidelně každých N cyklů (např. 1× za 6 hodin)?
  - pouze při podezřelých hodnotách (RH blízko 100 % po delší dobu)?
  - nebo kombinace obojího?
- [ ] **Detekce "čidlo měří správně"** — definovat kritéria:
  - po ohřevu musí teplota vzrůst alespoň o X °C
  - po ohřevu musí RH klesnout alespoň o Y %
  - pokud podmínky nesplněny → zalogovat jako chybu / poslat na S3 diagnostiku
- [ ] **Dopad na baterii** — změřit a spočítat:
  - SHT40 heater spotřeba dle datasheetu (různé výkony/doby ohřevu)
  - celková energie na jeden refresh cyklus
  - vliv na průměrnou spotřebu při různých intervalech spouštění
- [ ] **Minimální napětí baterie pro spuštění** — stanovit práh:
  - pod X V refresh neprovádět (šetřit baterii při nízkém nabití)
  - hodnotu otestovat v praxi
- [ ] **Testování v praxi** — ověřit chování při různých podmínkách (mlha, déšť, mráz)

> *Tato sekce je zatím pouze úvaha — implementace není plánována v blízké době.*

---

## Velká změna architektury — ESP-NOW + indoor gateway

> *Jedná se o dlouhodobou vizi, nikoliv o něco co se bude v blízké době dít.*


### Fáze 1 — Venkovní jednotka: přechod na ESP-NOW
- [ ] **DS3231 RTC** — zvážit přidání pro přesné timestampy bez NTP; relevatní až po přechodu na ESP-NOW
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

---

## Budoucí směr — univerzální firmware + konfigurace přes web portál

> *Dlouhodobá vize. Jednotlivé kroky jsou na sobě závislé — vhodné řešit najednou jako větší refaktoring.*

### Cíl
Firmware bez `secrets.h` — binárka neobsahuje žádné privátní údaje, vše se konfiguruje přes webový portál a ukládá do NVS flash. Jeden zkompilovaný `.bin` funguje pro libovolnou stanici s libovolnou kombinací čidel.

### Kroky

- [ ] **Secrets z NVS místo secrets.h** — server URLs přesunout z `secrets.h` do `Preferences` (NVS);
  při prvním bootu (prázdné NVS) spustit konfigurační portál
- [ ] **Rozšíření WiFiManager portálu o URL pole** — `WiFiManagerParameter` pro každý server URL;
  uložit do NVS po konfiguraci; `secrets.h` pak nepotřeba vůbec
- [ ] **Auto-detekce čidel přes I2C scan** — při každém bootu projít I2C bus, zaznamenat přítomná čidla;
  inicializovat a měřit pouze detekovaná čidla
  - SHT40/SHT45 = 0x44, BME280/BME688 = 0x76/0x77, LTR390 = 0x53
  - INA219/INA226 = 0x40–0x4F (solární panel), SCD41 = 0x62 (CO2), SGP41 = 0x59 (VOC/NOx)
- [ ] **Podmíněné odesílání podle detekovaných čidel** — S1 jen pokud SHT40 nebo BME280 přítomen;
  S2 jen pokud LTR390 přítomen; S3 (diagnostika) vždy
- [ ] **Trigger portálu při novém čidle** — pokud I2C scan odhalí čidlo bez přiřazené URL v NVS,
  automaticky spustit konfigurační portál pro doplnění
- [ ] **Generická OTA binárka** — po přesunu secrets do NVS lze `.bin` hostovat bez rizika úniku secrets;
  kombinovat s OTA flag přístupem (viz sekce OTA)

---

## OTA update — bez fyzického přístupu

Stanice je namontovaná venku bez přístupu k tlačítku ani USB.
Klasická OTA (Arduino OTA / `httpUpdate`) vyžaduje aby zařízení zůstalo bdělé — ale při deep sleep cyklech se budí jen na ~5–10 s.

### Možné přístupy

- [ ] **"OTA flag" na serveru** — každý boot zařízení zkontroluje URL (např. GitHub raw nebo vlastní endpoint),
  pokud soubor obsahuje `OTA=1`, zůstane bdělé a stáhne firmware přes `httpUpdate`
  - výhoda: nepotřebuje tlačítko, funguje přes existující WiFi
  - nevýhoda: každý boot = 1 HTTP request navíc (~100 ms, malá spotřeba)
- [ ] **Prodloužení bdělosti při detekci OTA** — po detekci příznaku přeskočit spánek a spustit OTA server
  - zařízení se stane dočasně dostupné přes WiFi pro Arduino OTA tool nebo webový upload
- [ ] **Zvážit OTA přes indoor gateway** (až bude ESP-NOW architektura) — gateway má stálé napájení

> *Dokud není vyřešen způsob triggeru bez fyzického přístupu, OTA není bezpečně implementovatelná.*
