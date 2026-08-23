/*
 * regtable on an Arduino Uno: three registers over the USB serial port.
 *
 *   led     BOOL RW   on_change drives the built-in LED
 *   a0      U16  RO   on_read samples analog pin A0
 *   uptime  U32  RO   on_read reads millis()
 *   pwm9    U8   RW   0..255, on_change sets PWM duty on pin 9
 *
 * Open Serial Monitor at 115200, line ending "Newline", and type:
 *   list
 *   set led true
 *   get a0
 *   info uptime --json
 *
 * No FLOAT register here: avr-libc's printf has no float support
 * unless the sketch is linked with -lprintf_flt. Everything else in
 * regtable works on the Uno as-is.
 */

#include "regtable_cli.h"

/* -- state ------------------------------------------------- */

static uint8_t  led    = 0;
static uint16_t a0     = 0;
static uint32_t uptime = 0;
static uint8_t  pwm9   = 0;

/* -- hooks ------------------------------------------------- */

static void led_changed(const RegEntry *e)
{
    (void)e;
    digitalWrite(LED_BUILTIN, led ? HIGH : LOW);
}

static void a0_read(const RegEntry *e)
{
    (void)e;
    a0 = (uint16_t)analogRead(A0);
}

static void uptime_read(const RegEntry *e)
{
    (void)e;
    uptime = millis();
}

static void pwm9_changed(const RegEntry *e)
{
    (void)e;
    analogWrite(9, pwm9);      /* dim an LED wired to pin 9 */
}

/* -- table ------------------------------------------------- */
/*  Every field is written out, in declaration order. The sketch is
 *  C++, and the AVR toolchain's g++ (7.x) only takes designated
 *  initializers when nothing is skipped. In C the short form works:
 *      { .name = "led", .ptr = &led, .type = REG_BOOL, .perm = REG_RW,
 *        .on_change = led_changed }                                   */

static const RegEntry registry[] = {
    { .name = "led",    .ptr = &led,    .type = REG_BOOL, .perm = REG_RW,
      .modbus_addr = 0, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = NULL, .on_change = led_changed,
      .description = "Built-in LED" },
    { .name = "a0",     .ptr = &a0,     .type = REG_U16,  .perm = REG_RO,
      .modbus_addr = 0, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = a0_read, .on_change = NULL,
      .description = "Analog pin A0, 0..1023" },
    { .name = "uptime", .ptr = &uptime, .type = REG_U32,  .perm = REG_RO,
      .modbus_addr = 0, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = uptime_read, .on_change = NULL,
      .description = "millis()" },
    { .name = "pwm9",   .ptr = &pwm9,   .type = REG_U8,   .perm = REG_RW,
      .modbus_addr = 0, .min = { .u = 0 }, .max = { .u = 255 },
      .on_write = NULL, .on_read = NULL, .on_change = pwm9_changed,
      .description = "PWM duty on pin 9" },
    { .name = NULL,     .ptr = NULL,    .type = REG_U8,   .perm = REG_RO,
      .modbus_addr = 0, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = NULL, .on_change = NULL,
      .description = NULL }
};

/* -- transport: Serial ------------------------------------- */

static int serial_write(const uint8_t *buf, uint16_t len)
{
    return (int)Serial.write(buf, len);
}

static int serial_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    Serial.setTimeout(timeout_ms);
    return (int)Serial.readBytes(buf, len);
}

/* -- Arduino ----------------------------------------------- */

static RegTable table;
static RegCli   cli;

void setup()
{
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);

    reg_table_init(&table, registry);
    RegTransport tx = { .read = serial_read, .write = serial_write };
    regcli_init(&cli, &table, tx);
    static const RegIdentity who = { "uno", "1.0", NULL, "ATmega328P" };   /* `id` reports these */
    regcli_set_identity(&cli, &who);
}

void loop()
{
    while (Serial.available() > 0) {
        regcli_feed(&cli, (uint8_t)Serial.read());
    }
    reg_poll(&table);       /* deferred on_change hooks (led_changed) run here */
}
