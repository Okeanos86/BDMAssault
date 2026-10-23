/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * net_udpfs_config.c - reading of IPCONFIG.DAT and UDPFSCONFIG.DAT, see
 * udpfs_config.h for format and search order.
 */
#include <stdint.h>
#include <stdio.h>

#include "udpfs_config.h"
#include "udpfs_client.h"
#include "ministack.h"

/* Diagnostics: filled by udpfs_config_apply, printed by net_init_thread once
 * the UDP tty is up (the config is read before the network exists). */
int  udpfs_cfg_last_err;
char udpfs_cfg_diag[128];
volatile int udpfs_cfg_done; /* set when udpfs_config_apply has finished */
char udpfs_cfg_diag2[64]; /* server IP result, see udpfs_config_apply */

#ifndef UDPFS_CFG_HOST
#include <thbase.h>
#include <sysclib.h>
#endif

#define CFG_BUF_SIZE 256

#if defined(DEBUG) && !defined(UDPFS_CFG_HOST)
static unsigned int g_mark_ms[UDPFS_M_COUNT];
static unsigned int g_mark_set;

void udpfs_mark(int id)
{
    iop_sys_clock_t clk;
    u32 sec, usec;

    GetSystemTime(&clk);
    SysClock2USec(&clk, &sec, &usec);
    g_mark_ms[id] = sec * 1000u + usec / 1000u;
    g_mark_set |= 1u << id;
}

/* Milliseconds since the first mark (start of this module instance). Lines
 * are short: the UDP tty truncates at ~64 characters. */
void udpfs_marks_print(void)
{
    udpfs_dbg_print();

    static const char *const names[UDPFS_M_COUNT] = {"start", "mc", "ip", "cfg", "dev9", "smap", "link", "disc", "ready"};
    char line[64];
    int i, n = 0;

    for (i = 1; i < UDPFS_M_COUNT; i++) {
        if (!(g_mark_set & (1u << i)))
            continue;
        if (n == 0)
            sprintf(line, "UDPFS: t(ms)");
        sprintf(line + strlen(line), " %s %u", names[i], g_mark_ms[i] - g_mark_ms[0]);
        if (++n == 4) {
            printf("%s\n", line);
            n = 0;
        }
    }
    if (n)
        printf("%s\n", line);
}
#endif


/* A token from s to end (excluded) -> strict IPv4 "a.b.c.d". */
static int cfg_token_ip(const char *s, const char *end, uint32_t *out)
{
    uint32_t ip = 0;
    int group;

    for (group = 0; group < 4; group++) {
        uint32_t v = 0;
        int digits = 0;

        while (s < end && *s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            if (++digits > 3 || v > 255)
                return -1;
            s++;
        }
        if (digits == 0)
            return -1;
        ip = (ip << 8) | v;
        if (group < 3) {
            if (s >= end || *s != '.')
                return -1;
            s++;
        }
    }
    if (s != end)
        return -1;
    *out = ip;
    return 0;
}

/* First meaningful line (skips BOM, empty lines and '#' / ';' comments)
 * split into tokens separated by spaces/tabs. Returns the number of tokens. */
#define CFG_MAX_TOKENS 4
static int cfg_tokens(const char *buf, int len, const char *ts[], const char *te[])
{
    const char *p = buf;
    const char *end = buf + len;
    int n = 0;

    if (len >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
        p += 3;

    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            p++;
        if (p >= end || *p == '\0')
            return 0;
        if (*p == '#' || *p == ';') {
            while (p < end && *p != '\n')
                p++;
            continue;
        }
        break;
    }

    while (p < end && *p != '\n' && *p != '\r' && *p != '\0') {
        const char *s;

        while (p < end && (*p == ' ' || *p == '\t'))
            p++;
        if (p >= end || *p == '\n' || *p == '\r' || *p == '\0')
            break;
        s = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\0')
            p++;
        if (n < CFG_MAX_TOKENS) {
            ts[n] = s;
            te[n] = p;
            n++;
        } else {
            break;   /* extra fields (e.g. DNS): ignored */
        }
    }
    return n;
}

static int cfg_host_ip_ok(uint32_t ip)
{
    uint32_t first = ip >> 24;

    if (first == 0 || first == 127 || first >= 224)
        return 0;
    return 1;
}

static int cfg_mask_ok(uint32_t mask)
{
    uint32_t inv = ~mask;

    if (mask == 0 || mask == 0xFFFFFFFFu)
        return 0;
    return (inv & (inv + 1)) == 0;   /* ones contiguous on the left */
}

int udpfs_cfg_parse_ipconfig(const char *buf, int len, udpfs_ipcfg_t *out)
{
    const char *ts[CFG_MAX_TOKENS], *te[CFG_MAX_TOKENS];
    uint32_t ip, mask, gw = 0, host;
    int n;

    n = cfg_tokens(buf, len, ts, te);
    if (n < 2)
        return -1;
    if (cfg_token_ip(ts[0], te[0], &ip) != 0 || !cfg_host_ip_ok(ip))
        return -1;
    if (cfg_token_ip(ts[1], te[1], &mask) != 0 || !cfg_mask_ok(mask))
        return -1;
    host = ip & ~mask;
    if (host == 0 || host == ~mask)
        return -1;      /* network or broadcast address */
    if (n >= 3 && cfg_token_ip(ts[2], te[2], &gw) != 0)
        gw = 0;         /* invalid gateway: ignored */

    out->ip = ip;
    out->mask = mask;
    out->gw = gw;
    return 0;
}

int udpfs_cfg_parse_server(const char *buf, int len, uint32_t *ip)
{
    const char *ts[CFG_MAX_TOKENS], *te[CFG_MAX_TOKENS];
    uint32_t v;

    if (cfg_tokens(buf, len, ts, te) < 1)
        return -1;
    if (cfg_token_ip(ts[0], te[0], &v) != 0 || !cfg_host_ip_ok(v))
        return -1;
    *ip = v;
    return 0;
}

int udpfs_cfg_same_subnet(uint32_t a, uint32_t b, uint32_t mask)
{
    return ((a ^ b) & mask) == 0;
}

int udpfs_cfg_load_ipconfig(udpfs_cfg_read_fn rd, udpfs_ipcfg_t *out, const char **where)
{
    static const char *const paths[] = {
        UDPFS_CFG_IPCONFIG_PATH,
    };
    char buf[CFG_BUF_SIZE];
    unsigned i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int n = rd(paths[i], buf, sizeof(buf));

        if (n > 0 && udpfs_cfg_parse_ipconfig(buf, n, out) == 0) {
            if (where)
                *where = paths[i];
            return 0;
        }
    }
    return -1;
}

int udpfs_cfg_load_server(udpfs_cfg_read_fn rd, uint32_t *ip, const char **where)
{
    static const char *const paths[] = {
        UDPFS_CFG_SERVER_PATH,
    };
    char buf[CFG_BUF_SIZE];
    unsigned i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        int n = rd(paths[i], buf, sizeof(buf));

        if (n > 0 && udpfs_cfg_parse_server(buf, n, ip) == 0) {
            if (where)
                *where = paths[i];
            return 0;
        }
    }
    return -1;
}

static void fmt_ip(char *dst, uint32_t ip)
{
    sprintf(dst, "%d.%d.%d.%d", (int)(ip >> 24), (int)((ip >> 16) & 255), (int)((ip >> 8) & 255), (int)(ip & 255));
}

/* rc: 0 = server file valid (srv), other = no file. Applies the
 * fixed server (subnet check), fills the diagnostics and marks the
 * configuration as complete. */
static void cfg_server_done(int rc, uint32_t srv, const udpfs_ipcfg_t *ipc)
{
    char ipstr[20];

    if (rc == 0) {
        if (udpfs_cfg_same_subnet(srv, ipc->ip, ipc->mask)) {
            udpfs_client_set_server_ip(srv);
            fmt_ip(ipstr, srv);
            sprintf(udpfs_cfg_diag2, "UDPFS: server: %s (UDPFSCONFIG.DAT)", ipstr);
        } else {
            sprintf(udpfs_cfg_diag2, "UDPFS: server: outside subnet, broadcast");
        }
    }
    UDPFS_MARK(UDPFS_M_CFG);
    udpfs_cfg_done = 1;
}

int udpfs_config_apply(udpfs_cfg_read_fn rd)
{
    udpfs_ipcfg_t ipc;
    const char *where = NULL;
    uint32_t srv;
    char ipstr[20];
    int rc;

    udpfs_cfg_last_err = 0;
    rc = udpfs_cfg_load_ipconfig(rd, &ipc, &where);
    if (rc != 0) {
        /* No valid file: default IP, like the UDPBD and Neutrino
         * ministack (192.168.1.10), to stay compatible. */
        ipc.ip   = UDPFS_DEFAULT_PS2_IP;
        ipc.mask = UDPFS_DEFAULT_PS2_MASK;
        ipc.gw   = 0;
        where    = NULL;
    }
    ms_ip_set_ip(ipc.ip);
    UDPFS_MARK(UDPFS_M_IP);

    /* Messages are stored and printed later, once the UDP tty is up (lines
     * printed earlier are lost). Short on purpose: the tty truncates at ~64. */
    fmt_ip(ipstr, ipc.ip);
    sprintf(udpfs_cfg_diag, "UDPFS: PS2 IP %s (%s)", ipstr, where ? "IPCONFIG.DAT" : "default");

    sprintf(udpfs_cfg_diag2, "UDPFS: server: broadcast");
    rc = udpfs_cfg_load_server(rd, &srv, &where);
    cfg_server_done(rc, srv, &ipc);
    return 0;
}
