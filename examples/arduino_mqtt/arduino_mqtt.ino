/*
 * regtable over MQTT on an Arduino with an Ethernet shield
 * (W5100/W5500) and the PubSubClient library. Same registers as
 * the other Arduino examples:
 *
 *   led     BOOL RW   on_change drives the built-in LED
 *   a0      U16  RO   on_read samples analog pin A0
 *   uptime  U32  RO   on_read reads millis()
 *
 * Status: compile-verified for the Uno (CI builds it); not yet run
 * on hardware, which needs an Ethernet shield. The CLI and RTU
 * examples are hardware-verified on an Uno.
 *
 * Topics (watch with: mosquitto_sub -h <broker> -t 'uno/#' -v):
 *   uno/led  uno/a0  uno/uptime     state, retained
 *   uno/<name>/set                  commands
 *   uno/$meta/$table  uno/$meta/<name>   the table's self-description, retained
 *   uno/status                      online/offline, retained (LWT)
 *
 * The connect sequence carries the availability conventions: a last
 * will marks the device offline when the broker loses it, and every
 * (re)connect publishes "online", the table's shape
 * (regmqtt_announce), and the full state (regmqtt_publish_all), so
 * the broker's retained values are never stale for longer than one
 * reconnect and a host that joins later finds the table without
 * asking.
 */

#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#include "regtable_mqtt.h"

static byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xEE };
static IPAddress ip(192, 168, 1, 178);
static IPAddress broker(192, 168, 1, 10);   /* the mosquitto host */

static EthernetClient eth;
static PubSubClient   client(eth);

/* -- state ------------------------------------------------- */

static uint8_t  led    = 0;
static uint16_t a0     = 0;
static uint32_t uptime = 0;

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

/* -- table (all fields, declaration order: avr g++ 7) ------- */

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
    { .name = NULL,     .ptr = NULL,    .type = REG_U8,   .perm = REG_RO,
      .modbus_addr = 0, .min = {0}, .max = {0},
      .on_write = NULL, .on_read = NULL, .on_change = NULL,
      .description = NULL }
};

/* -- glue between the adapter and PubSubClient -------------- */

static RegTable table;
static RegMqtt  mq;
static unsigned long last_connect_try;

static int mqtt_publish(const char *topic, const char *payload,
                        bool retain, void *user)
{
    (void)user;
    return client.publish(topic, payload, retain) ? 0 : -1;
}

static void mqtt_message(char *topic, byte *payload, unsigned int length)
{
    char buf[32];
    if (length >= sizeof(buf)) return;      /* no register value is this long */
    memcpy(buf, payload, length);
    buf[length] = '\0';
    regmqtt_handle(&mq, topic, buf);        /* refusals answer by silence */
}

static void mqtt_connect(void)
{
    /* last will: broker marks the device offline if it vanishes */
    if (client.connect("uno-regtable", "uno/status", 0, true, "offline")) {
        client.publish("uno/status", "online", true);
        client.subscribe("uno/+/set");
        regmqtt_announce(&mq);              /* the table's shape, */
        regmqtt_publish_all(&mq);           /* then its state: reconnect heals everything */
    }
    /* Start the retry interval after a potentially blocking attempt
     * returns, so a slow failure cannot trigger another one at once. */
    last_connect_try = millis();
}

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    Ethernet.begin(mac, ip);
    client.setServer(broker, 1883);
    client.setCallback(mqtt_message);

    reg_table_init(&table, registry);
    regmqtt_init(&mq, &table, "uno", mqtt_publish, NULL);
    static const RegIdentity who = { "uno", "1.0", NULL, "ATmega328P" };   /* $meta/$id carries these */
    regmqtt_set_identity(&mq, &who);     /* REG_ERR_TABLE when the strings would not fit REGTABLE_MQTT_META_SIZE; these do */
    mqtt_connect();
}

static unsigned long last_poll;

void loop()
{
    if (!client.connected()) {
        /* a connect attempt can block for seconds; spaced attempts
         * keep the main loop (and reg_poll) alive while the broker
         * is unreachable. millis() subtraction is wrap-safe. */
        if (millis() - last_connect_try >= 5000) {
            mqtt_connect();
        }
    } else {
        client.loop();                      /* keepalive + incoming messages */
    }
    reg_poll(&table);                       /* app hooks (led_changed) */

    if (millis() - last_poll >= 1000) {     /* telemetry cadence: 1 s */
        last_poll = millis();
        regmqtt_poll(&mq);
    }
}
