#include "regtable_core.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <ctype.h>

/* -- helpers --------------------------------------------- */

const char *reg_type_str(RegType t)
{
    switch (t) {
    case REG_U8:    return "U8";
    case REG_U16:   return "U16";
    case REG_U32:   return "U32";
    case REG_I8:    return "I8";
    case REG_I16:   return "I16";
    case REG_I32:   return "I32";
    case REG_FLOAT: return "FLOAT";
    case REG_BOOL:  return "BOOL";
    }
    return "?";
}

const char *reg_result_str(RegResult r)
{
    switch (r) {
    case REG_OK:            return "OK";
    case REG_ERR_NOT_FOUND: return "ERR: not found";
    case REG_ERR_READONLY:  return "ERR: read-only";
    case REG_ERR_TYPE:      return "ERR: invalid value";
    case REG_ERR_RANGE:     return "ERR: out of range";
    case REG_ERR_REJECTED:  return "ERR: rejected";
    case REG_ERR_TABLE:     return "ERR: bad table";
    }
    return "ERR: unknown";
}

static bool has_range(const RegEntry *entry)
{
    return !(entry->min.u == 0 && entry->max.u == 0);
}

static bool is_signed(RegType t)
{
    return t == REG_I8 || t == REG_I16 || t == REG_I32;
}

/* Current value of *ptr as raw, no hooks.
 * Accesses go through volatile so an entry may point straight at a
 * hardware register (GPIO ODR, ADC DR): every read really reads,
 * every write really writes. */
static uint32_t load_raw(const RegEntry *entry)
{
    uint32_t raw = 0;
    switch (entry->type) {
    case REG_U8:   raw = *(volatile uint8_t  *)entry->ptr; break;
    case REG_U16:  raw = *(volatile uint16_t *)entry->ptr; break;
    case REG_U32:  raw = *(volatile uint32_t *)entry->ptr; break;
    case REG_I8:   raw = (uint32_t)(int32_t)*(volatile int8_t  *)entry->ptr; break;
    case REG_I16:  raw = (uint32_t)(int32_t)*(volatile int16_t *)entry->ptr; break;
    case REG_I32:  raw = (uint32_t)*(volatile int32_t *)entry->ptr; break;
    case REG_BOOL: raw = *(volatile uint8_t  *)entry->ptr ? 1 : 0; break;
    case REG_FLOAT: {
        float f = *(volatile float *)entry->ptr;
        memcpy(&raw, &f, 4);
        break;
    }
    }
    return raw;
}

static void store_raw(const RegEntry *entry, uint32_t raw)
{
    switch (entry->type) {
    case REG_U8:   *(volatile uint8_t  *)entry->ptr = (uint8_t)raw;   break;
    case REG_U16:  *(volatile uint16_t *)entry->ptr = (uint16_t)raw;  break;
    case REG_U32:  *(volatile uint32_t *)entry->ptr = raw;            break;
    case REG_I8:   *(volatile int8_t   *)entry->ptr = (int8_t)(int32_t)raw;  break;
    case REG_I16:  *(volatile int16_t  *)entry->ptr = (int16_t)(int32_t)raw; break;
    case REG_I32:  *(volatile int32_t  *)entry->ptr = (int32_t)raw;   break;
    case REG_BOOL: *(volatile uint8_t  *)entry->ptr = (uint8_t)raw;   break;
    case REG_FLOAT: {
        float f;
        memcpy(&f, &raw, 4);
        *(volatile float *)entry->ptr = f;
        break;
    }
    }
}

/* -- table ----------------------------------------------- */

RegResult reg_table_init(RegTable *t, const RegEntry *entries)
{
    uint16_t n = 0;
    for (const RegEntry *e = entries; e->name != NULL; e++) {
        n++;
        if (n > REGTABLE_MAX_ENTRIES) {
            t->entries = NULL;
            t->count   = 0;
            return REG_ERR_TABLE;
        }
    }
    t->entries = entries;
    t->count   = n;
    memset(t->dirty, 0, sizeof(t->dirty));
    return REG_OK;
}

const RegEntry *reg_find(const RegTable *t, const char *name)
{
    for (const RegEntry *e = t->entries; e->name != NULL; e++) {
        if (strcmp(e->name, name) == 0) {
            return e;
        }
    }
    return NULL;
}

/* -- change tracking ------------------------------------- */

void reg_mark_dirty(RegTable *t, const RegEntry *entry)
{
    if (!t || !t->entries) return;

    /* compare as integers: relational compare of pointers into
     * different objects is undefined in C */
    uintptr_t lo = (uintptr_t)t->entries;
    uintptr_t hi = (uintptr_t)(t->entries + t->count);
    uintptr_t p  = (uintptr_t)entry;
    if (p < lo || p >= hi) return;

    uint16_t idx = (uint16_t)(entry - t->entries);
    t->dirty[idx / 32] |= (uint32_t)1 << (idx % 32);
}

uint16_t reg_poll(RegTable *t)
{
    uint16_t fired = 0;
    if (!t || !t->entries) return 0;

    for (uint16_t idx = 0; idx < t->count; idx++) {
        uint32_t bit = (uint32_t)1 << (idx % 32);
        if (!(t->dirty[idx / 32] & bit)) continue;

        t->dirty[idx / 32] &= ~bit;         /* clear first, then run */

        const RegEntry *e = &t->entries[idx];
        if (e->on_change) {
            e->on_change(e);
            fired++;
        }
    }
    return fired;
}

/* -- raw path: validation + commit (shared by all adapters) -- */

RegResult reg_set_raw(RegTable *t, const RegEntry *entry, uint32_t raw)
{
    if (entry->perm == REG_RO) {
        return REG_ERR_READONLY;
    }

    /* type-domain check */
    switch (entry->type) {
    case REG_U8:
        if (raw > 0xFF) return REG_ERR_RANGE;
        break;
    case REG_U16:
        if (raw > 0xFFFF) return REG_ERR_RANGE;
        break;
    case REG_I8:
        if ((int32_t)raw < INT8_MIN || (int32_t)raw > INT8_MAX) return REG_ERR_RANGE;
        break;
    case REG_I16:
        if ((int32_t)raw < INT16_MIN || (int32_t)raw > INT16_MAX) return REG_ERR_RANGE;
        break;
    case REG_BOOL:
        raw = raw ? 1 : 0;
        break;
    case REG_FLOAT: {
        /* NaN compares false against everything, so it would slip
         * through the range check; Inf is never a sane setpoint */
        float f;
        memcpy(&f, &raw, 4);
        if (isnan(f) || isinf(f)) return REG_ERR_TYPE;
        break;
    }
    default:
        break;
    }

    /* user range check, in the type's own domain */
    if (has_range(entry)) {
        if (entry->type == REG_FLOAT) {
            float f;
            memcpy(&f, &raw, 4);
            if (f < entry->min.f || f > entry->max.f) return REG_ERR_RANGE;
        } else if (is_signed(entry->type)) {
            int32_t v = (int32_t)raw;
            if (v < entry->min.i || v > entry->max.i) return REG_ERR_RANGE;
        } else if (entry->type != REG_BOOL) {
            if (raw < entry->min.u || raw > entry->max.u) return REG_ERR_RANGE;
        }
    }

    if (entry->on_write) {
        if (!entry->on_write(entry, raw)) return REG_ERR_REJECTED;
    }

    uint32_t old = load_raw(entry);
    store_raw(entry, raw);
    if (old != raw) {
        reg_mark_dirty(t, entry);
    }

    return REG_OK;
}

RegResult reg_get_raw(const RegEntry *entry, uint32_t *raw_out)
{
    if (entry->on_read) {
        entry->on_read(entry);
    }
    *raw_out = load_raw(entry);
    return REG_OK;
}

/* -- string layer: parse / format, then delegate to raw path -- */

int reg_get_str(const RegEntry *entry, char *buf, uint16_t buf_size)
{
    uint32_t raw;
    reg_get_raw(entry, &raw);

    switch (entry->type) {
    case REG_U8:
    case REG_U16:
    case REG_U32:
        return snprintf(buf, buf_size, "%lu", (unsigned long)raw);
    case REG_I8:
    case REG_I16:
    case REG_I32:
        return snprintf(buf, buf_size, "%ld", (long)(int32_t)raw);
    case REG_FLOAT: {
        float f;
        memcpy(&f, &raw, 4);
        /* %g keeps the length bounded (~13 chars at most), so a large
         * value shows as 3e+38 instead of being cut mid-digits */
        return snprintf(buf, buf_size, "%.6g", (double)f);
    }
    case REG_BOOL:
        return snprintf(buf, buf_size, "%s", raw ? "true" : "false");
    }
    return -1;
}

/* case-insensitive equality, for true/false */
static bool str_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
    }
    return *a == '\0' && *b == '\0';
}

/*  Integer text is decimal, or hex with a 0x/0X prefix. Nothing
 *  else: strtol's base 0 would also read "010" as octal 8, which
 *  is not what anyone typing into a terminal means. */
static int int_base(const char *s, const char **digits)
{
    if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
        *digits = s + 2;
        return 16;
    }
    *digits = s;
    return 10;
}

static RegResult parse_unsigned(const char *s, uint32_t *out)
{
    /* strtoul accepts "-1" by wrapping; reject the sign up front.
     * Skip the same whitespace strtoul skips (isspace), so a sign
     * hiding behind \v or \f is caught too. */
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-') return REG_ERR_TYPE;
    if (*p == '+') p++;

    const char *digits;
    int base = int_base(p, &digits);
    /* first char must be a digit: strtoul would otherwise skip
     * whitespace or a sign hiding after the 0x prefix */
    if (!isxdigit((unsigned char)*digits)) return REG_ERR_TYPE;

    char *end;
    errno = 0;
    unsigned long v = strtoul(digits, &end, base);
    if (*end != '\0') return REG_ERR_TYPE;
    if (errno == ERANGE) return REG_ERR_RANGE;
#if ULONG_MAX > 0xFFFFFFFFUL
    if (v > 0xFFFFFFFFUL) return REG_ERR_RANGE;
#endif
    *out = (uint32_t)v;
    return REG_OK;
}

static RegResult parse_signed(const char *s, uint32_t *out)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    bool neg = false;
    if (*p == '-' || *p == '+') { neg = (*p == '-'); p++; }

    const char *digits;
    int base = int_base(p, &digits);
    if (!isxdigit((unsigned char)*digits)) return REG_ERR_TYPE;

    /* magnitude first, so "-2147483648" fits without overflow */
    char *end;
    errno = 0;
    unsigned long mag = strtoul(digits, &end, base);
    if (*end != '\0') return REG_ERR_TYPE;
    if (errno == ERANGE) return REG_ERR_RANGE;
    if (neg) {
        if (mag > 0x80000000UL) return REG_ERR_RANGE;
        *out = (uint32_t)(0u - (uint32_t)mag);
    } else {
        if (mag > 0x7FFFFFFFUL) return REG_ERR_RANGE;
        *out = (uint32_t)mag;
    }
    return REG_OK;
}

RegResult reg_set_str(RegTable *t, const RegEntry *entry, const char *value_str)
{
    uint32_t raw = 0;
    RegResult r;

    switch (entry->type) {
    case REG_BOOL:
        if (str_ieq(value_str, "true") || strcmp(value_str, "1") == 0) {
            raw = 1;
        } else if (str_ieq(value_str, "false") || strcmp(value_str, "0") == 0) {
            raw = 0;
        } else {
            return REG_ERR_TYPE;
        }
        break;

    case REG_U8:
    case REG_U16:
    case REG_U32:
        r = parse_unsigned(value_str, &raw);
        if (r != REG_OK) return r;
        break;

    case REG_I8:
    case REG_I16:
    case REG_I32:
        r = parse_signed(value_str, &raw);
        if (r != REG_OK) return r;
        break;

    case REG_FLOAT: {
        char *end;
        double d = strtod(value_str, &end);
        if (end == value_str || *end != '\0') return REG_ERR_TYPE;
        if (isnan(d)) return REG_ERR_TYPE;         /* "nan" is not a value */
        float f = (float)d;
        if (isinf(f)) return REG_ERR_RANGE;        /* "inf", or 1e40: too big for float */
        memcpy(&raw, &f, 4);
        break;
    }
    }

    return reg_set_raw(t, entry, raw);
}
