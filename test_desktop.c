/*
 * Desktop test harness for regtable.
 * Compile: gcc -std=c99 -o test regtable_core.c regtable_cli.c test_desktop.c
 * Run:     ./test
 * Then type: list / get temp / set led true / info gain / help
 */

#include <stdio.h>
#include "regtable_cli.h"

/* -- fake device state ----------------------------------- */
static float    temperature = 23.4f;
static uint16_t interval    = 1000;
static uint8_t  led_state   = 0;
static uint32_t counter     = 0;
static uint32_t setpoint    = 0;
static int16_t  offset      = 0;
static float    gain        = 1.0f;

/* -- hooks ----------------------------------------------- */

static void led_changed(const RegEntry *e)
{
    printf("[on_change] %s -> %s\n", e->name, led_state ? "ON" : "OFF");
}

static void offset_changed(const RegEntry *e)
{
    printf("[on_change] %s -> %d\n", e->name, offset);
}

static void temp_changed(const RegEntry *e)
{
    printf("[on_change] %s -> %.2f (marked dirty by sensor loop)\n", e->name, temperature);
}

/* -- register table -------------------------------------- */
static const RegEntry registry[] = {
    {
        .name        = "temp",
        .ptr         = &temperature,
        .type        = REG_FLOAT,
        .perm        = REG_RO,
        .modbus_addr = 0x0000,
        .on_change   = temp_changed,
        .description = "Water temperature in Celsius"
    },
    {
        .name        = "interval",
        .ptr         = &interval,
        .type        = REG_U16,
        .perm        = REG_RW,
        .modbus_addr = 0x0002,
        .min.u       = 100,
        .max.u       = 60000,
        .description = "Sampling interval in ms"
    },
    {
        .name        = "led",
        .ptr         = &led_state,
        .type        = REG_BOOL,
        .perm        = REG_RW,
        .modbus_addr = 0x0004,
        .on_change   = led_changed,
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
    {
        .name        = "offset",
        .ptr         = &offset,
        .type        = REG_I16,
        .perm        = REG_RW,
        .modbus_addr = 0x000A,
        .min.i       = -50,
        .max.i       = 50,
        .on_change   = offset_changed,
        .description = "Calibration offset, signed"
    },
    {
        .name        = "gain",
        .ptr         = &gain,
        .type        = REG_FLOAT,
        .perm        = REG_RW,
        .modbus_addr = 0x000C,
        .min.f       = 0.5f,
        .max.f       = 2.5f,
        .description = "Sensor gain multiplier"
    },
    { .name = NULL } /* sentinel */
};

/* -- transport: stdin/stdout ----------------------------- */

static int transport_write(const uint8_t *buf, uint16_t len)
{
    return (int)fwrite(buf, 1, len, stdout);
}

static int transport_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return (int)fread(buf, 1, len, stdin);
}

/* -- main ------------------------------------------------ */

int main(void)
{
    RegTransport tx = {
        .read  = transport_read,
        .write = transport_write
    };

    RegTable table;
    if (reg_table_init(&table, registry) != REG_OK) {
        fprintf(stderr, "table init failed\n");
        return 1;
    }

    RegCli cli;
    regcli_init(&cli, &table, tx);
    cli.echo = false;  /* terminal already echoes */

    printf("regtable v0.2, type 'help'\n");
    printf("> ");
    fflush(stdout);

    int c;
    while ((c = getchar()) != EOF) {
        regcli_feed(&cli, (uint8_t)c);
        if (c == '\n') {
            /* "sensor loop": every 4th line the temperature drifts.
             * Your C code changed the variable, so it reports it. */
            if (++counter % 4 == 0) {
                temperature += 0.5f;
                reg_mark_dirty(&table, reg_find(&table, "temp"));
            }

            reg_poll(&table);   /* deferred on_change hooks run here */

            printf("> ");
            fflush(stdout);
        }
    }

    return 0;
}
