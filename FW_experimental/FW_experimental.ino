/* LaskaKit DIY Mini Weather Station — EXPERIMENTÁLNÍ VERZE
 * SHT40 + BME280 + LTR390 edition
 *
 * EXPERIMENTÁLNÍ FUNKCE:
 *  - Dvouúrovňový buffer:
 *      RTC RAM  — vrstva0 (1min) + vrstva1 (10min), pokrytí ~7 hodin
 *                 přežije deep sleep, reset při výpadku napájení
 *      NVS flash — vrstva2 (1hod), pokrytí 150 dní
 *                 přežije výpadek napájení, lazy load
 *  - Batch CSV POST (HTTPS) pro všechny 3 servery
 *  - Per-server buffer tracking (každý server ví co mu bylo odesláno)
 *  - Debug levely 0-3, detekce USB připojení
 *
 * Board:   ESP32-C3 Dev Module
 * Konfigurace: config.h
 */

// testovací provoz
//#define TEST_MODE           // zapne testovací URL
//#define CLEAR_BUFFERS     // jednorázové vymazání všech bufferů (RTC + NVS); po nahrání zakomentuj

// Interní kód: HTTP 400 + staré datum → přeskočit nejstarší záznam (viz HTTP_STALE_RESPONSE)
#define HTTP_STALE  (-2)

#include "config.h"
#include "pinout.h"
#ifdef HAS_NEOPIXEL
#include <Adafruit_NeoPixel.h>
#endif
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include "Adafruit_SHT4x.h"
#include <Adafruit_BME280.h>
#include "Adafruit_LTR390.h"
#include <time.h>

// hardwarové konstanty
#define BME280_ADDR  0x77
#define ADC_TO_BATV  1.7857f   // kalibrováno: 2240 mV ADC = 4.0 V
#define RTC_MAGIC    0x4D455447UL  // "METG" — detekce výpadku napájení (zvýšit při změně struktury)

// sizeof = 40 B (4+1+1+1+1 + 8×4)
struct Measurement {
    uint32_t timestamp;    // Unix UTC (0 = neznámý)
    uint8_t  tier;         // vrstva: 0=1min  1=10min  2=1h
    uint8_t  powerLossCnt; // počet výpadků napájení od prvního spuštění (z NVS, max 255)
    uint8_t  runDuration;  // délka předchozího boot cyklu v 0.1 s (0 = první boot / neznámý; max 25.5 s)
    uint8_t  _pad;         // padding na 4 B hranici
    float    temperature;  // °C   (-100 = nedostupný)
    float    relHumidity;  // % RH (  -1 = nedostupný)
    float    pressure;     // hPa  (  -1 = nedostupný)
    float    absHumidity;  // g/m³ (  -1 = nedostupný)
    float    als;          // ALS  (  -1 = nedostupný)
    float    uvs;          // UVS  (  -1 = nedostupný)
    float    batVoltage;   // V
    float    pcbTemp;      // °C   interní teplota ESP32 čipu (-100 = nedostupný)
};

// RTC RAM — přežije deep sleep, reset při výpadku napájení
RTC_DATA_ATTR uint32_t    rtcMagic       = 0;      // detekce výpadku napájení
RTC_DATA_ATTR uint16_t    bufferCount    = 0;
RTC_DATA_ATTR uint32_t    bootCount      = 0;
RTC_DATA_ATTR uint32_t    lastNTPTime    = 0;
RTC_DATA_ATTR uint32_t    lastNTPBoot    = 0;
RTC_DATA_ATTR bool        wifiConfigured = false;
// Per-server offsety pro RTC buffer
RTC_DATA_ATTR uint8_t     s1Sent         = 0;
RTC_DATA_ATTR uint8_t     s2Sent         = 0;
RTC_DATA_ATTR uint8_t     s3Sent         = 0;
// Per-server offsety pro NVS buffer (přežijí deep sleep; reset po výpadku = odešle vše znovu)
RTC_DATA_ATTR uint8_t     nvs1Sent       = 0;
RTC_DATA_ATTR uint8_t     nvs2Sent       = 0;
RTC_DATA_ATTR uint8_t     nvs3Sent       = 0;
RTC_DATA_ATTR uint8_t     nvsCount       = 0;      // cache počtu záznamů v NVS
// Diagnostika
RTC_DATA_ATTR uint8_t     lastRunDuration = 0;  // délka předchozího cyklu v 0.1 s (uložena před spánkem)
RTC_DATA_ATTR uint8_t     powerLossCnt    = 0;  // počet výpadků napájení (načten z NVS při powerLoss)
// Per-server statistiky selhání
RTC_DATA_ATTR uint8_t     s1Fail         = 0;
RTC_DATA_ATTR uint8_t     s2Fail         = 0;
RTC_DATA_ATTR uint8_t     s3Fail         = 0;
RTC_DATA_ATTR int16_t     s1LastCode     = 0;
RTC_DATA_ATTR int16_t     s2LastCode     = 0;
RTC_DATA_ATTR int16_t     s3LastCode     = 0;
RTC_DATA_ATTR Measurement buffer[BUFFER_MAX];

// NVS flash buffer — hodinové průměry, lazy load
static Measurement nvsLocalBuf[FLASH_MAX];  // 5 400 B, načten z NVS při potřebě
static bool        nvsLoaded = false;

// senzory
Adafruit_SHT4x  sht4;
Adafruit_BME280 bme;
Adafruit_LTR390 ltr;

// debug: 0 = ticho | 1 = chyby + souhrn | 2 = normal | 3 = verbose
static uint8_t debugLevel = 0;

#define DLOG(lvl, ...)  do { if (debugLevel >= (lvl)) Serial.printf(__VA_ARGS__); } while(0)
#define DLOGLN(lvl, s)  do { if (debugLevel >= (lvl)) Serial.println(s); } while(0)
#define DPRINT(lvl, s)  do { if (debugLevel >= (lvl)) Serial.print(s); } while(0)

#include "test.h"

// ----------------------------------------
// Timestamp

uint32_t getCurrentTimestamp() {
    if (lastNTPTime == 0) return 0;
    return lastNTPTime + (bootCount - lastNTPBoot) * SLEEP_SEC;
}

String formatTimestamp(uint32_t ts) {
    if (ts == 0) return "?";
    struct tm t;
    time_t tt = (time_t)ts;
    localtime_r(&tt, &t);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    return String(buf);
}

// ----------------------------------------
// Baterie

float readBatVoltage() {
    uint32_t mv = analogReadMilliVolts(PIN_ADC);
    float v = (float)mv * ADC_TO_BATV / 1000.0f;
    DLOG(2, "  Baterie: %u mV → %.3f V\n", mv, v);
    return v;
}

// ----------------------------------------
// Senzory

void readSHT40(Measurement& m) {
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sensors_event_t humidity, temp;
    sht4.getEvent(&humidity, &temp);
    m.temperature = temp.temperature;
    m.relHumidity = humidity.relative_humidity;
    float es = 6.112f * expf((17.67f * m.temperature) / (m.temperature + 243.5f));
    float e  = (m.relHumidity / 100.0f) * es;
    m.absHumidity = (e * 1000.0f) / (461.5f * (m.temperature + 273.15f)) * 100.0f;
    DLOG(2, "  SHT40:  T=%.2f°C  RH=%.2f%%  AH=%.4f g/m³\n",
         m.temperature, m.relHumidity, m.absHumidity);
}

void readBME280(Measurement& m) {
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X4,
                    Adafruit_BME280::SAMPLING_NONE,
                    Adafruit_BME280::FILTER_X4);
    delay(10);
    m.pressure = bme.readPressure() / 100.0f;
    DLOG(2, "  BME280: P=%.2f hPa\n", m.pressure);
}

void readLTR390(Measurement& m) {
    ltr.setGain(LTR390_GAIN_6);
    ltr.setResolution(LTR390_RESOLUTION_18BIT);
    DPRINT(2, "  LTR390: UVS...");
    ltr.setMode(LTR390_MODE_UVS);
    ltr.readUVS();   // vyčistí DATA_STATUS ze starého ALS měření (jinak vrátí 0)
    delay(600);      // čekej na čerstvé UV měření (18bit = 100ms integrace + rezerva)
    if (ltr.newDataAvailable()) {
        m.uvs = ltr.readUVS();
        DLOG(2, " %.2f\n", m.uvs);
    } else { DLOGLN(2, " žádná data"); }
    DPRINT(2, "  LTR390: ALS...");
    ltr.setMode(LTR390_MODE_ALS);
    delay(500);
    if (ltr.newDataAvailable()) {
        m.als = ltr.readALS();
        DLOG(2, " %.2f\n", m.als);
    } else { DLOGLN(2, " žádná data"); }
}

// ----------------------------------------
// WiFi

bool connectWiFi() {
    WiFi.mode(WIFI_STA);
    if (!wifiConfigured) {
        DLOGLN(1, "  První spuštění — portál: AP=LaskaKitMeteo heslo=meteostation");
        WiFiManager wm;
        wm.setConfigPortalTimeout(180);
        bool ok = wm.autoConnect("LaskaKitMeteo", "meteostation");
        if (ok) wifiConfigured = true;
        return ok;
    }
    DPRINT(1, "  WiFi");
    WiFi.begin();
    for (int i = 0; i < WIFI_TIMEOUT_SEC * 10; i++) {
        if (WiFi.status() == WL_CONNECTED) { DLOGLN(1, " OK"); return true; }
        if (i % 10 == 0) DPRINT(2, ".");
        delay(100);
    }
    DLOGLN(1, " TIMEOUT");
    return false;
}

// ----------------------------------------
// NTP

void syncNTP() {
    configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
    DPRINT(2, "  NTP");
    time_t now = 0;
    for (int i = 0; i < 30 && now < 1000000000UL; i++) {
        if (i % 5 == 0) DPRINT(2, ".");
        delay(200);
        time(&now);
    }
    if (now > 1000000000UL) {
        lastNTPTime = (uint32_t)now;
        lastNTPBoot = bootCount;
        DLOG(1, " OK: %s\n", formatTimestamp(lastNTPTime).c_str());
    } else {
        DLOGLN(1, " SELHALO");
    }
}

// ----------------------------------------
// NVS flash buffer — hodinové průměry

// Načte záznamy z NVS do nvsLocalBuf (lazy — volej před přístupem k nvsLocalBuf)
void nvsEnsureLoaded() {
    if (nvsLoaded) return;
    if (nvsCount > 0) {
        Preferences p;
        p.begin("meteo", true);
        p.getBytes("buf", nvsLocalBuf, nvsCount * sizeof(Measurement));
        p.end();
        DLOG(2, "  [NVS] Načteno %u záznamů z flash\n", nvsCount);
    }
    nvsLoaded = true;
}

// Zapíše nvsLocalBuf + nvsCount do NVS (volej po každé změně)
void nvsFlush() {
    Preferences p;
    p.begin("meteo", false);
    p.putUChar("cnt", nvsCount);
    if (nvsCount > 0)
        p.putBytes("buf", nvsLocalBuf, nvsCount * sizeof(Measurement));
    p.end();
}

// ----------------------------------------
// Buffer — tiered průměrování

// průměr n záznamů od startIdx → nový záznam s daným tier
Measurement computeAverage(uint8_t startIdx, uint8_t n, uint8_t toTier) {
    float tSum=0, rhSum=0, pSum=0, ahSum=0, alsSum=0, uvsSum=0, batSum=0, pcbSum=0;
    uint8_t vT=0, vRH=0, vP=0, vAH=0, vALS=0, vUVS=0, vPCB=0;
    uint16_t rdSum = 0;
    for (uint8_t i = 0; i < n; i++) {
        const Measurement& r = buffer[startIdx + i];
        batSum += r.batVoltage;
        rdSum  += r.runDuration;
        if (r.temperature > -99.f) { tSum   += r.temperature;  vT++;   }
        if (r.relHumidity  >= 0.f) { rhSum  += r.relHumidity;  vRH++;  }
        if (r.pressure     >= 0.f) { pSum   += r.pressure;     vP++;   }
        if (r.absHumidity  >= 0.f) { ahSum  += r.absHumidity;  vAH++;  }
        if (r.als          >= 0.f) { alsSum += r.als;          vALS++; }
        if (r.uvs          >= 0.f) { uvsSum += r.uvs;          vUVS++; }
        if (r.pcbTemp      > -99.f){ pcbSum += r.pcbTemp;      vPCB++; }
    }
    Measurement avg = {};
    avg.tier         = toTier;
    avg.timestamp    = ((uint64_t)buffer[startIdx].timestamp + buffer[startIdx + n - 1].timestamp) / 2;
    avg.batVoltage   = batSum / n;
    avg.runDuration  = (uint8_t)(rdSum / n);
    avg.powerLossCnt = buffer[startIdx + n - 1].powerLossCnt;  // poslední (nejvyšší) hodnota
    avg.temperature  = vT   ? tSum   / vT   : -100.f;
    avg.relHumidity  = vRH  ? rhSum  / vRH  : -1.f;
    avg.pressure     = vP   ? pSum   / vP   : -1.f;
    avg.absHumidity  = vAH  ? ahSum  / vAH  : -1.f;
    avg.als          = vALS ? alsSum / vALS : -1.f;
    avg.uvs          = vUVS ? uvsSum / vUVS : -1.f;
    avg.pcbTemp      = vPCB ? pcbSum / vPCB : -100.f;
    return avg;
}

// Přesune 6 nejstarších tier1 záznamů z RTC bufferu jako hodinový průměr do NVS
void flashAppend() {

    if (bufferCount < 6)
        return;  // ochrana

    // ověř, že prvních 6 jsou opravdu tier1
    for (uint8_t i = 0; i < 6; i++) {
        if (buffer[i].tier != 1)
            return;  // nekonzistence → raději nic nedělat
    }

    Measurement avg = computeAverage(0, 6, 2);

    // bezpečné odebrání prvních 6 prvků
    uint16_t remaining = bufferCount - 6;

    if (remaining > 0) {
        memmove(buffer, buffer + 6, remaining * sizeof(Measurement));
    }

    bufferCount = remaining;

    // ochrana proti přetečení
    if (bufferCount > BUFFER_MAX)
        bufferCount = BUFFER_MAX;

    // Opravit per-server offsety po odebrání prvních 6 tier1 záznamů z čela bufferu
    if (s1Sent >= 6) s1Sent -= 6; else s1Sent = 0;
    if (s2Sent >= 6) s2Sent -= 6; else s2Sent = 0;
    if (s3Sent >= 6) s3Sent -= 6; else s3Sent = 0;

    // ---- NVS část zůstává stejná ----
    nvsEnsureLoaded();

    if (nvsCount >= FLASH_MAX) {
        memmove(nvsLocalBuf,
                nvsLocalBuf + 1,
                (nvsCount - 1) * sizeof(Measurement));
        nvsCount--;
        if (nvs1Sent > 0) nvs1Sent--;
        if (nvs2Sent > 0) nvs2Sent--;
        if (nvs3Sent > 0) nvs3Sent--;
    }

    nvsLocalBuf[nvsCount++] = avg;
    nvsFlush();
}


// Kompakce RTC bufferu:
//   tier0 (70) → tier1  : 10 nejstarších tier0 průměruje do 1 tier1 (zůstane v RTC)
//   tier1 (42) → NVS    : 6 nejstarších tier1 průměruje do 1 tier2 (zapíše do NVS)
void tieredCompact() {
    // Průměrování N tier0 záznamů od startIdx → 1 tier1 v RTC bufferu
    auto doCompact = [&](uint8_t startIdx, uint8_t n) {
        if (bufferCount < startIdx + n)
            return;  // ochrana proti underflow

        Measurement avg = computeAverage(startIdx, n, 1);

        uint16_t moveSrc = startIdx + n;
        uint16_t moveDst = startIdx + 1;
        uint16_t moveLen = bufferCount - moveSrc;

        if (moveLen > 0) {
            memmove(buffer + moveDst,
                    buffer + moveSrc,
                    moveLen * sizeof(Measurement));
        }

        buffer[startIdx] = avg;
        bufferCount -= (n - 1);

        if (bufferCount > BUFFER_MAX)
            bufferCount = BUFFER_MAX;

        // Opravit per-server offsety — memmove posunul záznamy, indexy by jinak stály
        auto adjSent = [&](uint8_t& sent) {
            if (sent <= startIdx)     return;           // před kompakcí → beze změny
            if (sent >= startIdx + n) sent -= (n - 1); // za kompakcí → posunout zpět
            else                      sent  = startIdx; // uvnitř → tier1 avg ještě neodesláno
        };
        adjSent(s1Sent); adjSent(s2Sent); adjSent(s3Sent);

        DLOG(1, "  [RTC] Kompakce tier0→tier1  (%u×→1×)  buffer: %u/%u\n",
             n, bufferCount, (uint8_t)BUFFER_MAX);
    };

    bool changed = true;
    while (changed) {
        changed = false;

        uint8_t cnt0 = 0, cnt1 = 0;
        for (uint16_t i = 0; i < bufferCount; i++) {
            if      (buffer[i].tier == 0) cnt0++;
            else if (buffer[i].tier == 1) cnt1++;
        }
        uint8_t base0 = cnt1;  // tier0 záznamy začínají za tier1

        if (cnt0 >= TIER0_MAX) {
            doCompact(base0, 10);  // 10 nejstarších tier0 → 1 tier1
            changed = true;
            continue;
        }
        if (cnt1 >= TIER1_MAX) {
            flashAppend();         // 6 nejstarších tier1 → NVS
            changed = true;
            continue;
        }
    }
}

// ----------------------------------------
// Odeslání — HTTPS batch POST

String buildCsvRow(uint32_t ts,
                   const String& guid1, const String& guid2, const String& guid3,
                   float voltage, int rssi) {
    return formatTimestamp(ts)
         + ";" + guid1 + ";" + guid2 + ";" + guid3
         + ";" + String(voltage, 3)
         + ";" + String(rssi)
         + "\n";
}

// Sestaví CSV z pole záznamů (RTC nebo NVS buffer)
String buildCsvFrom(const Measurement* buf, uint8_t from, uint8_t count,
                    uint8_t srv, int rssi) {
    String csv;
    csv.reserve((count - from) * 55);
    for (uint8_t i = from; i < count; i++) {
        const Measurement& m = buf[i];
        if (srv == 1)
            csv += buildCsvRow(m.timestamp,
                               m.temperature > -99.f ? String(m.temperature, 2) : "",
                               m.relHumidity  >= 0.f ? String(m.relHumidity, 2) : "",
                               m.pressure     >= 0.f ? String(m.pressure, 2)    : "",
                               m.batVoltage, rssi);
        else if (srv == 2)
            csv += buildCsvRow(m.timestamp,
                               m.als         >= 0.f ? String(m.als, 2)         : "",
                               m.uvs         >= 0.f ? String(m.uvs, 2)         : "",
                               m.absHumidity >= 0.f ? String(m.absHumidity, 4) : "",
                               m.batVoltage, rssi);
        else  // srv == 3: diagnostika (PCB temp + výpadky napájení + délka runu)
            csv += buildCsvRow(m.timestamp,
                               m.pcbTemp > -99.f ? String(m.pcbTemp, 1) : "",
                               String(m.powerLossCnt),
                               (m.runDuration <= 150)
                                   ? String(m.runDuration / 10.0f, 1)
                                   : String((int)(m.runDuration - 135)),
                               m.batVoltage, rssi);
    }
    csv.trim();
    return csv;
}

// Vypíše náhled CSV: ≤4 řádky celé, jinak první 2 + "... N dalších ..." + poslední 2
// Volej jen uvnitř bloku (debugLevel >= 3).
void printCsvPreview(const String& csv, uint8_t rowCount) {
    if (rowCount <= 4) {
        Serial.println(csv);
        return;
    }
    // první 2 řádky
    int pos = 0;
    for (uint8_t i = 0; i < 2; i++) {
        int nl = csv.indexOf('\n', pos);
        if (nl < 0) break;
        Serial.println(csv.substring(pos, nl));
        pos = nl + 1;
    }
    Serial.printf("  ... %u dalších ...\n", rowCount - 4);
    // poslední 2 řádky (csv je trimmed → bez trailing \n)
    int last2 = csv.length();
    for (uint8_t i = 0; i < 2; i++) {
        int nl = csv.lastIndexOf('\n', last2 - 1);
        if (nl < 0) { last2 = 0; break; }
        last2 = nl + 1;
    }
    Serial.println(csv.substring(last2));
}

int httpPostCsv(const char* label, const char* url, const String& csvBody,
                uint8_t rowCount, uint8_t& failCount, int16_t& lastCode) {
    DLOG(1, "  [%s] %u řádků → %s\n", label, rowCount, url);
    if (debugLevel >= 3) {
        DLOGLN(3, "  ── CSV ──────────────────────────────────────");
        printCsvPreview(csvBody, rowCount);
        DLOGLN(3, "  ─────────────────────────────────────────────");
    }
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    unsigned long t = millis();
    http.begin(client, url);
    http.setTimeout(HTTP_TIMEOUT_SEC * 1000);
    http.addHeader("Content-Type", "text/csv");
    int code = http.POST(csvBody);
    String resp = http.getString();
    http.end();

    lastCode = (int16_t)code;
    if (code == 200) {
        failCount = 0;
        DLOG(1, "    → HTTP 200 OK  (%lu ms)\n", millis() - t);
    } else {
        // Staré datum — server odmítl záznam, přeskočit (nevykazovat jako selhání)
        if (code == 400
                && sizeof(HTTP_STALE_RESPONSE) > 1
                && resp.indexOf(HTTP_STALE_RESPONSE) >= 0) {
            DLOG(1, "    → HTTP 400 staré datum — přeskočuji nejstarší záznam  (%lu ms)\n", millis() - t);
            return HTTP_STALE;
        }
        failCount++;
        DLOG(1, "    → HTTP %d  (%lu ms)  selhání #%u\n", code, millis() - t, failCount);
        if (debugLevel >= 2 && resp.length() > 0) {
            if (resp.length() > 120)
                DLOG(2, "    Odpověď: %.120s…\n", resp.c_str());
            else
                DLOG(2, "    Odpověď: %s\n", resp.c_str());
        }
    }
    DLOG(1, "\n\n");
    return code;
}

// Zobrazí souhrn fronty: 2 nejstarší + "... N dalších ..." + 2 nejnovější
void printQueueSummary(const Measurement* buf, uint8_t from, uint8_t total) {
    if (total == 0) return;
    auto printRow = [&](uint8_t i) {
        const Measurement& m = buf[i];
        DLOG(2, "    [T%u] %s  T=%.1f°C  RH=%.1f%%  P=%.1f hPa  PCB=%.1f°C  BAT=%.3fV\n",
             m.tier,
             formatTimestamp(m.timestamp).c_str(),
             m.temperature, m.relHumidity, m.pressure, m.pcbTemp, m.batVoltage);
    };
    uint8_t end = from + total;
    if (total <= 4) {
        for (uint8_t i = from; i < end; i++) printRow(i);
    } else {
        printRow(from);
        printRow(from + 1);
        DLOG(2, "    ... %u dalších ...\n", total - 4);
        printRow(end - 2);
        printRow(end - 1);
    }
}

// Odešle NVS flash buffer (hodinové průměry) — nejdřív, před RTC daty
void sendNvsBuffered(int rssi) {
    if (nvsCount == 0) return;
    uint8_t minSent = min({nvs1Sent, nvs2Sent, nvs3Sent});
    if (minSent >= nvsCount) return;

    nvsEnsureLoaded();
    uint8_t pending = nvsCount - minSent;

    DLOG(1, "[NVS fronta] %u/%u  (S1:%u S2:%u S3:%u odesláno | čeká: %u)\n",
         nvsCount, (uint8_t)FLASH_MAX, nvs1Sent, nvs2Sent, nvs3Sent, pending);
    if (debugLevel >= 2) {
        printQueueSummary(nvsLocalBuf, minSent, pending);
        DLOGLN(2, "");
    }

    auto sendNvsServer = [&](uint8_t srv, const char* label, const char* url,
                              uint8_t& sent, uint8_t& fail, int16_t& lastCode) {
        while (sent < nvsCount) {
            String csv = buildCsvFrom(nvsLocalBuf, sent, nvsCount, srv, rssi);
            int code = httpPostCsv(label, url, csv, nvsCount - sent, fail, lastCode);
            if (code == 200)        { sent = nvsCount; return; }
            else if (code == HTTP_STALE) sent++;   // přeskočit nejstarší, zkusit znovu
            else                         return;   // jiná chyba → příští boot
        }
        DLOG(2, "  [%s] vše odesláno\n", label);
    };

    sendNvsServer(1, "NVS/S1 T+RH+P",      serverName1, nvs1Sent, s1Fail, s1LastCode);
    sendNvsServer(2, "NVS/S2 ALS+UV+AH",  serverName2, nvs2Sent, s2Fail, s2LastCode);
    sendNvsServer(3, "NVS/S3 diagnostika", serverName3, nvs3Sent, s3Fail, s3LastCode);

    // Compact — smazat záznamy přijaté všemi třemi servery
    uint8_t compact = min({nvs1Sent, nvs2Sent, nvs3Sent});
    if (compact > 0) {
        memmove(nvsLocalBuf, nvsLocalBuf + compact,
                (nvsCount - compact) * sizeof(Measurement));
        nvsCount -= compact;
        nvs1Sent -= compact; nvs2Sent -= compact; nvs3Sent -= compact;
        nvsFlush();
        DLOG(2, "  [NVS] Uvolněno %u hodinových záznamů, zbývá %u\n", compact, nvsCount);
    }
}

// Odešle RTC buffer (minutová + desetiminutová data)
void sendAllBuffered(int rssi) {
    uint8_t minSent = min({s1Sent, s2Sent, s3Sent});

    uint8_t cnt0 = 0, cnt1 = 0;
    for (uint16_t i = 0; i < bufferCount; i++) {
        if      (buffer[i].tier == 0) cnt0++;
        else if (buffer[i].tier == 1) cnt1++;
    }

    DLOG(1, "[RTC fronta] %u/%u  [%u×1m %u×10m]  (S1:%u S2:%u S3:%u | čeká: %u)\n",
         bufferCount, (uint8_t)BUFFER_MAX, cnt0, cnt1,
         s1Sent, s2Sent, s3Sent, bufferCount - minSent);

    if (debugLevel >= 2 && bufferCount > minSent) {
        printQueueSummary(buffer, minSent, bufferCount - minSent);
        DLOGLN(2, "");
    }

    auto sendServer = [&](uint8_t srv, const char* label, const char* url,
                          uint8_t& sent, uint8_t& fail, int16_t& lastCode) {
        while (sent < bufferCount) {
            String csv = buildCsvFrom(buffer, sent, bufferCount, srv, rssi);
            int code = httpPostCsv(label, url, csv, bufferCount - sent, fail, lastCode);
            if (code == 200)         { sent = bufferCount; return; }
            else if (code == HTTP_STALE) sent++;   // přeskočit nejstarší, zkusit znovu
            else                         return;   // jiná chyba → příští boot
        }
        DLOG(2, "  [%s] vše odesláno\n", label);
    };

    sendServer(1, "RTC/S1 T+RH+P",      serverName1, s1Sent, s1Fail, s1LastCode);
    sendServer(2, "RTC/S2 ALS+UV+AH",  serverName2, s2Sent, s2Fail, s2LastCode);
    sendServer(3, "RTC/S3 diagnostika", serverName3, s3Sent, s3Fail, s3LastCode);

    if (s1Fail > 0 || s2Fail > 0 || s3Fail > 0)
        DLOG(1, "  Selhání: S1=%u×(%d) S2=%u×(%d) S3=%u×(%d)\n",
             s1Fail, s1LastCode, s2Fail, s2LastCode, s3Fail, s3LastCode);

    // Compact RTC — uvolní záznamy přijaté všemi třemi servery
    uint8_t compact = min({s1Sent, s2Sent, s3Sent});
    if (compact > 0) {
        memmove(buffer, buffer + compact, (bufferCount - compact) * sizeof(Measurement));
        bufferCount -= compact;
        s1Sent -= compact; s2Sent -= compact; s3Sent -= compact;
        DLOG(2, "  [RTC] Uvolněno %u slotů, zbývá %u\n", compact, bufferCount);
    }
}

// ----------------------------------------
// Vymazání bufferů — voláno z CLEAR_BUFFERS i z checkBootButton()

void clearAllBuffers() {
    bufferCount = 0;
    s1Sent = s2Sent = s3Sent = 0;
    nvs1Sent = nvs2Sent = nvs3Sent = 0;
    lastRunDuration = 0;
    nvsLoaded = true; nvsCount = 0;
    nvsFlush();       // zapíše cnt=0 do NVS
    nvsLoaded = false;
    Serial.println("[clearAllBuffers] RTC buffer + NVS smazány.");
}

// ----------------------------------------
// Boot tlačítko + NeoPixel

#if defined(HAS_BUTTON) && defined(HAS_NEOPIXEL)

static Adafruit_NeoPixel btnLed(1, PIN_LED, NEO_GRB + NEO_KHZ800);

// R,G,B pro každou zónu (SK6812 RGB)
static const uint8_t ZONE_COLORS[6][3] = {
    { 0, 50,  0},  // 0: zelená  — normální boot   (0–ZONE1)
    { 0,  0, 50},  // 1: modrá   — reset WiFi      (ZONE1–ZONE2)
    { 0, 30, 30},  // 2: cyan    — smaž buffery    (ZONE2–ZONE3)
    {50, 50, 50},  // 3: bílá    — factory bez WiFi (ZONE3–ZONE4)
    {50,  0,  0},  // 4: červená — factory úplný   (ZONE4–ZONE5)
    { 0, 50,  0},  // 5: zelená  — přestřeleno     (ZONE5+)
};

static void btnLedSet(uint8_t zone) {
    btnLed.setPixelColor(0, btnLed.Color(
        ZONE_COLORS[zone][0], ZONE_COLORS[zone][1],
        ZONE_COLORS[zone][2]));
    btnLed.show();
}

static void btnLedOff() {
    btnLed.setPixelColor(0, 0);
    btnLed.show();
}

static uint8_t getButtonZone(unsigned long ms) {
    if (ms < BTN_MS_ZONE1) return 0;
    if (ms < BTN_MS_ZONE2) return 1;
    if (ms < BTN_MS_ZONE3) return 2;
    if (ms < BTN_MS_ZONE4) return 3;
    if (ms < BTN_MS_ZONE5) return 4;
    return 5;
}

void checkBootButton() {
    // VSENSOR musí být zapnuto před čtením tlačítka — pull-up IO5 je na VSENSOR railu
    pinMode(PIN_I2C_PWR, OUTPUT);
    digitalWrite(PIN_I2C_PWR, HIGH);
    delay(5);  // čas pro stabilizaci napájení

    pinMode(PIN_BTN, INPUT);
    if (digitalRead(PIN_BTN) == HIGH) return;  // tlačítko nestisknuto → přeskoč
    // (VSENSOR zůstane zapnutý — setup() ho stejně potřebuje pro I2C senzory)

    btnLed.begin();
    btnLed.setBrightness(40);
    btnLedSet(0);  // zelená — tlačítko detekováno

    unsigned long pressStart = millis();
    uint8_t zone = 0;

    // Sleduj zónu dokud je tlačítko stisknuté
    while (digitalRead(PIN_BTN) == LOW) {
        uint8_t z = getButtonZone(millis() - pressStart);
        if (z != zone) { zone = z; btnLedSet(zone); }
        delay(10);
    }

    // Zóna 0 nebo 5 = žádná akce
    if (zone == 0 || zone == 5) {
        btnLedOff();
        return;
    }

    // Potvrzovací fáze: BTN_CONFIRM_MS blikání, stisk = zrušení
    bool cancelled = false;
    bool blinkOn = true;
    unsigned long lastBlink = millis();
    unsigned long confirmStart = millis();

    while (millis() - confirmStart < BTN_CONFIRM_MS) {
        if (digitalRead(PIN_BTN) == LOW) { cancelled = true; break; }
        if (millis() - lastBlink >= 250) {
            blinkOn = !blinkOn;
            blinkOn ? btnLedSet(zone) : btnLedOff();
            lastBlink = millis();
        }
        delay(10);
    }

    btnLedOff();

    if (cancelled) {
        DLOGLN(1, "[Button] Akce zrušena.");
        while (digitalRead(PIN_BTN) == LOW) delay(10);  // čekej na uvolnění
        return;
    }

    // Proveď akci
    switch (zone) {
        case 1:
            DLOGLN(1, "[Button] Reset WiFi přihlašovacích údajů.");
            wifiConfigured = false;
            { WiFiManager wm; wm.resetSettings(); }
            break;
        case 2:
            DLOGLN(1, "[Button] Smazání RTC + NVS bufferů.");
            clearAllBuffers();
            break;
        case 3:
            DLOGLN(1, "[Button] Factory reset bez WiFi (buffery + bootCount).");
            clearAllBuffers();
            bootCount = 0; lastNTPTime = 0; lastNTPBoot = 0;
            break;
        case 4:
            DLOGLN(1, "[Button] Factory reset úplný (buffery + bootCount + WiFi).");
            clearAllBuffers();
            bootCount = 0; lastNTPTime = 0; lastNTPBoot = 0;
            wifiConfigured = false;
            { WiFiManager wm; wm.resetSettings(); }
            break;
    }

    // Ulož zónu do NVS — přežije restart spolehlivě (RTC zápisy nemusí být flushed)
    // rtcMagic = 0 → powerLoss detekce → zaručený reset bootCount přes powerLoss blok
    {
        Preferences p;
        p.begin("meteo", false);
        p.putUChar("btnact", zone);
        if (zone == 3 || zone == 4) p.putUChar("ploss", 0);  // factory reset → nuluj čítač výpadků
        p.end();
    }
    rtcMagic = 0;

    Serial.flush();
    delay(200);
    esp_restart();
}

#endif  // HAS_BUTTON && HAS_NEOPIXEL

// ----------------------------------------
// setup / loop

void setup() {
    unsigned long startMs = millis();

    WiFi.mode(WIFI_OFF);
    pinMode(PIN_ADC, INPUT);
    Serial.begin(115200);
    delay(100);  // čas pro USB CDC enumeraci
    debugLevel = Serial ? DEBUG_LEVEL : 0;

#if defined(HAS_BUTTON) && defined(HAS_NEOPIXEL)
    checkBootButton();
#endif

#ifdef CLEAR_BUFFERS
    clearAllBuffers();
#endif
#ifdef TEST_ADJSENT
    Serial.println("\n[TEST_ADJSENT] Spouštím testy adjSent...");
    testAdjSent();
#endif
#ifdef TEST_RUNDURATION
    Serial.println("\n[TEST_RUNDURATION] Spouštím testy runDuration encoding...");
    testRunDuration();
#endif

    pinMode(PIN_I2C_PWR, OUTPUT);
    digitalWrite(PIN_I2C_PWR, HIGH);
    delay(10);
    Wire.begin(PIN_SDA, PIN_SCL);

    // detekce výpadku napájení
    bool    powerLoss = (rtcMagic != RTC_MAGIC);
    uint8_t btnAction = 0;   // != 0 pokud byl restart vyvolán tlačítkem (zóna 1-4)
    if (powerLoss) {
        rtcMagic      = RTC_MAGIC;
        bufferCount   = 0;
        bootCount     = 0;
        lastNTPTime   = 0;
        lastNTPBoot   = 0;
        s1Sent = s2Sent = s3Sent = 0;
        s1Fail = s2Fail = s3Fail = 0;
        s1LastCode = s2LastCode = s3LastCode = 0;
        nvs1Sent = nvs2Sent = nvs3Sent = 0;
        wifiConfigured  = true;  // předpokládáme uložené WiFi credentials z NVS
        lastRunDuration = 0;
        Preferences p;
        p.begin("meteo", false);
        nvsCount  = p.getUChar("cnt",    0);
        btnAction = p.getUChar("btnact", 0);
        if (btnAction != 0) {
            p.remove("btnact");
            powerLossCnt = p.getUChar("ploss", 0);  // neinkrementuj — nebyl výpadek
            if (btnAction == 1 || btnAction == 4) wifiConfigured = false;
        } else {
            powerLossCnt = p.getUChar("ploss", 0);
            if (powerLossCnt < 255) powerLossCnt++;
            p.putUChar("ploss", powerLossCnt);
        }
        p.end();
    }

    bootCount++;
    setenv("TZ", TIMEZONE, 1);
    tzset();

    // záhlaví
    auto resetStr = [] {
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:   return "power-on";
            case ESP_RST_DEEPSLEEP: return "sleep";
            case ESP_RST_SW:        return "restart";
            case ESP_RST_EXT:       return "ext. reset";
            case ESP_RST_BROWNOUT:  return "BROWNOUT (!)";
            case ESP_RST_PANIC:     return "PANIC (!)";
            case ESP_RST_INT_WDT:   return "WDT-INT (!)";
            case ESP_RST_TASK_WDT:  return "WDT-TASK (!)";
            case ESP_RST_WDT:       return "WDT (!)";
            default:                return "?";
        }
    };
    DLOGLN(1, "");
    DLOGLN(1, "╔══════════════════════════════════════╗");
#ifdef TEST_MODE
    DLOGLN(1, "║ LaskaKit Meteo [EXPERIMENT+TEST]     ║");
#else
    DLOGLN(1, "║  LaskaKit Meteo Mini [EXPERIMENT]    ║");
#endif
    DLOG(1,   "║  Boot #%-5lu  Reset: %-14s ║\n", (unsigned long)bootCount, resetStr());
    DLOG(1,   "║  RTC: %3u/%-3u   NVS: %3u/%-3u          ║\n",
         bufferCount, (uint8_t)BUFFER_MAX, nvsCount, (uint8_t)FLASH_MAX);
    if (lastNTPTime > 0)
        DLOG(1, "║  Čas:  %-29s ║\n", formatTimestamp(getCurrentTimestamp()).c_str());
    else
        DLOGLN(1, "║  Čas:  nesynchronizováno             ║");
    if (powerLoss && btnAction == 0)
        DLOGLN(1, "║  [!] VÝPADEK NAPÁJENÍ — RTC resetován ║");
    if (btnAction != 0) {
        static const char* const BTN_NAMES[] =
            {"", "reset WiFi", "smazání bufferů", "factory bez WiFi", "factory úplný"};
        DLOG(1, "║  [Button] %-26s ║\n", btnAction <= 4 ? BTN_NAMES[btnAction] : "?");
    }
    DLOGLN(1, "╚══════════════════════════════════════╝");

    // konfigurace
    if (debugLevel >= 2) {
#ifdef TEST_MODE
        DLOGLN(2, "\n[Konfigurace] TESTOVACÍ (TEST_MODE)");
#else
        DLOGLN(2, "\n[Konfigurace] ostré data");
#endif
        auto host = [](const char* url) -> String {
            String s(url); int a = s.indexOf("//") + 2, b = s.indexOf("/", a);
            return (b > a) ? s.substring(a, b) : s.substring(a);
        };
        DLOG(2, "  S1 → %s\n", host(serverName1).c_str());
        DLOG(2, "  S2 → %s\n", host(serverName2).c_str());
        DLOG(2, "  S3 → %s\n", host(serverName3).c_str());
    }

    // inicializace senzorů
    DLOGLN(2, "\n[Senzory]");
    bool shtOK = sht4.begin();
    bool bmeOK = bme.begin(BME280_ADDR);
    bool ltrOK = ltr.begin();
    DLOG(2, "  SHT40:%s  BME280:%s  LTR390:%s\n",
         shtOK ? "OK" : "CHYBA", bmeOK ? "OK" : "CHYBA", ltrOK ? "OK" : "CHYBA");
    if (!shtOK || !bmeOK || !ltrOK)
        DLOG(1, "  [!] Senzor nedostupný: %s%s%s\n",
             shtOK ? "" : "SHT40 ", bmeOK ? "" : "BME280 ", ltrOK ? "" : "LTR390");

    // měření
    DLOGLN(2, "\n[Měření]");
    Measurement m = {};
    m.tier          = 0;
    m.powerLossCnt  = powerLossCnt;     // počet výpadků napájení z NVS
    m.runDuration   = lastRunDuration;  // délka předchozího cyklu (0 při prvním bootu)
    m.temperature   = -100.0f;
    m.pcbTemp       = -100.0f;
    m.relHumidity   = m.pressure = m.absHumidity = m.als = m.uvs = -1.0f;
    m.batVoltage    = readBatVoltage();
    m.pcbTemp       = temperatureRead() + PCB_TEMP_OFFSET;
    DLOG(2, "  ESP32 PCB: %.1f°C\n", m.pcbTemp);
    m.timestamp     = getCurrentTimestamp();
    if (shtOK) readSHT40(m);
    if (bmeOK) readBME280(m);
    if (ltrOK) readLTR390(m);

    // WiFi + NTP před uložením do bufferu, aby timestamp byl platný
    DLOGLN(2, "\n[WiFi]");
    //WiFiManager wm; wm.resetSettings();
    //wifiConfigured = false;

    bool wifiConnected = connectWiFi();
    int rssi = 0;
    if (wifiConnected) {
        rssi = WiFi.RSSI();
        DLOG(1, "  %s  RSSI:%d  IP:%s\n",
             WiFi.SSID().c_str(), rssi, WiFi.localIP().toString().c_str());
        DLOGLN(2, "\n[NTP]");
        syncNTP();
        // Opravit timestamp měření pokud při měření nebyl znám (lastNTPTime==0)
        if (m.timestamp == 0 && lastNTPTime > 0)
            m.timestamp = lastNTPTime;
    } else {
        DLOG(1, "  Offline — RTC: %u/%u  NVS: %u/%u\n",
             bufferCount, (uint8_t)BUFFER_MAX, nvsCount, (uint8_t)FLASH_MAX);
    }

    // buffer — timestamp==0 zahazujeme (WiFi selhala, NTP nikdy neprovedl)
    DLOGLN(2, "\n[Buffer]");
    // Validace fyzikálně smysluplných hodnot
    bool invalidT  = (m.temperature  < -60.f || m.temperature  > 85.f);
    bool invalidRH = (m.relHumidity  > 105.f);
    if (invalidT)  DLOG(1, "  [!] Teplota mimo rozsah (%.1f°C) — ukládám jako nedostupnou\n", m.temperature);
    if (invalidRH) DLOG(1, "  [!] Vlhkost mimo rozsah (%.1f%%) — ukládám jako nedostupnou\n", m.relHumidity);
    if (invalidT)  { m.temperature = -100.f; m.relHumidity = -1.f; m.absHumidity = -1.f; }
    if (invalidRH) { m.relHumidity = -1.f; m.absHumidity = -1.f; }

    if (m.timestamp == 0) {
        DLOGLN(1, "  [!] Měření zahozeno — žádný čas (WiFi nedostupná při prvním bootu)");
    } else {
        if (bufferCount < BUFFER_MAX) {
            buffer[bufferCount++] = m;
            tieredCompact();
        } else {
            DLOGLN(1, "  [!] RTC buffer overflow – záznam zahazuji");
        }
    }

    if (debugLevel >= 2) {
        uint8_t cnt0 = 0, cnt1 = 0;
        for (uint16_t i = 0; i < bufferCount; i++) {
            if      (buffer[i].tier == 0) cnt0++;
            else if (buffer[i].tier == 1) cnt1++;
        }
        DLOG(2, "  RTC: %u/%u  [%u×1m  %u×10m]\n",
             bufferCount, (uint8_t)BUFFER_MAX, cnt0, cnt1);
        DLOG(2, "  NVS: %u/%u  (hodinové průměry)\n", nvsCount, (uint8_t)FLASH_MAX);
        DLOG(2, "  Sent RTC: S1:%u S2:%u S3:%u\n", s1Sent, s2Sent, s3Sent);
        DLOG(2, "  Sent NVS: S1:%u S2:%u S3:%u\n", nvs1Sent, nvs2Sent, nvs3Sent);
    }

    // odesílání
    if (wifiConnected) {
        DLOGLN(1, "\n[Odesílání]");
#ifdef TEST_SEND
        sendTest();
#else
        sendNvsBuffered(rssi);   // nejdřív nejstarší NVS data (hodinové průměry)
        sendAllBuffered(rssi);   // pak aktuální RTC data (minutová + desetiminutová)
#endif 
        WiFi.disconnect(true);
    }

    // spánek
    digitalWrite(PIN_I2C_PWR, LOW);
    unsigned long elapsedMs = millis() - startMs;
    {   // nelineární encoding: 0–150 = 0.0–15.0 s (0.1 s kroky), 151–255 = 16–120 s (1 s kroky)
        uint32_t tenths = elapsedMs / 100UL;
        lastRunDuration = (tenths <= 150)
            ? (uint8_t)tenths
            : (uint8_t)min(135UL + elapsedMs / 1000UL, 255UL);
    }
    uint64_t sleepUs = (SLEEP_SEC * 1000UL > elapsedMs + BOOT_OVERHEAD_MS)
                       ? ((uint64_t)(SLEEP_SEC * 1000UL - elapsedMs - BOOT_OVERHEAD_MS)) * 1000ULL
                       : (uint64_t)SLEEP_MIN_SEC * 1000000ULL;
    DLOG(1, "\n[Spánek] běh:%lu ms → spánek:%llu ms\n\n", elapsedMs, sleepUs / 1000ULL);
    esp_sleep_enable_timer_wakeup(sleepUs);
    esp_deep_sleep_start();
}

void loop() {}
