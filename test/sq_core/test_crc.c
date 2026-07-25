/*
 * test_crc.c — Modbus RTU CRC16.
 *
 * modbus_crc16() is load-bearing twice over: it validates every RS485 response
 * AND it seals the config blob. A silent change here bricks provisioning for
 * every device already in the field, so it is pinned to published vectors rather
 * than to whatever the current implementation happens to return.
 */
#include "modbus.h"
#include "sq_test.h"

/* The canonical CRC-16/MODBUS check value for the ASCII string "123456789". */
void test_crc_standard_check_value(void)
{
    ASSERT_EQ(0x4B37, modbus_crc16((const uint8_t *)"123456789", 9));
}

void test_crc_edge_inputs(void)
{
    ASSERT_EQ(0xFFFF, modbus_crc16((const uint8_t *)"", 0)); /* init value, untouched */

    const uint8_t zero = 0x00;
    ASSERT_EQ(0x40BF, modbus_crc16(&zero, 1));

    const uint8_t ff[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    ASSERT_EQ(0xB001, modbus_crc16(ff, 4));
}

/* Real frames from the default poll plan — these are the exact bytes the engine
   puts on the wire, so a byte-order slip shows up here first. */
void test_crc_real_modbus_frames(void)
{
    const uint8_t req_read_2[6] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x02};
    ASSERT_EQ(0x0BC4, modbus_crc16(req_read_2, 6));

    const uint8_t req_absent[6] = {0x09, 0x03, 0x00, 0x00, 0x00, 0x01};
    ASSERT_EQ(0x4285, modbus_crc16(req_absent, 6));

    const uint8_t rsp[7] = {0x01, 0x03, 0x04, 0x03, 0xE8, 0x03, 0xE9};
    ASSERT_EQ(0x3DBB, modbus_crc16(rsp, 7));
}

/* Any single-bit flip must change the CRC — the property the retry logic relies
   on when it rejects a corrupt frame. */
void test_crc_detects_single_bit_flips(void)
{
    uint8_t frame[8] = {0x01, 0x03, 0x04, 0x12, 0x34, 0x56, 0x78, 0x9A};
    uint16_t base = modbus_crc16(frame, sizeof(frame));

    for (size_t i = 0; i < sizeof(frame); i++) {
        for (int bit = 0; bit < 8; bit++) {
            frame[i] ^= (uint8_t)(1u << bit);
            ASSERT_TRUE(modbus_crc16(frame, sizeof(frame)) != base);
            frame[i] ^= (uint8_t)(1u << bit);
        }
    }
}
