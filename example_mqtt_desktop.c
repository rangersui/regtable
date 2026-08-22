/*
 * regtable's MQTT projection on the desktop, with stdout standing
 * in for the broker. No network, no client library: the point is
 * to watch the pipeline, not to speak MQTT.
 *
 *   Build + run:  make run-mqtt      (or .\build mqttdemo, then .\mqttdemo)
 *
 * At start the table describes itself (regmqtt_announce:
 * demo/$meta/$table with count and fingerprint, one demo/$meta/<name>
 * per register, retained, the shape of a list --json entry), then the
 * full state goes out. Every loop turn the fake temperature drifts,
 * then regmqtt_poll publishes whatever moved:
 *
 *   PUB [r] demo/$meta/$table {"count":4,"schema":"..."}
 *   PUB [r] demo/$meta/temp {"name":"temp","type":"FLOAT",...}
 *   PUB [r] demo/temp 23.7
 *
 * Lines typed on stdin are treated as received MQTT messages,
 * topic and payload separated by a space:
 *
 *   demo/gain/set 1.5        -> OK, next poll publishes demo/gain
 *   demo/gain/set 9          -> ERR: out of range, state silent
 *   demo/temp/set 1          -> ERR: read-only
 *   quit
 *
 * On a real system the publish callback wraps the MQTT client's
 * publish call and the client's message callback feeds
 * regmqtt_handle; nothing else changes.
 */

#include <stdio.h>
#include <string.h>
#include "regtable_mqtt.h"

/* -- device state ------------------------------------------- */

static float    temp     = 23.4f;
static uint16_t interval = 1000;
static uint8_t  led      = 0;
static float    gain     = 1.0f;

static void led_changed(const RegEntry *e)
{
    (void)e;
    printf("            (led_changed hook: led is now %s)\n",
           led ? "on" : "off");
}

static const RegEntry registry[] = {
    { .name = "temp",     .ptr = &temp,     .type = REG_FLOAT, .perm = REG_RO,
      .description = "Drifting fake sensor" },
    { .name = "interval", .ptr = &interval, .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000 },
    { .name = "led",      .ptr = &led,      .type = REG_BOOL,  .perm = REG_RW,
      .on_change = led_changed },
    { .name = "gain",     .ptr = &gain,     .type = REG_FLOAT, .perm = REG_RW,
      .min.f = 0.5f, .max.f = 2.5f },
    { .name = NULL }
};

/* -- "broker": stdout --------------------------------------- */

static int print_publish(const char *topic, const char *payload,
                         bool retain, void *user)
{
    (void)user;
    printf("PUB [%c] %-24s %s\n", retain ? 'r' : '-', topic, payload);
    return 0;
}

int main(void)
{
    static RegTable table;
    static RegMqtt  mq;
    reg_table_init(&table, registry);
    regmqtt_init(&mq, &table, "demo", print_publish, NULL);

    printf("regtable MQTT demo. Publishes appear as PUB lines;\n");
    printf("type '<topic> <payload>' to inject a message, 'quit' to exit.\n");
    printf("Try: demo/gain/set 1.5\n\n");
    regmqtt_announce(&mq);                 /* after each connect: the table's shape, */
    regmqtt_publish_all(&mq);              /* then its state */

    char line[128];
    for (;;) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\r\n")] = '\0';
        if (strcmp(line, "quit") == 0) break;

        if (line[0] != '\0') {
            char *space = strchr(line, ' ');
            if (space) {
                *space = '\0';
                RegResult r = regmqtt_handle(&mq, line, space + 1);
                printf("            (handle: %s)\n", reg_result_str(r));
            } else {
                printf("            (usage: <topic> <payload>)\n");
            }
        }

        temp += 0.3f;                      /* the device lives on */
        reg_poll(&table);                  /* app hooks (led_changed) */
        regmqtt_poll(&mq);                 /* state convergence */
    }
    return 0;
}
