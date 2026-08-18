#ifndef REGTABLE_CORE_H
#define REGTABLE_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* -- configuration --------------------------------------- */
#ifndef REGTABLE_MAX_ENTRIES
#define REGTABLE_MAX_ENTRIES 64     /* sizes the dirty bitmap */
#endif

/* -- type enum ------------------------------------------- */
typedef enum RegType {
    REG_U8,
    REG_U16,
    REG_U32,
    REG_I8,
    REG_I16,
    REG_I32,
    REG_FLOAT,
    REG_BOOL
} RegType;

/* -- permission enum ------------------------------------- */
typedef enum RegPerm {
    REG_RO,
    REG_RW
} RegPerm;

/* -- result codes ---------------------------------------- */
typedef enum RegResult {
    REG_OK,
    REG_ERR_NOT_FOUND,
    REG_ERR_READONLY,
    REG_ERR_TYPE,
    REG_ERR_RANGE,
    REG_ERR_REJECTED,
    REG_ERR_TABLE           /* table too large or malformed */
} RegResult;

/* -- range limit ----------------------------------------- */
/*  U8/U16/U32 use .u, I8/I16/I32 use .i, FLOAT uses .f.
 *  Both zero = no range check. */
typedef union RegLimit {
    int32_t  i;
    uint32_t u;
    float    f;
} RegLimit;

/* -- register entry -------------------------------------- */
/*  "raw" convention (on_write, reg_set_raw, reg_get_raw):
 *  U8/U16/U32/BOOL: the value, zero-extended to 32 bits.
 *  I8/I16/I32:      the value, sign-extended to 32 bits.
 *  FLOAT:           the IEEE-754 bit pattern (memcpy).
 */
typedef struct RegEntry {
    const char *name;           /* CLI / MCP identifier          */
    volatile void *ptr;         /* -> user variable or HW reg     */
    RegType     type;
    RegPerm     perm;
    uint16_t    modbus_addr;    /* Modbus holding register addr  */
    RegLimit    min;            /* .min.u / .min.i / .min.f      */
    RegLimit    max;

    /* Hooks. NULL = no hook.
     *
     * on_write: command check. "Is this command allowed right now?"
     *   Synchronous; runs after perm and range checks, before the
     *   value is stored. Return false to refuse: nothing is stored
     *   and the caller gets REG_ERR_REJECTED.
     *
     * on_read: computed value. "Where does this value come from?"
     *   Synchronous; runs before the value is fetched. Fill *ptr
     *   from the real source (sample the ADC, convert to volts) so
     *   the caller sees engineering units, not the hardware.
     *
     * on_change: telemetry. "Who needs to know it changed?"
     *   Deferred; runs from reg_poll(), in the poller's context, for
     *   every entry whose dirty bit is set. The value is already
     *   stored: publish it, log it, refresh a display.
     */
    bool      (*on_write)(const struct RegEntry *entry, uint32_t raw);
    void      (*on_read)(const struct RegEntry *entry);
    void      (*on_change)(const struct RegEntry *entry);

    const char *description;    /* human / AI readable           */
} RegEntry;

/* -- table handle ---------------------------------------- */
/*  Runtime side of a register table: the entries (in flash) plus
 *  the dirty bitmap (in RAM) that on_change is driven from. */
typedef struct RegTable {
    const RegEntry *entries;    /* NULL-terminated               */
    uint16_t        count;      /* set by reg_table_init          */
    uint32_t        dirty[(REGTABLE_MAX_ENTRIES + 31) / 32];
} RegTable;

/* -- transport abstraction ------------------------------- */
typedef struct RegTransport {
    int  (*read)(uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    int  (*write)(const uint8_t *buf, uint16_t len);
} RegTransport;

/* -- core API -------------------------------------------- */

/*  Bind a table handle to a NULL-terminated entry array.
 *  Counts entries and clears the dirty bitmap.
 *  REG_ERR_TABLE if there are more than REGTABLE_MAX_ENTRIES. */
RegResult reg_table_init(RegTable *t, const RegEntry *entries);

/*  Find entry by name. Returns NULL if not found. */
const RegEntry *reg_find(const RegTable *t, const char *name);

/*  Typed core path. Every protocol adapter goes through here;
 *  perm, range, and on_write all happen in reg_set_raw.
 *
 *  reg_set_raw: validate and commit a raw value. If the value
 *    changed, the entry is marked dirty in t (t may be NULL to
 *    skip change tracking).
 *  reg_get_raw: run on_read, fetch the current value as raw. */
RegResult reg_set_raw(RegTable *t, const RegEntry *entry, uint32_t raw);
RegResult reg_get_raw(const RegEntry *entry, uint32_t *raw_out);

/*  Change tracking: one dirty bitmap, two ways in, one way out.
 *
 *  In:  reg_set_raw sets the bit when a write (from any adapter)
 *       changes the value. reg_mark_dirty sets it when your own
 *       C code changed *ptr directly (temp = read_sensor()).
 *  Out: reg_poll walks the bitmap in table order, clears each bit,
 *       runs that entry's on_change. It does not know or care who
 *       set the bit.
 *
 *  Call reg_poll from your main loop, at whatever cadence you
 *  choose. Returns the number of hooks run.
 *
 *  Both sides touch the bitmap with plain read-modify-write, so
 *  they belong in the same context: call reg_mark_dirty from the
 *  main loop too. An ISR that produces a value should set its own
 *  flag and let the main loop call reg_mark_dirty (README,
 *  Concurrency). */
void     reg_mark_dirty(RegTable *t, const RegEntry *entry);
uint16_t reg_poll(RegTable *t);

/*  String layer over the raw path (what the CLI uses).
 *  reg_get_str: format the current value into buf.
 *  Returns chars written (excl NUL), or -1 on error. */
int reg_get_str(const RegEntry *entry, char *buf, uint16_t buf_size);

/*  reg_set_str: parse value_str per entry->type, then reg_set_raw. */
RegResult reg_set_str(RegTable *t, const RegEntry *entry, const char *value_str);

/*  Human-readable result code. */
const char *reg_result_str(RegResult r);

/*  Human-readable type name. */
const char *reg_type_str(RegType t);

#endif /* REGTABLE_CORE_H */
