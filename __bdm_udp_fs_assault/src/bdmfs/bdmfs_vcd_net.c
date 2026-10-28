/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * bdmfs_vcd_net.c - the UDPFS server as the place where the .cue and .bin
 * files of a virtual VCD are, see bdmfs_vcd.h. Every file is one slot of the
 * UDPFS fallback (udpfs_stub.h), which already does the BREAD reads (aligned
 * body straight into the caller's buffer, head and tail through a one-sector
 * cache) and reopens a file by path when the server was restarted.
 */
#ifndef EMBER_BUILD

#include <errno.h>
#include <iomanX.h>

#include "udpfs_stub.h"
#include "bdmfs_vcd.h"

static int net_open(const char *path, uint32_t *size, void **h)
{
    int err = 0;
    void *s = udpfs_stub_open_ex(path, 1, &err);

    if (s == NULL)
        return err < 0 ? err : -EIO;
    *size = udpfs_stub_size(s);
    *h    = s;
    return 0;
}

static int net_read(void *h, uint32_t pos, void *buf, int n)
{
    s64 np = 0;

    if (udpfs_stub_lseek64(h, (s64)pos, 0, &np) != 0)
        return -EIO;
    return udpfs_stub_read(h, buf, n);
}

static void net_close(void *h)
{
    udpfs_stub_close(h);
}

/* the server is case sensitive: ".cue", then ".CUE" */
const vcd_ops_t vcd_net_ops = {0, net_open, net_read, net_close};

#endif /* EMBER_BUILD */
