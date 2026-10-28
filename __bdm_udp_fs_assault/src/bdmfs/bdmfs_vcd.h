/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * bdmfs_vcd.h - POPStarter VCD synthesized from a BIN/CUE.
 *
 * When POPS/<game>.VCD does not exist, <game>.cue and the .bin file(s) it
 * names (in the same folder) are presented as if they were a VCD: a 1 MiB
 * header generated in RAM (vcd_cue.c) followed by the .bin files in cue order.
 *
 * The same code serves the two places the files can be: the USB drive
 * (bdmfs_vcd_usb.c, through FatFs) and the UDPFS server (bdmfs_vcd_net.c,
 * through the UDPFS fallback). Each one supplies a vcd_ops_t: the code here
 * only needs to open a file read-only, read at an offset and close it.
 *
 * A handle returned by vcd_virtual_open() is recognised by
 * vcd_virtual_is_ours() (range check on a static array, no dereferencing),
 * like the UDPFS fallback slots.
 */
#ifndef BDMFS_VCD_H
#define BDMFS_VCD_H

#include <errno.h>
#include <stdint.h>

#ifdef EMBER_BUILD

/* Ember reads the .cue and .bin files itself: nothing is compiled in. */
#define vcd_virtual_is_ours(p)          0
#define vcd_virtual_read(p, b, s)       (-ENOENT)
#define vcd_virtual_lseek64(p, o, w, r) (-ENOENT)
#define vcd_virtual_close(p)            0

#else

typedef struct vcd_ops {
    /* 1 if file names are case insensitive (FatFs): only ".cue" is tried,
     * not ".CUE" as well. */
    int ci;
    /* Opens a file read-only. Returns 0 with its size and a handle, -ENOENT
     * if it does not exist, another negative errno if the backend failed
     * (drive not there, network down, no free handle). */
    int (*open)(const char *path, uint32_t *size, void **h);
    /* Reads up to n bytes at byte offset pos. Returns the bytes read (0 at
     * the end of the file) or a negative errno. */
    int (*read)(void *h, uint32_t pos, void *buf, int n);
    void (*close)(void *h);
} vcd_ops_t;

/* "name" is the path of the .VCD that was asked for, in the form the
 * backend wants (the USB path with its unit prefix, the server path as it
 * came). Returns 0 with *slot set to the handle to keep in fd->privdata, or
 * a negative errno: -ENOENT if there is no usable cue sheet there. */
int vcd_virtual_open(const vcd_ops_t *ops, const char *name, void **slot);

int vcd_virtual_is_ours(const void *p);
int vcd_virtual_read(void *p, void *buf, int size);
int vcd_virtual_lseek64(void *p, int64_t offset, int whence, int64_t *out_pos);
int vcd_virtual_close(void *p);

/* The two backends. */
const vcd_ops_t *vcd_usb_ops(void (*lock)(void), void (*unlock)(void), void *(*alloc_fil)(void));
extern const vcd_ops_t vcd_net_ops;

#endif /* EMBER_BUILD */

#endif /* BDMFS_VCD_H */
