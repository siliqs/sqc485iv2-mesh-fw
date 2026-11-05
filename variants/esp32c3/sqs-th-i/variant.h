// SQS-TH-I — Siliqs SQS-Sensor line: LoRa temperature/humidity node.
// Same hardware platform as SQC485Iv2 (Heltec HT-CT62 module, ESP32-C3 + SX1262),
// with the RS485 front-end removed and a built-in TI HDC1080 sensor added.
//
// RS485 transceiver / inverter / terminal block are DNP on this product. That frees
// GPIO0/1 (old RS485 RX/TX) and GPIO2/9 (old #RE/DE) — reserved here for a future
// LED / button, intentionally left undefined so this stays a headless node.
//
// I2C (built-in HDC1080 @ 0x40): GPIO20 (SDA) / GPIO21 (SCL) — the validated
// sqs1080 test wiring (14_SQC485Iv2_basic/hardware_basic_test/sensor_test).
//   NOTE: GPIO20/21 are the UART0 pins CONN2 uses for the *first* factory flash.
//   The production console runs over USB CDC (GPIO18/19), so after flashing the
//   app is free to drive 20/21 as I2C. Do NOT enable a UART0 console on this build.
//   HDC1080's fixed address 0x40 aliases the INA power-sensor scan — the sensor is
//   read directly by the SQS module, not via Meshtastic's I2C telemetry autoscan.

#define I2C_SDA 20
#define I2C_SCL 21

#define HAS_SCREEN 0
#define HAS_GPS 0
#undef GPS_RX_PIN
#undef GPS_TX_PIN

#define USE_SX1262
#define LORA_SCK 10
#define LORA_MISO 6
#define LORA_MOSI 7
#define LORA_CS 8
#define LORA_DIO0 RADIOLIB_NC
#define LORA_RESET 5
#define LORA_DIO1 3
#define LORA_DIO2 RADIOLIB_NC
#define LORA_BUSY 4
#define SX126X_CS LORA_CS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_BUSY
#define SX126X_RESET LORA_RESET
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
