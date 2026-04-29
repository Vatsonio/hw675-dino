# HW-675 Dino

Chrome T-Rex jump game for the **HW-675** dev board (ESP32-C3 + 0.42" SSD1306 OLED, single button on GPIO9).

## Hardware

| | |
|---|---|
| MCU | ESP32-C3 |
| Display | SSD1306 0.42" OLED, 72×40 visible px, I²C @ 0x3C |
| I²C pins | SDA=GPIO5, SCL=GPIO6 |
| Button | GPIO9 (BOOT, active LOW) |

## First-time setup

```powershell
.\build.ps1 -Setup
```

Downloads `arduino-cli` into `.\tools\`, installs the ESP32 core and the U8g2 library. Takes 5–10 minutes (the ESP32 core is ~250 MB).

## Build & flash

```powershell
.\build.ps1               # compile, auto-detect COM, upload
.\build.ps1 -Monitor      # also open serial monitor afterwards
.\build.ps1 -Port COM5    # force a specific COM port
.\build.ps1 -NoUpload     # compile only
```

If upload fails with `Failed to connect`: hold the **BOOT** button (GPIO9), tap **RESET**, release BOOT, rerun.

## How to play

- **READY** screen → press button to start (it doubles as the first jump).
- **PLAYING** → press button to jump over cacti. Speed increases over time.
- **GAME OVER** → wait ½ second, press button to restart. High score is kept in RAM.

## Project layout

```
HW-675_dino\
├── dino\
│   ├── dino.ino        # game logic + rendering
│   └── sprites.h       # XBM bitmaps in PROGMEM
├── tools\
│   └── arduino-cli.exe # downloaded by -Setup
├── build.ps1           # build / upload driver
└── README.md
```

## Notes

- The 0.42" OLED's visible 72×40 area sits at a column offset inside the 128×64 SSD1306 buffer. The U8g2 constructor `U8G2_SSD1306_72X40_ER_F_HW_I2C` handles this — the sketch uses local 0..71, 0..39 coordinates.
- FQBN includes `CDCOnBoot=cdc` — required for native USB CDC on the ESP32-C3, otherwise auto-reset for upload won't work and `Serial.print` is silent.
