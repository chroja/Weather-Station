# LaskaKit Meteo Mini — Weather Station

Firmware pro vlastní meteostanici na ESP32-C3 s odesíláním dat na [TMEP.cz](https://tmep.cz).
Zvládne výpadek WiFi až **~7 dní** bez ztráty dat díky dvouúrovňovému offline bufferu.

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
