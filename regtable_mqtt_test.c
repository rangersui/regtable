/*
 * MQTT adapter regression test. The publish callback captures
 * topics and payloads; no client, no broker, no network.
 *
 *   Build + run:  make test        (or build.bat test in cmd)
 *   Exit code:    0 = all pass, 1 = something failed
 */

#include <stdio.h>
#include <string.h>
#include "regtable_mqtt.h"

/* -- device state ------------------------------------------- */

static float    temp     = 23.4f;
static uint16_t interval = 1000;
static uint8_t  led      = 0;
static float    gain     = 1.0f;
static int16_t  offset   = 0;
static uint16_t rom      = 42;
static uint16_t adc      = 0;
static uint8_t  pump     = 0;

static int  led_changes  = 0;
static int  adc_reads    = 0;
static bool pump_blocked = false;

static void led_changed(const RegEntry *e) { (void)e; led_changes++; }
static void adc_read(const RegEntry *e)    { (void)e; adc_reads++; }
static bool pump_check(const RegEntry *e, uint32_t raw)
{
    (void)e;
    return !(raw && pump_blocked);
}

static const RegEntry registry[] = {
    { .name = "temp",     .ptr = &temp,     .type = REG_FLOAT, .perm = REG_RO },
    { .name = "interval", .ptr = &interval, .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000 },
    { .name = "led",      .ptr = &led,      .type = REG_BOOL,  .perm = REG_RW,
      .on_change = led_changed },
    { .name = "gain",     .ptr = &gain,     .type = REG_FLOAT, .perm = REG_RW,
      .min.f = 0.5f, .max.f = 2.5f },
    { .name = "offset",   .ptr = &offset,   .type = REG_I16,   .perm = REG_RW,
      .min.i = -50, .max.i = 50 },
    { .name = "rom",      .ptr = &rom,      .type = REG_U16,   .perm = REG_RO },
    { .name = "adc",      .ptr = &adc,      .type = REG_U16,   .perm = REG_RO,
      .on_read = adc_read },
    { .name = "pump",     .ptr = &pump,     .type = REG_BOOL,  .perm = REG_RW,
      .on_write = pump_check },
    { .name = NULL }
};

/* -- capture publish callback ------------------------------- */

#define CAP_MAX 32
static struct { char topic[64]; char payload[32]; bool retain; } cap[CAP_MAX];
static int  cap_n;
static bool broker_down;    /* publish refuses while true */

static int cap_publish(const char *topic, const char *payload,
                       bool retain, void *user)
{
    (void)user;
    if (broker_down) return -1;
    if (cap_n < CAP_MAX) {
        snprintf(cap[cap_n].topic, sizeof(cap[0].topic), "%s", topic);
        snprintf(cap[cap_n].payload, sizeof(cap[0].payload), "%s", payload);
        cap[cap_n].retain = retain;
    }
    cap_n++;
    return 0;
}

static void cap_reset(void) { cap_n = 0; }

/*  The captured message for a topic, or NULL. */
static const char *sent(const char *topic)
{
    for (int i = 0; i < cap_n && i < CAP_MAX; i++) {
        if (strcmp(cap[i].topic, topic) == 0) return cap[i].payload;
    }
    return NULL;
}

/* -- harness ------------------------------------------------ */

static RegTable table;
static RegMqtt  mq;
static int      failures;
static int      cases;

#define CHECK(cond) do {                                             \
    cases++;                                                         \
    if (!(cond)) {                                                   \
        failures++;                                                  \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                \
} while (0)

#define SENT(topic, want) do {                                       \
    const char *p_ = sent(topic);                                    \
    cases++;                                                         \
    if (!p_ || strcmp(p_, want) != 0) {                              \
        failures++;                                                  \
        printf("FAIL %s:%d  %s\n   want: \"%s\"\n   got:  \"%s\"\n", \
               __FILE__, __LINE__, topic, want, p_ ? p_ : "(nothing)"); \
    }                                                                \
} while (0)

/* -- cases -------------------------------------------------- */

static void test_init(void)
{
    CHECK(regmqtt_init(&mq, &table, "dev", cap_publish, NULL) == REG_OK);
    CHECK(regmqtt_init(&mq, NULL, "dev", cap_publish, NULL) == REG_ERR_TABLE);
    CHECK(regmqtt_init(&mq, &table, NULL, cap_publish, NULL) == REG_ERR_TABLE);
    CHECK(regmqtt_init(&mq, &table, "dev", NULL, NULL) == REG_ERR_TABLE);

    /* a prefix that pushes prefix/name/set past the topic scratch */
    static char longpfx[REGTABLE_MQTT_TOPIC_SIZE];
    memset(longpfx, 'p', sizeof(longpfx) - 1);
    longpfx[sizeof(longpfx) - 1] = '\0';
    CHECK(regmqtt_init(&mq, &table, longpfx, cap_publish, NULL) == REG_ERR_TABLE);

    /* names become topic levels, so MQTT's topic rules apply: no
     * separators (an entry "gain/set" would publish state onto the
     * command topic of "gain"), no wildcards, bounded length */
    static uint8_t v;
    static const RegEntry slashname[] = {
        { .name = "gain/set", .ptr = &v, .type = REG_U8, .perm = REG_RW },
        { .name = NULL }
    };
    static const RegEntry wildname[] = {
        { .name = "ga+in", .ptr = &v, .type = REG_U8, .perm = REG_RW },
        { .name = NULL }
    };
    static const RegEntry emptyname[] = {
        { .name = "", .ptr = &v, .type = REG_U8, .perm = REG_RW },
        { .name = NULL }
    };
    static const RegEntry longname[] = {
        { .name = "a234567890123456789012345678901x",  /* 32 chars */
          .ptr = &v, .type = REG_U8, .perm = REG_RW },
        { .name = NULL }
    };
    RegTable t2; RegMqtt m2;
    CHECK(reg_table_init(&t2, slashname) == REG_OK);
    CHECK(regmqtt_init(&m2, &t2, "dev", cap_publish, NULL) == REG_ERR_TABLE);
    CHECK(reg_table_init(&t2, wildname) == REG_OK);
    CHECK(regmqtt_init(&m2, &t2, "dev", cap_publish, NULL) == REG_ERR_TABLE);
    CHECK(reg_table_init(&t2, emptyname) == REG_OK);
    CHECK(regmqtt_init(&m2, &t2, "dev", cap_publish, NULL) == REG_ERR_TABLE);
    CHECK(reg_table_init(&t2, longname) == REG_OK);
    CHECK(regmqtt_init(&m2, &t2, "dev", cap_publish, NULL) == REG_ERR_TABLE);

    /* wildcards in the prefix would make illegal publish topics */
    CHECK(regmqtt_init(&mq, &table, "dev/+", cap_publish, NULL) == REG_ERR_TABLE);
    CHECK(regmqtt_init(&mq, &table, "#", cap_publish, NULL) == REG_ERR_TABLE);
    CHECK(regmqtt_init(&mq, &table, "", cap_publish, NULL) == REG_ERR_TABLE);

    /* a multi-level prefix is fine, and round-trips through handle */
    static uint8_t mv = 3;
    static const RegEntry mreg[] = {
        { .name = "mode", .ptr = &mv, .type = REG_U8, .perm = REG_RW },
        { .name = NULL }
    };
    CHECK(reg_table_init(&t2, mreg) == REG_OK);
    CHECK(regmqtt_init(&m2, &t2, "plant/boiler", cap_publish, NULL) == REG_OK);
    cap_reset();
    CHECK(regmqtt_poll(&m2) == 1);
    SENT("plant/boiler/mode", "3");
    CHECK(regmqtt_handle(&m2, "plant/boiler/mode/set", "7") == REG_OK);
    CHECK(mv == 7);

    /* the longest accepted name round-trips: init and handle agree */
    static uint8_t lv = 1;
    static const RegEntry lreg[] = {
        { .name = "a23456789012345678901234567890x",   /* 31 chars */
          .ptr = &lv, .type = REG_U8, .perm = REG_RW },
        { .name = NULL }
    };
    CHECK(reg_table_init(&t2, lreg) == REG_OK);
    CHECK(regmqtt_init(&m2, &t2, "d", cap_publish, NULL) == REG_OK);
    CHECK(regmqtt_handle(&m2, "d/a23456789012345678901234567890x/set", "9") == REG_OK);
    CHECK(lv == 9);

    CHECK(regmqtt_init(&mq, &table, "dev", cap_publish, NULL) == REG_OK);
}

static void test_first_poll_publishes_everything(void)
{
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 8);          /* all entries, first sync */
    SENT("dev/temp",     "23.4");
    SENT("dev/interval", "1000");
    SENT("dev/led",      "false");
    SENT("dev/gain",     "1");
    SENT("dev/offset",   "0");
    SENT("dev/rom",      "42");
    SENT("dev/adc",      "0");
    SENT("dev/pump",     "false");
    CHECK(cap[0].retain == true);           /* state is retained */

    /* in sync now: nothing more to say */
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 0);
    CHECK(cap_n == 0);
}

static void test_change_detection(void)
{
    /* through the raw path */
    const RegEntry *g = reg_find(&table, "gain");
    CHECK(reg_set_raw(&table, g, 0x40000000u) == REG_OK);   /* 2.0f */
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 1);
    SENT("dev/gain", "2");
    CHECK(sent("dev/temp") == NULL);        /* only the changed one */

    /* direct variable write, no reg_set_raw, no mark_dirty: the
     * shadow compare still catches it */
    temp = 25.0f;
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 1);
    SENT("dev/temp", "25");

    /* a change that reverts between polls publishes nothing */
    temp = 30.0f;
    temp = 25.0f;
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 0);
}

static void test_outbox_retry(void)
{
    /* broker refuses: value stays pending, no shadow update */
    interval = 2000;
    broker_down = true;
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 0);

    /* broker back: the pending value goes out on the next poll */
    broker_down = false;
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 1);
    SENT("dev/interval", "2000");
}

static void test_handle(void)
{
    /* a valid set travels the full validation path */
    CHECK(regmqtt_handle(&mq, "dev/gain/set", "1.5") == REG_OK);
    CHECK(gain == 1.5f);

    /* the echo arrives with the next poll, not synchronously */
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 1);
    SENT("dev/gain", "1.5");

    /* every refusal the core knows, through the same door */
    CHECK(regmqtt_handle(&mq, "dev/rom/set", "1") == REG_ERR_READONLY);
    CHECK(regmqtt_handle(&mq, "dev/gain/set", "9") == REG_ERR_RANGE);
    CHECK(regmqtt_handle(&mq, "dev/gain/set", "abc") == REG_ERR_TYPE);
    CHECK(regmqtt_handle(&mq, "dev/nothing/set", "1") == REG_ERR_NOT_FOUND);
    pump_blocked = true;
    CHECK(regmqtt_handle(&mq, "dev/pump/set", "true") == REG_ERR_REJECTED);
    pump_blocked = false;

    /* refused: state unchanged, so the next poll stays silent */
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 0);

    /* topics that are not <prefix>/<name>/set */
    CHECK(regmqtt_handle(&mq, "other/gain/set", "1") == REG_ERR_NOT_FOUND);
    CHECK(regmqtt_handle(&mq, "dev/gain", "1") == REG_ERR_NOT_FOUND);
    CHECK(regmqtt_handle(&mq, "dev/gain/get", "1") == REG_ERR_NOT_FOUND);
    CHECK(regmqtt_handle(&mq, "dev//set", "1") == REG_ERR_NOT_FOUND);
    CHECK(regmqtt_handle(&mq, "devx/gain/set", "1") == REG_ERR_NOT_FOUND);
    CHECK(gain == 1.5f);
}

static void test_two_consumers(void)
{
    /* the dirty bitmap serves on_change, the shadow serves MQTT;
     * one write reaches both, neither steals from the other */
    led_changes = 0;
    CHECK(regmqtt_handle(&mq, "dev/led/set", "true") == REG_OK);
    CHECK(led == 1);
    CHECK(led_changes == 0);                /* deferred */
    CHECK(reg_poll(&table) == 1);           /* app consumer */
    CHECK(led_changes == 1);
    cap_reset();
    CHECK(regmqtt_poll(&mq) == 1);          /* mqtt consumer */
    SENT("dev/led", "true");
}

static void test_on_read_cadence(void)
{
    /* on_read runs once per poll; a stable sample publishes once */
    int before = adc_reads;
    adc = 500;                              /* pretend the hook stored this */
    cap_reset();
    regmqtt_poll(&mq);
    regmqtt_poll(&mq);
    CHECK(adc_reads == before + 2);         /* sampled each poll */
    CHECK(cap_n == 1);                      /* published once */
}

static void test_reconnect(void)
{
    /* values move while the broker is unreachable */
    broker_down = true;
    interval = 3000;
    offset = -7;
    regmqtt_poll(&mq);
    broker_down = false;

    /* reconnect: everything goes out, moved or not */
    cap_reset();
    CHECK(regmqtt_publish_all(&mq) == 8);
    SENT("dev/interval", "3000");
    SENT("dev/offset",   "-7");
    SENT("dev/gain",     "1.5");
    SENT("dev/rom",      "42");
}

/* -- main --------------------------------------------------- */

int main(void)
{
    CHECK(reg_table_init(&table, registry) == REG_OK);

    test_init();
    test_first_poll_publishes_everything();
    test_change_detection();
    test_outbox_retry();
    test_handle();
    test_two_consumers();
    test_on_read_cadence();
    test_reconnect();

    if (failures) {
        printf("%d of %d checks FAILED\n", failures, cases);
        return 1;
    }
    printf("all %d checks passed\n", cases);
    return 0;
}
