#include "regtable_mqtt.h"
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
    for (const RegEntry *e = table->entries; e->name != NULL; e++) {
        size_t nlen = strlen(e->name);
        /* a name with '/' would fork the topic tree: an entry named
         * "gain/set" would publish state onto the command topic of
         * an entry named "gain". One name, one level. */
        if (nlen == 0 || nlen > REGMQTT_NAME_MAX ||
            strpbrk(e->name, "/+#") != NULL) {
            return REG_ERR_TABLE;
        }
        /* every topic, including the /set variant, must fit the scratch */
        if (plen + 1 + nlen + 4 + 1 > REGTABLE_MQTT_TOPIC_SIZE) {
            return REG_ERR_TABLE;
        }
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
