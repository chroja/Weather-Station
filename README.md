# LaskaKit Meteo Mini — Weather Station

Firmware pro vlastní meteostanici na ESP32-C3 s odesíláním dat na [TMEP.cz](https://tmep.cz).
Zvládne výpadek WiFi až **~7 dní** bez ztráty dat díky dvouúrovňovému offline bufferu.

---

## Poděkování

Velké díky patří dvěma projektům, bez kterých by tato meteostanice nevznikla:

### 🛠️ [LaskaKit](https://www.laskakit.cz)

Česká dílna, která navrhla a vyrábí desku **LaskaKit Meteo Mini** — kompaktní
ESP32-C3 modul se solárním nabíjením, LiPol konektorem a I2C sběrnicí připravenou
pro meteo senzory. Skvělý hardware s dobrou dokumentací a ochotnou podporou.
Pokud hledáš hotový základ pro vlastní IoT projekt, LaskaKit je správné místo.

Použité moduly od LaskaKit:
- **[LaskaKit Meteo Mini Meteostanice](https://www.laskakit.cz/laskakit-meteo-mini-meteostanice/?variantId=18737)** — kompletní kit s deskou, senzory a krabičkou
- **[LaskaKit Meteo Mini](https://www.laskakit.cz/laskakit-meteo-mini/?variantId=10473)** — samotná deska (ESP32-C3, solární nabíjení, LiPol)
- **[SHT40](https://www.laskakit.cz/laskakit-sht40-senzor-teploty-a-vlhkosti-vzduchu/)** — senzor teploty a relativní vlhkosti
- **[BME280](https://www.laskakit.cz/arduino-senzor-tlaku--teploty-a-vlhkosti-bme280/)** — senzor atmosferického tlaku
- **[LTR390](https://www.laskakit.cz/laskakit-ltr390-uv-senzor/)** — UV index a ambient light senzor

### 🌤️ [TMEP.cz](https://tmep.cz)

Český server pro sběr a vizualizaci dat z meteostanicí a IoT senzorů — provozovaný
jedním člověkem, **Michalem Ševčíkem**, a přesto na úrovni, které by se nemusela stydět
komerční služba. Přehledné grafy, dlouhá historie měření, mobilní aplikace (iOS + Android),
mapa senzorů, podpora desítek zařízení různých výrobců. První tři měsíce zdarma,
pak symbolický příspěvek 100 Kč/rok za první čidlo (50 Kč za každé další) —
na úrovni „káva pro autora".

Co ale oceňuji nejvíc, je **podpora**: rychlá, ochotná a vždy věcná. Ať jde o dotaz
na API, nestandardní případ nebo technický problém — odpověď přijde a je k věci.
Takový přístup není samozřejmost, zvláště u bezplatné komunitní služby.

Pokud stavíš vlastní meteostanici nebo senzorový uzel a chceš někam posílat data,
TMEP.cz je to nejlepší místo, které jsem našel. Díky!

---

## Hardware

| Komponenta | Funkce | I2C adresa |
|---|---|---|
| ESP32-C3 (LaskaKit Meteo Mini) | MCU, WiFi | — |
| SHT40 | teplota, relativní vlhkost | 0x44 |
| BME280 | atmosferický tlak | 0x77 |
| LTR390 | UV index, světlo (ALS) | 0x53 |
| LiPol 900 mAh | napájení | — |
| Solární panel 5V/4W | dobíjení | — |

## Struktura repozitáře

```
Weather-Station/
├── FW_experimental/      ← aktivní firmware (vyvíjet zde)
│   ├── FW_experimental.ino
│   ├── config.h          ← konfigurace (perioda, buffer, debug level)
│   ├── secrets.h         ← URL serverů (gitignore, spravuje uživatel)
│   ├── secrets.h.example ← šablona pro secrets.h
│   └── README.md         ← detailní dokumentace firmwaru
├── FW_old/               ← původní referenční firmware (neupravovat)
├── TODO.md               ← plán vývoje
└── README.md             ← tento soubor
```

## Klíčové vlastnosti

- **Dvouúrovňový offline buffer** — RTC RAM (~7 h) + NVS flash (~6 dní);
  data přežijí výpadek WiFi i výpadek napájení v různé granularitě
- **Batch HTTPS POST** na TMEP.cz — tři servery, per-server tracking bez duplicit
- **Adaptivní timestampy** — NTP sync + odhad z boot counteru, bez externího RTC
- **Měření PCB teploty** — interní teplotní senzor ESP32 čipu
- **Deep sleep** — typická průměrná spotřeba ~0.3–0.5 mA (60s cyklus)
- **Debug levely 0–3**, automatická detekce USB připojení

## Rychlý start

Viz [FW_experimental/README.md](FW_experimental/README.md) — kompletní návod:
instalace Arduino IDE, nastavení TMEP.cz, konfigurace, kalibrace baterie, debug výstup.

## Plán vývoje

Viz [TODO.md](TODO.md).
