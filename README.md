# GripMower

Hardvérový repozitár pre vlastnú robotickú kosačku **GripMower** riadenú systémom [MowgliNext](https://github.com/mowglinext/mowglinext) na Raspberry Pi.

Obsahuje nízkoúrovňový firmvér pre ESP32 (emulátor Mowgli STM32 mainboardu, protokol **v9**), komunikáciu s BLDC kontrolérmi MM32 (CANSimple cez UART), snímanie IMU/kompasu a riadenie predného kolesa.

## Štruktúra repozitára

```text
GripMower/
├── firmware/
│   └── esp32/
│       └── mainEmulatorMowgli/   # Arduino sketch pre ESP32 (MowgliNext protokol v9)
│           ├── mainEmulatorMowgli.ino
│           ├── Mm32Motors.h      # Riadenie a telemetria MM32 (kolesá + čepele)
│           ├── Mm32Uart.h        # CANSimple-over-UART rámcovanie + CRC16-XMODEM
│           ├── MotorWatchdog.h   # Hardvérový watchdog + okamžité odpojenie stykača
│           ├── imu.h / imu.cpp   # BMI088 (accel/gyro) + QMC5883L (magnetometer)
│           └── AS5600Wire1.h / AS5600.cpp  # Magnetický enkodér riadenia (Wire1)
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

## Kompilácia (Arduino IDE)

1. Otvor `firmware/esp32/mainEmulatorMowgli/mainEmulatorMowgli.ino` v Arduino IDE (doska **ESP32 Dev Module**).
2. Nainštaluj potrebné knižnice:
   - `FastCRC`
   - `PacketSerial`
   - `BTS7960`
   - `Bolder Flight Systems BMI088` (`BMI088.h`)
   - `FastIMU` (pre `QMC5883L`)
3. Kompiluj a nahraj do ESP32.
