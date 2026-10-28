/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

#ifndef NET_UDPFS_CLIENT_H
#define NET_UDPFS_CLIENT_H

#include <stdint.h>
#include "ministack.h"

/* Minimal UDPFS client:
 *  - server IP from UDPFSCONFIG.DAT or discovered via broadcast; data port (and MAC,
 *    for unicast) discovered with DISCOVERY/INFORM
 *  - OPEN/CLOSE/GETSTAT with single-packet reply
 *  - BREAD: 512-byte sector reads with a multi-packet reply,
 *    written directly into the caller's buffer (window and ACK as in
 *    Neutrino's udpfs module)
 *  - retry with increasing timeout and NACK for fast recovery of a
 *    lost reply
 *  - incoming packets read via PIO from the SMAP RX FIFO registers */

#define UDPFS_PORT        0xF5F6

/* Size of a path buffer, terminating NUL included: paths up to 255
 * characters. An OPEN/GETSTAT request (path + 8 bytes) must fit in the
 * 512-byte payload of a single packet. */
#define UDPFS_MAX_PATH    256

/* Stat of a remote file (from GETSTAT_REPLY). */
typedef struct {
    uint32_t mode;
    uint32_t attr;
    uint32_t size;
    uint32_t hisize;
    uint8_t  ctime[8];
    uint8_t  atime[8];
    uint8_t  mtime[8];
} udpfs_stat_t;


/* Fixed server IP (from UDPFSCONFIG.DAT). Without this call the server
 * is looked up with a broadcast DISCOVERY. Call before udpfs_client_init. */
void udpfs_client_set_server_ip(uint32_t ip);

/* Creates the semaphore that serializes network requests. Call ONCE,
 * BEFORE registering the "mass" driver (i.e. before InitFS):
 * fs_open/fs_getstat can arrive at any time after AddDrv. */
void udpfs_client_preinit(void);

/* Call if the network does not come up (dev9/smap failed): from then on
 * every UDPFS request fails immediately instead of waiting for the timeout. */
void udpfs_client_set_failed(void);

/* Stat of a file on the server. Returns 0 if it exists, <0 otherwise
 * (-errno from the server, or -1 for network error/timeout). */
int udpfs_client_getstat(const char *path, udpfs_stat_t *st);

/* Initializes the client: bind of the UDP socket. Call only once,
 * after smap_init() has succeeded. */
void udpfs_client_init(void);

/* Opens a remote file. Returns the handle assigned by the server (>= 0),
 * -2 if the server answered with an error (for example the file does not
 * exist) or -1 if there was no usable answer (network down, timeout).
 * *out_size receives the file size if the open succeeds. */
int udpfs_client_open(const char *path, uint32_t *out_size);

/* 512-byte sector read (BREAD): 'count' sectors from 'sector'
 * directly into 'buffer', with the reply spread over multiple packets (a single
 * request every 64 KB). Returns the bytes read (fewer than requested at end of
 * file) or a negative error: -1 if the server does not reply, -2 if it replies with an
 * error (for example handle no longer valid after the server restart). The buffer can have any alignment
 * (if it is not 4-byte aligned the copy is slower). */
int udpfs_client_bread(int handle, uint32_t sector, uint32_t count, void *buffer);

/* Closes the remote file specified by the handle. */
int udpfs_client_close(int handle);

#endif /* NET_UDPFS_CLIENT_H */
