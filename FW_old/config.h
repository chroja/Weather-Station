// ── Režim provozu ─────────────────────────────────────────────────────────────
// Odkomentuj pro odesílání na testovací servery místo produkčních

// #define TEST_MODE

// ── TMEP.cz server URLs ───────────────────────────────────────────────────────
#ifdef TEST_MODE

// Testovací servery — vyplň své testovací domény z tmep.cz
const char serverName1[] = "http://xxxxxx-xxxxxx.tmep.cz/index.php?";  // test: teplota, vlhkost, tlak
const char serverName2[] = "http://xxxxxx-xxxxxx.tmep.cz/index.php?";  // test: světlo, UV, abs. vlhkost
const char serverName3[] = "http://xxxxxx-xxxxxx.tmep.cz/index.php?";  // test: baterie

#else

// Produkční servery — vyplň své produkční domény z tmep.cz
const char serverName1[] = "http://byf23v-hgm8j5.tmep.cz/index.php?";  // teplota, vlhkost, tlak
const char serverName2[] = "http://4eca2k-ba9rv6.tmep.cz/index.php?";  // světlo, UV, abs. vlhkost
const char serverName3[] = "http://y4a6r6-dja89j.tmep.cz/index.php?";  // baterie

#endif

// ── Konfigurace chování ───────────────────────────────────────────────────────
#define SLEEP_SEC        60   // cílová perioda měření v sekundách (včetně doby běhu)
#define BUFFER_MAX      100   // maximální počet měření v RTC RAM (max ~187 při 32 B/měření)
#define BUFFER_DECIMATE  10   // při přeplnění zachovat každý N-tý záznam
#define WIFI_TIMEOUT_SEC 10   // max. čekání na WiFi připojení (sekundy)
