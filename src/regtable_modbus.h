#ifndef REGTABLE_MODBUS_H
#define REGTABLE_MODBUS_H

#include "regtable_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Modbus RTU slave adapter: one complete frame in, one complete
 * frame out. The library does no I/O and runs no timers; frame
 * boundaries (the 3.5-character silence from the serial line spec)
 * are detected by platform code, which then hands the whole frame
 * to regmb_process.
 *
 * Register map: an entry with modbus_addr != 0 occupies one word
 * (U8/I8/U16/I16/BOOL) or two words (U32/I32/FLOAT) starting at
 * that word address. modbus_addr 0 = not in the Modbus map, so the
 * map starts at word address 1. Holding and input register spaces
 * overlay: function codes 03 and 04 read the same map.
 *
 * Wire format per the application protocol spec: within a word the
 * high byte is first; for two-word values the word order is a
 * device-wide setting (word_swap below). A multi-word value is read
 * or written whole: a request that covers only half of it is
 * answered with exception 02 (ILLEGAL DATA ADDRESS, "the
 * combination of reference number and transfer length is invalid").
 *
 * Function codes: 03 (read holding), 04 (read input), 06 (write
 * single), 16 (write multiple). Anything else: exception 01.
 * A write the core refuses (read-only, out of range, on_write veto,
 * NaN) is answered with exception 04, per the write state diagrams
 * in the spec (value semantics are the application's, not the
 * protocol's). */

#define REGMB_FRAME_MAX 256    /* addr + fc + data + crc, serial line spec */

typedef struct RegModbus {
    RegTable *table;
    uint8_t   addr;         /* slave address, 1..247 */
    bool      word_swap;    /* two-word values: false = high word first
                               (ABCD), true = low word first (CDAB) */
} RegModbus;

/*  Bind the adapter to a table and slave address.
 *  Scans the mapped entries (modbus_addr != 0): overlapping word
 *  spans or a two-word entry at the top of the address space return
 *  REG_ERR_TABLE. word_swap starts false; set it after init if the
 *  master expects CDAB. */
RegResult regmb_init(RegModbus *mb, RegTable *table, uint8_t addr);

/*  Handle one received RTU frame (address + PDU + CRC, as captured
 *  between two idle gaps). Writes the response frame into resp and
 *  returns its length. Returns 0 when the slave must stay silent:
 *  frame shorter than 4 bytes, CRC mismatch, another slave's
 *  address, a broadcast (writes are applied, nothing is sent), or
 *  resp_cap too small for the response. */
uint16_t regmb_process(RegModbus *mb, const uint8_t *frame, uint16_t len,
                       uint8_t *resp, uint16_t resp_cap);

/*  CRC-16/MODBUS (init 0xFFFF, polynomial 0xA001), as specified in
 *  the serial line spec appendix. Exposed for master-side code and
 *  tests building their own frames. */
uint16_t regmb_crc16(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* REGTABLE_MODBUS_H */
