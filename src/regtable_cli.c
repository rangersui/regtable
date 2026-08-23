#include "regtable_cli.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

/* -- literal strings: flash on AVR, plain memory elsewhere ---- */
/*  AVR keeps string constants in RAM unless told otherwise, and the
 *  CLI's prompts, labels, formats, and JSON keys add up to about a
 *  kilobyte: half of an Uno. Every literal this file prints goes
 *  through LIT(), which is PSTR() on AVR (the string stays in flash,
 *  read byte by byte) and the plain string everywhere else. Strings
 *  that arrive at run time (names, values, descriptions, the
 *  identity) are RAM on every target and take the ordinary path. */
#ifdef __AVR__
#include <avr/pgmspace.h>
#define LIT(s)           PSTR(s)
#define LIT_DECL(n, s)   static const char n[] PROGMEM = s
#define LIT_STRCMP(ram, lit) strcmp_P((ram), (lit))
#define LIT_VSNPRINTF    vsnprintf_P
#define LIT_SNPRINTF     snprintf_P
#define LIT_BYTE(p)      ((char)pgm_read_byte(p))
#else
#define LIT(s)           (s)
#define LIT_DECL(n, s)   static const char n[] = s
#define LIT_STRCMP(ram, lit) strcmp((ram), (lit))
#define LIT_VSNPRINTF    vsnprintf
#define LIT_SNPRINTF     snprintf
#define LIT_BYTE(p)      (*(p))
#endif

/* "RO"/"RW" copied into buf, for a %s argument (printf reads its
 * arguments from RAM on every target) */
static const char *perm_str(const RegEntry *e, char buf[3])
{
    LIT_DECL(ro, "RO");
    LIT_DECL(rw, "RW");
    const char *l = e->perm == REG_RO ? ro : rw;
    for (uint8_t i = 0; i < 3; i++) buf[i] = LIT_BYTE(l + i);
    return buf;
}

/* -- output helpers ---------------------------------------- */

/* a run-time string (RAM) */
static void cli_puts(RegCli *cli, const char *s)
{
    cli->tx.write((const uint8_t *)s, (uint16_t)strlen(s));
}

/* a literal, wherever the target keeps it */
static void cli_lit(RegCli *cli, const char *lit)
{
    char buf[24];
    uint8_t n = 0;
    for (;;) {
        char c = LIT_BYTE(lit++);
        if (c != '\0') buf[n++] = c;
        if (c == '\0' || n == sizeof(buf)) {
            if (n) cli->tx.write((const uint8_t *)buf, n);
            n = 0;
            if (c == '\0') break;
        }
    }
}

/* printf with a literal format */
static void cli_print(RegCli *cli, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = LIT_VSNPRINTF(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    /* vsnprintf returns the length it wanted, not what fit */
    if ((size_t)n >= sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    cli->tx.write((const uint8_t *)tmp, (uint16_t)n);
}

/* -- JSON output ------------------------------------------- */
/*  Output only, no parser: a few keys, numbers, bools, strings.
 *  The one real job is escaping strings. Written piecewise
 *  through the transport, no big buffer. */

/* one escaped character, or nothing when c needs none */
static bool json_escape(RegCli *cli, unsigned char c)
{
    switch (c) {
    case '"':  cli_lit(cli, LIT("\\\"")); return true;
    case '\\': cli_lit(cli, LIT("\\\\")); return true;
    case '\n': cli_lit(cli, LIT("\\n"));  return true;
    case '\r': cli_lit(cli, LIT("\\r"));  return true;
    case '\t': cli_lit(cli, LIT("\\t"));  return true;
    default:
        if (c >= 0x20) return false;
        cli_print(cli, LIT("\\u%04x"), c);
        return true;
    }
}

/* emit a run-time string as a JSON string literal, with quotes.
 * Runs of plain characters go out in one write; only the escapes
 * break the run. */
static void json_str(RegCli *cli, const char *s)
{
    const char *run = s;
    cli_lit(cli, LIT("\""));
    for (;; s++) {
        unsigned char c = (unsigned char)*s;
        bool plain = c != '\0' && c >= 0x20 && c != '"' && c != '\\';
        if (plain) continue;              /* extend the run */
        /* flush the run before the escape (or the end) */
        if (s > run) cli->tx.write((const uint8_t *)run, (uint16_t)(s - run));
        if (c == '\0') break;
        json_escape(cli, c);
        run = s + 1;
    }
    cli_lit(cli, LIT("\""));
}

/* emit a literal as a JSON string, with quotes (built strings,
 * "RO"/"RW": short, read where the target keeps them) */
static void json_lit(RegCli *cli, const char *lit)
{
    cli_lit(cli, LIT("\""));
    for (;; lit++) {
        unsigned char c = (unsigned char)LIT_BYTE(lit);
        if (c == '\0') break;
        if (!json_escape(cli, c)) cli->tx.write(&c, 1);
    }
    cli_lit(cli, LIT("\""));
}

/* a float as a JSON number: enough digits to round-trip a
 * float, bounded length; NaN/Inf have no JSON form -> null */
static void json_float(RegCli *cli, float f)
{
    if (isnan(f) || isinf(f)) cli_lit(cli, LIT("null"));
    else                      cli_print(cli, LIT("%.9g"), (double)f);
}

/* current value as a JSON literal */
static void json_value(RegCli *cli, const RegEntry *e)
{
    if (e->type == REG_FLOAT) {
        uint32_t raw;
        float f;
        reg_get_raw(e, &raw);
        memcpy(&f, &raw, sizeof(f));
        json_float(cli, f);
    } else {
        /* integers and true/false from reg_get_str are already
         * valid JSON literals */
        char vbuf[32];
        reg_get_str(e, vbuf, sizeof(vbuf));
        cli_puts(cli, vbuf);
    }
}

static void json_perm(RegCli *cli, const RegEntry *e)
{
    json_lit(cli, e->perm == REG_RO ? LIT("RO") : LIT("RW"));
}

/* one register as a JSON object */
static void json_entry(RegCli *cli, const RegEntry *e)
{
    cli_lit(cli, LIT("{\"name\":"));   json_str(cli, e->name);
    cli_lit(cli, LIT(",\"type\":"));   json_str(cli, reg_type_str(e->type));
    cli_lit(cli, LIT(",\"perm\":"));   json_perm(cli, e);
    cli_lit(cli, LIT(",\"value\":"));  json_value(cli, e);

    if (!(e->min.u == 0 && e->max.u == 0)) {
        switch (e->type) {
        case REG_I8: case REG_I16: case REG_I32:
            cli_print(cli, LIT(",\"min\":%ld,\"max\":%ld"), (long)e->min.i, (long)e->max.i);
            break;
        case REG_FLOAT:
            cli_lit(cli, LIT(",\"min\":"));  json_float(cli, e->min.f);
            cli_lit(cli, LIT(",\"max\":"));  json_float(cli, e->max.f);
            break;
        default:
            cli_print(cli, LIT(",\"min\":%lu,\"max\":%lu"),
                      (unsigned long)e->min.u, (unsigned long)e->max.u);
            break;
        }
    }
    if (e->modbus_addr) {
        cli_print(cli, LIT(",\"modbus\":%u"), (unsigned)e->modbus_addr);
    }
    if (e->description) {
        cli_lit(cli, LIT(",\"desc\":"));
        json_str(cli, e->description);
    }
    cli_lit(cli, LIT("}"));
}

/* {"error":"<literal>"} */
static void json_error_lit(RegCli *cli, const char *lit)
{
    cli_lit(cli, LIT("{\"error\":"));
    json_lit(cli, lit);
    cli_lit(cli, LIT("}\r\n"));
}

/* {"error":"<the core's result string>"} */
static void json_error(RegCli *cli, RegResult r)
{
    cli_lit(cli, LIT("{\"error\":"));
    json_str(cli, reg_result_str(r));
    cli_lit(cli, LIT("}\r\n"));
}

/* an error line (a literal message) in whichever format the caller asked for */
static void cli_error(RegCli *cli, const char *lit, bool json)
{
    if (json) {
        json_error_lit(cli, lit);
    } else {
        cli_lit(cli, lit);
        cli_lit(cli, LIT("\r\n"));
    }
}

/* -- tokeniser: splits the line in place, argv points into it -- */

/* "--json" is an output-format flag, not an argument: it is taken
 * out here, wherever it sits, and does not count toward max_args.
 * Returns the token count, or -1 if the line holds more than max_args. */
static int cli_split(char *line, char **argv, int max_args, bool *json)
{
    int argc = 0;
    bool too_many = false;
    char *p = line;

    for (;;) {
        /* skip leading spaces */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        char *tok = p;
        /* find end of token */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';

        if (LIT_STRCMP(tok, LIT("--json")) == 0) { *json = true; continue; }
        if (argc == max_args) { too_many = true; continue; }   /* keep scanning for --json */
        argv[argc++] = tok;
    }
    return too_many ? -1 : argc;
}

/* -- commands -------------------------------------------- */

static void cmd_list(RegCli *cli, bool json)
{
    char vbuf[32];

    if (json) {
        cli_lit(cli, LIT("["));
        for (const RegEntry *e = cli->table->entries; e->name != NULL; e++) {
            if (e != cli->table->entries) cli_lit(cli, LIT(","));
            json_entry(cli, e);
        }
        cli_lit(cli, LIT("]\r\n"));
        return;
    }

    cli_lit(cli, LIT("NAME            TYPE   PERM  VALUE\r\n"));
    cli_lit(cli, LIT("--------------------------------------------\r\n"));

    for (const RegEntry *e = cli->table->entries; e->name != NULL; e++) {
        char perm[3];
        reg_get_str(e, vbuf, sizeof(vbuf));
        cli_print(cli, LIT("%-16s%-7s%-6s%s\r\n"),
                  e->name,
                  reg_type_str(e->type),
                  perm_str(e, perm),
                  vbuf);
    }
}

/* status line: "OK" / "ERR: ..." as text, {"result":..} / {"error":..} as JSON */
static void cli_result(RegCli *cli, RegResult r, bool json)
{
    if (!json) {
        cli_puts(cli, reg_result_str(r));
        cli_lit(cli, LIT("\r\n"));
    } else if (r == REG_OK) {
        cli_lit(cli, LIT("{\"result\":\"OK\"}\r\n"));
    } else {
        json_error(cli, r);
    }
}

static void cmd_get(RegCli *cli, const char *name, bool json)
{
    const RegEntry *e = reg_find(cli->table, name);
    if (!e) {
        cli_result(cli, REG_ERR_NOT_FOUND, json);
        return;
    }
    if (json) {
        cli_lit(cli, LIT("{\"value\":"));
        json_value(cli, e);
        cli_lit(cli, LIT("}\r\n"));
        return;
    }
    char vbuf[32];
    reg_get_str(e, vbuf, sizeof(vbuf));
    cli_puts(cli, vbuf);
    cli_lit(cli, LIT("\r\n"));
}

static void cmd_set(RegCli *cli, const char *name, const char *value, bool json)
{
    const RegEntry *e = reg_find(cli->table, name);
    if (!e) {
        cli_result(cli, REG_ERR_NOT_FOUND, json);
        return;
    }
    cli_result(cli, reg_set_str(cli->table, e, value), json);
}

static void cmd_info(RegCli *cli, const char *name, bool json)
{
    const RegEntry *e = reg_find(cli->table, name);
    if (!e) {
        cli_result(cli, REG_ERR_NOT_FOUND, json);
        return;
    }

    if (json) {
        json_entry(cli, e);
        cli_lit(cli, LIT("\r\n"));
        return;
    }

    char vbuf[32];
    reg_get_str(e, vbuf, sizeof(vbuf));

    cli_print(cli, LIT("name:   %s\r\n"), e->name);
    cli_print(cli, LIT("type:   %s\r\n"), reg_type_str(e->type));
    char perm[3];
    cli_print(cli, LIT("perm:   %s\r\n"), perm_str(e, perm));
    cli_print(cli, LIT("value:  %s\r\n"), vbuf);
    if (!(e->min.u == 0 && e->max.u == 0)) {
        switch (e->type) {
        case REG_I8: case REG_I16: case REG_I32:
            cli_print(cli, LIT("range:  %ld..%ld\r\n"), (long)e->min.i, (long)e->max.i);
            break;
        case REG_FLOAT:
            cli_print(cli, LIT("range:  %.6g..%.6g\r\n"), (double)e->min.f, (double)e->max.f);
            break;
        default:
            cli_print(cli, LIT("range:  %lu..%lu\r\n"),
                      (unsigned long)e->min.u, (unsigned long)e->max.u);
            break;
        }
    }
    if (e->modbus_addr) {
        cli_print(cli, LIT("modbus: 0x%04X\r\n"), e->modbus_addr);
    }
    if (e->description) {
        cli_lit(cli, LIT("desc:   "));
        cli_puts(cli, e->description);
        cli_lit(cli, LIT("\r\n"));
    }
}

/*  id: who the device is. Strings the application set (device, fw,
 *  hash), what the build knows (date, compiler), what the library and
 *  the table know (version, count, schema fingerprint). Text form is
 *  one `key  value` per line; --json is one object, absent strings
 *  left out. */
static void cmd_id(RegCli *cli, bool json)
{
    const RegIdentity *id = cli->identity;
    const char *device = id ? id->device : NULL;
    const char *fw     = id ? id->fw     : NULL;
    const char *hash   = id ? id->hash   : NULL;
    const char *chip   = id ? id->chip   : NULL;
    char schema[9];
    LIT_SNPRINTF(schema, sizeof(schema), LIT("%08lx"), (unsigned long)reg_table_schema(cli->table));

    if (json) {
        cli_lit(cli, LIT("{"));
        if (device) { cli_lit(cli, LIT("\"device\":")); json_str(cli, device); cli_lit(cli, LIT(",")); }
        if (fw)     { cli_lit(cli, LIT("\"fw\":"));     json_str(cli, fw);     cli_lit(cli, LIT(",")); }
        if (hash)   { cli_lit(cli, LIT("\"hash\":"));   json_str(cli, hash);   cli_lit(cli, LIT(",")); }
        if (chip)   { cli_lit(cli, LIT("\"chip\":"));   json_str(cli, chip);   cli_lit(cli, LIT(",")); }
        cli_lit(cli, LIT("\"built\":")); json_lit(cli, LIT(__DATE__ " " __TIME__));
#ifdef __VERSION__
        cli_lit(cli, LIT(",\"compiler\":")); json_lit(cli, LIT(__VERSION__));
#endif
        cli_lit(cli, LIT(",\"regtable\":\"" REGTABLE_VERSION "\""));
        cli_print(cli, LIT(",\"regs\":%u"), (unsigned)cli->table->count);
        cli_lit(cli, LIT(",\"schema\":\"")); cli_puts(cli, schema); cli_lit(cli, LIT("\"}\r\n"));
        return;
    }
    if (device) { cli_lit(cli, LIT("device    ")); cli_puts(cli, device); cli_lit(cli, LIT("\r\n")); }
    if (fw)     { cli_lit(cli, LIT("fw        ")); cli_puts(cli, fw);     cli_lit(cli, LIT("\r\n")); }
    if (hash)   { cli_lit(cli, LIT("hash      ")); cli_puts(cli, hash);   cli_lit(cli, LIT("\r\n")); }
    if (chip)   { cli_lit(cli, LIT("chip      ")); cli_puts(cli, chip);   cli_lit(cli, LIT("\r\n")); }
    cli_lit(cli, LIT("built     " __DATE__ " " __TIME__ "\r\n"));
#ifdef __VERSION__
    cli_lit(cli, LIT("compiler  " __VERSION__ "\r\n"));
#endif
    cli_lit(cli, LIT("regtable  " REGTABLE_VERSION "\r\n"));
    cli_print(cli, LIT("regs      %u\r\n"), (unsigned)cli->table->count);
    cli_lit(cli, LIT("schema    ")); cli_puts(cli, schema); cli_lit(cli, LIT("\r\n"));
}

/*  fetch: the device as a chip, then what `id` says. Pure ASCII; the
 *  package is fixed, the die carries the library version. --json is
 *  the same answer as `id --json`. */
static void cmd_fetch(RegCli *cli, bool json)
{
    if (json) { cmd_id(cli, true); return; }
    /* the version, centred on the die (21 columns between the walls),
     * laid out in RAM: a printf width star is not in every libc */
    LIT_DECL(ver, "v" REGTABLE_VERSION);
    char die[22];
    size_t len = 0;
    while (len < 21 && LIT_BYTE(ver + len) != '\0') len++;
    size_t left = (21 - len + 1) / 2;
    memset(die, ' ', 21);
    for (size_t i = 0; i < len; i++) die[left + i] = LIT_BYTE(ver + i);
    die[21] = '\0';
    cli_lit(cli, LIT("\r\n"
                     "           | | | | | | | | |\r\n"
                     "        +--+-+-+-+-+-+-+-+-+--+\r\n"
                     "    ----| o                   |----\r\n"
                     "    ----|       regtable      |----\r\n"
                     "    ----|"));
    cli_puts(cli, die);
    cli_lit(cli, LIT("|----\r\n"
                     "    ----|                     |----\r\n"
                     "        +--+-+-+-+-+-+-+-+-+--+\r\n"
                     "           | | | | | | | | |\r\n"
                     "\r\n"));
    cmd_id(cli, false);
}

static void cmd_help(RegCli *cli)
{
    cli_lit(cli, LIT("COMMANDS:\r\n"
                     "  get <name>          read register value\r\n"
                     "  set <name> <value>  write register value\r\n"
                     "  info <name>         show register metadata\r\n"
                     "  list                show all registers\r\n"
                     "  id                  show device identity\r\n"
                     "  fetch               show the device as a chip\r\n"
                     "  help                show this message\r\n"
                     "  add --json to list/get/set/info/id for machine-readable output\r\n"));
}

/* -- line processor -------------------------------------- */

/* the commands, their exact argument counts, and their usage lines:
 * anything else is an error, never a guess */
LIT_DECL(c_list, "list"); LIT_DECL(u_list, "ERR: usage: list");
LIT_DECL(c_get,  "get");  LIT_DECL(u_get,  "ERR: usage: get <name>");
LIT_DECL(c_set,  "set");  LIT_DECL(u_set,  "ERR: usage: set <name> <value>");
LIT_DECL(c_info, "info"); LIT_DECL(u_info, "ERR: usage: info <name>");
LIT_DECL(c_id,   "id");   LIT_DECL(u_id,   "ERR: usage: id");
LIT_DECL(c_fetch, "fetch"); LIT_DECL(u_fetch, "ERR: usage: fetch");
LIT_DECL(c_help, "help"); LIT_DECL(u_help, "ERR: usage: help");

static const struct { const char *name; int argc; const char *usage; } cmds[] = {
    { c_list,  1, u_list  },
    { c_get,   2, u_get   },
    { c_set,   3, u_set   },
    { c_info,  2, u_info  },
    { c_id,    1, u_id    },
    { c_fetch, 1, u_fetch },
    { c_help,  1, u_help  },
};

static void cli_process(RegCli *cli, char *line)
{
    char *argv[REGTABLE_CLI_MAX_ARGS];
    bool json = false;
    int argc = cli_split(line, argv, REGTABLE_CLI_MAX_ARGS, &json);

    if (argc == 0) {
        return;
    }
    if (argc < 0) {
        cli_error(cli, LIT("ERR: too many arguments"), json);
        return;
    }

    int which = -1;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (LIT_STRCMP(argv[0], cmds[i].name) == 0) {
            if (argc != cmds[i].argc) { cli_error(cli, cmds[i].usage, json); return; }
            which = (int)i;
            break;
        }
    }
    switch (which) {
    case 0:  cmd_list(cli, json);                   break;
    case 1:  cmd_get(cli, argv[1], json);           break;
    case 2:  cmd_set(cli, argv[1], argv[2], json);  break;
    case 3:  cmd_info(cli, argv[1], json);          break;
    case 4:  cmd_id(cli, json);                     break;
    case 5:  cmd_fetch(cli, json);                  break;
    case 6:  cmd_help(cli);                         break;
    default: cli_error(cli, LIT("ERR: unknown command. type 'help'"), json); break;
    }
}

/* -- public API ------------------------------------------ */

void regcli_init(RegCli *cli, RegTable *table, RegTransport tx)
{
    cli->table = table;
    cli->tx    = tx;
    cli->pos   = 0;
    cli->echo  = true;
    cli->overflow = false;
    cli->identity = NULL;
}

void regcli_set_identity(RegCli *cli, const RegIdentity *identity)
{
    cli->identity = identity;
}

void regcli_feed(RegCli *cli, uint8_t byte)
{
    /* echo */
    if (cli->echo) {
        cli->tx.write(&byte, 1);
    }

    /* line endings */
    if (byte == '\n' || byte == '\r') {
        if (cli->overflow) {
            /* the line was cut; running the remainder could write a
             * different, valid-looking value. Drop it whole. */
            if (cli->echo) cli_lit(cli, LIT("\r\n"));
            cli_lit(cli, LIT("ERR: line too long\r\n"));
            cli->overflow = false;
            cli->pos = 0;
        } else if (cli->pos > 0) {
            if (cli->echo) {
                cli_lit(cli, LIT("\r\n"));
            }
            cli->buf[cli->pos] = '\0';
            cli_process(cli, cli->buf);
            cli->pos = 0;
        }
        return;
    }

    /* backspace */
    if (byte == '\b' || byte == 0x7F) {
        if (cli->pos > 0) {
            cli->pos--;
            if (cli->echo) {
                cli_lit(cli, LIT("\b \b"));
            }
        }
        return;
    }

    /* ordinary character: keep it, or mark the line as cut */
    if (cli->pos < REGTABLE_CLI_BUF_SIZE - 1) {
        cli->buf[cli->pos++] = (char)byte;
    } else {
        cli->overflow = true;
    }
}
