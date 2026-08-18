/*
 * regtable desktop example.
 *
 * Runs on your PC with no hardware: the "UART" is your terminal.
 * Use it to learn the API and to prototype your register table
 * before the board arrives. Everything you write here (the table,
 * the hooks, the main loop) moves to the MCU unchanged; only the
 * transport (STEP 4) is swapped for real UART calls.
 *
 *   Build:  make            (or build.bat in cmd)
 *   Run:    make run        (or example.exe)
 *   Then type:  help / list / get temp / set led true / info gain
 *
 * Follow STEP 1..5 below. Lines marked "your ..." are the ones
 * you edit.
 *
 * About stdio: printf / getchar / fwrite in this file are the
 * desktop stand-in for a UART. The library itself does no I/O;
 * it only formats into buffers (snprintf) and calls the write
 * function you hand it in STEP 4. On the MCU there is no stdin
 * or stdout: bytes come from your UART RX and go to your UART
 * TX, and the printf calls inside the hooks are removed or
 * pointed at whatever debug channel you use.
 */

#include <stdio.h>
#include "regtable_cli.h"

/* ============================================================
 * STEP 1: your device state. Plain C variables. Nothing special.
 * ============================================================ */

static float    temperature = 23.4f;   /* sensor reading            */
static uint16_t interval    = 1000;    /* sampling period, ms       */
static uint8_t  led_state   = 0;       /* status LED                */
static uint32_t counter     = 0;       /* uptime ticks              */
static int16_t  offset      = 0;       /* calibration, may be < 0   */
static float    gain        = 1.0f;    /* multiplier                */
static uint8_t  pump        = 0;       /* actuator                  */
static float    voltage     = 0.0f;    /* filled by on_read below   */

/* your variables here */


/* ============================================================
 * STEP 2: your hooks. Optional. Each answers one question.
 * ============================================================ */

/* on_write: "Is this command allowed right now?"
 * Return false to refuse. Here: no pumping when overheated. */
static bool pump_check(const RegEntry *e, uint32_t raw)
{
    (void)e;
    if (raw && temperature > 90.0f) {
        printf("  [on_write] pump refused: too hot (%.1f)\n", temperature);
        return false;
    }
    return true;
}

/* on_read: "Where does this value come from?"
 * Outside it looks like a float register. Inside we sample a
 * (fake) 12-bit ADC and convert counts to volts. */
static void voltage_read(const RegEntry *e)
{
    (void)e;
    uint16_t adc_counts = 2048 + (uint16_t)(counter % 100);   /* pretend ADC */
    voltage = adc_counts * 3.3f / 4096.0f;
}

/* on_change: "Who needs to know it changed?"
 * Runs later from reg_poll(). Here: drive the LED, report it. */
static void led_changed(const RegEntry *e)
{
    (void)e;
    /* on MCU: HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, led_state); */
    printf("  [on_change] LED %s\n", led_state ? "ON" : "OFF");
}

static void offset_changed(const RegEntry *e)
{
    printf("  [on_change] %s is now %d\n", e->name, offset);
}

static void temp_changed(const RegEntry *e)
{
    /* on MCU: mqtt_publish("device/temp", ...); */
    printf("  [on_change] %s drifted to %.2f\n", e->name, temperature);
}

/* your callback code here */


/* ============================================================
 * STEP 3: your table. One line per register. Copy a row, edit it.
 *
 *   .name        what you type in the CLI
 *   .ptr         address of the variable from STEP 1
 *   .type        REG_U8 / U16 / U32 / I8 / I16 / I32 / FLOAT / BOOL
 *   .perm        REG_RO or REG_RW
 *   .min/.max    optional range. Use .u for unsigned, .i for signed,
 *                .f for float. Leave out for no range.
 *   .on_write    optional hooks from STEP 2
 *   .on_read
 *   .on_change
 *   .description shown by "info <name>"
 *
 * Last row must be { .name = NULL }.
 * ============================================================ */

static const RegEntry registry[] = {
    { .name = "temp",     .ptr = &temperature, .type = REG_FLOAT, .perm = REG_RO,
      .on_change = temp_changed,
      .description = "Water temperature, Celsius" },

    { .name = "interval", .ptr = &interval,    .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000,
      .description = "Sampling interval, ms" },

    { .name = "led",      .ptr = &led_state,   .type = REG_BOOL,  .perm = REG_RW,
      .on_change = led_changed,
      .description = "Status LED" },

    { .name = "counter",  .ptr = &counter,     .type = REG_U32,   .perm = REG_RO,
      .description = "Uptime tick counter" },

    { .name = "offset",   .ptr = &offset,      .type = REG_I16,   .perm = REG_RW,
      .min.i = -50, .max.i = 50,
      .on_change = offset_changed,
      .description = "Calibration offset, signed" },

    { .name = "gain",     .ptr = &gain,        .type = REG_FLOAT, .perm = REG_RW,
      .min.f = 0.5f, .max.f = 2.5f,
      .description = "Sensor gain multiplier" },

    { .name = "pump",     .ptr = &pump,        .type = REG_BOOL,  .perm = REG_RW,
      .on_write = pump_check,
      .description = "Pump on/off, refused when temp > 90" },

    { .name = "voltage",  .ptr = &voltage,     .type = REG_FLOAT, .perm = REG_RO,
      .on_read = voltage_read,
      .description = "Supply voltage from ADC" },

    /* your table rows here */

    { .name = NULL }   /* sentinel, keep last */
};


/* ============================================================
 * STEP 4: transport. Desktop version: stdout is the "UART TX".
 *
 * On the MCU, replace the body with your UART call, e.g.
 *   return HAL_UART_Transmit(&huart2, (uint8_t *)buf, len, 100);
 * ============================================================ */

static int transport_write(const uint8_t *buf, uint16_t len)
{
    return (int)fwrite(buf, 1, len, stdout);
}

static int transport_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    return (int)fread(buf, 1, len, stdin);
}


/* ============================================================
 * STEP 5: main loop. Feed bytes in, run your logic, poll hooks.
 * ============================================================ */

int main(void)
{
    RegTransport tx = { .read = transport_read, .write = transport_write };

    RegTable table;
    if (reg_table_init(&table, registry) != REG_OK) {
        fprintf(stderr, "table init failed (more than REGTABLE_MAX_ENTRIES?)\n");
        return 1;
    }

    RegCli cli;
    regcli_init(&cli, &table, tx);
    cli.echo = false;   /* the terminal already echoes what you type */

    printf("regtable desktop example. Type 'help'.\n> ");
    fflush(stdout);

    int c;
    while ((c = getchar()) != EOF) {

        /* 1. feed the byte in (on MCU: from UART RX) */
        regcli_feed(&cli, (uint8_t)c);

        if (c == '\n') {
            /* 2. your business logic. Here we fake a sensor: every
             *    4th command the temperature drifts. Your code
             *    changed the variable, so it tells the table. */
            counter++;
            if (counter % 4 == 0) {
                temperature += 0.5f;
                reg_mark_dirty(&table, reg_find(&table, "temp"));
            }

            /* your logic here */

            /* 3. run deferred on_change hooks */
            reg_poll(&table);

            printf("> ");
            fflush(stdout);
        }
    }
    return 0;
}
