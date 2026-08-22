/*
 * A stdio CLI over the generated table: what the Python client test
 * talks to through a pipe. Built by `make codegen` against the
 * registers.c/.h that `regtable gen` produced from example.yaml.
 * Echo is off and there is no prompt, so every line on stdout is
 * the CLI's own answer.
 */

#include <stdio.h>
#include "registers.h"
#include "regtable_cli.h"

/* the hook bodies the YAML names */
void led_changed(const RegEntry *e)  { (void)e; }
bool pump_check(const RegEntry *e, uint32_t raw) { (void)e; (void)raw; return true; }
void voltage_read(const RegEntry *e) { (void)e; voltage = 1.65f; }

static int out_write(const uint8_t *buf, uint16_t len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    return len;
}

static int no_read(uint8_t *buf, uint16_t len, uint32_t t)
{
    (void)buf; (void)len; (void)t;
    return 0;
}

int main(void)
{
    static RegTable table;
    static RegCli   cli;
    RegTransport tx = { .read = no_read, .write = out_write };

    if (reg_table_init(&table, demo_registry) != REG_OK) return 1;
    regcli_init(&cli, &table, tx);
    cli.echo = false;

    int c;
    while ((c = getchar()) != EOF) {
        regcli_feed(&cli, (uint8_t)c);
        reg_poll(&table);
    }
    return 0;
}
