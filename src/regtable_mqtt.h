#ifndef REGTABLE_MQTT_H
#define REGTABLE_MQTT_H

#include "regtable_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MQTT projection of the table: topics and payloads in, topics and
 * payloads out. The MQTT protocol itself (connect, keepalive, QoS,
 * reconnect) belongs to an MQTT client library in platform code,
 * the way the UART belongs to the HAL; this adapter never sees a
 * protocol byte.
 *
 * Topics:  <prefix>/<name>       state, published by the device
 *          <prefix>/<name>/set   command, subscribed by the device
 * Payloads are the register's text form (reg_get_str / reg_set_str):
 * "23.4", "500", "true". Platform code subscribes to <prefix>/+/set
 * and hands each message to regmqtt_handle.
 *
 * Change detection is a shadow array: the raw value last accepted by
 * publish(), one per entry. regmqtt_poll publishes every entry whose
 * current value differs from its shadow. The pair (value, shadow)
 * works like a transactional outbox with the difference as the
 * pending entry:
 *
 *   publish() returns 0     -> shadow catches up, value is out
 *   publish() returns non-0 -> shadow stays, next poll retries
 *   payloads are absolute values, so a repeat is harmless
 *   after (re)connect, regmqtt_publish_all resends everything
 *
 * That gives at-least-once delivery as far as publish()'s word is
 * good: return 0 to mean "the client accepted this message". True
 * delivery is the client's QoS business; the publish_all on
 * reconnect heals whatever a dropped connection lost.
 *
 * A rejected set is answered by silence on the state topic: the
 * value did not change, so nothing is published, and subscribers
 * see the old value still standing. regmqtt_handle returns the
 * RegResult for platform code to log.
 *
 * A /set message is a one-shot command, published with the retain
 * flag clear. A retained /set would be replayed by the broker on
 * every new subscription, so the command would re-execute after
 * each reconnect. Persisting a desired value for an offline device
 * is the broker or cloud layer's job (a device shadow service),
 * not this adapter's.
 *
 * Register names become topic levels, so regmqtt_init enforces
 * MQTT's topic rules on them: 1..31 characters, no '/' (one name,
 * one level), no '+' or '#' (wildcards are only legal in
 * subscriptions), no leading '$' (that level is the adapter's
 * metadata, below). The prefix may contain '/' for a multi-level
 * namespace, but no wildcards.
 *
 * Self-description, the same the CLI gives with list --json, lives
 * on retained metadata topics under <prefix>/$meta, so a host that
 * joins later finds the table without asking:
 *
 *   <prefix>/$meta/$table   {"count":4,"schema":"1a2b3c4d"}
 *   <prefix>/$meta/$id      {"device":"uno","fw":"1.0","chip":"ATmega328P","built":"Aug 22 2026 10:20:30",
 *                            "regtable":"0.1.0","regs":4,"schema":"1a2b3c4d"}
 *                           (strings set with regmqtt_set_identity; absent
 *                           ones left out; the compiler string stays on the
 *                           CLI's `id`, it is long)
 *   <prefix>/$meta/<name>   one JSON object per register: the shape
 *                           of a list --json entry minus value, plus
 *                           its index in the table and the schema:
 *     {"name":"led","type":"BOOL","perm":"RW","index":0,"desc":"Built-in LED","schema":"1a2b3c4d"}
 *     {"name":"gain","type":"FLOAT","perm":"RW","index":3,"min":0.5,"max":2.5,"schema":"1a2b3c4d"}
 *
 * schema is the table's fingerprint, reg_table_schema(): the same
 * registers in the same shape give the same value and a renamed
 * register, a changed range, or a moved entry gives another; the
 * CLI's `id` reports the same number. Retained messages outlive the firmware that
 * published them, and the fingerprint is what tells a current meta
 * from a leftover: a host takes $table, keeps only metas whose
 * schema matches it, and has the whole table once those cover
 * indices 0..count-1. Anything else under $meta is ignored. A
 * partial publish leaves the table incomplete, never mixed with an
 * older one. regmqtt_announce publishes all of it; call it after
 * each (re)connect, before regmqtt_publish_all.
 *
 * <prefix>/+ is state only: the metadata sits two levels down. A
 * name starting with '$' is refused by regmqtt_init, so $meta never
 * collides with a register, and $table never collides with a name.
 *
 * The dirty bitmap and on_change stay the application's: this
 * adapter never touches them. on_change is "something happened"
 * (each transition, local reaction); the shadow is "the net value
 * is out of sync" (state convergence). A change that reverts
 * between two polls publishes nothing. */

/*  Topic scratch size: prefix + "/$meta/" + name + NUL must fit.
 *  regmqtt_init checks this against every entry, and the descriptor
 *  topic prefix + "/$meta/$table" against the prefix alone. */
#ifndef REGTABLE_MQTT_TOPIC_SIZE
#define REGTABLE_MQTT_TOPIC_SIZE 64
#endif

/*  Scratch for one $meta payload. regmqtt_init refuses a table whose
 *  metadata would not fit it without the description (name, type,
 *  perm, index, range, modbus, schema), so every meta it announces
 *  goes out; a description that does not fit is left out of its
 *  meta. Nothing is ever sent truncated. The descriptor and the
 *  shortest meta need the lower bound below. */
#ifndef REGTABLE_MQTT_META_SIZE
#define REGTABLE_MQTT_META_SIZE 160
#endif
#if REGTABLE_MQTT_META_SIZE < 96
#error "REGTABLE_MQTT_META_SIZE must be at least 96"
#endif

typedef struct RegMqtt {
    RegTable   *table;
    const char *prefix;
    /*  Hand one message to the MQTT client. retain distinguishes
     *  state (1: the broker keeps the last value for late
     *  subscribers) from anything transient. Return 0 once the
     *  client has accepted the message; non-0 leaves it pending
     *  and the next regmqtt_poll sends it again. */
    int  (*publish)(const char *topic, const char *payload,
                    bool retain, void *user);
    void  *user;
    const RegIdentity *identity;   /* strings $id carries; NULL = none set */
    uint32_t shadow[REGTABLE_MAX_ENTRIES];
    uint32_t synced[(REGTABLE_MAX_ENTRIES + 31) / 32];
} RegMqtt;

/*  Bind the adapter. Fails with REG_ERR_TABLE when an argument is
 *  NULL, a name breaks the topic rules above, two entries share a
 *  name (they would share every topic), a topic would not fit
 *  REGTABLE_MQTT_TOPIC_SIZE, or an entry's metadata without its
 *  description would not fit REGTABLE_MQTT_META_SIZE. All entries
 *  start unsynced: the first regmqtt_poll (or regmqtt_publish_all)
 *  publishes everything. */
RegResult regmqtt_init(RegMqtt *mq, RegTable *table, const char *prefix,
                       int (*publish)(const char *, const char *, bool, void *),
                       void *user);

/*  Publish every entry whose value differs from its shadow. Call
 *  from the main loop at the cadence telemetry should flow; the
 *  cadence is also the throttle for on_read entries that move on
 *  every sample. Returns the number of successful publishes. */
uint16_t regmqtt_poll(RegMqtt *mq);

/*  Mark everything unsynced, then poll: the full state goes out.
 *  Call after the MQTT client (re)connects. */
uint16_t regmqtt_publish_all(RegMqtt *mq);

/*  Handle one received message. Acts only on <prefix>/<name>/set
 *  topics: parses the payload per the entry's type and writes it
 *  through reg_set_raw (type domain, range, on_write veto, dirty
 *  bit for on_change - the same path every adapter uses). Any
 *  other topic returns REG_ERR_NOT_FOUND. A successful write shows
 *  up on the state topic at the next regmqtt_poll. */
RegResult regmqtt_handle(RegMqtt *mq, const char *topic, const char *payload);

/*  The strings $id carries. The adapter keeps the pointer, as it
 *  keeps the prefix and the table: the struct and the strings it
 *  points to stay as they are for as long as they are set (a
 *  static const RegIdentity, as the examples have it). Call after
 *  regmqtt_init: the $id payload is built here, with the table's
 *  count and fingerprint, and REG_ERR_TABLE says it does not fit
 *  REGTABLE_MQTT_META_SIZE (the adapter keeps the identity it had).
 *  What this accepts, announce publishes. NULL clears the strings.
 *  Stack while it runs: REGTABLE_MQTT_META_SIZE bytes. */
RegResult regmqtt_set_identity(RegMqtt *mq, const RegIdentity *identity);

/*  Publish the table's self-description, retained: the descriptor
 *  <prefix>/$meta/$table, the identity <prefix>/$meta/$id, and one
 *  <prefix>/$meta/<name> per entry, all carrying the table's
 *  fingerprint (see above). Call after each (re)connect, before
 *  regmqtt_publish_all. Returns the number of messages publish()
 *  accepted (count + 2 when all went out,
 *  which is why it is wider than a count); a
 *  refusal is not retried here, call again. Stack while it runs:
 *  REGTABLE_MQTT_TOPIC_SIZE + REGTABLE_MQTT_META_SIZE bytes. */
uint32_t regmqtt_announce(RegMqtt *mq);

#ifdef __cplusplus
}
#endif

#endif /* REGTABLE_MQTT_H */
