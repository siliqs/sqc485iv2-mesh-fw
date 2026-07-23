// SQC485Iv2 (uses Heltec HT-CT62 module)
//
// Notes:
// - User key: GPIO9 (matches schematic net GPIO9_USER_KEY)
// - LED: GPIO2 (schematic note: LED on when GPIO2 == LOW)
// - Battery ADC measurement is not implemented/available on this board revision.

// GPIO9 (user key) and GPIO2 (LED) are NOT exposed on this headless board; they
// are reused for the RS485 transceiver so the field node can poll a Modbus PLC.
// No BUTTON_PIN / LED_POWER is defined — they would conflict with RS485 DE//RE.

// RS485 (MAX3485) — same wiring as the LoRaWAN v2 board (V230)
#define SQC485I_RS485_RX 0    // RO (receiver out -> ESP RX)
#define SQC485I_RS485_TX 1    // DI (ESP TX -> driver in)
#define SQC485I_RS485_DE 9    // driver enable (was the unexposed user key)
#define SQC485I_RS485_RE 2    // /RE, held LOW = receive (was the unexposed LED)
#ifndef SQC485I_DE_INVERTED
#define SQC485I_DE_INVERTED 1 // Lot 2+ default: GPIO9 -> inverter -> DE. v231 env overrides to 0 via -D.
#endif

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