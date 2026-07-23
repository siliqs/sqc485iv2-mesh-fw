#include "PLCModbusModule.h"

#ifdef SQC485IV2

#include "MeshService.h"
#include "NodeDB.h"
#include "configuration.h"
#include "main.h"
#include <Arduino.h>

PLCModbusModule *plcModbusModule;

// RS485 pins (overridable in the variant). Same wiring as the LoRaWAN v2 board.
#ifndef SQC485I_RS485_RX
#define SQC485I_RS485_RX 0 // MAX3485 RO
#endif
#ifndef SQC485I_RS485_TX
#define SQC485I_RS485_TX 1 // MAX3485 DI
#endif
#ifndef SQC485I_RS485_DE
#define SQC485I_RS485_DE 9 // driver enable
#endif
#ifndef SQC485I_RS485_RE
#define SQC485I_RS485_RE 2 // /RE (held LOW = receive)
#endif
#ifndef SQC485I_DE_INVERTED
#define SQC485I_DE_INVERTED 1 // Lot 2+: GPIO9 -> inverter -> DE (LOW on GPIO9 = transmit)
#endif

#define MODBUS_BAUD 9600
#define MODBUS_SLAVE 0x01
#define MODBUS_FUNC 0x03
#define MODBUS_REG_START 0x0000
#define MODBUS_REG_COUNT 0x000A      // 10 registers
#define MODBUS_EXPECTED_RESP 25      // addr+func+bytecount+20 data+CRC(2)
#define MODBUS_POLL_INTERVAL_MS 15000
#define MODBUS_RX_TIMEOUT_MS 500
#define MODBUS_MAX_ATTEMPTS 3   // read attempts per 15s cycle before reporting an error
#define MODBUS_RETRY_GAP_MS 50  // settle gap between attempts

PLCModbusModule::PLCModbusModule()
    : SinglePortModule("plcmodbus", meshtastic_PortNum_PRIVATE_APP), OSThread("PLCModbus")
{
}

uint16_t PLCModbusModule::crc16(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
}

void PLCModbusModule::rs485Begin()
{
    Serial1.begin(MODBUS_BAUD, SERIAL_8N1, SQC485I_RS485_RX, SQC485I_RS485_TX);
    pinMode(SQC485I_RS485_DE, OUTPUT);
    digitalWrite(SQC485I_RS485_DE, SQC485I_DE_INVERTED ? HIGH : LOW); // deassert (receive)
#if SQC485I_RS485_RE >= 0
    pinMode(SQC485I_RS485_RE, OUTPUT);
    digitalWrite(SQC485I_RS485_RE, LOW); // enable receiver
#endif
}

size_t PLCModbusModule::pollModbus(uint8_t *out, size_t outCap)
{
    uint8_t req[8] = {MODBUS_SLAVE,
                      MODBUS_FUNC,
                      (uint8_t)(MODBUS_REG_START >> 8),
                      (uint8_t)(MODBUS_REG_START & 0xFF),
                      (uint8_t)(MODBUS_REG_COUNT >> 8),
                      (uint8_t)(MODBUS_REG_COUNT & 0xFF),
                      0,
                      0};
    uint16_t c = crc16(req, 6);
    req[6] = c & 0xFF;
    req[7] = (c >> 8) & 0xFF;

    const uint8_t DE_ASSERT = SQC485I_DE_INVERTED ? LOW : HIGH;
    const uint8_t DE_DEASSERT = SQC485I_DE_INVERTED ? HIGH : LOW;

#if SQC485I_RS485_RE >= 0
    digitalWrite(SQC485I_RS485_RE, LOW);
#endif
    while (Serial1.available())
        Serial1.read(); // flush stale RX

    digitalWrite(SQC485I_RS485_DE, DE_ASSERT);
    delayMicroseconds(100);
    Serial1.write(req, sizeof(req));
    Serial1.flush();
    delay(3); // let last byte + its echo finish before deasserting
    digitalWrite(SQC485I_RS485_DE, DE_DEASSERT);
    delay(5); // inter-frame gap

    // /RE tied LOW means the RX line echoes the TX frame — discard echo_len bytes.
    const size_t echoLen = sizeof(req); // 8
    const size_t needed = echoLen + MODBUS_EXPECTED_RESP;
    uint8_t buf[64];
    size_t got = 0;
    uint32_t deadline = millis() + MODBUS_RX_TIMEOUT_MS;
    while (got < needed && got < sizeof(buf) && (int32_t)(deadline - millis()) > 0) {
        if (Serial1.available())
            buf[got++] = (uint8_t)Serial1.read();
    }
    if (got < echoLen + 5)
        return 0;

    const uint8_t *resp = buf + echoLen;
    size_t respLen = got - echoLen;
    uint16_t calc = crc16(resp, respLen - 2);
    uint16_t rcv = (uint16_t)resp[respLen - 2] | ((uint16_t)resp[respLen - 1] << 8);
    if (calc != rcv) {
        LOG_WARN("PLCModbus: CRC mismatch calc=%04X rcv=%04X", calc, rcv);
        return 0;
    }

    // Forward [addr][func][data...] (drop bytecount + CRC) — matches LoRaWAN payload.
    size_t dataBytes = respLen - 5; // addr+func+bytecount+CRC(2)
    size_t payloadLen = 2 + dataBytes;
    if (payloadLen > outCap)
        payloadLen = outCap;
    out[0] = resp[0];
    out[1] = resp[1];
    memcpy(out + 2, resp + 3, payloadLen - 2);
    return payloadLen;
}

int32_t PLCModbusModule::runOnce()
{
    // Gateway nodes (CLIENT_MUTE) bridge the mesh to MQTT and do not poll RS485.
    if (config.device.role == meshtastic_Config_DeviceConfig_Role_CLIENT_MUTE)
        return disable();

    if (firstTime) {
        firstTime = false;
        rs485Begin();
        // Priming transaction: the very first RS485 read can mis-align while the
        // line/echo settles. Discard it so the first forwarded packet is clean.
        uint8_t prime[32];
        (void)pollModbus(prime, sizeof(prime));
        LOG_INFO("PLCModbusModule: RS485 init 9600 8N1, FC03 slave 0x01; priming done, first uplink in 3s");
        return 3000;
    }

    uint8_t rs485[22]; // [addr][func][20 data] on success
    // Retry the Modbus read up to MODBUS_MAX_ATTEMPTS times within this cycle;
    // a single read can fail transiently (bus noise, slave busy). Only report an
    // error once every attempt has failed.
    size_t n = 0;
    int attempt = 0;
    for (; attempt < MODBUS_MAX_ATTEMPTS; attempt++) {
        n = pollModbus(rs485, sizeof(rs485));
        if (n > 0)
            break;
        if (attempt + 1 < MODBUS_MAX_ATTEMPTS)
            delay(MODBUS_RETRY_GAP_MS);
    }

    // Build the 51-byte Rs485Payload the USB receiver (project 08) decodes:
    //   uint32 id (dedup nonce) + uint8 data[47]
    //   data[0]=type (0x01 Modbus OK / 0xFF error), data[1]=rst_reason,
    //   data[2..23]=22 RS485 bytes, data[24..46]=reserved.
    uint8_t pkt[51];
    memset(pkt, 0, sizeof(pkt));
    uint32_t nonce = esp_random();
    memcpy(pkt, &nonce, 4);
    pkt[4] = (n > 0) ? 0x01 : 0xFF;       // type
    pkt[5] = (uint8_t)esp_reset_reason(); // rst_reason
    if (n > 0) {
        memcpy(pkt + 6, rs485, n > 22 ? 22 : n); // data[2..23]
    } else {
        // All MODBUS_MAX_ATTEMPTS reads failed — emit the fixed error code.
        static const uint8_t errCode[4] = {0x01, 0x02, 0x03, 0x04};
        memcpy(pkt + 6, errCode, sizeof(errCode)); // data[2..5] = 01 02 03 04
    }

    meshtastic_MeshPacket *p = allocDataPacket();
    if (!p)
        return MODBUS_POLL_INTERVAL_MS;
    p->want_ack = false;
    p->decoded.payload.size = sizeof(pkt);
    memcpy(p->decoded.payload.bytes, pkt, sizeof(pkt));
    if (n > 0)
        LOG_INFO("PLCModbus: tx Rs485Payload type=0x01 (%u RS485 bytes) on portnum 256", (unsigned)n);
    else
        LOG_WARN("PLCModbus: read failed after %d attempts; tx type=0xFF errcode=01 02 03 04 on portnum 256", MODBUS_MAX_ATTEMPTS);
    service->sendToMesh(p);
    return MODBUS_POLL_INTERVAL_MS;
}

#endif // SQC485IV2
