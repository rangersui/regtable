#ifndef REGTABLE_CORE_H
#define REGTABLE_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* ── type enum ─────────────────────────────────────────── */
typedef enum RegType {
    REG_U8,
    REG_U16,
    REG_U32,
    REG_FLOAT,
    REG_BOOL
} RegType;

/* ── permission enum ───────────────────────────────────── */
typedef enum RegPerm {
    REG_RO,
    REG_RW
} RegPerm;

/* ── result codes ──────────────────────────────────────── */
typedef enum RegResult {
    REG_OK,
    REG_ERR_NOT_FOUND,
    REG_ERR_READONLY,
    REG_ERR_TYPE,
    REG_ERR_RANGE,
    REG_ERR_REJECTED
} RegResult;

/* ── register entry ────────────────────────────────────── */
/*  "raw" convention (on_write, reg_set_raw, reg_get_raw):
 *  U8/U16/U32/BOOL — the numeric value, zero-extended to 32 bits.
 *  FLOAT           — the IEEE-754 bit pattern (memcpy).
 *
 *  min/max are int32_t: U32 ranges reach INT32_MAX, FLOAT ranges
 *  are whole numbers.
 */
typedef struct RegEntry {
    const char *name;           /* CLI / MCP identifier          */
    void       *ptr;            /* → user variable or HW reg     */
    RegType     type;
    RegPerm     perm;
    uint16_t    modbus_addr;    /* Modbus holding register addr  */
    int32_t     min;            /* range min  (min==max==0: no range) */
    int32_t     max;            /* range max                     */

    /* Hooks — side-effect points on the access path.
     * The core moves data; hooks are where the outside world gets
     * touched. NULL = no hook.
     *
     * on_write: pre-commit hook. Runs after perm/range checks,
     *   before the value is stored. Apply the value to hardware,
     *   notify other modules. Return false to veto: nothing is
     *   stored and the caller gets REG_ERR_REJECTED.
     *
     * on_read: refresh hook. Runs before the value is fetched.
     *   Update *ptr from the real source (trigger an ADC sample,
     *   read a HW register) so the caller sees the current value.
     */
    bool      (*on_write)(const struct RegEntry *entry, uint32_t raw);
    void      (*on_read)(const struct RegEntry *entry);

    const char *description;    /* human / AI readable           */
} RegEntry;

/* ── transport abstraction ─────────────────────────────── */
typedef struct RegTransport {
    int  (*read)(uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    int  (*write)(const uint8_t *buf, uint16_t len);
} RegTransport;

/* ── core API ──────────────────────────────────────────── */

/*  Find entry by name. Returns NULL if not found. */
const RegEntry *reg_find(const RegEntry *table, const char *name);

/*  Typed core path. Every protocol adapter goes through here;
 *  perm, range, and on_write all happen in reg_set_raw.
 *
 *  reg_set_raw: validate and commit a raw value.
 *  reg_get_raw: run on_read, fetch the current value as raw. */
RegResult reg_set_raw(const RegEntry *entry, uint32_t raw);
RegResult reg_get_raw(const RegEntry *entry, uint32_t *raw_out);

/*  String layer over the raw path (what the CLI uses).
 *  reg_get_str: format the current value into buf.
 *  Returns chars written (excl NUL), or -1 on error. */
int reg_get_str(const RegEntry *entry, char *buf, uint16_t buf_size);

/*  reg_set_str: parse value_str per entry->type, then reg_set_raw. */
RegResult reg_set_str(const RegEntry *entry, const char *value_str);

/*  Human-readable result code. */
const char *reg_result_str(RegResult r);

/*  Human-readable type name. */
const char *reg_type_str(RegType t);

#endif /* REGTABLE_CORE_H */
