#include "regtable_modbus.h"
#include <stddef.h>

/* -- constants from the specs ----------------------------- */

#define FC_READ_HOLDING   0x03
#define FC_READ_INPUT     0x04
#define FC_WRITE_SINGLE   0x06
#define FC_WRITE_MULTIPLE 0x10

#define EXC_ILLEGAL_FUNCTION 0x01
#define EXC_ILLEGAL_ADDRESS  0x02
#define EXC_ILLEGAL_VALUE    0x03
#define EXC_DEVICE_FAILURE   0x04

#define MAX_READ_QTY  125   /* FC 03/04, app spec 6.3/6.4 */
#define MAX_WRITE_QTY 123   /* FC 16, app spec 6.12 */

/* -- CRC-16/MODBUS (serial line spec, appendix B) ---------- */

uint16_t regmb_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
            else         crc >>= 1;
        }
    }
    return crc;
}

/* -- word span of a mapped entry --------------------------- */

static uint16_t entry_words(const RegEntry *e)
{
    switch (e->type) {
    case REG_U32:
    case REG_I32:
    case REG_FLOAT:
        return 2;
    default:
        return 1;
    }
}

/*  The entry whose span starts exactly at word addr, or NULL. */
static const RegEntry *entry_at(const RegTable *t, uint16_t addr)
{
    for (const RegEntry *e = t->entries; e->name != NULL; e++) {
        if (e->modbus_addr != 0 && e->modbus_addr == addr) return e;
    }
    return NULL;
}

/*  Check that [start, start+qty) is exactly tiled by mapped
 *  entries: no gaps, no entry sticking out past either end. */
static bool span_covered(const RegTable *t, uint16_t start, uint16_t qty)
{
    uint32_t addr = start;
    uint32_t end  = (uint32_t)start + qty;
    while (addr < end) {
        const RegEntry *e = entry_at(t, (uint16_t)addr);
        if (!e) return false;
        addr += entry_words(e);
    }
    return addr == end;
}

/* -- raw <-> wire words ------------------------------------ */

static void raw_to_words(const RegModbus *mb, const RegEntry *e,
                         uint32_t raw, uint8_t *out)
{
    if (entry_words(e) == 1) {
        out[0] = (uint8_t)(raw >> 8);
        out[1] = (uint8_t)raw;
        return;
    }
    uint16_t hi = (uint16_t)(raw >> 16);
    uint16_t lo = (uint16_t)raw;
    uint16_t first  = mb->word_swap ? lo : hi;
    uint16_t second = mb->word_swap ? hi : lo;
    out[0] = (uint8_t)(first >> 8);
    out[1] = (uint8_t)first;
    out[2] = (uint8_t)(second >> 8);
    out[3] = (uint8_t)second;
}

static uint32_t words_to_raw(const RegModbus *mb, const RegEntry *e,
                             const uint8_t *in)
{
    uint16_t w0 = (uint16_t)((in[0] << 8) | in[1]);
    if (entry_words(e) == 1) {
        /* signed types travel as 16-bit two's complement; the raw
         * convention is sign-extension to 32 bits */
        if (e->type == REG_I8 || e->type == REG_I16) {
            return (uint32_t)(int32_t)(int16_t)w0;
        }
        return w0;
    }
    uint16_t w1 = (uint16_t)((in[2] << 8) | in[3]);
    uint16_t hi = mb->word_swap ? w1 : w0;
    uint16_t lo = mb->word_swap ? w0 : w1;
    return ((uint32_t)hi << 16) | lo;
}

/* -- response assembly ------------------------------------- */

static uint16_t finish(uint8_t *resp, uint16_t len, uint16_t cap)
{
    if ((uint16_t)(len + 2) > cap) return 0;
    uint16_t crc = regmb_crc16(resp, len);
    resp[len]     = (uint8_t)crc;          /* low byte first */
    resp[len + 1] = (uint8_t)(crc >> 8);
    return (uint16_t)(len + 2);
}

static uint16_t exception(uint8_t *resp, uint16_t cap,
                          uint8_t addr, uint8_t fc, uint8_t code)
{
    if (cap < 5) return 0;
    resp[0] = addr;
    resp[1] = (uint8_t)(fc | 0x80);
    resp[2] = code;
    return finish(resp, 3, cap);
}

/* -- function code handlers -------------------------------- */
/*  Each returns the response length, or fills in an exception.
 *  pdu points at the function code byte; plen counts from there
 *  to the end of the data (CRC already stripped). */

static uint16_t do_read(RegModbus *mb, const uint8_t *pdu, uint16_t plen,
                        uint8_t *resp, uint16_t cap)
{
    uint8_t fc = pdu[0];
    if (plen != 5) {
        return exception(resp, cap, mb->addr, fc, EXC_ILLEGAL_VALUE);
    }
    uint16_t start = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t qty   = (uint16_t)((pdu[3] << 8) | pdu[4]);

    if (qty < 1 || qty > MAX_READ_QTY) {
        return exception(resp, cap, mb->addr, fc, EXC_ILLEGAL_VALUE);
    }
    if (!span_covered(mb->table, start, qty)) {
        return exception(resp, cap, mb->addr, fc, EXC_ILLEGAL_ADDRESS);
    }
    if ((uint16_t)(3 + 2 * qty + 2) > cap) return 0;

    resp[0] = mb->addr;
    resp[1] = fc;
    resp[2] = (uint8_t)(2 * qty);
    uint32_t addr = start;          /* 32-bit: 0xFFFF + 1 must not wrap */
    uint8_t *out = resp + 3;
    while (addr < (uint32_t)start + qty) {
        const RegEntry *e = entry_at(mb->table, (uint16_t)addr);
        uint32_t raw = 0;
        reg_get_raw(e, &raw);              /* runs on_read */
        raw_to_words(mb, e, raw, out);
        out  += 2 * entry_words(e);
        addr += entry_words(e);
    }
    return finish(resp, (uint16_t)(3 + 2 * qty), cap);
}

static uint16_t do_write_single(RegModbus *mb, const uint8_t *pdu, uint16_t plen,
                                uint8_t *resp, uint16_t cap, bool broadcast)
{
    if (plen != 5) {
        return exception(resp, cap, mb->addr, pdu[0], EXC_ILLEGAL_VALUE);
    }
    uint16_t addr = (uint16_t)((pdu[1] << 8) | pdu[2]);

    const RegEntry *e = entry_at(mb->table, addr);
    if (!e || entry_words(e) != 1) {
        /* no entry here, or one word of a two-word value */
        return exception(resp, cap, mb->addr, pdu[0], EXC_ILLEGAL_ADDRESS);
    }
    /* the response must be deliverable before any state changes:
     * a write the master cannot hear about must not happen */
    if (!broadcast && cap < 8) return 0;

    uint32_t raw = words_to_raw(mb, e, pdu + 3);
    if (reg_set_raw(mb->table, e, raw) != REG_OK) {
        return exception(resp, cap, mb->addr, pdu[0], EXC_DEVICE_FAILURE);
    }
    if (broadcast) return 0;

    /* normal response echoes the request */
    resp[0] = mb->addr;
    for (int i = 0; i < 5; i++) resp[1 + i] = pdu[i];
    return finish(resp, 6, cap);
}

static uint16_t do_write_multiple(RegModbus *mb, const uint8_t *pdu, uint16_t plen,
                                  uint8_t *resp, uint16_t cap, bool broadcast)
{
    if (plen < 6) {
        return exception(resp, cap, mb->addr, pdu[0], EXC_ILLEGAL_VALUE);
    }
    uint16_t start = (uint16_t)((pdu[1] << 8) | pdu[2]);
    uint16_t qty   = (uint16_t)((pdu[3] << 8) | pdu[4]);
    uint8_t  bytes = pdu[5];

    if (qty < 1 || qty > MAX_WRITE_QTY || bytes != 2 * qty ||
        plen != (uint16_t)(6 + bytes)) {
        return exception(resp, cap, mb->addr, pdu[0], EXC_ILLEGAL_VALUE);
    }
    if (!span_covered(mb->table, start, qty)) {
        return exception(resp, cap, mb->addr, pdu[0], EXC_ILLEGAL_ADDRESS);
    }
    /* the response must be deliverable before any state changes:
     * a write the master cannot hear about must not happen */
    if (!broadcast && cap < 8) return 0;

    /* apply entry by entry; the first refused write stops the scan
     * and answers exception 04, values already stored stay stored */
    uint32_t addr = start;          /* 32-bit: 0xFFFF + 1 must not wrap */
    const uint8_t *in = pdu + 6;
    while (addr < (uint32_t)start + qty) {
        const RegEntry *e = entry_at(mb->table, (uint16_t)addr);
        uint32_t raw = words_to_raw(mb, e, in);
        if (reg_set_raw(mb->table, e, raw) != REG_OK) {
            return exception(resp, cap, mb->addr, pdu[0], EXC_DEVICE_FAILURE);
        }
        in   += 2 * entry_words(e);
        addr += entry_words(e);
    }
    if (broadcast) return 0;

    if (cap < 8) return 0;
    resp[0] = mb->addr;
    resp[1] = pdu[0];
    resp[2] = pdu[1];
    resp[3] = pdu[2];
    resp[4] = pdu[3];
    resp[5] = pdu[4];
    return finish(resp, 6, cap);
}

/* -- public API -------------------------------------------- */

RegResult regmb_init(RegModbus *mb, RegTable *table, uint8_t addr)
{
    if (addr < 1 || addr > 247) return REG_ERR_TABLE;
    mb->table = table;
    mb->addr = addr;
    mb->word_swap = false;

    /* mapped spans must fit the address space and not overlap */
    for (const RegEntry *a = table->entries; a->name != NULL; a++) {
        if (a->modbus_addr == 0) continue;
        uint32_t a0 = a->modbus_addr;
        uint32_t a1 = a0 + entry_words(a);        /* exclusive */
        if (a1 > 0x10000) return REG_ERR_TABLE;
        for (const RegEntry *b = a + 1; b->name != NULL; b++) {
            if (b->modbus_addr == 0) continue;
            uint32_t b0 = b->modbus_addr;
            uint32_t b1 = b0 + entry_words(b);
            if (a0 < b1 && b0 < a1) return REG_ERR_TABLE;
        }
    }
    return REG_OK;
}

uint16_t regmb_process(RegModbus *mb, const uint8_t *frame, uint16_t len,
                       uint8_t *resp, uint16_t resp_cap)
{
    /* smallest frame: addr + fc + crc */
    if (len < 4 || len > REGMB_FRAME_MAX) return 0;

    uint16_t crc = regmb_crc16(frame, (uint16_t)(len - 2));
    if (frame[len - 2] != (uint8_t)crc ||
        frame[len - 1] != (uint8_t)(crc >> 8)) {
        return 0;                          /* corrupt frame: stay silent */
    }

    bool broadcast = (frame[0] == 0);
    if (!broadcast && frame[0] != mb->addr) return 0;

    const uint8_t *pdu = frame + 1;
    uint16_t plen = (uint16_t)(len - 3);   /* fc + data, without crc */
    uint8_t fc = pdu[0];

    switch (fc) {
    case FC_READ_HOLDING:
    case FC_READ_INPUT:
        if (broadcast) return 0;           /* broadcasts are writes only */
        return do_read(mb, pdu, plen, resp, resp_cap);
    case FC_WRITE_SINGLE: {
        /* a broadcast is never answered, not even with an exception */
        uint16_t n = do_write_single(mb, pdu, plen, resp, resp_cap, broadcast);
        return broadcast ? 0 : n;
    }
    case FC_WRITE_MULTIPLE: {
        uint16_t n = do_write_multiple(mb, pdu, plen, resp, resp_cap, broadcast);
        return broadcast ? 0 : n;
    }
    default:
        if (broadcast) return 0;
        return exception(resp, resp_cap, mb->addr, fc, EXC_ILLEGAL_FUNCTION);
    }
}
