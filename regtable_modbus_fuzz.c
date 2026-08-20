/*
 * libFuzzer harness for the Modbus RTU frame path.
 *
 * Each input is processed twice: once as raw bytes (exercises the
 * CRC/length gate) and once with a valid CRC appended (exercises the
 * PDU parser and the read/write handlers behind it). ASan/UBSan turn
 * any over-read or bad arithmetic into a crash with a reproducer.
 *
 *   make fuzz-mb          (see Makefile; needs clang)
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "regtable_modbus.h"

static uint16_t interval = 1000;
static uint32_t setpoint = 0;
static float    gain     = 1.0f;
static int16_t  offset   = 0;
static uint8_t  led      = 0;
static float    temp     = 23.4f;
static uint8_t  small    = 5;
static uint16_t rom      = 42;

static void noop_change(const RegEntry *e) { (void)e; }
static void temp_read(const RegEntry *e) { (void)e; temp = 25.0f; }
static bool led_check(const RegEntry *e, uint32_t raw)
{
    (void)e;
    return raw != 0x5A;    /* an occasionally-vetoing hook */
}

/* mixed widths, a gap at 14, hooks on several entries */
static const RegEntry registry[] = {
    { .name = "interval", .ptr = &interval, .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000, .modbus_addr = 1, .on_change = noop_change },
    { .name = "setpoint", .ptr = &setpoint, .type = REG_U32,   .perm = REG_RW,
      .modbus_addr = 2 },
    { .name = "gain",     .ptr = &gain,     .type = REG_FLOAT, .perm = REG_RW,
      .min.f = 0.5f, .max.f = 2.5f, .modbus_addr = 4 },
    { .name = "offset",   .ptr = &offset,   .type = REG_I16,   .perm = REG_RW,
      .min.i = -50, .max.i = 50, .modbus_addr = 6 },
    { .name = "led",      .ptr = &led,      .type = REG_BOOL,  .perm = REG_RW,
      .modbus_addr = 7, .on_write = led_check, .on_change = noop_change },
    { .name = "temp",     .ptr = &temp,     .type = REG_FLOAT, .perm = REG_RO,
      .modbus_addr = 8, .on_read = temp_read },
    { .name = "small",    .ptr = &small,    .type = REG_U8,    .perm = REG_RW,
      .modbus_addr = 10 },
    { .name = "rom",      .ptr = &rom,      .type = REG_U16,   .perm = REG_RO,
      .modbus_addr = 15 },
    { .name = NULL }
};

static RegTable  table;
static RegModbus mb;
static bool      ready;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!ready) {
        reg_table_init(&table, registry);
        regmb_init(&mb, &table, 1);
        ready = true;
    }
    if (size > REGMB_TCP_ADU_MAX) return 0;

    /* sized for the largest legal response on either envelope */
    uint8_t resp[REGMB_TCP_ADU_MAX];

    /* raw: mostly rejected at the CRC gate, occasionally not */
    mb.word_swap = false;
    if (size <= REGMB_FRAME_MAX) {
        regmb_process(&mb, data, (uint16_t)size, resp, sizeof(resp));
    }

    /* CRC fixed up: the parser sees every input */
    if (size >= 2 && size + 2 <= REGMB_FRAME_MAX) {
        uint8_t frame[REGMB_FRAME_MAX];   /* CRC-fixed RTU stays within 256 */
        memcpy(frame, data, size);
        uint16_t crc = regmb_crc16(frame, (uint16_t)size);
        frame[size]     = (uint8_t)crc;
        frame[size + 1] = (uint8_t)(crc >> 8);
        mb.word_swap = (size & 1);
        regmb_process(&mb, frame, (uint16_t)(size + 2), resp, sizeof(resp));
    }

    /* TCP path: MBAP with the protocol id and length made valid,
     * so the fuzzer reaches the PDU behind the envelope checks */
    if (size >= 2 && size + 7 <= REGMB_TCP_ADU_MAX) {
        uint8_t adu[REGMB_TCP_ADU_MAX];
        adu[0] = (uint8_t)size;            /* transaction id: anything */
        adu[1] = data[0];
        adu[2] = 0;                        /* protocol id 0 */
        adu[3] = 0;
        adu[4] = (uint8_t)((size + 1) >> 8);
        adu[5] = (uint8_t)(size + 1);      /* unit id + PDU */
        adu[6] = data[size - 1];           /* unit id: anything */
        memcpy(adu + 7, data, size);
        regmb_process_tcp(&mb, adu, (uint16_t)(size + 7), resp, sizeof(resp));
    }
    /* and raw: the envelope checks themselves */
    regmb_process_tcp(&mb, data, (uint16_t)size, resp, sizeof(resp));

    reg_poll(&table);
    return 0;
}
