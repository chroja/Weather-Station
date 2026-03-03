
// ── TMEP.cz server URLs ───────────────────────────────────────────────────────
// URL jsou uloženy v secrets.h (není v gitu).
// Zkopíruj secrets.h.example → secrets.h a vyplň své domény z tmep.cz.
#include "secrets.h"

// ── Konfigurace chování ───────────────────────────────────────────────────────
#define SLEEP_SEC        60   // cílová perioda měření v sekundách (včetně doby běhu)
#define SLEEP_MIN_SEC    30   // minimální spánek při překročení SLEEP_SEC (sekundy)
#define WIFI_TIMEOUT_SEC 10   // max. čekání na WiFi připojení (sekundy)
#define HTTP_TIMEOUT_SEC  8   // max. čekání na odpověď HTTP serveru (sekundy)

// ── RTC RAM buffer (přežije deep sleep, NE výpadek napájení) ─────────────────
//
// Obsahuje pouze nejnovější záznamy — vrstva0 (1min) a vrstva1 (10min).
// Při naplnění TIER1_MAX se nejstarší desetiminutové záznamy průměrují
// do hodinového průměru a přesunou se do NVS flash bufferu.
//
//   vrstva │ granularita │ slotů │ min. pokrytí  │ výpočet
//   ───────┼─────────────┼───────┼───────────────┼──────────────────
//      0   │    1 min    │   70  │    1 hodina   │ (70-10)×1min
//      1   │   10 min    │   42  │    6 hodin    │ (42-6)×10min
//   ───────┼─────────────┼───────┼───────────────┼──────────────────
//   celkem │             │  112  │    7 hodin    │
//
// Paměť: 112 × 40 B = 4 480 B  (limit RTC RAM: ~8 192 B)
//
#define TIER0_MAX        70   // vrstva 0: max minutových záznamů před kompakcí 10→1
#define TIER1_MAX        42   // vrstva 1: max desetiminutových záznamů před kompakcí 6→NVS
#define BUFFER_MAX      112   // celkem RTC slotů: TIER0_MAX + TIER1_MAX

// ── NVS flash buffer (přežije výpadek napájení) ───────────────────────────────
//
// Hodinové průměry (tier2) vzniklé kompakcí vrstva1.
// Uloženy v NVS flash (Preferences), načítány lazy — jen při potřebě.
// Při plném bufferu se zahazuje nejstarší záznam (FIFO).
//
//   granularita │ slotů │ pokrytí
//   ────────────┼───────┼──────────
//      1 hod    │  150  │  150 hod (~6 dní)
//
// RAM při načtení: 150 × 40 B = 6 000 B (statická globální proměnná)
// NVS flash zápisy: ~1×/hodinu → opotřebení ~8 760/rok → výdrž 11+ let
//
#define FLASH_MAX       150   // max hodinových průměrů v NVS

// ── HTTP chování ──────────────────────────────────────────────────────────────
// Pokud server vrátí HTTP 400 a odpověď obsahuje tento řetězec, nejstarší záznam
// v bufferu se přeskočí (datum je starší než poslední přijatá hodnota na serveru).
// Nastav na "" pro vypnutí automatického přeskakování.
#define HTTP_STALE_RESPONSE "older than last known value"

// ── Debug ─────────────────────────────────────────────────────────────────────
// 0 = ticho (produkce bez USB) | 1 = chyby + souhrn | 2 = normální | 3 = verbose (CSV)
// Pokud USB CDC není připojeno, debug level se automaticky nastaví na 0.
#define DEBUG_LEVEL       3

// ── Časová zóna (POSIX TZ string) ────────────────────────────────────────────
// CET-1CEST = střední Evropa: zimní čas UTC+1, letní čas UTC+2
// M3.5.0    = přechod na letní čas: poslední neděle v březnu
// M10.5.0/3 = přechod na zimní čas: poslední neděle v říjnu ve 3:00
#define TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
