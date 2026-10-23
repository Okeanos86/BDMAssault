/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * (EMBER_BUILD, make EMBER=1: no copy is written to IOP RAM and none is
 * looked for; the card is simply read once at start.)
 *
 * net_udpfs_stash.c - IPCONFIG.DAT / UDPFSCONFIG.DAT from the memory card
 * (mc?:/SYS-CONF/IPCONFIG.DAT and mc?:/SYS-CONF/UDPFSCONFIG.DAT, mc0: then mc1:), carried over the IOP reset in IOP RAM.
 *
 * POPStarter loads this module twice: before and after an IOP reset. In the
 * first load the memory card can be read; after the reset it can not
 * (mc0: open returns -19, ENODEV). The IOP reset does not clear the RAM, so:
 *
 *  - first load (mc0: reachable): the two files are read and copied, with a
 *    signature and a checksum, into windows of IOP RAM taken with
 *    AllocSysMemory(ALLOC_ADDR), so nothing in use is overwritten;
 *  - second load (mc0: gives ENODEV): the first valid copy is used and all the
 *    valid copies are wiped.
 *
 * Three copies are kept in distant windows, because the reset (IOPRP image,
 * modules loaded before this one) may overwrite one of them. A copy is only
 * trusted if signature, size and checksum match, and only valid copies are
 * wiped (a damaged one may belong to another module by now).
 * The windows were checked on hardware (survival test, 19 of 20 windows
 * intact after the reset; the lost one was at 0x100000).
 *
 * The stashed data has priority over mass:/SYS-CONF/ (see udpfs_config.h). If
 * there is nothing (no files on the card, no copy left) the USB drive is read.
 */
#include <stdint.h>
#include <stdio.h>
#include <ioman.h>
#include <sysclib.h>
#include <sysmem.h>

#include "udpfs_config.h"

#ifndef ALLOC_ADDR
#define ALLOC_ADDR 2
#endif

#define STASH_N     3
#define STASH_SIZE  256
#define STASH_MAGIC 0x4B534455u /* "UDSK" */
#define STASH_MAX   120         /* bytes kept for each file */
#define E_NODEV     (-19)

typedef struct {
    uint32_t magic;
    uint16_t ip_len, srv_len;
    uint32_t hash;
    uint8_t data[STASH_SIZE - 12];
} stash_t;

#ifndef STASH_WINDOW
static const uintptr_t stash_addr[STASH_N] = {0x1D0000u, 0x1E0000u, 0x1F0000u};
#define STASH_WINDOW(i) ((stash_t *)stash_addr[i])
#endif

static char g_ip[STASH_MAX + 1], g_srv[STASH_MAX + 1];
static int g_ip_len, g_srv_len;
char udpfs_stash_diag[64] = "UDPFS: cfg src: -";

static uint32_t stash_hash(const stash_t *w)
{
    uint32_t h = 2166136261u;
    int i, n = w->ip_len + w->srv_len;

    h = (h ^ w->ip_len) * 16777619u;
    h = (h ^ w->srv_len) * 16777619u;
    for (i = 0; i < n; i++)
        h = (h ^ w->data[i]) * 16777619u;
    return h ^ (uint32_t)(uintptr_t)w;
}

static int stash_valid(const stash_t *w)
{
    return w->magic == STASH_MAGIC && w->ip_len <= STASH_MAX && w->srv_len <= STASH_MAX &&
           w->hash == stash_hash(w);
}

/* Reads a small file; returns its size (1..STASH_MAX), 0 if absent, empty or
 * too big, E_NODEV if the device is not there. */
static int mc_read(const char *path, char *dst)
{
    char tmp[STASH_MAX + 1];
    int fd, n;

    fd = open(path, FIO_O_RDONLY);
    if (fd < 0)
        return (fd == E_NODEV) ? E_NODEV : 0;
    n = read(fd, tmp, sizeof(tmp));
    close(fd);
    if (n < 1 || n > STASH_MAX)
        return 0;
    memcpy(dst, tmp, n);
    return n;
}

/* mc0: then mc1: for each file. */
static int mc_read_pair(const char *p0, const char *p1, char *dst)
{
    int n = mc_read(p0, dst);

    if (n == E_NODEV)
        return E_NODEV;
    if (n > 0)
        return n;
    n = mc_read(p1, dst);
    return n > 0 ? n : 0;
}

static int stash_save(void)
{
    int i, k, ok = 0;

#ifdef EMBER_BUILD
    return 0; /* Ember: one module instance, no IOP reset, nothing to carry over */
#endif

    for (i = 0; i < STASH_N; i++) {
        stash_t *w = STASH_WINDOW(i);

        if (AllocSysMemory(ALLOC_ADDR, sizeof(*w), w) == NULL)
            continue; /* window in use */
        memset(w, 0, sizeof(*w));
        w->ip_len  = (uint16_t)g_ip_len;
        w->srv_len = (uint16_t)g_srv_len;
        for (k = 0; k < g_ip_len; k++)
            w->data[k] = (uint8_t)g_ip[k];
        for (k = 0; k < g_srv_len; k++)
            w->data[g_ip_len + k] = (uint8_t)g_srv[k];
        w->hash  = stash_hash(w);
        w->magic = STASH_MAGIC; /* last: a copy is valid only when complete */
        ok++;
    }
    return ok;
}

/* Takes the first valid copy and wipes every valid one. Returns the number of
 * valid copies found (0 = nothing). */
static int stash_load(void)
{
    int i, k, found = 0;

#ifdef EMBER_BUILD
    return 0; /* Ember: never a second instance, do not look at IOP RAM */
#endif

    for (i = 0; i < STASH_N; i++) {
        stash_t *w = STASH_WINDOW(i);

        if (!stash_valid(w))
            continue;
        if (found == 0) {
            g_ip_len  = w->ip_len;
            g_srv_len = w->srv_len;
            for (k = 0; k < g_ip_len; k++)
                g_ip[k] = (char)w->data[k];
            for (k = 0; k < g_srv_len; k++)
                g_srv[k] = (char)w->data[g_ip_len + k];
        }
        found++;
        memset(w, 0, sizeof(*w));
    }
    return found;
}

void udpfs_stash_init(void)
{
    int n_ip, n_srv, nodev;

    g_ip_len = g_srv_len = 0;
    n_ip  = mc_read_pair(UDPFS_CFG_MC0_IPCONFIG_PATH, UDPFS_CFG_MC1_IPCONFIG_PATH, g_ip);
    n_srv = mc_read_pair(UDPFS_CFG_MC0_SERVER_PATH, UDPFS_CFG_MC1_SERVER_PATH, g_srv);
    nodev = (n_ip == E_NODEV || n_srv == E_NODEV);

    if (!nodev) {
        /* First load: the card is reachable. Always (re)write the copies,
         * also when no file exists, so an old copy can not be used. */
        g_ip_len  = n_ip > 0 ? n_ip : 0;
        g_srv_len = n_srv > 0 ? n_srv : 0;
        sprintf(udpfs_stash_diag, "UDPFS: cfg src: mc ip %d srv %d, stash %d", g_ip_len, g_srv_len, stash_save());
    } else {
        /* Second load: mc0: is gone, use what the first load left. */
        g_ip_len = g_srv_len = 0;
        int found = stash_load();

        sprintf(udpfs_stash_diag, "UDPFS: cfg src: stash %d copies ip %d srv %d", found, g_ip_len, g_srv_len);
    }
}

/* Config reader: the stashed data (memory card) first, the USB drive for
 * whatever the card did not provide. */
int udpfs_cfg_read_stash_or_usb(const char *path, char *buf, int size)
{
    const char *src = NULL;
    int n = 0;

    if (strcmp(path, UDPFS_CFG_IPCONFIG_PATH) == 0 && g_ip_len > 0) {
        src = g_ip;
        n   = g_ip_len;
    } else if (strcmp(path, UDPFS_CFG_SERVER_PATH) == 0 && g_srv_len > 0) {
        src = g_srv;
        n   = g_srv_len;
    }
    if (src == NULL)
        return udpfs_cfg_read_file_iop(path, buf, size);
    if (n > size)
        n = size;
    memcpy(buf, src, n);
    return n;
}
