/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * udpfs.c - the "mass" ioman device served entirely by a UDPFS server.
 *
 * POPStarter and Ember keep opening "mass:/..." as if it were a USB drive; the
 * requests go to the UDPFS server instead.
 *
 * Error codes: the USB driver returns the FatFs error code negated (-4 =
 * FR_NO_FILE, -5 = FR_NO_PATH, ...) and the programs rely on it, so every
 * server errno is translated (mass_err).
 *
 * Writes: open for writing, write, mkdir, remove, rmdir. Not supported:
 * rename (neither POPStarter nor Ember use it).
 *
 * The server writes at ITS cursor, while lseek is local here: before a write
 * the cursor is moved with LSEEK unless it is known to be where we want it
 * (srv_pos). BREAD does not tell whether the cursor moves, so after a read
 * srv_pos is unknown.
 */
#include <errno.h>
#include <stdio.h>
#include <intrman.h>
#include <iomanX.h>
#include <sysclib.h>
#include <thbase.h>
#include <usbhdfsd-common.h>

#include "udpfs_client.h"
#include "udpfs.h"

#ifdef DEBUG
#define MDBG(fmt, args...) printf("UDPFS: " fmt, ##args)
#else
#define MDBG(fmt, args...) ((void)0)
#endif

/* FatFs FRESULT values (the driver returns them negated). */
#define FR_DISK_ERR          1
#define FR_NO_FILE           4
#define FR_NO_PATH           5
#define FR_INVALID_NAME      6
#define FR_DENIED            7
#define FR_EXIST             8
#define FR_WRITE_PROTECTED   10
#define FR_TOO_MANY_OPEN     18
#define FR_INVALID_PARAMETER 19

/* FIO open flags that the server understands (same bit values). */
#define FIO_ACCMODE   0x0003
#define UDPFS_FLAG_MASK 0x0703

/* ---------------------------------------------------------------- slots */

#define MASS_MAX_SLOTS 16

typedef struct {
    int in_use;
    int is_dir;
    int server_handle;
    int flags;              /* UDPFS flags used for the open */
    unsigned int position;  /* logical position: lseek is local */
    unsigned int size;      /* 0 = unknown (asked with GETSTAT when needed) */
    int64_t srv_pos;        /* server cursor, -1 = unknown */
    char path[UDPFS_MAX_PATH];  /* to reopen after a server restart */
    /* One-sector cache for unaligned reads: the tail sector of a read is the
     * head sector of the next sequential one. cache_n = bytes the server
     * returned for cache_sector (0..512), -1 = empty. */
    uint32_t cache_sector;
    int cache_n;
    uint8_t cache_buf[512] __attribute__((aligned(4)));
} mass_slot_t;

static mass_slot_t g_slots[MASS_MAX_SLOTS];

static mass_slot_t *slot_alloc(void)
{
    mass_slot_t *found = NULL;
    int OldState, i;

    CpuSuspendIntr(&OldState);
    for (i = 0; i < MASS_MAX_SLOTS; i++) {
        if (!g_slots[i].in_use) {
            g_slots[i].in_use = 1;     /* reserved: filled in by the caller */
            found = &g_slots[i];
            break;
        }
    }
    CpuResumeIntr(OldState);
    return found;
}

static int slot_valid(const void *p)
{
    return p >= (const void *)&g_slots[0] && p < (const void *)&g_slots[MASS_MAX_SLOTS];
}

/* ---------------------------------------------------------------- errors */

/* Server result (-errno, or UDPFS_E_NET / -1 for no reply) -> negated FatFs
 * code. is_dir: an ENOENT on a directory open means "path not found". */
static int mass_err(int r, int is_dir)
{
    int e = -r;

    if (r == UDPFS_E_NET || r == -1)
        return -FR_DISK_ERR;
    switch (e) {
        case ENOENT:
            return is_dir ? -FR_NO_PATH : -FR_NO_FILE;
        case ENOTDIR:
            return -FR_NO_PATH;
        case EEXIST:
            return -FR_EXIST;
        case EACCES:
        case EPERM:
            return -FR_DENIED;
        case EROFS:
            return -FR_WRITE_PROTECTED;
        case EMFILE:
            return -FR_TOO_MANY_OPEN;
        case EINVAL:
            return -FR_INVALID_PARAMETER;
        case ENAMETOOLONG:
            return -FR_INVALID_NAME;
        default:
            return -FR_DISK_ERR;
    }
}

/* ---------------------------------------------------------------- paths */

/* Copies a path for GETSTAT/DOPEN without trailing slashes ("/POPS/GAME/" ->
 * "/POPS/GAME"); "/" stays "/". Returns 0, or -1 if too long. */
static int path_clean(const char *name, char *out)
{
    size_t n = strlen(name);

    if (n >= UDPFS_MAX_PATH)
        return -1;
    memcpy(out, name, n + 1);
    while (n > 1 && out[n - 1] == '/')
        out[--n] = 0;
    return 0;
}

static int path_is_root(const char *name)
{
    while (*name == '/')
        name++;
    return *name == 0 || (name[0] == '.' && name[1] == 0);
}

/* ---------------------------------------------------------------- stat */

static void fill_stat(iox_stat_t *stat, const udpfs_stat_t *st, int is_dir)
{
    memset(stat, 0, sizeof(*stat));
    stat->mode = FIO_S_IROTH | FIO_S_IXOTH | (is_dir ? FIO_S_IFDIR : FIO_S_IFREG);
    stat->mode |= FIO_S_IWOTH;
    stat->attr   = 0777;
    stat->size   = st->size;
    stat->hisize = st->hisize;
    memcpy(stat->ctime, st->ctime, 8);
    memcpy(stat->atime, st->atime, 8);
    memcpy(stat->mtime, st->mtime, 8);
}

/* A network interruption or a server restart in the middle of a write must not
 * cost the save: the call waits (up to about WRITE_RETRY_MAX * WRITE_RETRY_MS,
 * 10 s) for the server to come back and repeats what did not get through. The
 * repetition is safe: every attempt moves the server cursor first, and the
 * server writes a group of chunks only when it is complete. */
#define WRITE_RETRY_MAX 40
#define WRITE_RETRY_MS  250
#define CLOSE_RETRY_MAX 20
#define OPEN_RETRY_MAX  32

static void retry_pause(void)
{
    DelayThread(WRITE_RETRY_MS * 1000);
}

/* ---------------------------------------------------------------- open */

static int fs_open(iop_file_t *fd, const char *name, int flags, int mode)
{
    mass_slot_t *slot;
    udpfs_stat_t st;
    int uflags = flags & UDPFS_FLAG_MASK;
    int h;

    (void)mode;
    MDBG("open %s flags %x\n", name, flags);

    if (strlen(name) >= UDPFS_MAX_PATH)
        return -FR_INVALID_NAME;
    if ((uflags & FIO_ACCMODE) == 0)
        uflags |= FIO_O_RDONLY;

    slot = slot_alloc();
    if (slot == NULL)
        return -FR_TOO_MANY_OPEN;

    h = udpfs_client_open_ex(name, uflags, 0, &st);
    /* Opening for writing while the network is down must wait, not fail: a
     * program that cannot open an existing file for writing may fall back to
     * creating it (Ember does: flags 0x602), which truncates the file. A read
     * open fails at once, so a server that is off does not stall a scan. */
    if (h == UDPFS_E_NET && (uflags & (FIO_O_WRONLY | FIO_O_CREAT | FIO_O_TRUNC | FIO_O_APPEND))) {
        int i;

        for (i = 0; i < OPEN_RETRY_MAX && h == UDPFS_E_NET; i++) {
            retry_pause();
            h = udpfs_client_open_ex(name, uflags, 0, &st);
        }
    }
    if (h < 0) {
        slot->in_use = 0;
        MDBG("open -> %d\n", h);
        return mass_err(h, 0);
    }

    slot->is_dir        = 0;
    slot->server_handle = h;
    slot->flags         = uflags;
    slot->position      = 0;
    slot->size          = (uflags & FIO_O_TRUNC) ? 0 : st.size;
    slot->srv_pos       = (uflags & FIO_O_APPEND) ? -1 : 0;   /* a new open starts at 0 */
    slot->cache_n       = -1;
    strncpy(slot->path, name, sizeof(slot->path) - 1);
    slot->path[sizeof(slot->path) - 1] = 0;
    fd->privdata = slot;
    MDBG("open -> handle %d size %u\n", h, (unsigned)st.size);
    return 1;
}

static int fs_close(iop_file_t *fd)
{
    mass_slot_t *slot = (mass_slot_t *)fd->privdata;

    if (slot == NULL || !slot_valid(slot))
        return -ENOENT;
    MDBG("close handle %d\n", slot->server_handle);
    /* The server must see the CLOSE before the next OPEN of the same file
     * (it refuses a read open on a file still open for writing). If the
     * network fails the slot is freed anyway. */
    {
        int i, r;

        for (i = 0;; i++) {
            r = udpfs_client_close(slot->server_handle);
            /* -1: no reply (network). A file open for writing must really be
             * closed: the server keeps it locked until then. */
            if (r != -1 || (slot->flags & FIO_ACCMODE) == FIO_O_RDONLY || i >= CLOSE_RETRY_MAX)
                break;
            retry_pause();
        }
    }
    slot->in_use = 0;
    fd->privdata = NULL;
    return 0;
}

/* ---------------------------------------------------------------- read */

/* Reopens the file after the server restart (the old handle is gone). The
 * slot is kept only if the file has the same size. */
static int slot_reopen(mass_slot_t *slot)
{
    udpfs_stat_t st;
    int h = udpfs_client_open_ex(slot->path, slot->flags & ~(FIO_O_CREAT | FIO_O_TRUNC), 0, &st);

    if (h < 0)
        return -1;
    /* A read-only file must be the same file; a file being written may
     * legitimately differ (the last group may not have reached the server). */
    if ((slot->flags & FIO_ACCMODE) == FIO_O_RDONLY && slot->size != 0 && st.size != 0 && st.size != slot->size) {
        udpfs_client_close(h);
        return -1;
    }
    slot->server_handle = h;
    slot->srv_pos       = (slot->flags & FIO_O_APPEND) ? -1 : 0;
    return 0;
}

/* BREAD with at most one reopen: -2 = the server replies with an error
 * (invalid handle): it is alive but no longer knows the file. */
static int slot_bread(mass_slot_t *slot, uint32_t sector, uint32_t count, void *buf)
{
    int n = udpfs_client_bread(slot->server_handle, sector, count, buf);

    slot->srv_pos = -1;
    if (n == -2 && slot_reopen(slot) == 0)
        n = udpfs_client_bread(slot->server_handle, sector, count, buf);
    return n;
}

/* Copies up to 'want' bytes starting at 'pos' (inside one sector) through the
 * one-sector cache: a request is sent only if the sector is not the cached
 * one. */
static int slot_bread_copy(mass_slot_t *slot, unsigned int pos, uint8_t *out, int want)
{
    unsigned int off = pos & 511;
    uint32_t sector = pos >> 9;
    int n, take;

    if (slot->cache_n >= 0 && slot->cache_sector == sector) {
        n = slot->cache_n;
    } else {
        n = slot_bread(slot, sector, 1, slot->cache_buf);
        if (n < 0) {
            slot->cache_n = -1;
            return 0;
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

static int fs_read(iop_file_t *fd, void *buffer, int size)
{
    mass_slot_t *slot = (mass_slot_t *)fd->privdata;
    uint8_t *out = (uint8_t *)buffer;
    unsigned int pos;
    int total = 0, n;

    if (slot == NULL || !slot_valid(slot) || slot->is_dir)
        return -ENOENT;
    pos = slot->position;
    if (size <= 0)
        return 0;

    /* head: up to the next sector boundary */
    if (pos & 511) {
        int want = 512 - (int)(pos & 511);

        if (want > size)
            want = size;
        n = slot_bread_copy(slot, pos, out, want);
        total += n;
        pos += (unsigned int)n;
        if (n < want)
            goto done;
    }

    /* body: whole sectors, directly into the caller's buffer */
    {
        int mid = (size - total) & ~511;

        if (mid > 0) {
            n = slot_bread(slot, pos >> 9, (uint32_t)mid >> 9, out + total);
            if (n < 0)
                n = 0;
            total += n;
            pos += (unsigned int)n;
            if (n < mid)
                goto done;
        }
    }

    /* tail: less than one sector */
    if (size - total > 0) {
        n = slot_bread_copy(slot, pos, out + total, size - total);
        total += n;
        pos += (unsigned int)n;
    }

done:
    slot->position = pos;
    return total;
}

/* One attempt: puts the server cursor where the slot is, then writes. Returns
 * the bytes written (> 0), 0, UDPFS_E_NET if the network failed, or -EIO if
 * the server refused (typically: it no longer knows the handle). */
static int slot_write_once(mass_slot_t *slot, const uint8_t *buf, int left)
{
    uint64_t pos;
    int r;

    if (slot->flags & FIO_O_APPEND) {
        r = udpfs_client_lseek(slot->server_handle, 0, 2, &pos);
        if (r == 0) {
            slot->position = (unsigned int)pos;
            slot->srv_pos  = (int64_t)pos;
        }
    } else if (slot->srv_pos != (int64_t)slot->position) {
        r = udpfs_client_lseek(slot->server_handle, (int64_t)slot->position, 0, &pos);
        if (r == 0)
            slot->srv_pos = (int64_t)pos;
    } else {
        r = 0;
    }
    if (r != 0)
        return (r == UDPFS_E_NET) ? UDPFS_E_NET : -EIO;

    return udpfs_client_write(slot->server_handle, buf, (uint32_t)left);
}

static int fs_write(iop_file_t *fd, void *buffer, int size)
{
    mass_slot_t *slot = (mass_slot_t *)fd->privdata;
    const uint8_t *buf = (const uint8_t *)buffer;
    int total = 0, fails = 0, need_reopen = 0, reopens = 0, r = 0;

    if (slot == NULL || !slot_valid(slot) || slot->is_dir)
        return -ENOENT;
    if ((slot->flags & FIO_ACCMODE) == FIO_O_RDONLY)
        return -EBADF;
    if (size <= 0)
        return 0;

    slot->cache_n = -1;         /* the read cache may now be stale */

    while (total < size) {
        if (need_reopen) {
            /* the server forgot the handle (restart): reopen by path */
            if (slot_reopen(slot) == 0) {
                need_reopen = 0;
            } else {
                if (++fails > WRITE_RETRY_MAX)
                    break;
                retry_pause();
                continue;
            }
        }

        r = slot_write_once(slot, buf + total, size - total);
        if (r > 0) {
            total          += r;
            slot->position += (unsigned int)r;
            slot->srv_pos   = (int64_t)slot->position;
            if (slot->position > slot->size)
                slot->size = slot->position;
            fails = 0;
            continue;
        }
        if (r == 0)
            break;

        slot->srv_pos = -1;
        MDBG("write -> %d (retry %d)\n", r, fails);
        if (r == UDPFS_E_NET) {
            if (++fails > WRITE_RETRY_MAX)
                break;
            retry_pause();
        } else if (r == -EIO || r == -EINVAL) {
            if (reopens >= 3)
                break;          /* reopened and still refused: give up */
            reopens++;
            need_reopen = 1;
        } else {
            break;              /* a real server error (disk full, ...) */
        }
    }

    if (total > 0)
        return total;           /* short write if the retries ran out */
    return mass_err(r == 0 ? -EIO : r, 0);
}

/* ---------------------------------------------------------------- lseek */

/* LAZY lseek: the position is kept locally, BREAD carries the offset in
 * every request. SEEK_END needs the size (from the open, or GETSTAT). */
static s64 fs_lseek64(iop_file_t *fd, s64 offset, int whence)
{
    mass_slot_t *slot = (mass_slot_t *)fd->privdata;
    s64 np;

    if (slot == NULL || !slot_valid(slot) || slot->is_dir)
        return -ENOENT;

    if (whence == 0) {
        np = offset;
    } else if (whence == 1) {
        np = (s64)slot->position + offset;
    } else if (whence == 2) {
        if (slot->size == 0) {
            udpfs_stat_t st;
            char clean[UDPFS_MAX_PATH];

            if (path_clean(slot->path, clean) == 0 && udpfs_client_getstat(clean, &st) == 0)
                slot->size = st.size;
        }
        np = (s64)slot->size + offset;
    } else {
        return -EINVAL;
    }

    if (np < 0 || np > 0xFFFFFFFFLL)
        return -EINVAL;
    slot->position = (unsigned int)np;
    return np;
}

static int fs_lseek(iop_file_t *fd, int offset, int whence)
{
    return (int)fs_lseek64(fd, (s64)offset, whence);
}

/* ---------------------------------------------------------------- dirs */

static int fs_dopen(iop_file_t *fd, const char *name)
{
    mass_slot_t *slot;
    char clean[UDPFS_MAX_PATH];
    udpfs_stat_t st;
    int h;

    MDBG("dopen %s\n", name);
    if (path_clean(name, clean) != 0)
        return -FR_INVALID_NAME;

    slot = slot_alloc();
    if (slot == NULL)
        return -FR_TOO_MANY_OPEN;

    h = udpfs_client_open_ex(clean, FIO_O_RDONLY, 1, &st);
    if (h < 0) {
        slot->in_use = 0;
        MDBG("dopen -> %d\n", h);
        return mass_err(h, 1);
    }
    slot->is_dir        = 1;
    slot->server_handle = h;
    slot->flags         = FIO_O_RDONLY;
    slot->position      = 0;
    slot->size          = 0;
    slot->srv_pos       = -1;
    slot->cache_n       = -1;
    strncpy(slot->path, clean, sizeof(slot->path) - 1);
    slot->path[sizeof(slot->path) - 1] = 0;
    fd->privdata = slot;
    return 0;
}

static int fs_dclose(iop_file_t *fd)
{
    mass_slot_t *slot = (mass_slot_t *)fd->privdata;

    if (slot == NULL || !slot_valid(slot))
        return -ENOENT;
    udpfs_client_close(slot->server_handle);
    slot->in_use = 0;
    fd->privdata = NULL;
    return 0;
}

static int fs_dread(iop_file_t *fd, iox_dirent_t *buffer)
{
    mass_slot_t *slot = (mass_slot_t *)fd->privdata;
    udpfs_stat_t st;
    char name[UDPFS_MAX_PATH];
    int r;

    if (slot == NULL || !slot_valid(slot) || !slot->is_dir)
        return -ENOENT;

    r = udpfs_client_dread(slot->server_handle, name, sizeof(name), &st);
    if (r <= 0)
        return r < 0 ? mass_err(r, 1) : 0;      /* 0 = end of directory */

    strncpy(buffer->name, name, 255);
    buffer->name[255] = 0;
    fill_stat(&buffer->stat, &st, (st.mode & FIO_S_IFDIR) != 0);
    return 1;
}

/* ---------------------------------------------------------------- getstat */

static int fs_getstat(iop_file_t *fd, const char *name, iox_stat_t *stat)
{
    char clean[UDPFS_MAX_PATH];
    udpfs_stat_t st;
    int r;

    (void)fd;
    MDBG("getstat %s\n", name);

    /* The root is answered locally, as the USB driver did. */
    if (path_is_root(name)) {
        memset(stat, 0, sizeof(*stat));
        stat->mode = FIO_S_IROTH | FIO_S_IWOTH | FIO_S_IXOTH | FIO_S_IFDIR;
        return 0;
    }
    if (path_clean(name, clean) != 0)
        return -FR_INVALID_NAME;

    memset(&st, 0, sizeof(st));
    r = udpfs_client_getstat(clean, &st);
    if (r == 0) {
        fill_stat(stat, &st, (st.mode & FIO_S_IFDIR) != 0);
        return 0;
    }

    /* The Go server (udpfsd) answers -EIO to a GETSTAT of a path that is not
     * open. Ask with OPEN+CLOSE (file), then DOPEN+CLOSE (directory): if both
     * fail the path is not there. Network errors and other server errors are
     * not retried. */
    if (r == -EIO) {
        int h = udpfs_client_open_ex(clean, FIO_O_RDONLY, 0, &st);

        if (h >= 0) {
            udpfs_client_close(h);
            fill_stat(stat, &st, 0);
            return 0;
        }
        h = udpfs_client_open_ex(clean, FIO_O_RDONLY, 1, &st);
        if (h >= 0) {
            udpfs_client_close(h);
            fill_stat(stat, &st, 1);
            return 0;
        }
        return mass_err(-ENOENT, 0);
    }
    MDBG("getstat -> %d\n", r);
    return mass_err(r, 0);
}

/* ---------------------------------------------------------------- others */

static int fs_dummy(void)
{
    return -5;
}

static int fs_init(iop_device_t *driver)
{
    (void)driver;
    return 1;
}

static int fs_pathop(const char *name, int op)
{
    char clean[UDPFS_MAX_PATH];
    int r;

    if (path_clean(name, clean) != 0)
        return -FR_INVALID_NAME;
    if (op == 0)
        r = udpfs_client_mkdir(clean);
    else if (op == 1)
        r = udpfs_client_remove(clean);
    else
        r = udpfs_client_rmdir(clean);
    MDBG("pathop %d %s -> %d\n", op, clean, r);
    return r == 0 ? 0 : mass_err(r, 1);
}

static int fs_remove(iop_file_t *fd, const char *name)
{
    (void)fd;
    return fs_pathop(name, 1);
}

static int fs_rmdir(iop_file_t *fd, const char *name)
{
    (void)fd;
    return fs_pathop(name, 2);
}

static int fs_mkdir(iop_file_t *fd, const char *name, int mode)
{
    (void)fd;
    (void)mode;
    return fs_pathop(name, 0);
}

static int fs_ioctl2(iop_file_t *fd, int cmd, void *data, unsigned int datalen, void *rdata, unsigned int rdatalen)
{
    (void)fd;
    (void)cmd;
    (void)data;
    (void)datalen;
    (void)rdata;
    (void)rdatalen;
    MDBG("ioctl2 cmd %d\n", cmd);
    return -EINVAL;     /* cluster/LBA/fragment queries make no sense here */
}

static int fs_ioctl(iop_file_t *fd, int cmd, void *data)
{
    return fs_ioctl2(fd, cmd, data, 0, NULL, 0);
}

static int fs_rename(iop_file_t *fd, const char *path, const char *newpath)
{
    (void)fd;
    (void)path;
    (void)newpath;
    return -FR_WRITE_PROTECTED;
}

static int fs_devctl(iop_file_t *fd, const char *name, int cmd, void *arg, unsigned int arglen, void *buf, unsigned int buflen)
{
    (void)fd;
    (void)name;
    (void)arg;
    (void)arglen;
    (void)buf;
    (void)buflen;
    MDBG("devctl %x\n", cmd);
    /* "stop the drive" requests: nothing to stop */
    if (cmd == USBMASS_DEVCTL_STOP_UNIT || cmd == USBMASS_DEVCTL_STOP_ALL)
        return 0;
    return -ENXIO;
}

static iop_device_ops_t fs_functarray = {
    &fs_init,
    (void *)&fs_dummy,
    (void *)&fs_dummy,
    &fs_open,
    &fs_close,
    &fs_read,
    &fs_write,
    &fs_lseek,
    &fs_ioctl,
    &fs_remove,
    &fs_mkdir,
    &fs_rmdir,
    &fs_dopen,
    &fs_dclose,
    &fs_dread,
    &fs_getstat,
    (void *)&fs_dummy,
    &fs_rename,
    (void *)&fs_dummy,
    (void *)&fs_dummy,
    (void *)&fs_dummy,
    (void *)&fs_dummy,
    &fs_lseek64,
    &fs_devctl,
    (void *)&fs_dummy,
    (void *)&fs_dummy,
    &fs_ioctl2,
};

static iop_device_t fs_driver = {
    "mass",
    IOP_DT_FS | IOP_DT_FSEXT,
    2,
    "UDPFS mass driver",
    &fs_functarray,
};

int udpfs_init_driver(void)
{
    DelDrv(fs_driver.name);
    return (AddDrv(&fs_driver) == 0) ? 0 : -1;
}
