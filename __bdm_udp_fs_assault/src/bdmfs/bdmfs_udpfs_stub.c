/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * udpfs_stub.c - read-only UDPFS fallback for the BDMFS "mass" driver,
 * see udpfs_stub.h for the purpose.
 */
#include <errno.h>
#include <intrman.h>
#include <iomanX.h>
#include <sysclib.h>

#include "udpfs_stub.h"
#include "udpfs_client.h"


/* ---------------------------------------------------------------- slots */

typedef struct {
    int in_use;
    int server_handle;   /* handle assigned by the server via OPEN_REQ */
    unsigned int position; /* logical file position: lseek updates it locally only */
    unsigned int size;   /* real file size (from OPEN_REPLY; 0 if unknown) */
    char path[128];      /* to reopen the file if the server was restarted */
} udpfs_stub_slot_t;

static udpfs_stub_slot_t g_slots[UDPFS_STUB_MAX_HANDLES];

/* Case-insensitive prefix comparison: "prefix" is UPPERCASE.
 * toupper is already imported via sysclib. */
static int udpfs_ci_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (toupper((unsigned char)*s) != (unsigned char)*prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

int udpfs_stub_path_matches(const char *name)
{
    size_t len;
    size_t prefix_len;

    if (name == NULL)
        return 0;

    /* "/POPS/X.VCD", "POPS/X.VCD" and upper/lower case variants:
     * the exact format in which POPStarter passes the path is not
     * guaranteed, and the comparison used to be case-sensitive and
     * required exactly one leading slash. The path sent to the server
     * remains the original one passed by fs_open/fs_getstat: the
     * server itself strips the leading slashes (resolvePath). */
    while (*name == '/')
        name++;

    len = strlen(name);
    if (len < 4)
        return 0;

    if (!udpfs_ci_prefix(name, "POPS"))
        return 0;

    /* The folders POPStarter scans at startup: POPS/, and the numbered
     * variants POPS0/ ... POPS9/ (multi-disc / alternate slots). Only
     * one character after "POPS": either the slash right away, or a
     * digit followed by the slash - no other case (e.g. "POPSTARTER/"
     * must not match). */
    if (name[4] == '/') {
        prefix_len = 5; /* "POPS/" */
    } else if (len >= 6 && name[4] >= '0' && name[4] <= '9' && name[5] == '/') {
        prefix_len = 6; /* "POPSx/" */
    } else {
        return 0;
    }

    if (len < prefix_len + 5) /* at least "X.VCD" after the prefix */
        return 0;

    return udpfs_ci_prefix(name + len - 4, ".VCD");
}


void *udpfs_stub_open(const char *path)
{
    int i;
    int handle;
    uint32_t size = 0;

    /* The actual open over the network happens here: a single attempt,
     * no retry. If the network is not ready yet or the server does not
     * reply, the open fails and fs_open() returns an error to
     * POPStarter, exactly as the original driver does when the file
     * does not exist. */
    handle = udpfs_client_open(path, &size);
    if (handle < 0)
        return NULL;

    /* The slot scan must be protected: fs_open can be called from
     * several IOP threads, and in_use is read/written without any
     * other lock. Very short critical section: no network I/O in
     * here, that was already done above. */
    {
        int OldState;
        udpfs_stub_slot_t *found = NULL;

        CpuSuspendIntr(&OldState);
        for (i = 0; i < UDPFS_STUB_MAX_HANDLES; i++) {
            if (!g_slots[i].in_use) {
                g_slots[i].in_use        = 1;
                g_slots[i].server_handle = handle;
                g_slots[i].position      = 0;
                g_slots[i].size          = size;
                {
                    int k;
                    for (k = 0; k < (int)sizeof(g_slots[i].path) - 1 && path[k]; k++)
                        g_slots[i].path[k] = path[k];
                    g_slots[i].path[k] = 0;
                }
                found = &g_slots[i];
                break;
            }
        }
        CpuResumeIntr(OldState);

        if (found != NULL)
            return found;
    }

    /* slot pool full: now that CLOSE_REQ exists (see below), we free
     * the handle on the server right away instead of leaving it open
     * until restart. */
    udpfs_client_close(handle);
    return NULL;
}

int udpfs_stub_is_ours(void *privdata)
{
    return (privdata >= (void *)&g_slots[0] &&
            privdata < (void *)&g_slots[UDPFS_STUB_MAX_HANDLES]);
}

/* POPStarter read, always with BREAD (512-byte sectors, offset in every
 * request: the server cursor is not needed).
 *
 * The aligned part (position and size multiples of 512: all reads in the
 * tracks, 37888 and 38400 bytes) goes directly into the caller's buffer,
 * one request every 64 KB. Unaligned reads (VCD header at open:
 * 4/48/32/64 bytes) go through a bounce sector: head and tail are read
 * with a whole sector and only the requested bytes are copied. At end of
 * file the sectors are short: only what is there is copied. */
/* Reopens the file after the server restart: the old handle no longer exists.
 * The slot is reused if the file has the same size. */
static int stub_reopen(udpfs_stub_slot_t *slot)
{
    uint32_t size = 0;
    int handle;

    if (slot->path[0] == 0)
        return -1;
    handle = udpfs_client_open(slot->path, &size);
    if (handle < 0)
        return -1;
    if (slot->size != 0 && size != slot->size) {
        udpfs_client_close(handle);
        return -1;
    }
    slot->server_handle = handle;
    return 0;
}

/* BREAD with at most one reopen: -2 = the server replies with an error
 * (invalid handle), i.e. it is alive but no longer knows the file. */
static int stub_bread(udpfs_stub_slot_t *slot, uint32_t sector, uint32_t count, void *buf)
{
    int n = udpfs_client_bread(slot->server_handle, sector, count, buf);

    if (n == -2 && stub_reopen(slot) == 0)
        n = udpfs_client_bread(slot->server_handle, sector, count, buf);
    return n;
}

static int stub_bread_copy(udpfs_stub_slot_t *slot, unsigned int pos, uint8_t *bounce, uint8_t *out, int want)
{
    unsigned int off = pos & 511;
    int n = stub_bread(slot, pos >> 9, 1, bounce);
    int take;

    if (n <= (int)off)
        return 0;               /* error or past the end of the file */
    take = n - (int)off;
    if (take > want)
        take = want;
    memcpy(out, bounce + off, (size_t)take);
    return take;
}

int udpfs_stub_read(void *privdata, void *buffer, int size)
{
    udpfs_stub_slot_t *slot = (udpfs_stub_slot_t *)privdata;
    uint8_t bounce[512] __attribute__((aligned(4)));
    uint8_t *out = (uint8_t *)buffer;
    unsigned int pos = slot->position;
    int total = 0;
    int n;
#ifdef UDPFS_STATS
    uint32_t st_t0 = udpfs_stats_now_ms();
#endif

    if (size <= 0) {
        goto done;
    }

#ifdef UDPFS_STATS
    if ((pos & 511) != 0 || ((unsigned int)size & 511) != 0) {
        static unsigned reported;
        udpfs_stat_t st;
        char msg[64];

        if (reported < 20) {   /* the first 20 are enough to see the pattern */
            reported++;
            sprintf(msg, "UDPFS_UNAL pos=%u size=%d", pos, size);
            udpfs_client_getstat(msg, &st);
        }
    }
#endif

    /* head: up to the next sector boundary */
    if (pos & 511) {
        int want = 512 - (int)(pos & 511);

        if (want > size)
            want = size;
        n = stub_bread_copy(slot, pos, bounce, out, want);
        total += n;
        pos += (unsigned int)n;
        if (n < want)
            goto done;
    }

    /* body: whole sectors, directly into the buffer */
    {
        int mid = (size - total) & ~511;

        if (mid > 0) {
            n = stub_bread(slot, pos >> 9, (uint32_t)mid >> 9, out + total);
            if (n < 0)
                n = 0;          /* error = no bytes */
            total += n;
            pos += (unsigned int)n;
            if (n < mid)
                goto done;
        }
    }

    /* tail: less than one sector */
    if (size - total > 0) {
        n = stub_bread_copy(slot, pos, bounce, out + total, size - total);
        total += n;
        pos += (unsigned int)n;
    }

done:
    slot->position = pos;
#ifdef UDPFS_STATS
    udpfs_stats_read_done(st_t0, (uint32_t)(size > 0 ? size : 0), (uint32_t)total);
#endif
    return total;
}

/* LAZY lseek: the position is computed locally, no network traffic
 * (BREAD carries the offset in every request). SEEK_END requires the size
 * received with the open. */
int udpfs_stub_lseek64(void *privdata, s64 offset, int whence, s64 *out_pos)
{
    udpfs_stub_slot_t *slot = (udpfs_stub_slot_t *)privdata;
    s64 np;

    if (whence == 0)
        np = offset;
    else if (whence == 1)
        np = (s64)slot->position + offset;
    else if (whence == 2 && slot->size != 0)
        np = (s64)slot->size + offset;
    else
        return -EINVAL;

    if (np < 0 || np > 0xFFFFFFFFLL)
        return -EINVAL;

    slot->position = (unsigned int)np;
    if (out_pos)
        *out_pos = np;
    return 0;
}

int udpfs_stub_close(void *privdata)
{
    udpfs_stub_slot_t *slot = (udpfs_stub_slot_t *)privdata;

    /* CLOSE_REQ to the server BEFORE freeing the slot: every open leaves
     * a handle open on the server (which keeps a finite number of them)
     * and, because of the server-side "file already open" rule, it can
     * prevent later opens of the same file. If the network does not
     * reply the local slot is freed anyway: better an orphan handle on
     * the server than a local slot lost forever. */
    udpfs_client_close(slot->server_handle);
    slot->in_use = 0;
    return 0;
}

int udpfs_stub_getstat(const char *path, iox_stat_t *stat)
{
    udpfs_stat_t st;
    int res;

    /* Real stat from the server (GETSTAT_REQ), no longer a fixed fake
     * value: the real size is needed because POPS computes the CD-ROM
     * sector offsets from it. */
    res = udpfs_client_getstat(path, &st);
    if (res < 0)
        return res;

    memset(stat, 0, sizeof(*stat));
    stat->mode   = FIO_S_IROTH | FIO_S_IXOTH | FIO_S_IFREG; /* read-only, by design */
    stat->attr   = 0555;
    stat->size   = st.size;
    stat->hisize = st.hisize;
    memcpy(stat->ctime, st.ctime, 8);
    memcpy(stat->atime, st.atime, 8);
    memcpy(stat->mtime, st.mtime, 8);
    return 0;
}
