# variant: sqs-th-i (SQS-TH-I)

Siliqs **SQS-TH-I** — first product of the SQS-Sensor line. ESP32-C3 + SX1262
(Heltec HT-CT62 module), same platform as **SQC485Iv2** with the RS485 front-end
removed and a built-in **TI HDC1080** temperature/humidity sensor on I²C.

| | |
|---|---|
| Env | `sqs-th-i-esp32c3-sx1262` (see `platformio.ini` here) |
| Board define | `SQS_TH_I` (does **not** define `SQC485IV2`, so no RS485/Modbus module) |
| I²C | `SDA=GPIO20`, `SCL=GPIO21` — HDC1080 @ 0x40 |
| Console | USB CDC (GPIO18/19); UART0 (20/21) is CONN2 first-flash only |
| LoRa factory default | TW DTS profile: 922.5 MHz / BW500 / SF9 / CR4:5 / +22 dBm |
| BLE | off by default (enable via configurator over USB) |

## Notes / gotchas
- **I²C pins 20/21 = UART0 pins.** The production console is USB CDC, so the app
  drives 20/21 as I²C after flashing. Do not enable a UART0 console on this build.
- **HDC1080 address 0x40 aliases the INA power-sensor scan.** The sensor is read
  directly by the SQS module, not via Meshtastic's I²C telemetry autoscan.
- LoRa factory default lives in `src/mesh/Channels.cpp` `initDefaultLoraConfig()`
  and `src/mesh/NodeDB.cpp` `installDefaultConfig()`, both guarded by
  `defined(SQC485IV2) || defined(SQS_TH_I)`. Keep the two in sync.
- Factory channel PSK is injected at build time via `SQ_FACTORY_CH1_*` env vars
  (never committed) — same pattern as SQC485Iv2.
