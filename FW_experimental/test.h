#pragma once

// Testovací přepínače — odkomentuj pro spuštění, po použití zakomentuj
//#define TEST_SEND           // diagnostika: odeslat pevný CSV řádek místo bufferu
//#define TEST_ADJSENT        // jednotkový test: ověří opravu adjSent po tieredCompact/flashAppend
//#define TEST_RUNDURATION    // jednotkový test: ověří nelineární encoding runDuration

// ----------------------------------------

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

// ----------------------------------------

// unit testy adjSent
#ifdef TEST_ADJSENT
void testAdjSent() {
    uint8_t fails = 0;

    // test1: doCompact — s*Sent za kompakcí → posunout zpět
    // 70 tier0, S1+S3 odeslaly 69, S2 nic → bc=61, s1=60, s2=0, s3=60
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

    // test2: doCompact — s*Sent uvnitř kompakcí → zarovnat na startIdx
    // 70 tier0, S1 odeslal 5, S3 vše, S2 nic → s1=0, s2=0, s3=60
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

    // test3: flashAppend — s*Sent >= 6 → posunout zpět
    // 42 tier1, S1+S3 odeslaly 40, S2 nic → bc=36, s1=34, s2=0, s3=34
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

    // test4: flashAppend — s*Sent < 6 → zarovnat na 0
    // 42 tier1, S1 odeslal 3, S2 nic, S3 vše → s1=0, s2=0, s3=34
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

// ----------------------------------------

// unit test: nelineární encoding runDuration
#ifdef TEST_RUNDURATION
void testRunDuration() {
    uint8_t fails = 0;

    // pomocná lambda: encode ms → uint8_t (kopie produkční logiky)
    auto enc = [](unsigned long ms) -> uint8_t {
        uint32_t tenths = ms / 100UL;
        return (tenths <= 150)
            ? (uint8_t)tenths
            : (uint8_t)min(135UL + ms / 1000UL, 255UL);
    };
    // pomocná lambda: decode uint8_t → float sekund
    auto dec = [](uint8_t v) -> float {
        return (v <= 150) ? v / 10.0f : (float)(v - 135);
    };

    struct { unsigned long ms; float expS; const char* label; } cases[] = {
        {     0,   0.0f, "0 ms"},
        {   100,   0.1f, "100 ms"},
        {  3700,   3.7f, "3.7 s"},
        { 15000,  15.0f, "15.0 s (hranice)"},
        { 16000,  16.0f, "16 s"},
        { 41000,  41.0f, "41 s (worst case)"},
        {120000, 120.0f, "120 s (max)"},
        {180000, 120.0f, "180 s → clamp na 120 s"},
    };

    for (auto& c : cases) {
        uint8_t v = enc(c.ms);
        float got = dec(v);
        if (fabsf(got - c.expS) > 0.05f) {
            Serial.printf("  [FAIL] %s: enc=%u dec=%.1f exp=%.1f\n",
                          c.label, v, got, c.expS);
            fails++;
        } else {
            Serial.printf("  [PASS] %s: %.1f s\n", c.label, got);
        }
    }
    Serial.printf("  → Výsledek: %s (%u/%u testů OK)\n\n",
                  fails == 0 ? "VSE PROSEL" : "CHYBY!",
                  (uint8_t)(sizeof(cases)/sizeof(cases[0])) - fails,
                  (uint8_t)(sizeof(cases)/sizeof(cases[0])));
}
#endif
