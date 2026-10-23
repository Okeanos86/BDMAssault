/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

#ifndef NET_UDPFS_CONFIG_H
#define NET_UDPFS_CONFIG_H

#include <stdint.h>

/* Configuration: two optional files on the memory card, in mc0:/SYS-CONF/
 * then mc1:/SYS-CONF/ (see net_udpfs_stash.c). Each file is looked up
 * separately.
 *
 *  IPCONFIG.DAT   PS2 IP, one line:  "IP NETMASK [GATEWAY]"
 *                 e.g. "192.168.1.10 255.255.255.0 192.168.1.1"
 *                 An unreadable or invalid file is treated as missing.
 *                 If no file is valid the default IP is used:
 *                 192.168.1.10 / 255.255.255.0 (like UDPBD and Neutrino).
 *  UDPFSCONFIG.DAT  Server IP, e.g. "192.168.1.11". If present it takes
 *                 precedence over broadcast; if missing (or not in the PS2's
 *                 subnet) the server is looked up with a broadcast DISCOVERY.
 *
 * The gateway is read but not used: the ministack does no routing, the
 * server must be on the same network segment. */

/* Names the files are asked for through the reader (udpfs_cfg_read_fn). */
#define UDPFS_CFG_IPCONFIG_PATH "IPCONFIG.DAT"
#define UDPFS_CFG_SERVER_PATH   "UDPFSCONFIG.DAT"

/* Where they are on the memory card, read in the first load of the module and
 * carried over the IOP reset in IOP RAM (see net_udpfs_stash.c). */
#define UDPFS_CFG_MC0_IPCONFIG_PATH "mc0:/SYS-CONF/IPCONFIG.DAT"
#define UDPFS_CFG_MC1_IPCONFIG_PATH "mc1:/SYS-CONF/IPCONFIG.DAT"
#define UDPFS_CFG_MC0_SERVER_PATH   "mc0:/SYS-CONF/UDPFSCONFIG.DAT"
#define UDPFS_CFG_MC1_SERVER_PATH   "mc1:/SYS-CONF/UDPFSCONFIG.DAT"

#define UDPFS_DEFAULT_PS2_IP   0xC0A8010Au   /* 192.168.1.10 */
#define UDPFS_DEFAULT_PS2_MASK 0xFFFFFF00u   /* 255.255.255.0 */

typedef struct {
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;      /* 0 if absent or invalid */
} udpfs_ipcfg_t;

/* Reads a file: returns the bytes read (>= 0) or < 0 if missing/error.
 * The content is not NUL-terminated. */
typedef int (*udpfs_cfg_read_fn)(const char *path, char *buf, int size);

/* Pure parsers (testable on the host). Return 0 if valid. */
int udpfs_cfg_parse_ipconfig(const char *buf, int len, udpfs_ipcfg_t *out);
int udpfs_cfg_parse_server(const char *buf, int len, uint32_t *ip);
int udpfs_cfg_same_subnet(uint32_t a, uint32_t b, uint32_t mask);

/* Search in the paths above. 'where' (optional) receives the path used. */
int udpfs_cfg_load_ipconfig(udpfs_cfg_read_fn rd, udpfs_ipcfg_t *out, const char **where);
int udpfs_cfg_load_server(udpfs_cfg_read_fn rd, uint32_t *ip, const char **where);

#ifndef UDPFS_CFG_HOST
/* net_udpfs_stash.c: call once, early in _start, before anything else. */
void udpfs_stash_init(void);
/* Reader for udpfs_config_apply: the memory card data kept by udpfs_stash_init. */
int udpfs_cfg_read_stash(const char *path, char *buf, int size);
extern char udpfs_stash_diag[64];
#endif

/* Sets the PS2 IP (ministack) and, if present, the fixed server IP.
 * Always returns 0: without a valid IPCONFIG.DAT it uses the default IP. */
int udpfs_config_apply(udpfs_cfg_read_fn rd);


/* Text describing how the config was obtained (for the debug log). */
extern char udpfs_cfg_diag[128];
extern char udpfs_cfg_diag2[64]; /* "UDPFS: server: ..." result of UDPFSCONFIG.DAT */

/* Debug builds only: timestamps of the start-up phases, printed once by
 * udpfs_marks_print() when the client is ready (the tty is not up earlier). */
#if defined(DEBUG) && !defined(UDPFS_CFG_HOST)
enum { UDPFS_M_START, UDPFS_M_MC, /* memory card config read */ UDPFS_M_IP, UDPFS_M_CFG, UDPFS_M_DEV9, UDPFS_M_SMAP, UDPFS_M_LINK, UDPFS_M_DISC, UDPFS_M_READY, UDPFS_M_COUNT };
void udpfs_mark(int id);
void udpfs_marks_print(void);
/* net_dbg.c: arguments the module was started with; printed together with
 * the timings. */
void udpfs_dbg_capture(int argc, char *argv[]);
void udpfs_dbg_print(void);
#define UDPFS_MARK(id) udpfs_mark(id)
#else
#define UDPFS_MARK(id) ((void)0)
#define udpfs_marks_print() ((void)0)
#endif

#endif /* NET_UDPFS_CONFIG_H */
