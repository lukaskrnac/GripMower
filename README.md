# GripMower

Hardvérový repozitár pre vlastnú robotickú kosačku **GripMower** riadenú systémom [MowgliNext](https://github.com/mowglinext/mowglinext) na Raspberry Pi.

Obsahuje nízkoúrovňový firmvér pre ESP32 (emulátor Mowgli STM32 mainboardu, protokol **v9**), komunikáciu s BLDC kontrolérmi MM32 (CANSimple cez UART), snímanie IMU/kompasu a riadenie predného kolesa.

## Štruktúra repozitára

```text
GripMower/
├── .github/
│   └── workflows/
│       └── build-esp32-firmware.yml  # Automatický build ESP32 firmvéru + GitHub Release
├── firmware/
│   └── esp32/
│       └── mainEmulatorMowgli/   # Arduino sketch pre ESP32 (MowgliNext protokol v9)
│           ├── mainEmulatorMowgli.ino
│           ├── Mm32Motors.h      # Riadenie a telemetria MM32 (kolesá + čepele)
│           ├── Mm32Uart.h        # CANSimple-over-UART rámcovanie + CRC16-XMODEM
│           ├── MotorWatchdog.h   # Hardvérový watchdog + okamžité odpojenie stykača
│           ├── imu.h / imu.cpp   # BMI088 (accel/gyro) + QMC5883L (magnetometer)
│           └── AS5600Wire1.h / AS5600.cpp  # Magnetický enkodér riadenia (Wire1)
├── tools/
│   ├── flash-esp32.sh            # Stiahne najnovší build z GitHubu a nahrá ho cez USB do ESP32
│   └── install-flasher.sh        # Vytvorí ikonu na plochu Raspberry Pi (vzdialená plocha)
├── hardware/                     # (pripravené pre schémy, PCB, CAD/3D modely)
└── docs/                         # (pripravené pre poznámky a dokumentáciu)
```

## Hardvér a zapojenie ESP32

| Funkcia | Pin(y) ESP32 | Poznámka |
|---|---|---|
| Komunikácia s Raspberry Pi (MowgliNext) | `Serial` (USB) | 115200 baud, COBS + CRC-16/CCITT, protokol v9 |
| MM32 kolesá (ID 1 ľavé, ID 2 pravé) | `Serial1`: RX `GPIO14`, TX `GPIO23` | 115200 baud, open-drain zbernica s pull-up odporom |
| MM32 čepele (ID 3, 4, 5) | `Serial2`: RX `GPIO32`, TX `GPIO33` | 115200 baud, open-drain zbernica s pull-up odporom |
| Hlavný NO stykač motorov | `GPIO4` | `HIGH` = zapnuté, vypína sa aj v ISR watchdogu |
| Fyzické tlačidlo STOP (e-stop) | `GPIO13` | `INPUT_PULLUP`, aktívne `LOW` |
| Detekcia dokovacej stanice | `GPIO34` | Aktívne `LOW` |
| IMU (BMI088 + QMC5883L) | `Wire` (I2C0) | 400 kHz (`0x19`, `0x69`, `0x0D`) |
| Enkodér natočenia predného kolesa (AS5600) | `Wire1`: SDA `GPIO19`, SCL `GPIO18` | Upravená knižnica `AS5600Wire1.h` |
| Driver serva riadenia (BTS7960) | `GPIO25`, `GPIO26`, `GPIO27` | EN, L_PWM, R_PWM |

> **Poznámka k ESP32 Arduino core 3.x:** Po `Serial1.begin()` / `Serial2.begin()` nikdy nevolať `pinMode(rxPin, INPUT_PULLUP)` (odpojí RX od UARTu). V kóde sa používa `gpio_pullup_en()` spolu s externým pull-up odporom.

## Parametre pohonu a bezpečnosť

- **BLDC kontroléry:** 2× MM32 pre kolesá (ID `1`, `2`) + 3× MM32 pre čepele (ID `3`, `4`, `5`), firmvér [Hoverboard-Firmware-Hack-Gen2.X-MM32](https://github.com/lukaskrnac/Hoverboard-Firmware-Hack-Gen2.X-MM32).
- **Geometria:** `WHEEL_DIST = 0.52 m`, `STEER_DIST = 0.52 m`, `WHEEL_DIAMETER_M = 0.29 m`, prevod `WHEEL_GEAR_RATIO = 9.7`.
- **Aktívne brzdenie v stoji (`WHEEL_HOLD_BRAKE = 1`):** Kolesá zostávajú zaarmované v `MM32_RUN` na `0 ot./s` (aj pri `tilt_latch` na svahu, kým čepele idú do `IDLE`). Fyzická STOPka, výpadok heartbeatu (> 500 ms), chyba motora alebo watchdog okamžite odpoja stykač `GPIO4`.
- **Test bez čepelí:** V `checkMotorPower()` je aktuálne zakomentovaná kontrola čepelí (`blades`) — po pripojení dosiek čepelí ju treba odkomentovať.

## Automatický build (GitHub Actions)

Každý push do vetvy `main` (alebo manuálne spustenie v záložke **Actions → Build ESP32 Firmware**) automaticky skompiluje `mainEmulatorMowgli.ino` cez `arduino-cli` (`esp32:esp32:esp32`, ESP32 core `3.3.13`) a zverejní výsledný `gripmower-esp32-merged.bin` (+ `SHA256SUMS.txt` a `firmware-info.json`) do GitHub Release s tagom **`firmware-latest`**.

Pri pushnutí verzného tagu (napr. `v0.9.3`) sa navyše vytvorí samostatný archívny GitHub Release.

## Nahratie firmvéru na kosačke (Raspberry Pi cez USB)

### 1. Jednorazová inštalácia ikony na plochu (pre vzdialenú plochu)

Na Raspberry Pi otvor terminál a spusti jeden príkaz:

```bash
curl -fsSL https://raw.githubusercontent.com/lukaskrnac/GripMower/main/tools/install-flasher.sh | bash
```

Tým sa nainštaluje skript `~/.local/bin/gripmower-flash-esp32` a vytvorí ikona **GripMower ESP32 Flash** priamo na ploche (aj v menu aplikácií).

### 2. Aktualizácia ESP32 kedykoľvek cez vzdialenú plochu

1. Uisti sa, že kosačka stojí na mieste a nože sú bezpečne vypnuté (ideálne stlač fyzickú STOPku).
2. Dvojklikni na ikonu **GripMower ESP32 Flash** na ploche (alebo v termináli spusti `~/.local/bin/gripmower-flash-esp32`).
3. Skript automaticky:
   - skontroluje najnovší build v GitHub Release `firmware-latest` a porovná ho s naposledy nahratou verziou,
   - nájde USB port ESP32 (uprednostní `/dev/mowgli` a odlíši GPS/LiDAR),
   - stiahne `gripmower-esp32-merged.bin` a overí jeho SHA-256 kontrolný súčet,
   - pri prvom spustení si sám nainštaluje `esptool` do vlastného venv (`~/.local/share/gripmower-flasher/venv`),
   - dočasne zastaví Docker kontajner `mowgli-ros2`, nahrá firmware do ESP32 (pri chybe skúsi pomalších 115200 baud) a kontajner znova spustí.

Užitočné prepínače z terminálu:

```bash
~/.local/bin/gripmower-flash-esp32 --port /dev/ttyUSB0     # ručný výber portu
~/.local/bin/gripmower-flash-esp32 --tag v0.9.3            # konkrétny release tag
~/.local/bin/gripmower-flash-esp32 --self-update           # aktualizácia samotného skriptu
```

## Ručná kompilácia (Arduino IDE)

1. Otvor `firmware/esp32/mainEmulatorMowgli/mainEmulatorMowgli.ino` v Arduino IDE (doska **ESP32 Dev Module**).
2. Nainštaluj potrebné knižnice:
   - `FastCRC`
   - `PacketSerial`
   - `BTS7960`
   - `Bolder Flight Systems BMI088` (`BMI088.h`)
   - `FastIMU` (pre `QMC5883L`)
3. Kompiluj a nahraj do ESP32.
