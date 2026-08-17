/*
 * Desktop test harness for regtable.
 * Compile: gcc -std=c99 -o test regtable_core.c regtable_cli.c test_desktop.c
 * Run:     ./test
 * Then type: list / get temp / set led true / help
 */

#include <stdio.h>
#include "regtable_cli.h"

/* ── fake device state ─────────────────────────────────── */
static float    temperature = 23.4f;
static uint16_t interval    = 1000;
static uint8_t  led_state   = 0;
static uint32_t counter     = 0;
static uint32_t setpoint    = 0;

/* ── register table ────────────────────────────────────── */
static const RegEntry registry[] = {
    {
        .name        = "temp",
        .ptr         = &temperature,
        .type        = REG_FLOAT,
        .perm        = REG_RO,
        .modbus_addr = 0x0000,
        .description = "Water temperature in Celsius"
    },
    {
        .name        = "interval",
        .ptr         = &interval,
        .type        = REG_U16,
        .perm        = REG_RW,
        .modbus_addr = 0x0002,
        .min         = 100,
        .max         = 60000,
        .description = "Sampling interval in ms"
    },
    {
        .name        = "led",
        .ptr         = &led_state,
        .type        = REG_BOOL,
        .perm        = REG_RW,
        .modbus_addr = 0x0004,
        .description = "Status LED on/off"
    },
    {
        .name        = "counter",
        .ptr         = &counter,
        .type        = REG_U32,
        .perm        = REG_RO,
        .modbus_addr = 0x0006,
        .description = "Uptime tick counter"
    },
    {
        .name        = "setpoint",
        .ptr         = &setpoint,
        .type        = REG_U32,
        .perm        = REG_RW,
        .modbus_addr = 0x0008,
        .description = "Target value, unbounded u32"
    },
    { .name = NULL } /* sentinel */
};

/* ── transport: stdin/stdout ───────────────────────────── */

static int transport_write(const uint8_t *buf, uint16_t len)
{
    return (int)fwrite(buf, 1, len, stdout);
}

/* not used in this test — CLI is fed byte-by-byte from stdin */
static int transport_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return (int)fread(buf, 1, len, stdin);
}

/* ── main ──────────────────────────────────────────────── */

int main(void)
{
    RegTransport tx = {
        .read  = transport_read,
        .write = transport_write
    };

    RegCli cli;
    regcli_init(&cli, registry, tx);
    cli.echo = false;  /* terminal already echoes */

    printf("regtable v0.1 — type 'help'\n");
    printf("> ");
    fflush(stdout);

    int c;
    while ((c = getchar()) != EOF) {
        regcli_feed(&cli, (uint8_t)c);
        if (c == '\n') {
            printf("> ");
            fflush(stdout);
        }
    }

    return 0;
}
