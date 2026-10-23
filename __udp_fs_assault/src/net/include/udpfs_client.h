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
 * or a negative error. *out_size receives the file size if the
 * open succeeds. */
int udpfs_client_open(const char *path, uint32_t *out_size);

/* 512-byte sector read (BREAD): 'count' sectors from 'sector'
 * directly into 'buffer', with the reply spread over multiple packets (a single
 * request every 64 KB). Returns the bytes read (fewer than requested at end of
 * file) or a negative error: -1 if the server does not reply, -2 if it replies with an
 * error (for example handle no longer valid after the server restart). The buffer can have any alignment
 * (if it is not 4-byte aligned the copy is slower). */
int udpfs_client_bread(int handle, uint32_t sector, uint32_t count, void *buffer);

/* Network error / no usable reply (as opposed to -errno from the server). */
#define UDPFS_E_NET  (-1000)

/* OPEN / DOPEN with the original FIO flags (FIO_O_*, same bit values as the
 * UDPFS flags) and is_dir=1 for a directory. Returns the server handle (>= 0),
 * -errno if the server refused, or UDPFS_E_NET. st (optional) receives the
 * stat of the OPEN_REPLY. */
int udpfs_client_open_ex(const char *path, int fio_flags, int is_dir, udpfs_stat_t *st);

/* Next directory entry: 1 = entry (name, st), 0 = end, -errno, UDPFS_E_NET. */
int udpfs_client_dread(int handle, char *name, int name_size, udpfs_stat_t *st);

/* Server file cursor: whence 0/1/2 (SET/CUR/END). Returns 0 (and the new
 * position in *pos), -1 if the server refused, UDPFS_E_NET. */
int udpfs_client_lseek(int handle, int64_t offset, int whence, uint64_t *pos);

/* Writes 'size' bytes at the SERVER cursor (call udpfs_client_lseek first if
 * the cursor is not where you want it). Multi-packet: the first packet carries
 * WRITE_REQ plus the first chunk, every chunk but the last is confirmed by a
 * pure ACK, the last by WRITE_DONE. Returns the bytes written (possibly fewer
 * than requested), -errno from the server, or UDPFS_E_NET. */
int udpfs_client_write(int handle, const void *data, uint32_t size);

/* Directory operations: 0, -errno from the server, or UDPFS_E_NET. */
int udpfs_client_mkdir(const char *path);
int udpfs_client_remove(const char *path);
int udpfs_client_rmdir(const char *path);

/* Closes the remote file specified by the handle. */
int udpfs_client_close(int handle);

#endif /* NET_UDPFS_CLIENT_H */
