#include "regtable_cli.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

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
    if (n > 0) {
        cli->tx.write((const uint8_t *)tmp, (uint16_t)n);
    }
}

/* -- tokeniser: splits the line in place, argv points into it -- */

static int cli_split(char *line, char **argv, int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        /* skip leading spaces */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        argv[argc++] = p;

        /* find end of token */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* -- commands -------------------------------------------- */

static void cmd_list(RegCli *cli)
{
    char vbuf[32];

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

static void cmd_get(RegCli *cli, const char *name)
{
    const RegEntry *e = reg_find(cli->table, name);
    if (!e) {
        cli_puts(cli, reg_result_str(REG_ERR_NOT_FOUND));
        cli_puts(cli, "\r\n");
        return;
    }
    char vbuf[32];
    reg_get_str(e, vbuf, sizeof(vbuf));
    cli_puts(cli, vbuf);
    cli_puts(cli, "\r\n");
}

static void cmd_set(RegCli *cli, const char *name, const char *value)
{
    const RegEntry *e = reg_find(cli->table, name);
    if (!e) {
        cli_puts(cli, reg_result_str(REG_ERR_NOT_FOUND));
        cli_puts(cli, "\r\n");
        return;
    }
    RegResult r = reg_set_str(cli->table, e, value);
    cli_puts(cli, reg_result_str(r));
    cli_puts(cli, "\r\n");
}

static void cmd_info(RegCli *cli, const char *name)
{
    const RegEntry *e = reg_find(cli->table, name);
    if (!e) {
        cli_puts(cli, reg_result_str(REG_ERR_NOT_FOUND));
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
            cli_print(cli, "range:  %.2f..%.2f\r\n", (double)e->min.f, (double)e->max.f);
            break;
        default:
            cli_print(cli, "range:  %lu..%lu\r\n",
                      (unsigned long)e->min.u, (unsigned long)e->max.u);
            break;
        }
    }
    cli_print(cli, "modbus: 0x%04X\r\n", e->modbus_addr);
    if (e->description) {
        cli_print(cli, "desc:   %s\r\n", e->description);
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
}

/* -- line processor -------------------------------------- */

static void cli_process(RegCli *cli, char *line)
{
    char *argv[REGTABLE_CLI_MAX_ARGS];
    int argc = cli_split(line, argv, REGTABLE_CLI_MAX_ARGS);

    if (argc == 0) {
        return;
    }

    if (strcmp(argv[0], "list") == 0) {
        cmd_list(cli);
    } else if (strcmp(argv[0], "get") == 0 && argc >= 2) {
        cmd_get(cli, argv[1]);
    } else if (strcmp(argv[0], "set") == 0 && argc >= 3) {
        cmd_set(cli, argv[1], argv[2]);
    } else if (strcmp(argv[0], "info") == 0 && argc >= 2) {
        cmd_info(cli, argv[1]);
    } else if (strcmp(argv[0], "help") == 0) {
        cmd_help(cli);
    } else {
        cli_puts(cli, "ERR: unknown command. type 'help'\r\n");
    }
}

/* -- public API ------------------------------------------ */

void regcli_init(RegCli *cli, RegTable *table, RegTransport tx)
{
    cli->table = table;
    cli->tx    = tx;
    cli->pos   = 0;
    cli->echo  = true;
}

void regcli_feed(RegCli *cli, uint8_t byte)
{
    /* echo */
    if (cli->echo) {
        cli->tx.write(&byte, 1);
    }

    /* line endings */
    if (byte == '\n' || byte == '\r') {
        if (cli->pos > 0) {
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

    /* normal char; beyond the buffer, keystrokes are dropped */
    if (cli->pos < REGTABLE_CLI_BUF_SIZE - 1) {
        cli->buf[cli->pos++] = (char)byte;
    }
}
