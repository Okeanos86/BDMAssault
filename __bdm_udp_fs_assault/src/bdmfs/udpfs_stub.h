/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * udpfs_stub.h - UDPFS read-only fallback for the BDMFS "mass" driver.
 *
 * When a read-only open/getstat of a POPS/POPSx/<name>.VCD or
 * EMBER/games/<folder>/<name>.cue|.bin path fails on the
 * USB filesystem (FatFs), fs_open/fs_getstat ask the UDPFS client
 * (net/net_udpfs_client.c) for the same path. A handle returned by this stub
 * is recognised by udpfs_stub_is_ours() and served with BREAD requests;
 * everything else keeps going to FatFs unchanged. If the server does not have
 * the file either, the original FatFs error is returned.
 */
#ifndef UDPFS_STUB_H
#define UDPFS_STUB_H

#include <iomanX.h>

/* One slot per open UDPFS file: a plain .VCD uses one, a virtual VCD made of
 * a cue sheet and several .bin files (bdmfs_vcd.h) up to three at a time. */
#define UDPFS_STUB_MAX_HANDLES 6

/* Returns 1 if "name" is a file for which the UDPFS fallback is allowed,
 * otherwise 0. One rule per build (case-insensitive comparison, leading
 * slashes ignored):
 *   - POPStarter build (default): POPS/<x>.VCD and POPS0/ ... POPS9/<x>.VCD
 *     (the folders POPStarter scans at startup)
 *   - Ember build (make EMBER=1): EMBER/games/<folder>/<x>.cue and <x>.bin
 *     (one folder level)
 * It does NOT say that the file
 * exists: the fallback is only attempted after FatFs has failed. */
int udpfs_stub_path_matches(const char *name);

/* Allocates a slot and opens the REAL file over the network (UDPFS client).
 * Returns an opaque pointer to put in fd->privdata, or NULL if the
 * open fails (network not ready, timeout, file not found on the
 * server side, slot pool full). */
void *udpfs_stub_open(const char *path);

/* Same, and says why it failed: *err = 0, -ENOENT (the server answered that
 * it has no such file), -EIO (no answer: network down, server off) or
 * -EMFILE (slot pool full). need_size = 1 makes the open fail if the size of
 * the file cannot be found out. */
void *udpfs_stub_open_ex(const char *path, int need_size, int *err);

/* Size of the file of an open slot (0 = unknown). */
unsigned int udpfs_stub_size(void *privdata);

/* Checks whether an fd->privdata pointer belongs to the fake slot pool
 * (range-check on the static array, no dereferencing). */
int udpfs_stub_is_ours(void *privdata);

int udpfs_stub_read(void *privdata, void *buffer, int size);
int udpfs_stub_lseek64(void *privdata, s64 offset, int whence, s64 *out_pos);
int udpfs_stub_close(void *privdata);

/* Stat via GETSTAT on the server (used by fs_getstat, which works by path,
 * not by handle). Returns 0 and fills stat if the file exists on the server,
 * <0 otherwise. */
int udpfs_stub_getstat(const char *path, iox_stat_t *stat);

#endif /* UDPFS_STUB_H */
