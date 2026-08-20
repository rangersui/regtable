/*
 * Modbus RTU adapter regression test. Byte arrays in, byte arrays
 * out; no transport, no hardware.
 *
 *   Build + run:  make test        (or build.bat test in cmd)
 *   Exit code:    0 = all pass, 1 = something failed
 *
 * Request frames are built with regmb_crc16, which is itself checked
 * first against reference values computed independently from the
 * serial line spec's appendix B algorithm (and the widely published
 * 11 03 00 6B 00 03 -> 76 87 example).
 */

#include <stdio.h>
#include <string.h>
#include "regtable_modbus.h"

/* -- device state ------------------------------------------- */

static uint16_t interval = 1000;
static uint32_t setpoint = 0;
static float    gain     = 1.0f;
static int16_t  offset   = 0;
static uint8_t  led      = 0;
static float    temp     = 23.4f;
static uint8_t  small    = 5;
static uint8_t  pump     = 0;
static float    voltage  = 0.0f;
static uint8_t  hidden   = 9;
static uint16_t lonely   = 7;
static uint16_t rom      = 42;

static int led_changes   = 0;
static int voltage_reads = 0;
static bool pump_blocked = false;

static void led_changed(const RegEntry *e)   { (void)e; led_changes++; }
static void voltage_read(const RegEntry *e)
{
    (void)e;
    voltage_reads++;
    voltage = 1.65f;
}
static bool pump_check(const RegEntry *e, uint32_t raw)
{
    (void)e;
    return !(raw && pump_blocked);
}

/* word map: 1 interval | 2-3 setpoint | 4-5 gain | 6 offset | 7 led
 * | 8-9 temp (RO) | 10 small | 11 pump | 12-13 voltage (RO, on_read)
 * | gap at 14 | 15 lonely | 16 rom (RO); hidden is not mapped */
static const RegEntry registry[] = {
    { .name = "interval", .ptr = &interval, .type = REG_U16,   .perm = REG_RW,
      .min.u = 100, .max.u = 60000, .modbus_addr = 1 },
    { .name = "setpoint", .ptr = &setpoint, .type = REG_U32,   .perm = REG_RW,
      .modbus_addr = 2 },
    { .name = "gain",     .ptr = &gain,     .type = REG_FLOAT, .perm = REG_RW,
      .min.f = 0.5f, .max.f = 2.5f, .modbus_addr = 4 },
    { .name = "offset",   .ptr = &offset,   .type = REG_I16,   .perm = REG_RW,
      .min.i = -50, .max.i = 50, .modbus_addr = 6 },
    { .name = "led",      .ptr = &led,      .type = REG_BOOL,  .perm = REG_RW,
      .modbus_addr = 7, .on_change = led_changed },
    { .name = "temp",     .ptr = &temp,     .type = REG_FLOAT, .perm = REG_RO,
      .modbus_addr = 8 },
    { .name = "small",    .ptr = &small,    .type = REG_U8,    .perm = REG_RW,
      .modbus_addr = 10 },
    { .name = "pump",     .ptr = &pump,     .type = REG_BOOL,  .perm = REG_RW,
      .modbus_addr = 11, .on_write = pump_check },
    { .name = "voltage",  .ptr = &voltage,  .type = REG_FLOAT, .perm = REG_RO,
      .modbus_addr = 12, .on_read = voltage_read },
    { .name = "hidden",   .ptr = &hidden,   .type = REG_U8,    .perm = REG_RW },
    { .name = "lonely",   .ptr = &lonely,   .type = REG_U16,   .perm = REG_RW,
      .modbus_addr = 15 },
    { .name = "rom",      .ptr = &rom,      .type = REG_U16,   .perm = REG_RO,
      .modbus_addr = 16 },
    { .name = NULL }
};

/* -- harness ------------------------------------------------ */

static RegTable  table;
static RegModbus mb;
static int       failures;
static int       cases;

#define CHECK(cond) do {                                             \
    cases++;                                                         \
    if (!(cond)) {                                                   \
        failures++;                                                  \
        printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                \
} while (0)

/*  Append the CRC to req (in place), run it through the adapter,
 *  and return the response length. */
static uint8_t resp[REGMB_FRAME_MAX];

static uint16_t xfer(uint8_t *req, uint16_t len)
{
    uint16_t crc = regmb_crc16(req, len);
    req[len]     = (uint8_t)crc;
    req[len + 1] = (uint8_t)(crc >> 8);
    memset(resp, 0xAA, sizeof(resp));
    return regmb_process(&mb, req, (uint16_t)(len + 2), resp, sizeof(resp));
}

/*  Compare the response with want (CRC appended here). */
static void expect_resp(uint16_t got_len, const uint8_t *want, uint16_t want_len,
                        int line)
{
    uint8_t buf[REGMB_FRAME_MAX];
    memcpy(buf, want, want_len);
    uint16_t crc = regmb_crc16(buf, want_len);
    buf[want_len]     = (uint8_t)crc;
    buf[want_len + 1] = (uint8_t)(crc >> 8);
    cases++;
    if (got_len != want_len + 2 || memcmp(resp, buf, got_len) != 0) {
        failures++;
        printf("FAIL %s:%d  response mismatch (len %u vs %u)\n   got: ",
               __FILE__, line, got_len, (unsigned)(want_len + 2));
        for (uint16_t i = 0; i < got_len; i++) printf("%02X ", resp[i]);
        printf("\n  want: ");
        for (uint16_t i = 0; i < want_len + 2u; i++) printf("%02X ", buf[i]);
        printf("\n");
    }
}

#define EXPECT_RESP(len, ...) do {                                   \
    static const uint8_t want_[] = { __VA_ARGS__ };                  \
    expect_resp(len, want_, (uint16_t)sizeof(want_), __LINE__);      \
} while (0)

#define EXPECT_EXC(len, fc, code) EXPECT_RESP(len, 0x01, (fc) | 0x80, code)

/* -- cases -------------------------------------------------- */

static void test_crc_reference(void)
{
    /* values computed independently from the spec's algorithm */
    static const uint8_t spec_example[] = { 0x11, 0x03, 0x00, 0x6B, 0x00, 0x03 };
    CHECK(regmb_crc16(spec_example, 6) == 0x8776);   /* lo 76, hi 87 */
    static const uint8_t read1[] = { 0x01, 0x03, 0x00, 0x01, 0x00, 0x01 };
    CHECK(regmb_crc16(read1, 6) == 0xCAD5);
    static const uint8_t write1[] = { 0x01, 0x06, 0x00, 0x01, 0x02, 0x03 };
    CHECK(regmb_crc16(write1, 6) == 0x6B99);
    static const uint8_t resp1[] = { 0x01, 0x03, 0x02, 0x03, 0xE8 };
    CHECK(regmb_crc16(resp1, 5) == 0xFAB8);
}

static void test_init(void)
{
    CHECK(regmb_init(&mb, &table, 1) == REG_OK);
    CHECK(regmb_init(&mb, &table, 0) == REG_ERR_TABLE);     /* broadcast addr */
    CHECK(regmb_init(&mb, &table, 248) == REG_ERR_TABLE);   /* reserved */

    /* overlap: U32 at 1 spans 1-2, U16 at 2 collides */
    static uint32_t v32; static uint16_t v16;
    static const RegEntry bad1[] = {
        { .name = "a", .ptr = &v32, .type = REG_U32, .perm = REG_RW, .modbus_addr = 1 },
        { .name = "b", .ptr = &v16, .type = REG_U16, .perm = REG_RW, .modbus_addr = 2 },
        { .name = NULL }
    };
    RegTable t2; RegModbus m2;
    CHECK(reg_table_init(&t2, bad1) == REG_OK);
    CHECK(regmb_init(&m2, &t2, 1) == REG_ERR_TABLE);

    /* two-word entry at the top of the address space does not fit */
    static const RegEntry bad2[] = {
        { .name = "a", .ptr = &v32, .type = REG_U32, .perm = REG_RW, .modbus_addr = 0xFFFF },
        { .name = NULL }
    };
    CHECK(reg_table_init(&t2, bad2) == REG_OK);
    CHECK(regmb_init(&m2, &t2, 1) == REG_ERR_TABLE);

    CHECK(regmb_init(&mb, &table, 1) == REG_OK);   /* leave mb usable */
}

/* the full exchange from the hardcoded vectors: no helper involved */
static void test_read_exact_bytes(void)
{
    static const uint8_t req[] = { 0x01, 0x03, 0x00, 0x01, 0x00, 0x01, 0xD5, 0xCA };
    static const uint8_t want[] = { 0x01, 0x03, 0x02, 0x03, 0xE8, 0xB8, 0xFA };
    uint16_t n = regmb_process(&mb, req, sizeof(req), resp, sizeof(resp));
    cases++;
    if (n != sizeof(want) || memcmp(resp, want, n) != 0) {
        failures++;
        printf("FAIL %s:%d  exact read exchange\n", __FILE__, __LINE__);
    }
}

static void test_read(void)
{
    uint8_t req[16];

    /* single-word entries */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x01, 0x00, 0x01 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x03, 0x02, 0x03, 0xE8);

    /* FC04 reads the same map */
    memcpy(req, (uint8_t[]){ 0x01, 0x04, 0x00, 0x01, 0x00, 0x01 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x04, 0x02, 0x03, 0xE8);

    /* two-word U32, high word first (ABCD) */
    setpoint = 0xDEADBEEFu;
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x02, 0x00, 0x02 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x03, 0x04, 0xDE, 0xAD, 0xBE, 0xEF);

    /* same value with word_swap (CDAB) */
    mb.word_swap = true;
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x02, 0x00, 0x02 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x03, 0x04, 0xBE, 0xEF, 0xDE, 0xAD);
    mb.word_swap = false;

    /* FLOAT 1.0f = 0x3F800000 */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x04, 0x00, 0x02 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x03, 0x04, 0x3F, 0x80, 0x00, 0x00);

    /* I16 negative as 16-bit two's complement */
    offset = -10;
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x06, 0x00, 0x01 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x03, 0x02, 0xFF, 0xF6);

    /* a span crossing several entries: interval + setpoint + gain */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x01, 0x00, 0x05 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x03, 0x0A,
                0x03, 0xE8, 0xDE, 0xAD, 0xBE, 0xEF, 0x3F, 0x80, 0x00, 0x00);

    /* on_read runs for each read of voltage */
    int before = voltage_reads;
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x0C, 0x00, 0x02 }, 6);
    CHECK(xfer(req, 6) > 0);
    CHECK(voltage_reads == before + 1);
}

static void test_read_errors(void)
{
    uint8_t req[16];

    /* half of a two-word value, both halves */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x02, 0x00, 0x01 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x03, 0x02);
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x03, 0x00, 0x01 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x03, 0x02);

    /* span ending inside a two-word value */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x01, 0x00, 0x02 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x03, 0x02);

    /* gap at 14 breaks the span 13..15 */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x0D, 0x00, 0x03 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x03, 0x02);

    /* nothing at address 0x20; the unmapped entry is invisible */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x20, 0x00, 0x01 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x03, 0x02);

    /* quantity out of spec */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x01, 0x00, 0x00 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x03, 0x03);
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x01, 0x00, 0x7E }, 6);
    EXPECT_EXC(xfer(req, 6), 0x03, 0x03);

    /* truncated PDU (CRC valid, structure wrong) */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x01 }, 4);
    EXPECT_EXC(xfer(req, 4), 0x03, 0x03);
}

static void test_write_single(void)
{
    uint8_t req[16];

    /* write interval = 500, response echoes the request */
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x01, 0x01, 0xF4 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x06, 0x00, 0x01, 0x01, 0xF4);
    CHECK(interval == 500);

    /* I16 negative write */
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x06, 0xFF, 0xF6 }, 6);
    EXPECT_RESP(xfer(req, 6), 0x01, 0x06, 0x00, 0x06, 0xFF, 0xF6);
    CHECK(offset == -10);

    /* BOOL write sets the dirty bit; reg_poll runs on_change */
    led_changes = 0;
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x07, 0x00, 0x01 }, 6);
    CHECK(xfer(req, 6) == 8);
    CHECK(led == 1);
    CHECK(led_changes == 0);           /* deferred */
    CHECK(reg_poll(&table) == 1);
    CHECK(led_changes == 1);

    /* range reject: interval 50 < min 100 -> exception 04, unchanged */
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x01, 0x00, 0x32 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x06, 0x04);
    CHECK(interval == 500);

    /* U8 type domain: 256 does not fit small */
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x0A, 0x01, 0x00 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x06, 0x04);

    /* read-only register */
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x10, 0x00, 0x01 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x06, 0x04);
    CHECK(rom == 42);

    /* on_write veto */
    pump_blocked = true;
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x0B, 0x00, 0x01 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x06, 0x04);
    CHECK(pump == 0);
    pump_blocked = false;

    /* single-word write cannot address a two-word value */
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x02, 0x00, 0x01 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x06, 0x02);
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x03, 0x00, 0x01 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x06, 0x02);
}

static void test_write_multiple(void)
{
    uint8_t req[32];

    /* setpoint + gain in one request (addr 2, qty 4) */
    memcpy(req, (uint8_t[]){ 0x01, 0x10, 0x00, 0x02, 0x00, 0x04, 0x08,
                             0x12, 0x34, 0x56, 0x78,     /* setpoint */
                             0x3F, 0xC0, 0x00, 0x00 }, 15);  /* gain 1.5 */
    EXPECT_RESP(xfer(req, 15), 0x01, 0x10, 0x00, 0x02, 0x00, 0x04);
    CHECK(setpoint == 0x12345678u);
    CHECK(gain == 1.5f);

    /* CDAB word order on the same write */
    mb.word_swap = true;
    memcpy(req, (uint8_t[]){ 0x01, 0x10, 0x00, 0x02, 0x00, 0x04, 0x08,
                             0xBE, 0xEF, 0xDE, 0xAD,
                             0x00, 0x00, 0x3F, 0x80 }, 15);
    CHECK(xfer(req, 15) == 8);
    CHECK(setpoint == 0xDEADBEEFu);
    CHECK(gain == 1.0f);
    mb.word_swap = false;

    /* byte count disagreeing with quantity */
    memcpy(req, (uint8_t[]){ 0x01, 0x10, 0x00, 0x02, 0x00, 0x02, 0x02,
                             0x00, 0x01 }, 9);
    EXPECT_EXC(xfer(req, 9), 0x10, 0x03);

    /* span with a hole */
    memcpy(req, (uint8_t[]){ 0x01, 0x10, 0x00, 0x0D, 0x00, 0x03, 0x06,
                             0x00, 0x01, 0x00, 0x02, 0x00, 0x03 }, 13);
    EXPECT_EXC(xfer(req, 13), 0x10, 0x02);

    /* partial failure: small (addr 10) accepts, pump (addr 11) vetoes;
     * exception 04, the first write stays applied */
    pump_blocked = true;
    small = 5;
    memcpy(req, (uint8_t[]){ 0x01, 0x10, 0x00, 0x0A, 0x00, 0x02, 0x04,
                             0x00, 0x4D, 0x00, 0x01 }, 11);
    EXPECT_EXC(xfer(req, 11), 0x10, 0x04);
    CHECK(small == 77);
    CHECK(pump == 0);
    pump_blocked = false;

    /* NaN refused into a FLOAT (raw path checks the bits) */
    memcpy(req, (uint8_t[]){ 0x01, 0x10, 0x00, 0x04, 0x00, 0x02, 0x04,
                             0x7F, 0xC0, 0x00, 0x00 }, 11);
    EXPECT_EXC(xfer(req, 11), 0x10, 0x04);
    CHECK(gain == 1.0f);
}

static void test_silence(void)
{
    uint8_t req[16];

    /* another slave's address */
    memcpy(req, (uint8_t[]){ 0x22, 0x03, 0x00, 0x01, 0x00, 0x01 }, 6);
    CHECK(xfer(req, 6) == 0);

    /* corrupt CRC */
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x01, 0x00, 0x01, 0xD5, 0xCB }, 8);
    CHECK(regmb_process(&mb, req, 8, resp, sizeof(resp)) == 0);

    /* runt frames */
    CHECK(regmb_process(&mb, req, 3, resp, sizeof(resp)) == 0);
    CHECK(regmb_process(&mb, req, 0, resp, sizeof(resp)) == 0);

    /* broadcast write: applied, never answered */
    memcpy(req, (uint8_t[]){ 0x00, 0x06, 0x00, 0x01, 0x03, 0xE8 }, 6);
    CHECK(xfer(req, 6) == 0);
    CHECK(interval == 1000);

    /* broadcast read: ignored (broadcasts are writes only) */
    memcpy(req, (uint8_t[]){ 0x00, 0x03, 0x00, 0x01, 0x00, 0x01 }, 6);
    CHECK(xfer(req, 6) == 0);

    /* broadcast of a refused write: still silent */
    memcpy(req, (uint8_t[]){ 0x00, 0x06, 0x00, 0x01, 0x00, 0x32 }, 6);
    CHECK(xfer(req, 6) == 0);
    CHECK(interval == 1000);
}

static void test_unsupported(void)
{
    uint8_t req[8];
    memcpy(req, (uint8_t[]){ 0x01, 0x01, 0x00, 0x00, 0x00, 0x01 }, 6);
    EXPECT_EXC(xfer(req, 6), 0x01, 0x01);
}

static void test_resp_cap(void)
{
    uint8_t req[16];
    uint8_t tiny[7];
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0x00, 0x01, 0x00, 0x05 }, 6);
    uint16_t crc = regmb_crc16(req, 6);
    req[6] = (uint8_t)crc; req[7] = (uint8_t)(crc >> 8);
    /* read response needs 15 bytes; 7 do not fit -> silence, not overflow */
    CHECK(regmb_process(&mb, req, 8, tiny, sizeof(tiny)) == 0);

    /* a write whose response cannot be delivered must not happen:
     * silence with the register unchanged, not a silent commit */
    uint16_t before = interval;
    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0x00, 0x01, 0x0B, 0xB8 }, 6);
    crc = regmb_crc16(req, 6);
    req[6] = (uint8_t)crc; req[7] = (uint8_t)(crc >> 8);
    CHECK(regmb_process(&mb, req, 8, tiny, sizeof(tiny)) == 0);
    CHECK(interval == before);

    memcpy(req, (uint8_t[]){ 0x01, 0x10, 0x00, 0x01, 0x00, 0x01, 0x02, 0x0B, 0xB8 }, 9);
    crc = regmb_crc16(req, 9);
    req[9] = (uint8_t)crc; req[10] = (uint8_t)(crc >> 8);
    CHECK(regmb_process(&mb, req, 11, tiny, sizeof(tiny)) == 0);
    CHECK(interval == before);
}

/* an entry mapped at the last word address: reads and writes must
 * terminate instead of wrapping the cursor past 0xFFFF */
static uint16_t top = 0x1234;
static void test_top_address(void)
{
    static const RegEntry reg2[] = {
        { .name = "top", .ptr = &top, .type = REG_U16, .perm = REG_RW,
          .modbus_addr = 0xFFFF },
        { .name = NULL }
    };
    RegTable t2; RegModbus m2;
    CHECK(reg_table_init(&t2, reg2) == REG_OK);
    CHECK(regmb_init(&m2, &t2, 1) == REG_OK);

    uint8_t req[16], out[REGMB_FRAME_MAX];
    memcpy(req, (uint8_t[]){ 0x01, 0x03, 0xFF, 0xFF, 0x00, 0x01 }, 6);
    uint16_t crc = regmb_crc16(req, 6);
    req[6] = (uint8_t)crc; req[7] = (uint8_t)(crc >> 8);
    uint16_t n = regmb_process(&m2, req, 8, out, sizeof(out));
    CHECK(n == 7);
    CHECK(out[2] == 2 && out[3] == 0x12 && out[4] == 0x34);

    memcpy(req, (uint8_t[]){ 0x01, 0x06, 0xFF, 0xFF, 0x56, 0x78 }, 6);
    crc = regmb_crc16(req, 6);
    req[6] = (uint8_t)crc; req[7] = (uint8_t)(crc >> 8);
    CHECK(regmb_process(&m2, req, 8, out, sizeof(out)) == 8);
    CHECK(top == 0x5678);

    memcpy(req, (uint8_t[]){ 0x01, 0x10, 0xFF, 0xFF, 0x00, 0x01, 0x02, 0x9A, 0xBC }, 9);
    crc = regmb_crc16(req, 9);
    req[9] = (uint8_t)crc; req[10] = (uint8_t)(crc >> 8);
    CHECK(regmb_process(&m2, req, 11, out, sizeof(out)) == 8);
    CHECK(top == 0x9ABC);
}

/* -- Modbus TCP: same PDU, MBAP envelope -------------------- */

static uint16_t tcp(const uint8_t *adu, uint16_t len)
{
    memset(resp, 0xAA, sizeof(resp));
    return regmb_process_tcp(&mb, adu, len, resp, sizeof(resp));
}

static void test_tcp(void)
{
    /* write interval = 1000: response echoes the whole ADU */
    static const uint8_t w[] = { 0xAB, 0xCD, 0x00, 0x00, 0x00, 0x06, 0xFF,
                                 0x06, 0x00, 0x01, 0x03, 0xE8 };
    uint16_t n = tcp(w, sizeof(w));
    CHECK(n == sizeof(w) && memcmp(resp, w, n) == 0);
    CHECK(interval == 1000);

    /* read it back: transaction and unit ids echoed, length computed */
    static const uint8_t r[] = { 0x01, 0x02, 0x00, 0x00, 0x00, 0x06, 0x11,
                                 0x03, 0x00, 0x01, 0x00, 0x01 };
    static const uint8_t want[] = { 0x01, 0x02, 0x00, 0x00, 0x00, 0x05, 0x11,
                                    0x03, 0x02, 0x03, 0xE8 };
    n = tcp(r, sizeof(r));
    CHECK(n == sizeof(want) && memcmp(resp, want, n) == 0);

    /* unit id 0 is also accepted and echoed (TCP guide remark) */
    static const uint8_t r0[] = { 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00,
                                  0x03, 0x00, 0x01, 0x00, 0x01 };
    n = tcp(r0, sizeof(r0));
    CHECK(n == 11 && resp[6] == 0x00 && resp[7] == 0x03);

    /* exceptions travel the same envelope */
    static const uint8_t rbad[] = { 0x00, 0x02, 0x00, 0x00, 0x00, 0x06, 0xFF,
                                    0x03, 0x00, 0x20, 0x00, 0x01 };
    static const uint8_t wantx[] = { 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0xFF,
                                     0x83, 0x02 };
    n = tcp(rbad, sizeof(rbad));
    CHECK(n == sizeof(wantx) && memcmp(resp, wantx, n) == 0);

    static const uint8_t wro[] = { 0x00, 0x03, 0x00, 0x00, 0x00, 0x06, 0xFF,
                                   0x06, 0x00, 0x10, 0x00, 0x07 };
    n = tcp(wro, sizeof(wro));
    CHECK(n == 9 && resp[7] == 0x86 && resp[8] == 0x04);
    CHECK(rom == 42);

    /* MBAP the guide says to discard: wrong protocol id, length
     * disagreeing with the data, runt ADU */
    static const uint8_t badpid[] = { 0x00, 0x04, 0x00, 0x01, 0x00, 0x06, 0xFF,
                                      0x03, 0x00, 0x01, 0x00, 0x01 };
    CHECK(tcp(badpid, sizeof(badpid)) == 0);
    static const uint8_t badlen[] = { 0x00, 0x05, 0x00, 0x00, 0x00, 0x09, 0xFF,
                                      0x03, 0x00, 0x01, 0x00, 0x01 };
    CHECK(tcp(badlen, sizeof(badlen)) == 0);
    CHECK(tcp(w, 7) == 0);

    /* a write whose response cannot be delivered must not happen */
    uint8_t tiny[11];
    static const uint8_t w2[] = { 0x00, 0x06, 0x00, 0x00, 0x00, 0x06, 0xFF,
                                  0x06, 0x00, 0x01, 0x01, 0xF4 };
    CHECK(regmb_process_tcp(&mb, w2, sizeof(w2), tiny, sizeof(tiny)) == 0);
    CHECK(interval == 1000);
}

/* -- main --------------------------------------------------- */

int main(void)
{
    CHECK(reg_table_init(&table, registry) == REG_OK);

    test_crc_reference();
    test_init();
    test_read_exact_bytes();
    test_read();
    test_read_errors();
    test_write_single();
    test_write_multiple();
    test_silence();
    test_unsupported();
    test_resp_cap();
    test_top_address();
    test_tcp();

    if (failures) {
        printf("%d of %d checks FAILED\n", failures, cases);
        return 1;
    }
    printf("all %d checks passed\n", cases);
    return 0;
}
