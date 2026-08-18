#include "regtable_cli.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

/* -- output helper --------------------------------------- */

static void cli_puts(RegCli *cli, const char *s)
{
    cli->tx.write((const uint8_t *)s, (uint16_t)strlen(s));
}

static void cli_print(RegCli *cli, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
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

/* emit s as a JSON string literal, with quotes.
 * Runs of plain characters go out in one write; only the escapes
 * break the run. */
static void json_str(RegCli *cli, const char *s)
{
    const char *run = s;
    cli_puts(cli, "\"");
    for (;; s++) {
        unsigned char c = (unsigned char)*s;
        const char *esc = NULL;
        switch (c) {
        case '\0': break;
        case '"':  esc = "\\\""; break;
        case '\\': esc = "\\\\"; break;
        case '\n': esc = "\\n";  break;
        case '\r': esc = "\\r";  break;
        case '\t': esc = "\\t";  break;
        default:
            if (c >= 0x20) continue;   /* plain: extend the run */
        }
        /* flush the run before the escape (or the end) */
        if (s > run) cli->tx.write((const uint8_t *)run, (uint16_t)(s - run));
        if (c == '\0') break;
        if (esc) cli_puts(cli, esc);
        else     cli_print(cli, "\\u%04x", c);
        run = s + 1;
    }
    cli_puts(cli, "\"");
}

/* a float as a JSON number: enough digits to round-trip a
 * float, bounded length; NaN/Inf have no JSON form -> null */
static void json_float(RegCli *cli, float f)
{
    if (isnan(f) || isinf(f)) cli_puts(cli, "null");
    else                      cli_print(cli, "%.9g", (double)f);
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

/* one register as a JSON object */
static void json_entry(RegCli *cli, const RegEntry *e)
{
    cli_puts(cli, "{\"name\":");   json_str(cli, e->name);
    cli_puts(cli, ",\"type\":");   json_str(cli, reg_type_str(e->type));
    cli_puts(cli, ",\"perm\":");   json_str(cli, e->perm == REG_RO ? "RO" : "RW");
    cli_puts(cli, ",\"value\":");  json_value(cli, e);

    if (!(e->min.u == 0 && e->max.u == 0)) {
        switch (e->type) {
        case REG_I8: case REG_I16: case REG_I32:
            cli_print(cli, ",\"min\":%ld,\"max\":%ld", (long)e->min.i, (long)e->max.i);
            break;
        case REG_FLOAT:
            cli_puts(cli, ",\"min\":");  json_float(cli, e->min.f);
            cli_puts(cli, ",\"max\":");  json_float(cli, e->max.f);
            break;
        default:
            cli_print(cli, ",\"min\":%lu,\"max\":%lu",
                      (unsigned long)e->min.u, (unsigned long)e->max.u);
            break;
        }
    }
    cli_print(cli, ",\"modbus\":%u", (unsigned)e->modbus_addr);
    if (e->description) {
        cli_puts(cli, ",\"desc\":");
        json_str(cli, e->description);
    }
    cli_puts(cli, "}");
}

static void json_error_str(RegCli *cli, const char *msg)
{
    cli_puts(cli, "{\"error\":");
    json_str(cli, msg);
    cli_puts(cli, "}\r\n");
}

static void json_error(RegCli *cli, RegResult r)
{
    json_error_str(cli, reg_result_str(r));
}

/* an error line in whichever format the caller asked for */
static void cli_error(RegCli *cli, const char *msg, bool json)
{
    if (json) {
        json_error_str(cli, msg);
    } else {
        cli_puts(cli, msg);
        cli_puts(cli, "\r\n");
    }
}

/* -- tokeniser: splits the line in place, argv points into it -- */

/* returns the token count, or -1 if the line holds more than max_args */
static int cli_split(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    for (;;) {
        /* skip leading spaces */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        if (argc == max_args) return -1;

        argv[argc++] = p;

        /* find end of token */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* -- commands -------------------------------------------- */

static void cmd_list(RegCli *cli, bool json)
{
    char vbuf[32];

    if (json) {
        cli_puts(cli, "[");
        for (const RegEntry *e = cli->table->entries; e->name != NULL; e++) {
            if (e != cli->table->entries) cli_puts(cli, ",");
            json_entry(cli, e);
        }
        cli_puts(cli, "]\r\n");
        return;
    }

    cli_puts(cli, "NAME            TYPE   PERM  VALUE\r\n");
    cli_puts(cli, "--------------------------------------------\r\n");

    for (const RegEntry *e = cli->table->entries; e->name != NULL; e++) {
        reg_get_str(e, vbuf, sizeof(vbuf));
        cli_print(cli, "%-16s%-7s%-6s%s\r\n",
                  e->name,
                  reg_type_str(e->type),
                  e->perm == REG_RO ? "RO" : "RW",
                  vbuf);
    }
}

/* status line: "OK" / "ERR: ..." as text, {"result":..} / {"error":..} as JSON */
static void cli_result(RegCli *cli, RegResult r, bool json)
{
    if (!json) {
        cli_puts(cli, reg_result_str(r));
        cli_puts(cli, "\r\n");
    } else if (r == REG_OK) {
        cli_puts(cli, "{\"result\":\"OK\"}\r\n");
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
        cli_puts(cli, "{\"value\":");
        json_value(cli, e);
        cli_puts(cli, "}\r\n");
        return;
    }
    char vbuf[32];
    reg_get_str(e, vbuf, sizeof(vbuf));
    cli_puts(cli, vbuf);
    cli_puts(cli, "\r\n");
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
        cli_puts(cli, "\r\n");
        return;
    }

    char vbuf[32];
    reg_get_str(e, vbuf, sizeof(vbuf));

    cli_print(cli, "name:   %s\r\n", e->name);
    cli_print(cli, "type:   %s\r\n", reg_type_str(e->type));
    cli_print(cli, "perm:   %s\r\n", e->perm == REG_RO ? "RO" : "RW");
    cli_print(cli, "value:  %s\r\n", vbuf);
    if (!(e->min.u == 0 && e->max.u == 0)) {
        switch (e->type) {
        case REG_I8: case REG_I16: case REG_I32:
            cli_print(cli, "range:  %ld..%ld\r\n", (long)e->min.i, (long)e->max.i);
            break;
        case REG_FLOAT:
            cli_print(cli, "range:  %.6g..%.6g\r\n", (double)e->min.f, (double)e->max.f);
            break;
        default:
            cli_print(cli, "range:  %lu..%lu\r\n",
                      (unsigned long)e->min.u, (unsigned long)e->max.u);
            break;
        }
    }
    cli_print(cli, "modbus: 0x%04X\r\n", e->modbus_addr);
    if (e->description) {
        cli_puts(cli, "desc:   ");
        cli_puts(cli, e->description);
        cli_puts(cli, "\r\n");
    }
}

static void cmd_help(RegCli *cli)
{
    cli_puts(cli, "COMMANDS:\r\n");
    cli_puts(cli, "  get <name>          read register value\r\n");
    cli_puts(cli, "  set <name> <value>  write register value\r\n");
    cli_puts(cli, "  info <name>         show register metadata\r\n");
    cli_puts(cli, "  list                show all registers\r\n");
    cli_puts(cli, "  help                show this message\r\n");
    cli_puts(cli, "  add --json to list/get/set/info for machine-readable output\r\n");
}

/* -- line processor -------------------------------------- */

static void cli_process(RegCli *cli, char *line)
{
    char *argv[REGTABLE_CLI_MAX_ARGS];
    int argc = cli_split(line, argv, REGTABLE_CLI_MAX_ARGS);

    if (argc == 0) {
        return;
    }
    if (argc < 0) {
        cli_error(cli, "ERR: too many arguments", false);
        return;
    }

    /* pull "--json" out of argv wherever it sits */
    bool json = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j + 1];
            argc--;
            break;
        }
    }

    /* each command takes an exact number of arguments; anything
     * else is an error, never a guess */
    static const struct { const char *name; int argc; const char *usage; } cmds[] = {
        { "list", 1, "ERR: usage: list" },
        { "get",  2, "ERR: usage: get <name>" },
        { "set",  3, "ERR: usage: set <name> <value>" },
        { "info", 2, "ERR: usage: info <name>" },
        { "help", 1, "ERR: usage: help" },
    };
    const char *usage = NULL;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (strcmp(argv[0], cmds[i].name) == 0) {
            usage = cmds[i].usage;
            if (argc != cmds[i].argc) { cli_error(cli, usage, json); return; }
            break;
        }
    }
    if (!usage) {
        cli_error(cli, "ERR: unknown command. type 'help'", json);
        return;
    }

    if      (strcmp(argv[0], "list") == 0) cmd_list(cli, json);
    else if (strcmp(argv[0], "get")  == 0) cmd_get(cli, argv[1], json);
    else if (strcmp(argv[0], "set")  == 0) cmd_set(cli, argv[1], argv[2], json);
    else if (strcmp(argv[0], "info") == 0) cmd_info(cli, argv[1], json);
    else                                   cmd_help(cli);
}

/* -- public API ------------------------------------------ */

void regcli_init(RegCli *cli, RegTable *table, RegTransport tx)
{
    cli->table = table;
    cli->tx    = tx;
    cli->pos   = 0;
    cli->echo  = true;
    cli->overflow = false;
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
            if (cli->echo) cli_puts(cli, "\r\n");
            cli_puts(cli, "ERR: line too long\r\n");
            cli->overflow = false;
            cli->pos = 0;
        } else if (cli->pos > 0) {
            if (cli->echo) {
                cli_puts(cli, "\r\n");
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
                cli_puts(cli, "\b \b");
            }
        }
        return;
    }

    /* normal char; past the buffer the line is marked bad and
     * rejected at the line ending */
    if (cli->pos < REGTABLE_CLI_BUF_SIZE - 1) {
        cli->buf[cli->pos++] = (char)byte;
    } else {
        cli->overflow = true;
    }
}
