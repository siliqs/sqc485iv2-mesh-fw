#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

// UART0 (CONN2, first factory flash only — console is USB CDC on this product)
static const uint8_t TX = 21;
static const uint8_t RX = 20;

// Built-in HDC1080 temp/humidity sensor — GPIO20 (SDA) / GPIO21 (SCL).
// (Shares the physical pins with UART0 above; the app uses USB CDC for console,
//  so these become the I2C bus after flashing.)
static const uint8_t SDA = 20;
static const uint8_t SCL = 21;

static const uint8_t SS = 8;
static const uint8_t MOSI = 7;
static const uint8_t MISO = 6;
static const uint8_t SCK = 10;

static const uint8_t A0 = 0;
static const uint8_t A1 = 1;
static const uint8_t A2 = 2;
static const uint8_t A3 = 3;
static const uint8_t A4 = 4;
static const uint8_t A5 = 5;

#endif /* Pins_Arduino_h */
