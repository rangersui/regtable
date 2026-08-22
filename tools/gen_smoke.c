/*
 * Smoke test for generated tables: initialise the table and every
 * adapter against it, exercise a few values, exit 0. Compiled by
 * the codegen check (Makefile `make codegen`, CI job) against the
 * registers.c/.h that `regtable gen` just produced.
 */

#include <stdio.h>
#include <string.h>
#include "registers.h"
#include "regtable_cli.h"
#include "regtable_modbus.h"
#include "regtable_mqtt.h"

/* the hook bodies the YAML names */
static int led_changes;
void led_changed(const RegEntry *e)  { (void)e; led_changes++; }
bool pump_check(const RegEntry *e, uint32_t raw) { (void)e; (void)raw; return true; }
void voltage_read(const RegEntry *e) { (void)e; voltage = 1.65f; }

static int null_write(const uint8_t *buf, uint16_t len)
{
    (void)buf; return len;
}
static int null_read(uint8_t *buf, uint16_t len, uint32_t t)
{
    (void)buf; (void)len; (void)t; return 0;
}
static int null_publish(const char *t, const char *p, bool r, void *u)
{
    (void)t; (void)p; (void)r; (void)u; return 0;
}

#define REQUIRE(cond) do {                                        \
    if (!(cond)) {                                                \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        return 1;                                                 \
    }                                                             \
} while (0)

int main(void)
{
    RegTable table;
    REQUIRE(reg_table_init(&table, demo_registry) == REG_OK);

    /* every adapter's init validator accepts the generated table */
    RegCli cli;
    RegTransport tx = { .read = null_read, .write = null_write };
    regcli_init(&cli, &table, tx);

    RegModbus mb;
    REQUIRE(regmb_init(&mb, &table, 1) == REG_OK);

    RegMqtt mq;
    REQUIRE(regmqtt_init(&mq, &table, "demo", null_publish, NULL) == REG_OK);

    /* generated initial values and ranges are live */
    REQUIRE(interval == 1000);
    REQUIRE(temp > 23.3f && temp < 23.5f);
    const RegEntry *e = reg_find(&table, "interval");
    REQUIRE(reg_set_str(&table, e, "50") == REG_ERR_RANGE);
    REQUIRE(reg_set_str(&table, e, "500") == REG_OK);
    REQUIRE(interval == 500);

    /* generated hook wiring is live */
    e = reg_find(&table, "led");
    REQUIRE(reg_set_str(&table, e, "true") == REG_OK);
    REQUIRE(reg_poll(&table) == 1);
    REQUIRE(led_changes == 1);
    e = reg_find(&table, "voltage");
    uint32_t raw;
    REQUIRE(reg_get_raw(e, &raw) == REG_OK);
    REQUIRE(voltage == 1.65f);

    printf("generated table: all checks passed\n");
    return 0;
}
