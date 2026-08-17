#include "regtable_core.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

/* ── helpers ───────────────────────────────────────────── */

const char *reg_type_str(RegType t)
{
    switch (t) {
    case REG_U8:    return "U8";
    case REG_U16:   return "U16";
    case REG_U32:   return "U32";
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
    }
    return "ERR: unknown";
}

static bool has_range(const RegEntry *entry)
{
    return !(entry->min == 0 && entry->max == 0);
}

/* ── find ──────────────────────────────────────────────── */

const RegEntry *reg_find(const RegEntry *table, const char *name)
{
    for (const RegEntry *e = table; e->name != NULL; e++) {
        if (strcmp(e->name, name) == 0) {
            return e;
        }
    }
    return NULL;
}

/* ── get (read value → formatted string) ───────────────── */

int reg_get_str(const RegEntry *entry, char *buf, uint16_t buf_size)
{
    if (entry->on_read) {
        entry->on_read(entry);
    }

    switch (entry->type) {
    case REG_U8:
        return snprintf(buf, buf_size, "%u", (unsigned)*(uint8_t *)entry->ptr);
    case REG_U16:
        return snprintf(buf, buf_size, "%u", (unsigned)*(uint16_t *)entry->ptr);
    case REG_U32:
        return snprintf(buf, buf_size, "%lu", (unsigned long)*(uint32_t *)entry->ptr);
    case REG_FLOAT:
        return snprintf(buf, buf_size, "%.2f", (double)*(float *)entry->ptr);
    case REG_BOOL:
        return snprintf(buf, buf_size, "%s",
                        *(uint8_t *)entry->ptr ? "true" : "false");
    }
    return -1;
}

/* ── raw path: validation + commit (shared by all adapters) ── */

RegResult reg_set_raw(const RegEntry *entry, uint32_t raw)
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
    case REG_BOOL:
        raw = raw ? 1 : 0;
        break;
    default:
        break;
    }

    /* user range check */
    if (has_range(entry)) {
        if (entry->type == REG_FLOAT) {
            float f;
            memcpy(&f, &raw, 4);
            if (f < (float)entry->min || f > (float)entry->max) {
                return REG_ERR_RANGE;
            }
        } else if (entry->type != REG_BOOL) {
            /* compare in 64-bit so U32 values above INT32_MAX
             * are rejected instead of misread as negative */
            int64_t v = (int64_t)raw;
            if (v < entry->min || v > entry->max) {
                return REG_ERR_RANGE;
            }
        }
    }

    if (entry->on_write) {
        if (!entry->on_write(entry, raw)) return REG_ERR_REJECTED;
    }

    switch (entry->type) {
    case REG_U8:   *(uint8_t  *)entry->ptr = (uint8_t)raw;   break;
    case REG_U16:  *(uint16_t *)entry->ptr = (uint16_t)raw;  break;
    case REG_U32:  *(uint32_t *)entry->ptr = raw;            break;
    case REG_BOOL: *(uint8_t  *)entry->ptr = (uint8_t)raw;   break;
    case REG_FLOAT: memcpy(entry->ptr, &raw, 4);             break;
    }

    return REG_OK;
}

RegResult reg_get_raw(const RegEntry *entry, uint32_t *raw_out)
{
    if (entry->on_read) {
        entry->on_read(entry);
    }

    switch (entry->type) {
    case REG_U8:   *raw_out = *(uint8_t  *)entry->ptr; return REG_OK;
    case REG_U16:  *raw_out = *(uint16_t *)entry->ptr; return REG_OK;
    case REG_U32:  *raw_out = *(uint32_t *)entry->ptr; return REG_OK;
    case REG_BOOL: *raw_out = *(uint8_t  *)entry->ptr ? 1 : 0; return REG_OK;
    case REG_FLOAT: memcpy(raw_out, entry->ptr, 4);    return REG_OK;
    }
    return REG_ERR_TYPE;
}

/* ── string layer: parse only, then delegate to raw path ── */

static RegResult parse_unsigned(const char *s, uint32_t *out)
{
    /* strtoul silently accepts "-1" by wrapping — reject the
     * sign up front so unsigned registers never see it */
    const char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '-') return REG_ERR_TYPE;

    char *end;
    errno = 0;
    unsigned long ulval = strtoul(s, &end, 0);
    if (end == s || *end != '\0') return REG_ERR_TYPE;
    if (errno == ERANGE) return REG_ERR_RANGE;
#if ULONG_MAX > 0xFFFFFFFFUL
    if (ulval > 0xFFFFFFFFUL) return REG_ERR_RANGE;
#endif
    *out = (uint32_t)ulval;
    return REG_OK;
}

RegResult reg_set_str(const RegEntry *entry, const char *value_str)
{
    uint32_t raw = 0;

    switch (entry->type) {
    case REG_BOOL:
        if (strcmp(value_str, "true") == 0 || strcmp(value_str, "1") == 0) {
            raw = 1;
        } else if (strcmp(value_str, "false") == 0 || strcmp(value_str, "0") == 0) {
            raw = 0;
        } else {
            return REG_ERR_TYPE;
        }
        break;

    case REG_U8:
    case REG_U16:
    case REG_U32: {
        RegResult r = parse_unsigned(value_str, &raw);
        if (r != REG_OK) return r;
        break;
    }

    case REG_FLOAT: {
        char *end;
        float fval = (float)strtod(value_str, &end);
        if (end == value_str || *end != '\0') return REG_ERR_TYPE;
        memcpy(&raw, &fval, 4);
        break;
    }
    }

    return reg_set_raw(entry, raw);
}
