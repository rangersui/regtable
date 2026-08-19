/*
 * libFuzzer harness for the CLI byte path.
 *
 * Feeds arbitrary bytes into regcli_feed, the same way a UART would,
 * with ASan/UBSan watching. Anything the parser chain does wrong on
 * some byte sequence (over-read, bad cast, unbounded loop) shows up
 * as a crash with a stack trace and a reproducer file.
 *
 *   make fuzz            (or by hand:)
 *   clang -g -O1 -Isrc -fsanitize=fuzzer,address,undefined \
 *         -o fuzz regtable_fuzz.c src/regtable_core.c src/regtable_cli.c
 *   mkdir -p corpus && ./fuzz corpus -max_len=256
 *
 * The regression test asserts what the CLI must do with inputs we
 * thought of; this searches for inputs we did not.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "regtable_cli.h"

/* -- device state: same shape as the test table ------------- */

static float    temperature = 23.4f;
static uint16_t interval    = 1000;
static uint8_t  led_state   = 0;
static uint32_t counter     = 0;
static uint32_t setpoint    = 0;
static int16_t  offset      = 0;
static float    gain        = 1.0f;
static uint8_t  pump        = 0;
static uint8_t  small       = 5;
static float    voltage     = 0.0f;
static volatile uint32_t hwreg = 0;

static void noop_change(const RegEntry *e) { (void)e; }
static bool pump_check(const RegEntry *e, uint32_t raw)
{
    (void)e;
    return !(raw && temperature > 90.0f);
}
static void voltage_read(const RegEntry *e)
{
    (void)e;
    voltage = 2048 * 3.3f / 4096.0f;
}

static const RegEntry registry[] = {
    { .name = "temp",     .ptr = &temperature, .type = REG_FLOAT, .perm = REG_RO,
      .on_change = noop_change, .description = "Water temperature" },
    { .name = "interval", .ptr = &interval,    .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000, .description = "Sampling interval in ms" },
    { .name = "led",      .ptr = &led_state,   .type = REG_BOOL,  .perm = REG_RW,
      .on_change = noop_change },
    { .name = "counter",  .ptr = &counter,     .type = REG_U32,   .perm = REG_RO },
    { .name = "setpoint", .ptr = &setpoint,    .type = REG_U32,   .perm = REG_RW },
    { .name = "offset",   .ptr = &offset,      .type = REG_I16,   .perm = REG_RW,
      .min.i = -50, .max.i = 50, .on_change = noop_change },
    { .name = "gain",     .ptr = &gain,        .type = REG_FLOAT, .perm = REG_RW,
      .min.f = 0.5f, .max.f = 2.5f },
    { .name = "pump",     .ptr = &pump,        .type = REG_BOOL,  .perm = REG_RW,
      .on_write = pump_check },
    { .name = "small",    .ptr = &small,       .type = REG_U8,    .perm = REG_RW },
    { .name = "voltage",  .ptr = &voltage,     .type = REG_FLOAT, .perm = REG_RO,
      .on_read = voltage_read },
    { .name = "hwreg",    .ptr = &hwreg,       .type = REG_U32,   .perm = REG_RW,
      .description = "say \"hi\" \\ tab\there" },
    { .name = NULL }
};

/* -- transport: output goes nowhere, but every byte is touched so
 *    ASan sees any bad pointer/length handed to write ------------ */

static volatile uint8_t sink;

static int null_write(const uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) sink = buf[i];
    return len;
}
static int null_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)buf; (void)len; (void)timeout_ms;
    return 0;
}

/* -- harness ------------------------------------------------ */

static RegTable table;
static RegCli   cli;
static bool     ready;

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!ready) {
        reg_table_init(&table, registry);
        RegTransport tx = { .read = null_read, .write = null_write };
        regcli_init(&cli, &table, tx);
        cli.echo = false;
        ready = true;
    }

    for (size_t i = 0; i < size; i++) {
        regcli_feed(&cli, data[i]);
    }
    /* end the line so a trailing partial command is executed too */
    regcli_feed(&cli, '\n');
    reg_poll(&table);
    return 0;
}
