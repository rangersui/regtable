/*
 * regtable regression test. Self-checking, no hardware, no shell.
 *
 *   Build + run:  make test        (or build.bat test in cmd)
 *   Exit code:    0 = all pass, 1 = something failed
 *
 * The CLI's transport writes into a capture buffer instead of a
 * UART; each case feeds a command line and asserts on the exact
 * bytes the CLI produced, then checks any side effects on the
 * variables and hooks.
 */

#include <stdio.h>
#include <string.h>
#include "regtable_cli.h"

/* -- device state under test ------------------------------- */

static float    temperature = 23.4f;
static uint16_t interval    = 1000;
static uint8_t  led_state   = 0;
static uint32_t counter     = 0;
static uint32_t setpoint    = 0;
static int16_t  offset      = 0;
static float    gain        = 1.0f;
static uint8_t  pump        = 0;
static uint8_t  small       = 5;
static float    voltage     = 0.0f;
static volatile uint32_t hwreg = 0;   /* stands in for a CMSIS peripheral register */

static int led_changes    = 0;
static int offset_changes = 0;
static int temp_changes   = 0;
static int voltage_reads  = 0;

static void led_changed(const RegEntry *e)    { (void)e; led_changes++; }
static void offset_changed(const RegEntry *e) { (void)e; offset_changes++; }
static void temp_changed(const RegEntry *e)   { (void)e; temp_changes++; }

/* refuse pump=1 when temp > 90 */
static bool pump_check(const RegEntry *e, uint32_t raw)
{
    (void)e;
    return !(raw && temperature > 90.0f);
}

/* fake ADC: 2048 counts -> 1.65 V */
static void voltage_read(const RegEntry *e)
{
    (void)e;
    voltage_reads++;
    voltage = 2048 * 3.3f / 4096.0f;
}

static const RegEntry registry[] = {
    { .name = "temp",     .ptr = &temperature, .type = REG_FLOAT, .perm = REG_RO,
      .on_change = temp_changed, .description = "Water temperature" },
    { .name = "interval", .ptr = &interval,    .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000, .description = "Sampling interval in ms" },
    { .name = "led",      .ptr = &led_state,   .type = REG_BOOL,  .perm = REG_RW,
      .on_change = led_changed },
    { .name = "counter",  .ptr = &counter,     .type = REG_U32,   .perm = REG_RO },
    { .name = "setpoint", .ptr = &setpoint,    .type = REG_U32,   .perm = REG_RW },
    { .name = "offset",   .ptr = &offset,      .type = REG_I16,   .perm = REG_RW,
      .min.i = -50, .max.i = 50, .on_change = offset_changed },
    { .name = "gain",     .ptr = &gain,        .type = REG_FLOAT, .perm = REG_RW,
      .min.f = 0.5f, .max.f = 2.5f },
    { .name = "pump",     .ptr = &pump,        .type = REG_BOOL,  .perm = REG_RW,
      .on_write = pump_check },
    { .name = "small",    .ptr = &small,       .type = REG_U8,    .perm = REG_RW },
    { .name = "voltage",  .ptr = &voltage,     .type = REG_FLOAT, .perm = REG_RO,
      .on_read = voltage_read },
    { .name = "hwreg",    .ptr = &hwreg,       .type = REG_U32,   .perm = REG_RW },
    { .name = NULL }
};

/* -- capture transport ------------------------------------- */

static char   cap[1024];
static size_t cap_len;

static int    cap_writes;   /* transport calls, to check batching */

static int cap_write(const uint8_t *buf, uint16_t len)
{
    cap_writes++;
    if (cap_len + len < sizeof(cap)) {
        memcpy(cap + cap_len, buf, len);
        cap_len += len;
        cap[cap_len] = '\0';
    }
    return len;
}

static int cap_read(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)buf; (void)len; (void)timeout_ms;
    return 0;
}

/* -- harness ----------------------------------------------- */

static RegTable table;
static RegCli   cli;
static int      failures;
static int      cases;

/* feed a line to the CLI, return what it printed */
static const char *run(const char *line)
{
    cap_len = 0;
    cap[0]  = '\0';
    for (const char *p = line; *p; p++) {
        regcli_feed(&cli, (uint8_t)*p);
    }
    regcli_feed(&cli, '\n');
    return cap;
}

#define CHECK(cond) do {                                             \
    cases++;                                                         \
    if (!(cond)) {                                                   \
        failures++;                                                  \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                \
} while (0)

#define EXPECT(line, want) do {                                      \
    const char *got_ = run(line);                                    \
    cases++;                                                         \
    if (strcmp(got_, want) != 0) {                                   \
        failures++;                                                  \
        printf("FAIL %s:%d  '%s'\n   want: \"%s\"\n   got:  \"%s\"\n",\
               __FILE__, __LINE__, line, want, got_);                \
    }                                                                \
} while (0)

#define CONTAINS(line, needle) do {                                  \
    const char *got_ = run(line);                                    \
    cases++;                                                         \
    if (strstr(got_, needle) == NULL) {                              \
        failures++;                                                  \
        printf("FAIL %s:%d  '%s' output lacks \"%s\"\n   got: \"%s\"\n",\
               __FILE__, __LINE__, line, needle, got_);              \
    }                                                                \
} while (0)

/* -- cases ------------------------------------------------- */

static void test_get_set_basic(void)
{
    EXPECT("get temp",           "23.4\r\n");
    EXPECT("set temp 30",        "ERR: read-only\r\n");
    EXPECT("set counter 5",      "ERR: read-only\r\n");
    EXPECT("get nothing",        "ERR: not found\r\n");
    EXPECT("set nothing 1",      "ERR: not found\r\n");
    EXPECT("bogus",              "ERR: unknown command. type 'help'\r\n");
    EXPECT("",                   "");
    EXPECT("   ",                "");
}

static void test_bool(void)
{
    EXPECT("set led true",  "OK\r\n");   CHECK(led_state == 1);
    EXPECT("set led 0",     "OK\r\n");   CHECK(led_state == 0);
    EXPECT("set led 1",     "OK\r\n");   CHECK(led_state == 1);
    EXPECT("set led false", "OK\r\n");   CHECK(led_state == 0);
    EXPECT("set led maybe", "ERR: invalid value\r\n");
    EXPECT("set led TRUE",  "OK\r\n");   CHECK(led_state == 1);
    EXPECT("set led False", "OK\r\n");   CHECK(led_state == 0);
    EXPECT("set led 2",     "ERR: invalid value\r\n");
    EXPECT("get led",       "false\r\n");
}

static void test_unsigned_range(void)
{
    EXPECT("set interval 50",     "ERR: out of range\r\n"); CHECK(interval == 1000);
    EXPECT("set interval 60001",  "ERR: out of range\r\n");
    EXPECT("set interval 500",    "OK\r\n");                CHECK(interval == 500);
    EXPECT("set interval -1",     "ERR: invalid value\r\n");
    EXPECT("set interval abc",    "ERR: invalid value\r\n");
    EXPECT("set interval 12x",    "ERR: invalid value\r\n");
    EXPECT("set interval 0x1F4",  "OK\r\n");                CHECK(interval == 500);

    /* U8 type domain, no user range */
    EXPECT("set small 255",       "OK\r\n");                CHECK(small == 255);
    EXPECT("set small 256",       "ERR: out of range\r\n");

    /* U32 full range, no user range */
    EXPECT("set setpoint -1",         "ERR: invalid value\r\n"); CHECK(setpoint == 0);
    /* a sign behind whitespace strtoul would skip: must be caught as
     * a type error here, not left to the 64-bit-only ULONG_MAX guard */
    EXPECT("set setpoint \v-1",       "ERR: invalid value\r\n"); CHECK(setpoint == 0);
    EXPECT("set setpoint \f-1",       "ERR: invalid value\r\n"); CHECK(setpoint == 0);
    EXPECT("set setpoint 4294967295", "OK\r\n");                CHECK(setpoint == 0xFFFFFFFFu);
    EXPECT("get setpoint",            "4294967295\r\n");
    EXPECT("set setpoint 4294967296", "ERR: out of range\r\n");
    EXPECT("set setpoint 0x10",       "OK\r\n");
    EXPECT("get setpoint",            "16\r\n");
    EXPECT("set setpoint 0X1f",       "OK\r\n");                CHECK(setpoint == 31);
    EXPECT("set setpoint +7",         "OK\r\n");                CHECK(setpoint == 7);

    /* decimal and 0x hex only: a leading zero is not octal */
    EXPECT("set setpoint 010",        "OK\r\n");                CHECK(setpoint == 10);
    EXPECT("set interval 0100",       "OK\r\n");                CHECK(interval == 100);
    EXPECT("set setpoint 0x",         "ERR: invalid value\r\n");
    EXPECT("set setpoint 0x-1",       "ERR: invalid value\r\n");
    EXPECT("set setpoint 08",         "OK\r\n");                CHECK(setpoint == 8);
    EXPECT("set setpoint 0xG",        "ERR: invalid value\r\n");
    EXPECT("set setpoint 0x100000000","ERR: out of range\r\n");
    run("set interval 500");
}

static void test_signed(void)
{
    EXPECT("set offset -30",  "OK\r\n");                CHECK(offset == -30);
    EXPECT("get offset",      "-30\r\n");
    EXPECT("set offset -51",  "ERR: out of range\r\n"); CHECK(offset == -30);
    EXPECT("set offset 51",   "ERR: out of range\r\n");
    EXPECT("set offset 40",   "OK\r\n");                CHECK(offset == 40);
    EXPECT("set offset abc",  "ERR: invalid value\r\n");
    EXPECT("set offset -0x10","OK\r\n");                CHECK(offset == -16);
    EXPECT("set offset +12",  "OK\r\n");                CHECK(offset == 12);
    EXPECT("set offset -010", "OK\r\n");                CHECK(offset == -10);   /* not octal */
    EXPECT("set offset --1",  "ERR: invalid value\r\n");
    EXPECT("set offset -",    "ERR: invalid value\r\n");
    EXPECT("set offset 0",    "OK\r\n");                CHECK(offset == 0);

    /* full I32 edges through the raw path's type domain: offset is
     * I16 with a range, so use the parser result via a wide entry */
    static int32_t wide = 0;
    static const RegEntry reg2[] = {
        { .name = "w", .ptr = &wide, .type = REG_I32, .perm = REG_RW },
        { .name = NULL }
    };
    RegTable t2;
    CHECK(reg_table_init(&t2, reg2) == REG_OK);
    CHECK(reg_set_str(&t2, &reg2[0], "-2147483648") == REG_OK);   CHECK(wide == INT32_MIN);
    CHECK(reg_set_str(&t2, &reg2[0], "2147483647")  == REG_OK);   CHECK(wide == INT32_MAX);
    CHECK(reg_set_str(&t2, &reg2[0], "2147483648")  == REG_ERR_RANGE);
    CHECK(reg_set_str(&t2, &reg2[0], "-2147483649") == REG_ERR_RANGE);
    CHECK(reg_set_str(&t2, &reg2[0], "-0x80000000") == REG_OK);   CHECK(wide == INT32_MIN);
}

static void test_float(void)
{
    EXPECT("set gain 0.75",  "OK\r\n");                CHECK(gain > 0.74f && gain < 0.76f);
    EXPECT("get gain",       "0.75\r\n");
    EXPECT("set gain 3",     "ERR: out of range\r\n");
    EXPECT("set gain 0.4",   "ERR: out of range\r\n");
    EXPECT("set gain x",     "ERR: invalid value\r\n");
    EXPECT("set gain 2.5",   "OK\r\n");

    /* NaN/Inf must not slip past the range check */
    EXPECT("set gain nan",   "ERR: invalid value\r\n");  CHECK(gain == 2.5f);
    EXPECT("set gain NaN",   "ERR: invalid value\r\n");
    /* inf and values too big for a float are a range problem, not a
     * type problem: the text is a valid number */
    EXPECT("set gain inf",   "ERR: out of range\r\n");
    EXPECT("set gain -inf",  "ERR: out of range\r\n");   CHECK(gain == 2.5f);
    EXPECT("set gain 1e40",  "ERR: out of range\r\n");   CHECK(gain == 2.5f);
    EXPECT("set gain 1e-50", "ERR: out of range\r\n");   /* underflows to 0, below 0.5 */

    /* same via the raw path (what Modbus/MQTT would hit) */
    const RegEntry *g = reg_find(&table, "gain");
    CHECK(reg_set_raw(&table, g, 0x7FC00000u) == REG_ERR_TYPE);   /* quiet NaN */
    CHECK(reg_set_raw(&table, g, 0x7F800000u) == REG_ERR_TYPE);   /* +Inf */
    CHECK(gain == 2.5f);
}

static void test_on_write_veto(void)
{
    temperature = 25.0f;
    EXPECT("set pump 1", "OK\r\n");            CHECK(pump == 1);
    EXPECT("set pump 0", "OK\r\n");            CHECK(pump == 0);

    temperature = 95.0f;
    EXPECT("set pump 1", "ERR: rejected\r\n"); CHECK(pump == 0);   /* nothing stored */
    EXPECT("set pump 0", "OK\r\n");                                 /* turning off is fine */
    temperature = 23.4f;
}

static void test_on_read(void)
{
    voltage_reads = 0;
    EXPECT("get voltage", "1.65\r\n");
    CHECK(voltage_reads == 1);
}

static void test_on_change_deferred(void)
{
    reg_poll(&table);                   /* drain bits left by earlier cases */
    led_changes = 0;
    led_state   = 0;

    run("set led 1");
    CHECK(led_changes == 0);            /* not yet: deferred */
    CHECK(reg_poll(&table) == 1);
    CHECK(led_changes == 1);

    run("set led 1");                   /* same value: no change */
    CHECK(reg_poll(&table) == 0);
    CHECK(led_changes == 1);

    run("set led 0");
    run("set led 1");                   /* two writes, one poll */
    CHECK(reg_poll(&table) == 1);       /* one dirty bit, hook runs once */
    CHECK(led_changes == 2);

    /* refused write must not mark dirty */
    temperature = 95.0f;
    run("set pump 1");
    CHECK(reg_poll(&table) == 0);
    temperature = 23.4f;

    /* out-of-range write must not mark dirty */
    offset_changes = 0;
    run("set offset 99");
    CHECK(reg_poll(&table) == 0);
    CHECK(offset_changes == 0);
}

static void test_mark_dirty(void)
{
    temp_changes = 0;
    temperature += 0.5f;                            /* our own code changed it */
    reg_mark_dirty(&table, reg_find(&table, "temp"));
    CHECK(reg_poll(&table) == 1);
    CHECK(temp_changes == 1);
    CHECK(reg_poll(&table) == 0);                   /* bit cleared */

    /* marking an entry with no on_change: dirty bit is consumed, no hook counted */
    reg_mark_dirty(&table, reg_find(&table, "counter"));
    CHECK(reg_poll(&table) == 0);

    /* out-of-table pointer is ignored, not a crash */
    RegEntry stray = { .name = "stray" };
    reg_mark_dirty(&table, &stray);
    CHECK(reg_poll(&table) == 0);
}

static void test_raw_api(void)
{
    const RegEntry *e = reg_find(&table, "small");
    uint32_t v;

    CHECK(reg_set_raw(&table, e, 42) == REG_OK);
    CHECK(reg_get_raw(e, &v) == REG_OK && v == 42);
    CHECK(reg_set_raw(&table, e, 256) == REG_ERR_RANGE);
    CHECK(reg_get_raw(e, &v) == REG_OK && v == 42);

    /* NULL table: still validates and stores, just no dirty tracking */
    CHECK(reg_set_raw(NULL, e, 7) == REG_OK);
    CHECK(small == 7);
    CHECK(reg_poll(&table) == 0);

    /* signed raw convention: sign-extended */
    const RegEntry *o = reg_find(&table, "offset");
    CHECK(reg_set_raw(&table, o, (uint32_t)(int32_t)-10) == REG_OK);
    CHECK(offset == -10);
    CHECK(reg_get_raw(o, &v) == REG_OK && (int32_t)v == -10);
    reg_poll(&table);

    /* float raw convention: bit pattern */
    const RegEntry *g = reg_find(&table, "gain");
    float f = 1.5f; uint32_t bits; memcpy(&bits, &f, 4);
    CHECK(reg_set_raw(&table, g, bits) == REG_OK);
    CHECK(gain == 1.5f);

    /* entry pointing at a volatile (hardware-style) register */
    const RegEntry *h = reg_find(&table, "hwreg");
    CHECK(reg_set_raw(&table, h, 0xDEADBEEFu) == REG_OK);
    CHECK(hwreg == 0xDEADBEEFu);
    CHECK(reg_get_raw(h, &v) == REG_OK && v == 0xDEADBEEFu);
    reg_poll(&table);
}

static void test_list_and_info(void)
{
    CONTAINS("list", "NAME            TYPE   PERM  VALUE\r\n");
    CONTAINS("list", "interval        U16    RW    500\r\n");
    CONTAINS("list", "offset          I16    RW    -10\r\n");

    CONTAINS("info interval", "range:  100..60000\r\n");
    CONTAINS("info interval", "desc:   Sampling interval in ms\r\n");
    CONTAINS("info offset",   "range:  -50..50\r\n");
    CONTAINS("info gain",     "range:  0.5..2.5\r\n");
    CONTAINS("info counter",  "modbus: 0x0000\r\n");
    EXPECT  ("info nothing",  "ERR: not found\r\n");
    CONTAINS("help",          "  info <name>         show register metadata\r\n");
}

static void test_line_editing(void)
{
    /* backspace (0x08 and 0x7F) removes the previous char */
    EXPECT("set led truX\b" "e",  "OK\r\n");
    EXPECT("set led falsX\x7f" "e", "OK\r\n");
    CHECK(led_state == 0);

    /* tabs and extra spaces are separators */
    EXPECT("set\tled   1", "OK\r\n");
    CHECK(led_state == 1);

    /* '\r' also ends a line; a following '\n' with empty buffer is a no-op */
    cap_len = 0; cap[0] = '\0';
    const char *s = "get led\r\n";
    for (; *s; s++) regcli_feed(&cli, (uint8_t)*s);
    CHECK(strcmp(cap, "true\r\n") == 0);
    reg_poll(&table);
}

static void test_json(void)
{
    /* single objects, exact */
    EXPECT("info interval --json",
        "{\"name\":\"interval\",\"type\":\"U16\",\"perm\":\"RW\",\"value\":500,"
        "\"min\":100,\"max\":60000,\"modbus\":0,\"desc\":\"Sampling interval in ms\"}\r\n");
    EXPECT("info --json offset",                       /* flag position is free */
        "{\"name\":\"offset\",\"type\":\"I16\",\"perm\":\"RW\",\"value\":-10,"
        "\"min\":-50,\"max\":50,\"modbus\":0}\r\n");
    EXPECT("info gain --json",
        "{\"name\":\"gain\",\"type\":\"FLOAT\",\"perm\":\"RW\",\"value\":1.5,"
        "\"min\":0.5,\"max\":2.5,\"modbus\":0}\r\n");
    EXPECT("info led --json",
        "{\"name\":\"led\",\"type\":\"BOOL\",\"perm\":\"RW\",\"value\":true,\"modbus\":0}\r\n");
    EXPECT("info nothing --json", "{\"error\":\"ERR: not found\"}\r\n");

    /* whole table: array, comma-separated, first and last entry present */
    const char *all = run("list --json");
    cases++;
    if (!(all[0] == '[' &&
          strstr(all, "{\"name\":\"temp\",") == all + 1 &&
          strstr(all, "},{\"name\":\"interval\"") != NULL &&
          strstr(all, "{\"name\":\"hwreg\",\"type\":\"U32\",\"perm\":\"RW\",\"value\":3735928559,\"modbus\":0}]\r\n") != NULL)) {
        failures++;
        printf("FAIL %s:%d  list --json shape\n   got: %s\n", __FILE__, __LINE__, all);
    }

    /* get / set / errors are JSON too, so a client in JSON mode
     * never sees a mixed format */
    EXPECT("get led --json",         "{\"value\":true}\r\n");
    EXPECT("get gain --json",        "{\"value\":1.5}\r\n");
    EXPECT("get nothing --json",     "{\"error\":\"ERR: not found\"}\r\n");
    EXPECT("set led false --json",   "{\"result\":\"OK\"}\r\n");   CHECK(led_state == 0);
    EXPECT("set led true --json",    "{\"result\":\"OK\"}\r\n");   CHECK(led_state == 1);
    EXPECT("set interval 1 --json",  "{\"error\":\"ERR: out of range\"}\r\n");
    EXPECT("set counter 1 --json",   "{\"error\":\"ERR: read-only\"}\r\n");
    EXPECT("bogus --json",           "{\"error\":\"ERR: unknown command. type 'help'\"}\r\n");

    /* floats: JSON path reads the raw value, not the "%.2f" text.
     * temp is RO so poke the variable directly. */
    float saved = temperature;
    temperature = 0.004f;                       /* "%.2f" would say 0.00 */
    CONTAINS("info temp --json", "\"value\":0.00400000019,");
    temperature = 3.40282347e+38f;              /* FLT_MAX, bounded output */
    CONTAINS("info temp --json", "\"value\":3.40282347e+38,");
    {
        uint32_t nan_bits = 0x7FC00000u;        /* NaN has no JSON form */
        memcpy(&temperature, &nan_bits, sizeof(temperature));
    }
    CONTAINS("info temp --json", "\"value\":null,");
    temperature = saved;

    /* text path: bounded length, large values keep their magnitude
     * instead of being cut mid-digits by the 32-byte value buffer */
    temperature = 3e38f;
    EXPECT("get temp", "3e+38\r\n");
    temperature = 0.004f;
    EXPECT("get temp", "0.004\r\n");
    temperature = saved;
}

/* strings with characters that must be escaped */
static uint8_t esc_var = 0;
static void test_json_escape(void)
{
    static const RegEntry reg2[] = {
        { .name = "q", .ptr = &esc_var, .type = REG_U8, .perm = REG_RO,
          .description = "say \"hi\" \\ tab\there\nnew" },
        { .name = NULL }
    };
    RegTable t2;
    RegCli   c2;
    RegTransport tx = { .read = cap_read, .write = cap_write };
    CHECK(reg_table_init(&t2, reg2) == REG_OK);
    regcli_init(&c2, &t2, tx);
    c2.echo = false;

    cap_len = 0; cap[0] = '\0';
    for (const char *p = "info q --json\n"; *p; p++) regcli_feed(&c2, (uint8_t)*p);
    CHECK(strcmp(cap,
        "{\"name\":\"q\",\"type\":\"U8\",\"perm\":\"RO\",\"value\":0,\"modbus\":0,"
        "\"desc\":\"say \\\"hi\\\" \\\\ tab\\there\\nnew\"}\r\n") == 0);

    /* plain runs go out in one write each, not one byte per call:
     * a 160-char description with nothing to escape must not cost
     * 160 transport calls */
    static const RegEntry reg3[] = {
        { .name = "p", .ptr = &esc_var, .type = REG_U8, .perm = REG_RO,
          .description =
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef" },
        { .name = NULL }
    };
    CHECK(reg_table_init(&t2, reg3) == REG_OK);
    cap_len = 0; cap[0] = '\0'; cap_writes = 0;
    for (const char *p = "info p --json\n"; *p; p++) regcli_feed(&c2, (uint8_t)*p);
    CHECK(strstr(cap, "\"desc\":\"0123456789abcdef") != NULL);
    CHECK(cap_writes < 30);
}

/* a description longer than cli_print's 128-byte scratch buffer:
 * desc goes straight to the transport, so it comes out whole */
static uint8_t long_desc_var = 1;
static const char long_desc[] =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef";   /* 160 chars */

static void test_long_description(void)
{
    static const RegEntry reg2[] = {
        { .name = "big", .ptr = &long_desc_var, .type = REG_U8, .perm = REG_RO,
          .description = long_desc },
        { .name = NULL }
    };
    RegTable t2;
    RegCli   c2;
    RegTransport tx = { .read = cap_read, .write = cap_write };
    CHECK(reg_table_init(&t2, reg2) == REG_OK);
    regcli_init(&c2, &t2, tx);
    c2.echo = false;

    cap_len = 0; cap[0] = '\0';
    for (const char *p = "info big\n"; *p; p++) regcli_feed(&c2, (uint8_t)*p);

    const char *d = strstr(cap, "desc:   ");
    CHECK(d != NULL);
    if (d) {
        CHECK(strstr(d, long_desc) != NULL);          /* full text, no truncation */
        CHECK(strstr(d + 8 + strlen(long_desc), "\r\n") != NULL);
    }
}

static void test_overflow_line(void)
{
    /* a line longer than the buffer is rejected whole, not run
     * truncated: a cut "set interval 60000" must not become
     * "set interval 6" */
    char big[REGTABLE_CLI_BUF_SIZE + 50];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    EXPECT(big, "ERR: line too long\r\n");
    EXPECT("get led", "true\r\n");          /* CLI still works after */

    /* padding puts "60000" across the buffer edge so only "60" fits;
     * the line is rejected and nothing is written */
    char cut[REGTABLE_CLI_BUF_SIZE + 8];
    memset(cut, ' ', sizeof(cut) - 1);
    memcpy(cut, "set setpoint", 12);
    memcpy(cut + REGTABLE_CLI_BUF_SIZE - 3, "60000", 5);
    cut[sizeof(cut) - 1] = '\0';
    uint32_t before = setpoint;
    EXPECT(cut, "ERR: line too long\r\n");
    CHECK(setpoint == before);
}

static void test_arg_count(void)
{
    /* wrong argument counts say so, they are not "unknown command"
     * and extras are not swallowed */
    EXPECT("get",                    "ERR: usage: get <name>\r\n");
    EXPECT("set led",                "ERR: usage: set <name> <value>\r\n");
    EXPECT("set led 1 junk",         "ERR: usage: set <name> <value>\r\n");
    EXPECT("info",                   "ERR: usage: info <name>\r\n");
    EXPECT("list extra",             "ERR: usage: list\r\n");
    EXPECT("help me",                "ERR: usage: help\r\n");
    EXPECT("set led --json",         "{\"error\":\"ERR: usage: set <name> <value>\"}\r\n");
    /* --json is a flag, not an argument: it never counts toward
     * MAX_ARGS and it is honoured wherever it sits, even after
     * the overflow point */
    EXPECT("set led 1 junk --json",  "{\"error\":\"ERR: usage: set <name> <value>\"}\r\n");
    EXPECT("a b c d e",              "ERR: too many arguments\r\n");
    EXPECT("a b c d e --json",       "{\"error\":\"ERR: too many arguments\"}\r\n");
    EXPECT("--json get led",         "{\"value\":true}\r\n");
    EXPECT("--json",                 "");
    EXPECT("get led",                "true\r\n");                      /* still fine */
}

/* -- main -------------------------------------------------- */

int main(void)
{
    RegTransport tx = { .read = cap_read, .write = cap_write };

    if (reg_table_init(&table, registry) != REG_OK) {
        printf("FAIL reg_table_init\n");
        return 1;
    }
    CHECK(table.count == 11);

    regcli_init(&cli, &table, tx);
    cli.echo = false;

    test_get_set_basic();
    test_bool();
    test_unsigned_range();
    test_signed();
    test_float();
    test_on_write_veto();
    test_on_read();
    test_on_change_deferred();
    test_mark_dirty();
    test_raw_api();
    test_list_and_info();
    test_line_editing();
    test_json();
    test_json_escape();
    test_long_description();
    test_overflow_line();
    test_arg_count();

    if (failures) {
        printf("\n%d of %d checks FAILED\n", failures, cases);
        return 1;
    }
    printf("all %d checks passed\n", cases);
    return 0;
}
