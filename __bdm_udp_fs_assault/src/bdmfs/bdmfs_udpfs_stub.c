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
#include <stdio.h>
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
    char path[UDPFS_MAX_PATH]; /* to reopen the file if the server was restarted */
    /* One-sector cache for unaligned reads: the tail sector of a read is
     * the head sector of the next sequential one. cache_n = bytes the
     * server returned for cache_sector (0..512), -1 = empty. */
    uint32_t cache_sector;
    int cache_n;
    uint8_t cache_buf[512] __attribute__((aligned(4)));
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

/* POPStarter: POPS/<x>.VCD and POPS0/ ... POPS9/<x>.VCD ("name" has no
 * leading slashes). Only one character after "POPS": either the slash
 * right away, or a digit followed by the slash - no other case (e.g.
 * "POPSTARTER/" must not match). */
static int __attribute__((unused)) udpfs_match_pops(const char *name, size_t len)
{
    size_t prefix_len;

    if (len < 4 || !udpfs_ci_prefix(name, "POPS"))
        return 0;

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

/* Ember: EMBER/games/<folder>/<file>.cue or <file>.bin, exactly one folder
 * level. Any .cue/.bin in the folder is accepted (CDs with several bins);
 * nothing else (settings.txt, bios.bin one level up, *.vmc, ...), so that
 * optional files never cost a network round trip. "." and ".." as folder
 * name are refused. */
static int __attribute__((unused)) udpfs_match_ember(const char *name, size_t len)
{
    const char *folder, *file, *slash;
    size_t folder_len, file_len;

    if (!udpfs_ci_prefix(name, "EMBER/GAMES/"))
        return 0;

    folder = name + 12;
    slash  = folder;
    while (*slash && *slash != '/')
        slash++;
    if (*slash != '/')
        return 0;                    /* no file part */

    folder_len = (size_t)(slash - folder);
    if (folder_len == 0 ||
        (folder_len == 1 && folder[0] == '.') ||
        (folder_len == 2 && folder[0] == '.' && folder[1] == '.'))
        return 0;

    file     = slash + 1;
    file_len = len - (size_t)(file - name);
    if (file_len < 5)                /* at least "x.cue" / "x.bin" */
        return 0;
    {
        size_t k;
        for (k = 0; k < file_len; k++)
            if (file[k] == '/')
                return 0;            /* deeper than one folder level */
    }

    return udpfs_ci_prefix(file + file_len - 4, ".CUE") ||
           udpfs_ci_prefix(file + file_len - 4, ".BIN");
}

int udpfs_stub_path_matches(const char *name)
{
    size_t len;

    if (name == NULL)
        return 0;

    /* The exact format in which the emulator passes the path is not
     * guaranteed: upper/lower case variants and leading slashes are
     * accepted. The path sent to the server remains the original one
     * passed by fs_open/fs_getstat: the server itself strips the leading
     * slashes (resolvePath). */
    while (*name == '/')
        name++;

    len = strlen(name);

    /* One rule per build: POPStarter by default, Ember with make EMBER=1.
     * The other program's files never cause a network request. */
#ifdef EMBER_BUILD
    return udpfs_match_ember(name, len);
#else
    return udpfs_match_pops(name, len);
#endif
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
    printf("UDPFS: open -> %d size %u\n", handle, (unsigned)size);
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
                g_slots[i].cache_n       = -1;
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
 * 4/48/32/64 bytes) go through the slot's one-sector cache: head and tail are read
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

/* Copies up to 'want' bytes starting at 'pos' (inside one sector) through
 * the slot's one-sector cache: a request is sent only if the sector is not
 * the cached one. */
static int stub_bread_copy(udpfs_stub_slot_t *slot, unsigned int pos, uint8_t *out, int want)
{
    unsigned int off = pos & 511;
    uint32_t sector = pos >> 9;
    int n, take;

    if (slot->cache_n >= 0 && slot->cache_sector == sector) {
        n = slot->cache_n;
    } else {
        n = stub_bread(slot, sector, 1, slot->cache_buf);
        if (n < 0) {
            slot->cache_n = -1;
            return 0;           /* error */
        }
        slot->cache_sector = sector;
        slot->cache_n      = n;
    }

    if (n <= (int)off)
        return 0;               /* past the end of the file */
    take = n - (int)off;
    if (take > want)
        take = want;
    memcpy(out, slot->cache_buf + off, (size_t)take);
    return take;
}

int udpfs_stub_read(void *privdata, void *buffer, int size)
{
    udpfs_stub_slot_t *slot = (udpfs_stub_slot_t *)privdata;
    uint8_t *out = (uint8_t *)buffer;
    unsigned int pos = slot->position;
    int total = 0;
    int n;

    if (size <= 0) {
        goto done;
    }

    /* head: up to the next sector boundary */
    if (pos & 511) {
        int want = 512 - (int)(pos & 511);

        if (want > size)
            want = size;
        n = stub_bread_copy(slot, pos, out, want);
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
        n = stub_bread_copy(slot, pos, out + total, size - total);
        total += n;
        pos += (unsigned int)n;
    }

done:
    slot->position = pos;
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

    /* The Go server (udpfsd) answers -EIO to GETSTAT of a file that is not
     * open (Ember calls getstat on the .bin before opening it). Ask with
     * OPEN + CLOSE instead: if the file is really missing the OPEN fails
     * too and the original error is returned. Network errors (-1) and
     * other server errors (e.g. -ENOENT) are not retried. */
    if (res == -EIO) {
        uint32_t size = 0;
        int handle = udpfs_client_open(path, &size);

        if (handle < 0)
            return res;
        udpfs_client_close(handle);
        memset(&st, 0, sizeof(st));
        st.size = size;
        res = 0;
    } else if (res < 0) {
        return res;
    }

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
