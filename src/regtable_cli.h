#ifndef REGTABLE_CLI_H
#define REGTABLE_CLI_H

#include "regtable_core.h"

#ifdef __cplusplus
extern "C" {
#endif


/* -- configuration --------------------------------------- */
#ifndef REGTABLE_CLI_BUF_SIZE
#define REGTABLE_CLI_BUF_SIZE 128
#endif

#ifndef REGTABLE_CLI_MAX_ARGS
#define REGTABLE_CLI_MAX_ARGS 4
#endif

/* -- CLI context ----------------------------------------- */
/*  One CLI session: the table it serves, the transport it
 *  answers on, and the line being typed. */
typedef struct RegCli {
    RegTable       *table;      /* the table this CLI serves      */
    RegTransport    tx;         /* output goes through tx.write   */
    char            buf[REGTABLE_CLI_BUF_SIZE];  /* current line  */
    uint16_t        pos;        /* chars in buf so far            */
    bool            echo;       /* echo typed chars back (default on) */
    bool            overflow;   /* line outgrew buf: reject it at line end */
} RegCli;

/* -- API ------------------------------------------------- */

/*  Initialise CLI context. */
void regcli_init(RegCli *cli, RegTable *table, RegTransport tx);

/*  Feed one received byte. Handles echo, backspace, and line
 *  ending; on '\n' or '\r' the line is parsed and executed,
 *  so hooks run in the caller's context. Call from the main loop. */
void regcli_feed(RegCli *cli, uint8_t byte);

#ifdef __cplusplus
}
#endif

#endif /* REGTABLE_CLI_H */
