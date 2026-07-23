#pragma once

// PLCModbusModule — SQC485Iv2 field node only.
// Polls a generic Modbus-RTU slave over RS485 (FC03, slave 0x01) every 15 s, packs
// the response into the 51-byte Rs485Payload struct that the USB receiver
// (the companion USB receiver project) decodes, and sends it over the
// mesh on portnum PRIVATE_APP (256). Gateway nodes (role=CLIENT_MUTE) do not poll.
// Built only when -D SQC485IV2.

#ifdef SQC485IV2

#include "SinglePortModule.h"
#include "concurrency/OSThread.h"

class PLCModbusModule : public SinglePortModule, private concurrency::OSThread
{
  public:
    PLCModbusModule();

  protected:
    virtual int32_t runOnce() override;

  private:
    bool firstTime = true;
    void rs485Begin();
    // Run one FC03 transaction; on success fills out[] with the 22-byte payload
    // ([addr][func][20 data]) and returns its length, else returns 0.
    size_t pollModbus(uint8_t *out, size_t outCap);
    static uint16_t crc16(const uint8_t *buf, size_t len);
};

extern PLCModbusModule *plcModbusModule;

#endif // SQC485IV2
