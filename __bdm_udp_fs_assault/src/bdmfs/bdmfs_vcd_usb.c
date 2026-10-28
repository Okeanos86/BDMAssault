/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * bdmfs_vcd_usb.c - the USB drive (FatFs) as the place where the .cue and
 * .bin files of a virtual VCD are, see bdmfs_vcd.h.
 *
 * FatFs reads at any byte offset, so unlike the UDPFS server there is no
 * alignment to care about: a read is an f_lseek (skipped when the file is
 * already there) and one f_read. The FIL objects come from the driver's own
 * pool (alloc_fil) and every FatFs call runs under the driver's lock, which
 * is private to bdmfs_fs_driver.c and handed over by vcd_usb_ops().
 */
#ifndef EMBER_BUILD

#include <errno.h>
#include <stddef.h>

#include "include/ff.h"
#include "bdmfs_vcd.h"

static void (*g_lock)(void);
static void (*g_unlock)(void);
static void *(*g_alloc)(void);

static int usb_open(const char *path, uint32_t *size, void **h)
{
    FIL *f;
    FRESULT r;
    int rc;

    g_lock();
    f = (FIL *)g_alloc();
    if (f == NULL) {
        g_unlock();
        return -EMFILE;
    }
    r = f_open(f, path, FA_READ | FA_OPEN_EXISTING);
    if (r != FR_OK) {
        g_unlock();
        /* a name FatFs cannot represent cannot exist on the drive either */
        return (r == FR_NO_FILE || r == FR_NO_PATH || r == FR_INVALID_NAME) ? -ENOENT : -EIO;
    }
    if ((uint64_t)f_size(f) > 0xFFFFFFFFu) {    /* exFAT: bigger than a VCD can be */
        f_close(f);
        rc = -EINVAL;
    } else {
        *size = (uint32_t)f_size(f);
        *h    = f;
        rc    = 0;
    }
    g_unlock();
    return rc;
}

static int usb_read(void *h, uint32_t pos, void *buf, int n)
{
    FIL *f = (FIL *)h;
    FRESULT r = FR_OK;
    UINT br = 0;

    g_lock();
    if (f->fptr != (FSIZE_t)pos)
        r = f_lseek(f, pos);
    if (r == FR_OK)
        r = f_read(f, buf, (UINT)n, &br);
    g_unlock();
    return (r == FR_OK) ? (int)br : -EIO;
}

static void usb_close(void *h)
{
    g_lock();
    f_close((FIL *)h);
    g_unlock();
}

static const vcd_ops_t g_usb_ops = {1, usb_open, usb_read, usb_close};

const vcd_ops_t *vcd_usb_ops(void (*lock)(void), void (*unlock)(void), void *(*alloc_fil)(void))
{
    g_lock   = lock;
    g_unlock = unlock;
    g_alloc  = alloc_fil;
    return &g_usb_ops;
}

#endif /* EMBER_BUILD */
