/*
 * regtable as a Modbus TCP slave on the desktop. No board, no
 * serial port: any Modbus TCP master on this machine can connect.
 *
 *   Build + run:  make run-tcp      (or .\build tcpslave, then .\tcpslave)
 *   Connect:      QModMaster / pymodbus to 127.0.0.1 port 1502
 *                 (502 is the registered port but needs admin rights)
 *
 * Register map (same shape as the Arduino example):
 *   word 1    led       BOOL RW    on_change prints when it flips
 *   word 2    counter   U16  RO    on_read increments per read
 *   word 3-4  uptime    U32  RO    seconds since start
 *   word 5-6  gain      FLOAT RW   range 0.5 .. 2.5
 *
 * The socket loop below is the TCP counterpart of t3.5 framing on
 * serial: read the 7-byte MBAP header, let its length field say how
 * many bytes complete the ADU, hand the whole ADU to
 * regmb_process_tcp, send back whatever it returns.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "regtable_modbus.h"

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

#define PORT 1502

/* -- device state ------------------------------------------- */

static uint8_t  led     = 0;
static uint16_t counter = 0;
static uint32_t uptime  = 0;
static float    gain    = 1.0f;
static time_t   started;

static void led_changed(const RegEntry *e)
{
    (void)e;
    printf("led -> %s\n", led ? "on" : "off");
}

static void counter_read(const RegEntry *e)
{
    (void)e;
    counter++;
}

static void uptime_read(const RegEntry *e)
{
    (void)e;
    uptime = (uint32_t)(time(NULL) - started);
}

static const RegEntry registry[] = {
    { .name = "led",     .ptr = &led,     .type = REG_BOOL,  .perm = REG_RW,
      .modbus_addr = 1, .on_change = led_changed,
      .description = "Demo LED" },
    { .name = "counter", .ptr = &counter, .type = REG_U16,   .perm = REG_RO,
      .modbus_addr = 2, .on_read = counter_read,
      .description = "Increments per read" },
    { .name = "uptime",  .ptr = &uptime,  .type = REG_U32,   .perm = REG_RO,
      .modbus_addr = 3, .on_read = uptime_read,
      .description = "Seconds since start" },
    { .name = "gain",    .ptr = &gain,    .type = REG_FLOAT, .perm = REG_RW,
      .min.f = 0.5f, .max.f = 2.5f, .modbus_addr = 5,
      .description = "Demo float" },
    { .name = NULL }
};

/* -- one complete ADU off the stream ------------------------ */

static int send_all(sock_t s, const uint8_t *buf, int n)
{
    int sent = 0;
    while (sent < n) {
        int r = send(s, (const char *)buf + sent, n - sent, 0);
        if (r <= 0) return 0;              /* send may be partial */
        sent += r;
    }
    return 1;
}

static int read_exact(sock_t s, uint8_t *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(s, (char *)buf + got, n - got, 0);
        if (r <= 0) return 0;              /* closed or error */
        got += r;
    }
    return 1;
}

/*  Reads MBAP + PDU into adu, returns the ADU length (0 = connection
 *  gone, -1 = unusable header, drop the connection). */
static int read_adu(sock_t s, uint8_t *adu)
{
    if (!read_exact(s, adu, 7)) return 0;
    uint16_t mlen = (uint16_t)((adu[4] << 8) | adu[5]);
    if (mlen < 2 || 6 + mlen > REGMB_TCP_ADU_MAX) return -1;
    if (!read_exact(s, adu + 7, mlen - 1)) return 0;
    return 6 + mlen;
}

int main(void)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    started = time(NULL);

    static RegTable  table;
    static RegModbus mb;
    reg_table_init(&table, registry);
    regmb_init(&mb, &table, 1);

    sock_t lsock = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(PORT);
    if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(lsock, 1) != 0) {
        printf("cannot listen on 127.0.0.1:%d\n", PORT);
        return 1;
    }
    printf("Modbus TCP slave on 127.0.0.1:%d, unit id ignored.\n", PORT);
    printf("Map: 1=led(RW) 2=counter 3-4=uptime 5-6=gain(RW float)\n");

    for (;;) {
        sock_t c = accept(lsock, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        printf("master connected\n");

        uint8_t adu[REGMB_TCP_ADU_MAX];
        uint8_t resp[REGMB_TCP_ADU_MAX];
        for (;;) {
            int len = read_adu(c, adu);
            if (len <= 0) break;
            uint16_t n = regmb_process_tcp(&mb, adu, (uint16_t)len,
                                           resp, sizeof(resp));
            if (n > 0 && !send_all(c, resp, n)) break;
            reg_poll(&table);              /* deferred on_change hooks */
        }
        CLOSESOCK(c);
        printf("master disconnected\n");
    }
}
