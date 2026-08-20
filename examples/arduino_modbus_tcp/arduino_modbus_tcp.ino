/*
 * regtable as a Modbus TCP slave on an Arduino with an Ethernet
 * shield (W5100/W5500, the standard Ethernet library). Same
 * registers as the other Arduino examples.
 *
 * Status: compile-verified for the Uno (CI builds it); not yet run
 * on hardware, which needs an Ethernet shield. The CLI and RTU
 * examples are hardware-verified on an Uno.
 *
 * Registers:
 *
 *   word 1    led     BOOL RW   on_change drives the built-in LED
 *   word 2    a0      U16  RO   on_read samples analog pin A0
 *   word 3-4  uptime  U32  RO   on_read reads millis()
 *
 * The sketch uses a static IP (edit below to fit the LAN) and
 * listens on port 502. Point a Modbus TCP master at it; the unit
 * id is echoed, not filtered.
 *
 * Framing on TCP is the MBAP length field, not silence: read the
 * 7-byte header, then the number of bytes it announces.
 */

#include <SPI.h>
#include <Ethernet.h>
#include "regtable_modbus.h"

static byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
static IPAddress ip(192, 168, 1, 177);
static EthernetServer server(502);

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

/* -- ADU reading ------------------------------------------- */

static RegTable  table;
static RegModbus mb;

/*  Read exactly n bytes, waiting up to 500 ms between bytes. */
static bool read_exact(EthernetClient &c, uint8_t *buf, uint16_t n)
{
    uint16_t got = 0;
    unsigned long last = millis();
    while (got < n) {
        if (!c.connected()) return false;
        int b = c.read();
        if (b < 0) {
            if (millis() - last > 500) return false;
            continue;
        }
        buf[got++] = (uint8_t)b;
        last = millis();
    }
    return true;
}

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    Ethernet.begin(mac, ip);
    server.begin();

    reg_table_init(&table, registry);
    regmb_init(&mb, &table, 1);
}

void loop()
{
    EthernetClient client = server.available();
    if (client) {
        uint8_t adu[64];    /* enough for every request this map accepts */
        uint8_t resp[64];
        if (read_exact(client, adu, 7)) {
            uint16_t mlen = (uint16_t)((adu[4] << 8) | adu[5]);
            if (mlen >= 2 && 6 + mlen <= (uint16_t)sizeof(adu) &&
                read_exact(client, adu + 7, (uint16_t)(mlen - 1))) {
                uint16_t n = regmb_process_tcp(&mb, adu, (uint16_t)(6 + mlen),
                                               resp, sizeof(resp));
                if (n > 0) client.write(resp, n);
            } else {
                client.stop();   /* oversized or broken ADU: drop it */
            }
        }
    }
    reg_poll(&table);       /* deferred on_change hooks (led_changed) */
}
