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

// ── Přepínač testovacího provozu ─────────────────────────────────────────────
#define TEST_MODE
//#define TEST_SEND        // diagnostika: odeslat pevný CSV řádek místo bufferu
//#define TEST_ADJSENT     // jednotkový test: ověří opravu adjSent po tieredCompact/flashAppend
//#define CLEAR_BUFFERS    // jednorázové vymazání všech bufferů (RTC + NVS); po nahrání zakomentuj

// Interní kód: HTTP 400 + staré datum → přeskočit nejstarší záznam (viz HTTP_STALE_RESPONSE)
#define HTTP_STALE  (-2)

#include "config.h"
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

// ── Pinout ────────────────────────────────────────────────────────────────────
#define PIN_SDA      19
#define PIN_SCL      18
#define PIN_I2C_PWR   4   // EN uSUP (pro v4.1 = IO4, pro v3.5 = IO3)
#define PIN_ADC       0

// ── Hardwarové konstanty ──────────────────────────────────────────────────────
#define BME280_ADDR  0x77
#define ADC_TO_BATV  1.7857f   // kalibrováno: 2240 mV ADC = 4.0 V
#define RTC_MAGIC    0x4D455447UL  // "METG" — detekce výpadku napájení (zvýšit při změně struktury)

// ── Datová struktura jednoho měření ──────────────────────────────────────────
// sizeof = 40 B (4+1+1+1+1 + 8×4)
struct Measurement {
    uint32_t timestamp;    // Unix UTC (0 = neznámý)
    uint8_t  tier;         // vrstva: 0=1min  1=10min  2=1h
    uint8_t  powerLossCnt; // počet výpadků napájení od prvního spuštění (z NVS, max 255)
    uint8_t  runDuration;  // délka předchozího boot cyklu v s (0 = první boot / neznámý)
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

// ── RTC RAM — přežije deep sleep, reset při výpadku napájení ─────────────────
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
RTC_DATA_ATTR uint8_t     lastRunDuration = 0;  // délka předchozího cyklu v s (uložena před spánkem)
RTC_DATA_ATTR uint8_t     powerLossCnt    = 0;  // počet výpadků napájení (načten z NVS při powerLoss)
// Per-server statistiky selhání
RTC_DATA_ATTR uint8_t     s1Fail         = 0;
RTC_DATA_ATTR uint8_t     s2Fail         = 0;
RTC_DATA_ATTR uint8_t     s3Fail         = 0;
RTC_DATA_ATTR int16_t     s1LastCode     = 0;
RTC_DATA_ATTR int16_t     s2LastCode     = 0;
RTC_DATA_ATTR int16_t     s3LastCode     = 0;
RTC_DATA_ATTR Measurement buffer[BUFFER_MAX];

// ── NVS flash buffer — hodinové průměry, lazy load ───────────────────────────
static Measurement nvsLocalBuf[FLASH_MAX];  // 5 400 B, načten z NVS při potřebě
static bool        nvsLoaded = false;

// ── Senzory ───────────────────────────────────────────────────────────────────
Adafruit_SHT4x  sht4;
Adafruit_BME280 bme;
Adafruit_LTR390 ltr;

// ── Debug ─────────────────────────────────────────────────────────────────────
// 0 = ticho | 1 = chyby + souhrn | 2 = normal | 3 = verbose (CSV těla)
static uint8_t debugLevel = 0;

#define DLOG(lvl, ...)  do { if (debugLevel >= (lvl)) Serial.printf(__VA_ARGS__); } while(0)
#define DLOGLN(lvl, s)  do { if (debugLevel >= (lvl)) Serial.println(s); } while(0)
#define DPRINT(lvl, s)  do { if (debugLevel >= (lvl)) Serial.print(s); } while(0)

// =============================================================================
// Timestamp
// =============================================================================

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

// =============================================================================
// Baterie
// =============================================================================

float readBatVoltage() {
    uint32_t mv = analogReadMilliVolts(PIN_ADC);
    float v = (float)mv * ADC_TO_BATV / 1000.0f;
    DLOG(2, "  Baterie: %u mV → %.3f V\n", mv, v);
    return v;
}

// =============================================================================
// Senzory
// =============================================================================

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
    delay(500);
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

// =============================================================================
// WiFi
// =============================================================================

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

// =============================================================================
// NTP
// =============================================================================

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

// =============================================================================
// NVS flash buffer — hodinové průměry
// =============================================================================

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

// =============================================================================
// Buffer — tiered průměrování (tier0 → tier1 v RTC, tier1 → tier2 do NVS)
// =============================================================================

// Průměr n záznamů RTC bufferu od startIdx → nový záznam s daným tier
Measurement computeAverage(uint8_t startIdx, uint8_t n, uint8_t toTier) {
    float tSum=0, rhSum=0, pSum=0, ahSum=0, alsSum=0, uvsSum=0, batSum=0, pcbSum=0;
    uint8_t vT=0, vRH=0, vP=0, vAH=0, vALS=0, vUVS=0, vPCB=0;
    uint32_t tsSum = 0;
    uint16_t rdSum = 0;
    for (uint8_t i = 0; i < n; i++) {
        const Measurement& r = buffer[startIdx + i];
        tsSum  += r.timestamp / n;
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
    avg.timestamp    = tsSum;
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
        for (uint8_t i = 0; i < bufferCount; i++) {
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

// =============================================================================
// Odeslání — HTTPS batch POST
// =============================================================================

String buildCsvRow(uint32_t ts,
                   const String& g1, const String& g2, const String& g3,
                   float voltage, int rssi) {
    return formatTimestamp(ts)
         + ";" + g1 + ";" + g2 + ";" + g3
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
                               String(m.temperature, 2), String(m.relHumidity, 2),
                               String(m.pressure, 2), m.batVoltage, rssi);
        else if (srv == 2)
            csv += buildCsvRow(m.timestamp,
                               String(m.als, 2), String(m.uvs, 2),
                               String(m.absHumidity, 4), m.batVoltage, rssi);
        else  // srv == 3: diagnostika (PCB temp + výpadky napájení + délka runu)
            csv += buildCsvRow(m.timestamp,
                               m.pcbTemp > -99.f ? String(m.pcbTemp, 1) : "",
                               String(m.powerLossCnt),
                               m.runDuration > 0 ? String(m.runDuration) : "",
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
    for (uint8_t i = 0; i < bufferCount; i++) {
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

// =============================================================================
// Test funkce (TEST_SEND)
// =============================================================================

#ifdef TEST_SEND
void sendTest() {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, serverName1);
    http.addHeader("Content-Type", "text/csv");
    String csv = "2026-03-01 16:30:00;23.17;54.43;1004.07;4.084;-44";
    int code = http.POST(csv);
    Serial.printf("TEST_SEND: HTTP %d\n", code);
    Serial.println(http.getString());
    http.end();
}
#endif

// =============================================================================
// Jednotkový test adjSent (aktivovat: #define TEST_ADJSENT)
// =============================================================================

#ifdef TEST_ADJSENT
void testAdjSent() {
    uint8_t fails = 0;

    // ── Test 1: doCompact — s*Sent za kompakcí (posunout zpět) ───────────────
    // Stav: 70 tier0, S1+S3 odeslaly 69, S2 nic
    // Po compact(startIdx=0, n=10): bufferCount=61, s1=60, s2=0, s3=60
    bufferCount = 70;
    for (uint8_t i = 0; i < 70; i++) {
        buffer[i] = {}; buffer[i].tier = 0;
        buffer[i].timestamp = 1700000000UL + i * 60;
    }
    s1Sent = 69; s2Sent = 0; s3Sent = 69;
    tieredCompact();
    if (bufferCount != 61 || s1Sent != 60 || s2Sent != 0 || s3Sent != 60) {
        Serial.printf("  [FAIL] test1: bc=%u s1=%u s2=%u s3=%u  (exp 61/60/0/60)\n",
                      bufferCount, s1Sent, s2Sent, s3Sent);
        fails++;
    } else { Serial.println("  [PASS] test1: doCompact — posun zpět"); }

    // ── Test 2: doCompact — s*Sent uvnitř kompakcí (zarovnat na startIdx) ───
    // Stav: 70 tier0, S1 odeslal 5 (uvnitř prvních 10), S3 vše, S2 nic
    // Po compact(0,10): s1=0 (zarovnáno), s2=0, s3=60
    bufferCount = 70;
    for (uint8_t i = 0; i < 70; i++) {
        buffer[i] = {}; buffer[i].tier = 0;
        buffer[i].timestamp = 1700000000UL + i * 60;
    }
    s1Sent = 5; s2Sent = 0; s3Sent = 69;
    tieredCompact();
    if (bufferCount != 61 || s1Sent != 0 || s2Sent != 0 || s3Sent != 60) {
        Serial.printf("  [FAIL] test2: bc=%u s1=%u s2=%u s3=%u  (exp 61/0/0/60)\n",
                      bufferCount, s1Sent, s2Sent, s3Sent);
        fails++;
    } else { Serial.println("  [PASS] test2: doCompact — zarovnání uvnitř"); }

    // ── Test 3: flashAppend — s*Sent >= 6 (posunout zpět) ────────────────────
    // Stav: 42 tier1, S1+S3 odeslaly 40, S2 nic
    // Po flashAppend: bufferCount=36, s1=34, s2=0, s3=34
    bufferCount = 42;
    for (uint8_t i = 0; i < 42; i++) {
        buffer[i] = {}; buffer[i].tier = 1;
        buffer[i].timestamp = 1700000000UL + i * 600;
    }
    s1Sent = 40; s2Sent = 0; s3Sent = 40;
    nvsLoaded = true; nvsCount = 0;  // lazy load přeskočit, NVS zapíše 1 testovací záznam
    tieredCompact();
    if (bufferCount != 36 || s1Sent != 34 || s2Sent != 0 || s3Sent != 34) {
        Serial.printf("  [FAIL] test3: bc=%u s1=%u s2=%u s3=%u  (exp 36/34/0/34)\n",
                      bufferCount, s1Sent, s2Sent, s3Sent);
        fails++;
    } else { Serial.println("  [PASS] test3: flashAppend — posun zpět"); }

    // ── Test 4: flashAppend — s*Sent < 6 (zarovnat na 0) ─────────────────────
    // Stav: 42 tier1, S1 odeslal 3 (méně než 6), S2 nic, S3 vše
    // Po flashAppend: s1=0, s2=0, s3=34
    bufferCount = 42;
    for (uint8_t i = 0; i < 42; i++) {
        buffer[i] = {}; buffer[i].tier = 1;
        buffer[i].timestamp = 1700000000UL + i * 600;
    }
    s1Sent = 3; s2Sent = 0; s3Sent = 40;
    nvsLoaded = true; nvsCount = 0;
    tieredCompact();
    if (bufferCount != 36 || s1Sent != 0 || s2Sent != 0 || s3Sent != 34) {
        Serial.printf("  [FAIL] test4: bc=%u s1=%u s2=%u s3=%u  (exp 36/0/0/34)\n",
                      bufferCount, s1Sent, s2Sent, s3Sent);
        fails++;
    } else { Serial.println("  [PASS] test4: flashAppend — zarovnání pod 6"); }

    Serial.printf("  → Výsledek: %s (%u/%u testů OK)\n\n",
                  fails == 0 ? "VSE PROSEL" : "CHYBY!", 4 - fails, 4);

    // Reset pro normální provoz — vymazat i testovací záznamy z NVS flash
    bufferCount = 0; s1Sent = s2Sent = s3Sent = 0;
    nvs1Sent = nvs2Sent = nvs3Sent = 0;
    nvsLoaded = true; nvsCount = 0;
    nvsFlush();       // zapíše cnt=0 do NVS → testovací záznamy se nepřenesou do reálného provozu
    nvsLoaded = false;
}
#endif

// =============================================================================
// Jednorázové vymazání bufferů (CLEAR_BUFFERS)
// =============================================================================

#ifdef CLEAR_BUFFERS
void clearAllBuffers() {
    bufferCount = 0;
    s1Sent = s2Sent = s3Sent = 0;
    nvs1Sent = nvs2Sent = nvs3Sent = 0;
    lastRunDuration = 0;
    nvsLoaded = true; nvsCount = 0;
    nvsFlush();       // zapíše cnt=0 do NVS
    nvsLoaded = false;
    Serial.println("[CLEAR_BUFFERS] RTC buffer + NVS smazány. Zakomentuj CLEAR_BUFFERS a nahraj znovu.");
}
#endif

// =============================================================================
// Hlavní program
// =============================================================================

void setup() {
    unsigned long startMs = millis();

    WiFi.mode(WIFI_OFF);
    pinMode(PIN_ADC, INPUT);
    Serial.begin(115200);
    delay(100);  // čas pro USB CDC enumeraci
    debugLevel = Serial ? DEBUG_LEVEL : 0;

#ifdef CLEAR_BUFFERS
    clearAllBuffers();
#endif
#ifdef TEST_ADJSENT
    Serial.println("\n[TEST_ADJSENT] Spouštím testy adjSent...");
    testAdjSent();
#endif

    pinMode(PIN_I2C_PWR, OUTPUT);
    digitalWrite(PIN_I2C_PWR, HIGH);
    delay(10);
    Wire.begin(PIN_SDA, PIN_SCL);

    // ── Detekce výpadku napájení ──────────────────────────────────────────────
    bool powerLoss = (rtcMagic != RTC_MAGIC);
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
        lastRunDuration = 0;     // neznámá délka předchozího cyklu po výpadku
        // Načíst NVS metadata a inkrementovat počítadlo výpadků
        Preferences p;
        p.begin("meteo", false);
        nvsCount     = p.getUChar("cnt",   0);
        powerLossCnt = p.getUChar("ploss", 0);
        if (powerLossCnt < 255) powerLossCnt++;
        p.putUChar("ploss", powerLossCnt);
        p.end();
    }

    bootCount++;
    setenv("TZ", TIMEZONE, 1);
    tzset();

    // ── Záhlaví ──────────────────────────────────────────────────────────────
    DLOGLN(1, "");
    DLOGLN(1, "╔══════════════════════════════════════╗");
#ifdef TEST_MODE
    DLOGLN(1, "║ LaskaKit Meteo [EXPERIMENT+TEST]     ║");
#else
    DLOGLN(1, "║  LaskaKit Meteo Mini [EXPERIMENT]    ║");
#endif
    DLOG(1,   "║  Boot #%-5lu                          ║\n", (unsigned long)bootCount);
    DLOG(1,   "║  RTC: %3u/%-3u   NVS: %3u/%-3u          ║\n",
         bufferCount, (uint8_t)BUFFER_MAX, nvsCount, (uint8_t)FLASH_MAX);
    if (lastNTPTime > 0)
        DLOG(1, "║  Čas:  %-29s ║\n", formatTimestamp(getCurrentTimestamp()).c_str());
    else
        DLOGLN(1, "║  Čas:  nesynchronizováno             ║");
    if (powerLoss)
        DLOGLN(1, "║  [!] VÝPADEK NAPÁJENÍ — RTC resetován ║");
    DLOGLN(1, "╚══════════════════════════════════════╝");

    // ── Konfigurace ───────────────────────────────────────────────────────────
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

    // ── Senzory ───────────────────────────────────────────────────────────────
    DLOGLN(2, "\n[Senzory]");
    bool shtOK = sht4.begin();
    bool bmeOK = bme.begin(BME280_ADDR);
    bool ltrOK = ltr.begin();
    DLOG(2, "  SHT40:%s  BME280:%s  LTR390:%s\n",
         shtOK ? "OK" : "CHYBA", bmeOK ? "OK" : "CHYBA", ltrOK ? "OK" : "CHYBA");
    if (!shtOK || !bmeOK || !ltrOK)
        DLOG(1, "  [!] Senzor nedostupný: %s%s%s\n",
             shtOK ? "" : "SHT40 ", bmeOK ? "" : "BME280 ", ltrOK ? "" : "LTR390");

    // ── Měření ───────────────────────────────────────────────────────────────
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

    // ── WiFi + NTP (před uložením do bufferu) ────────────────────────────────
    // NTP sync proběhne dřív než uložení měření, aby timestamp byl platný.
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

    // ── Buffer ────────────────────────────────────────────────────────────────
    // Měření s timestamp==0 zahazujeme — nastává při prvním bootu nebo po výpadku
    // napájení, kdy WiFi selže a NTP se nikdy neprovedl. Data bez časového kontextu
    // nemají v time-series databázi smysl (TMEP by jim přiřadil čas odeslání).
    DLOGLN(2, "\n[Buffer]");
    // Validace fyzikálně smysluplných hodnot
    bool validT  = (m.temperature  < -60.f || m.temperature  > 85.f);
    bool validRH = (m.relHumidity  > 105.f);
    if (validT)  DLOG(1, "  [!] Teplota mimo rozsah (%.1f°C) — ukládám jako nedostupnou\n", m.temperature);
    if (validRH) DLOG(1, "  [!] Vlhkost mimo rozsah (%.1f%%) — ukládám jako nedostupnou\n", m.relHumidity);
    if (validT)  { m.temperature = -100.f; m.relHumidity = -1.f; m.absHumidity = -1.f; }
    if (validRH) { m.relHumidity = -1.f; m.absHumidity = -1.f; }

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
        for (uint8_t i = 0; i < bufferCount; i++) {
            if      (buffer[i].tier == 0) cnt0++;
            else if (buffer[i].tier == 1) cnt1++;
        }
        DLOG(2, "  RTC: %u/%u  [%u×1m  %u×10m]\n",
             bufferCount, (uint8_t)BUFFER_MAX, cnt0, cnt1);
        DLOG(2, "  NVS: %u/%u  (hodinové průměry)\n", nvsCount, (uint8_t)FLASH_MAX);
        DLOG(2, "  Sent RTC: S1:%u S2:%u S3:%u\n", s1Sent, s2Sent, s3Sent);
        DLOG(2, "  Sent NVS: S1:%u S2:%u S3:%u\n", nvs1Sent, nvs2Sent, nvs3Sent);
    }

    // ── Odesílání ─────────────────────────────────────────────────────────────
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

    // ── Spánek ────────────────────────────────────────────────────────────────
    digitalWrite(PIN_I2C_PWR, LOW);
    unsigned long elapsedMs = millis() - startMs;
    lastRunDuration = (uint8_t)min(elapsedMs / 1000UL, 255UL);  // uložit pro příští cyklus
    uint64_t sleepUs = (SLEEP_SEC * 1000UL > elapsedMs)
                       ? ((uint64_t)(SLEEP_SEC * 1000UL - elapsedMs)) * 1000ULL
                       : (uint64_t)SLEEP_MIN_SEC * 1000000ULL;
    DLOG(1, "\n[Spánek] běh:%lu ms → spánek:%llu ms\n\n", elapsedMs, sleepUs / 1000ULL);
    esp_sleep_enable_timer_wakeup(sleepUs);
    esp_deep_sleep_start();
}

void loop() {}
