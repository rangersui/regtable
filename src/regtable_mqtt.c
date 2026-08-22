#include "regtable_mqtt.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/*  Longest register name the adapter accepts; regmqtt_handle
 *  extracts names into a buffer of this size + 1. */
#define REGMQTT_NAME_MAX 31

/* index of an entry within its table */
static uint16_t entry_index(const RegMqtt *mq, const RegEntry *e)
{
    return (uint16_t)(e - mq->table->entries);
}

static bool is_synced(const RegMqtt *mq, uint16_t i)
{
    return (mq->synced[i / 32] >> (i % 32)) & 1u;
}

static void set_synced(RegMqtt *mq, uint16_t i, bool on)
{
    if (on) mq->synced[i / 32] |= (uint32_t)1u << (i % 32);
    else    mq->synced[i / 32] &= ~((uint32_t)1u << (i % 32));
}

/*  "<prefix>/<name>" into buf; init guaranteed it fits. */
static void state_topic(const RegMqtt *mq, const RegEntry *e, char *buf)
{
    snprintf(buf, REGTABLE_MQTT_TOPIC_SIZE, "%s/%s", mq->prefix, e->name);
}

/*  defined with the self-description below; init uses it to prove
 *  every entry's metadata fits before the table is accepted */
static bool meta_fits(const RegEntry *e, uint16_t index);

/* -- public API -------------------------------------------- */

RegResult regmqtt_init(RegMqtt *mq, RegTable *table, const char *prefix,
                       int (*publish)(const char *, const char *, bool, void *),
                       void *user)
{
    if (!mq || !table || !prefix || !publish) return REG_ERR_TABLE;

    /* wildcards are only legal in subscriptions, never in a
     * published topic name (MQTT 3.1.1, 4.7); the prefix may
     * contain '/' for a multi-level namespace */
    if (prefix[0] == '\0' || strpbrk(prefix, "+#") != NULL) {
        return REG_ERR_TABLE;
    }

    size_t plen = strlen(prefix);
    /* the table descriptor topic exists even for an empty table */
    if (plen + sizeof("/$meta/$table") > REGTABLE_MQTT_TOPIC_SIZE) {
        return REG_ERR_TABLE;
    }
    for (const RegEntry *e = table->entries; e->name != NULL; e++) {
        size_t nlen = strlen(e->name);
        /* a name with '/' would fork the topic tree: an entry named
         * "gain/set" would publish state onto the command topic of
         * an entry named "gain". One name, one level. */
        if (nlen == 0 || nlen > REGMQTT_NAME_MAX ||
            strpbrk(e->name, "/+#") != NULL || e->name[0] == '$') {
            return REG_ERR_TABLE;       /* '$' names are the metadata level */
        }
        /* every topic, the /set and the $meta/ variants included,
         * must fit the scratch: prefix + "/$meta/" + name + NUL is
         * the longest */
        if (plen + 7 + nlen + 1 > REGTABLE_MQTT_TOPIC_SIZE) {
            return REG_ERR_TABLE;
        }
        /* two entries with one name would share every topic; the
         * CLI would find only the first, MQTT would announce both */
        for (const RegEntry *o = table->entries; o != e; o++) {
            if (strcmp(o->name, e->name) == 0) return REG_ERR_TABLE;
        }
        /* the metadata announce will send, without the optional
         * description, must fit the scratch: what init accepts,
         * announce can publish */
        if (!meta_fits(e, (uint16_t)(e - table->entries))) return REG_ERR_TABLE;
    }

    mq->table   = table;
    mq->prefix  = prefix;
    mq->publish = publish;
    mq->user    = user;
    memset(mq->synced, 0, sizeof(mq->synced));
    return REG_OK;
}

uint16_t regmqtt_poll(RegMqtt *mq)
{
    char topic[REGTABLE_MQTT_TOPIC_SIZE];
    char payload[32];
    uint16_t sent = 0;

    for (const RegEntry *e = mq->table->entries; e->name != NULL; e++) {
        uint16_t i = entry_index(mq, e);
        uint32_t raw = 0;
        if (reg_get_raw(e, &raw) != REG_OK) continue;   /* runs on_read */
        if (is_synced(mq, i) && raw == mq->shadow[i]) continue;

        /* out of sync: publish the raw just compared (formatting it
         * directly keeps on_read at one sample per poll and makes
         * the payload exactly the value the shadow will record),
         * retained so late subscribers get state without waiting
         * for a change */
        state_topic(mq, e, topic);
        if (reg_raw_str(e, raw, payload, sizeof(payload)) < 0) continue;
        if (mq->publish(topic, payload, true, mq->user) != 0) {
            /* not accepted: leave unsynced, the next poll retries */
            set_synced(mq, i, false);
            continue;
        }
        mq->shadow[i] = raw;
        set_synced(mq, i, true);
        sent++;
    }
    return sent;
}

uint16_t regmqtt_publish_all(RegMqtt *mq)
{
    memset(mq->synced, 0, sizeof(mq->synced));
    return regmqtt_poll(mq);
}

RegResult regmqtt_handle(RegMqtt *mq, const char *topic, const char *payload)
{
    /* expected shape: <prefix>/<name>/set */
    size_t plen = strlen(mq->prefix);
    if (strncmp(topic, mq->prefix, plen) != 0 || topic[plen] != '/') {
        return REG_ERR_NOT_FOUND;
    }
    const char *name = topic + plen + 1;
    const char *slash = strchr(name, '/');
    if (!slash || strcmp(slash, "/set") != 0) {
        return REG_ERR_NOT_FOUND;
    }

    char nbuf[REGMQTT_NAME_MAX + 1];
    size_t nlen = (size_t)(slash - name);
    if (nlen == 0 || nlen > REGMQTT_NAME_MAX) return REG_ERR_NOT_FOUND;
    memcpy(nbuf, name, nlen);
    nbuf[nlen] = '\0';

    const RegEntry *e = reg_find(mq->table, nbuf);
    if (!e) return REG_ERR_NOT_FOUND;

    /* full validation path; a success reaches the state topic at
     * the next poll, a refusal leaves it unchanged */
    return reg_set_str(mq->table, e, payload);
}

/* -- self-description ----------------------------------------- */

/*  A bounded text appender: one buffer, one flag that says whether
 *  everything fit. Nothing truncated is ever sent. */
typedef struct {
    char  *buf;
    size_t size;
    size_t len;
    bool   overflow;
} Appender;

static void ap_putc(Appender *a, char c)
{
    if (a->len + 1 < a->size) a->buf[a->len++] = c;
    else                      a->overflow = true;
    a->buf[a->len] = '\0';
}

static void ap_puts(Appender *a, const char *s)
{
    while (*s) ap_putc(a, *s++);
}

static void ap_printf(Appender *a, const char *fmt, ...)
{
    va_list ap;
    size_t room = a->size - a->len;             /* includes the NUL */
    va_start(ap, fmt);
    int n = vsnprintf(a->buf + a->len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= room) {
        a->overflow = true;
        a->buf[a->len] = '\0';
        return;
    }
    a->len += (size_t)n;
}

/*  A JSON string, escaped the way the CLI's list --json escapes it:
 *  '"' '\\' and \n \r \t by name, other control characters as \u00xx. */
static void ap_json_str(Appender *a, const char *s)
{
    ap_putc(a, '"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  ap_puts(a, "\\\""); break;
        case '\\': ap_puts(a, "\\\\"); break;
        case '\n': ap_puts(a, "\\n");  break;
        case '\r': ap_puts(a, "\\r");  break;
        case '\t': ap_puts(a, "\\t");  break;
        default:
            if (c >= 0x20) ap_putc(a, (char)c);
            else           ap_printf(a, "\\u%04x", c);
        }
    }
    ap_putc(a, '"');
}

/*  A float as JSON, as the CLI does it: %.9g round-trips a float;
 *  NaN and Inf have no JSON form and become null. */
static void ap_json_float(Appender *a, float f)
{
    if (f != f || f > 3.4028235e38f || f < -3.4028235e38f) ap_puts(a, "null");
    else ap_printf(a, "%.9g", (double)f);
}

/*  ,"schema":"xxxxxxxx"}  closes every meta; the body leaves room for it */
#define META_TRAILER_LEN (sizeof(",\"schema\":\"xxxxxxxx\"}") - 1)

/*  The body of one $meta payload, up to but not including the
 *  trailer: a list --json entry without value, plus the entry's
 *  index in the table. The same bytes feed the fingerprint. */
#define META_BODY_SIZE (REGTABLE_MQTT_META_SIZE - META_TRAILER_LEN)

static void meta_body(Appender *a, const RegEntry *e, uint16_t index, bool with_desc)
{
    ap_puts(a, "{\"name\":");  ap_json_str(a, e->name);
    ap_puts(a, ",\"type\":");  ap_json_str(a, reg_type_str(e->type));
    ap_puts(a, ",\"perm\":");  ap_puts(a, e->perm == REG_RO ? "\"RO\"" : "\"RW\"");
    ap_printf(a, ",\"index\":%u", (unsigned)index);
    if (!(e->min.u == 0 && e->max.u == 0)) {
        switch (e->type) {
        case REG_I8: case REG_I16: case REG_I32:
            ap_printf(a, ",\"min\":%ld,\"max\":%ld", (long)e->min.i, (long)e->max.i);
            break;
        case REG_FLOAT:
            ap_puts(a, ",\"min\":"); ap_json_float(a, e->min.f);
            ap_puts(a, ",\"max\":"); ap_json_float(a, e->max.f);
            break;
        default:
            ap_printf(a, ",\"min\":%lu,\"max\":%lu",
                      (unsigned long)e->min.u, (unsigned long)e->max.u);
            break;
        }
    }
    if (e->modbus_addr) ap_printf(a, ",\"modbus\":%u", (unsigned)e->modbus_addr);
    if (with_desc && e->description) {
        /* the description is the one unbounded field: if it does not
         * fit, the meta goes out without it rather than not at all */
        size_t before = a->len;
        ap_puts(a, ",\"desc\":"); ap_json_str(a, e->description);
        if (a->overflow) {
            a->overflow = false;
            a->len = before;
            a->buf[a->len] = '\0';
        }
    }
}

/*  FNV-1a over the bytes of every meta body, in table order: the
 *  table's fingerprint. Two firmwares with the same registers in the
 *  same shape share it; a renamed register, a changed range, a moved
 *  entry changes it. */
static uint32_t fnv1a(uint32_t h, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static bool fits(int n, size_t size)
{
    return n > 0 && (size_t)n < size;
}

/*  Does the entry's metadata, description aside, fit the scratch
 *  with room for the trailer? init asks this for every entry. */
static bool meta_fits(const RegEntry *e, uint16_t index)
{
    char scratch[META_BODY_SIZE];
    Appender a = { scratch, sizeof(scratch), 0, false };
    meta_body(&a, e, index, false);
    return !a.overflow;
}

uint32_t regmqtt_announce(RegMqtt *mq)
{
    char topic[REGTABLE_MQTT_TOPIC_SIZE];
    char payload[REGTABLE_MQTT_META_SIZE];
    uint32_t sent = 0;
    uint32_t fp = 2166136261u;

    if (!mq || !mq->table) return 0;            /* never bound: nothing to describe */

    /* pass 1: the fingerprint, over exactly the bodies pass 2 sends
     * (init proved every body fits without its description, and the
     * description degrades inside meta_body) */
    for (const RegEntry *e = mq->table->entries; e->name != NULL; e++) {
        Appender a = { payload, META_BODY_SIZE, 0, false };
        meta_body(&a, e, entry_index(mq, e), true);
        fp = fnv1a(fp, a.buf, a.len);
    }

    /* the descriptor: how many metas make the table, and which ones */
    int n = snprintf(topic, sizeof(topic), "%s/$meta/$table", mq->prefix);
    if (fits(n, sizeof(topic))) {
        n = snprintf(payload, sizeof(payload), "{\"count\":%u,\"schema\":\"%08lx\"}",
                     (unsigned)mq->table->count, (unsigned long)fp);
        if (fits(n, sizeof(payload)) && mq->publish(topic, payload, true, mq->user) == 0) sent++;
    }

    /* pass 2: one meta per entry, each carrying the fingerprint */
    for (const RegEntry *e = mq->table->entries; e->name != NULL; e++) {
        Appender a = { payload, META_BODY_SIZE, 0, false };
        meta_body(&a, e, entry_index(mq, e), true);
        a.size = sizeof(payload);                   /* the reserved room */
        ap_printf(&a, ",\"schema\":\"%08lx\"}", (unsigned long)fp);
        if (a.overflow) continue;                   /* cannot happen after init; never truncated */
        n = snprintf(topic, sizeof(topic), "%s/$meta/%s", mq->prefix, e->name);
        if (!fits(n, sizeof(topic))) continue;
        if (mq->publish(topic, payload, true, mq->user) == 0) sent++;
    }
    return sent;
}
