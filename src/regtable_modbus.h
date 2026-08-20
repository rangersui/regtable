#ifndef REGTABLE_MODBUS_H
#define REGTABLE_MODBUS_H

#include "regtable_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Modbus slave adapter: one complete frame in, one complete frame
 * out. RTU (regmb_process) and TCP (regmb_process_tcp) share the
 * same PDU handling and register map; only the envelope differs.
 * The library does no I/O and runs no timers; frame boundaries are
 * found by platform code: for RTU the 3.5-character silence from
 * the serial line spec, for TCP the length field in the MBAP
 * header read off the stream.
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

#define REGMB_FRAME_MAX   256  /* RTU: addr + fc + data + crc, serial line spec */
#define REGMB_TCP_ADU_MAX 260  /* TCP: MBAP(7) + fc + data, messaging guide */

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

/*  Handle one Modbus TCP ADU (MBAP header + PDU, as read off the
 *  stream: 7 header bytes, then the length the header announces).
 *  The PDU handling and the register map are the same as RTU; only
 *  the envelope differs: no CRC, no broadcast, transaction and unit
 *  identifiers echoed back. Returns the response length, or 0 for
 *  an ADU the messaging guide says to discard (protocol id not 0,
 *  length field disagreeing with the data) or a response that does
 *  not fit resp_cap. */
uint16_t regmb_process_tcp(RegModbus *mb, const uint8_t *adu, uint16_t len,
                           uint8_t *resp, uint16_t resp_cap);

/*  CRC-16/MODBUS (init 0xFFFF, polynomial 0xA001), as specified in
 *  the serial line spec appendix. Exposed for master-side code and
 *  tests building their own frames. */
uint16_t regmb_crc16(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* REGTABLE_MODBUS_H */
