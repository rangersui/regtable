/*
 * regtable as a Modbus RTU slave on an Arduino Uno, over the USB
 * serial port. Same registers as the CLI example, reachable from
 * any Modbus master (QModMaster, pymodbus, a PLC):
 *
 *   word 1    led     BOOL RW   on_change drives the built-in LED
 *   word 2    a0      U16  RO   on_read samples analog pin A0
 *   word 3-4  uptime  U32  RO   on_read reads millis()
 *
 * Master settings: 115200 baud, slave address 1, function codes
 * 03/04 to read, 06/16 to write. Opening the port resets the Uno;
 * give it two seconds before the first request.
 *
 * Frame boundaries: Modbus RTU separates frames by 3.5 characters
 * of silence (t3.5). Here that is a micros() gap check on the
 * receive buffer; the serial line spec fixes t3.5 at 1.75 ms for
 * baud rates above 19200. Behind a USB adapter that inserts its
 * own gaps, raise T35_US until frames stop splitting.
 */

#include "regtable_modbus.h"

#define T35_US 1750UL

/* -- state ------------------------------------------------- */

static uint8_t  led    = 0;
static uint16_t a0     = 0;
static uint32_t uptime = 0;

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

/* -- table ------------------------------------------------- */
/*  Every field is written out, in declaration order: the sketch is
 *  C++, and the AVR toolchain's g++ (7.x) only takes designated
 *  initializers when nothing is skipped. */

static const RegEntry registry[] = {
    { .name = "led",    .ptr = &led,    .type = REG_BOOL, .perm = REG_RW,
      .modbus_addr = 1, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = NULL, .on_change = led_changed,
      .description = "Built-in LED" },
    { .name = "a0",     .ptr = &a0,     .type = REG_U16,  .perm = REG_RO,
      .modbus_addr = 2, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = a0_read, .on_change = NULL,
      .description = "Analog pin A0, 0..1023" },
    { .name = "uptime", .ptr = &uptime, .type = REG_U32,  .perm = REG_RO,
      .modbus_addr = 3, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = uptime_read, .on_change = NULL,
      .description = "millis()" },
    { .name = NULL,     .ptr = NULL,    .type = REG_U8,   .perm = REG_RO,
      .modbus_addr = 0, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = NULL, .on_change = NULL,
      .description = NULL }
};

/* -- frame collection --------------------------------------- */

static RegTable  table;
static RegModbus mb;

static uint8_t  frame[REGMB_FRAME_MAX];
static uint16_t frame_len;
static unsigned long last_byte_us;

void setup()
{
    Serial.begin(115200);
    pinMode(LED_BUILTIN, OUTPUT);

    reg_table_init(&table, registry);
    regmb_init(&mb, &table, 1);        /* slave address 1 */
}

void loop()
{
    /* collect bytes; a t3.5 gap ends the frame */
    while (Serial.available() > 0) {
        int c = Serial.read();
        if (frame_len < sizeof(frame)) {
            frame[frame_len++] = (uint8_t)c;
        }
        last_byte_us = micros();
    }

    if (frame_len > 0 && (micros() - last_byte_us) > T35_US) {
        uint8_t resp[REGMB_FRAME_MAX];
        uint16_t n = regmb_process(&mb, frame, frame_len, resp, sizeof(resp));
        if (n > 0) {
            Serial.write(resp, n);
        }
        frame_len = 0;
    }

    reg_poll(&table);       /* deferred on_change hooks (led_changed) */
}
