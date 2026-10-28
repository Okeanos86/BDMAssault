/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * bdmfs_vcd.c - VCD synthesized from a cue sheet and its .bin files,
 * see bdmfs_vcd.h.
 */
#ifndef EMBER_BUILD

#include <errno.h>
#include <stdio.h>
#include <intrman.h>
#include <sysclib.h>

#include "bdmfs_vcd.h"
#include "vcd_cue.h"

#define VCD_PATH_MAX 256    /* a path, NUL included (same as UDPFS_MAX_PATH) */
#define VCD_MAX_OPEN 2      /* POPStarter opens one VCD at a time */
#define VCD_HANDLES  3      /* .bin files of one VCD kept open at once (at least 2) */

#if defined(DEBUG)
#define VLOG(fmt, args...) printf("VCD: " fmt, ##args)
#else
#define VLOG(fmt, args...) ((void)0)
#endif

typedef struct {
    int used;
    const vcd_ops_t *ops;
    unsigned int position;                  /* lseek is local */
    unsigned int size;                      /* 1 MiB + all the .bin files */
    int nfiles;
    int dir_len;                            /* directory of the VCD, with the trailing '/' */
    uint32_t tick;
    uint32_t age[VCD_HANDLES];
    int hf[VCD_HANDLES];                    /* file each handle belongs to, -1 = free entry */
    void *h[VCD_HANDLES];
    uint32_t fstart[VCD_MAX_FILES + 1];     /* offset of each file in the image; [nfiles] = total */
    uint16_t name_off[VCD_MAX_FILES + 1];
    uint8_t hdr[VCD_HDR_BYTES];             /* the meaningful bytes of the 1 MiB header */
    char dir[VCD_PATH_MAX];
    char names[VCD_NAMES_MAX];              /* base names, next to the cue */
} vcd_slot_t;

static vcd_slot_t g_slots[VCD_MAX_OPEN];
static uint8_t g_cue_buf[VCD_CUE_MAX + 4] __attribute__((aligned(4)));
static vcd_cue_t g_cue;
static uint32_t g_fsz[VCD_MAX_FILES];
static int g_busy;      /* g_cue_buf, g_cue and g_fsz serve one open at a time */

static vcd_slot_t *slot_alloc(void)
{
    vcd_slot_t *found = NULL;
    int OldState, i;

    CpuSuspendIntr(&OldState);
    for (i = 0; i < VCD_MAX_OPEN; i++) {
        if (!g_slots[i].used) {
            g_slots[i].used = 1;
            found = &g_slots[i];
            break;
        }
    }
    CpuResumeIntr(OldState);
    return found;
}

/* Full path of file f, 0 or -1 if too long. */
static int vd_path(const vcd_slot_t *v, int f, char *out)
{
    const char *nm = v->names + v->name_off[f];
    size_t nl = strlen(nm);

    if ((size_t)v->dir_len + nl >= VCD_PATH_MAX)
        return -1;
    memcpy(out, v->dir, (size_t)v->dir_len);
    memcpy(out + v->dir_len, nm, nl + 1);
    return 0;
}

/* Length of "name" without a trailing ".VCD" (any case), 0 if it does not end so. */
static int stem_len(const char *name)
{
    size_t n = strlen(name);

    if (n < 5 || name[n - 4] != '.')
        return 0;
    if ((name[n - 3] | 0x20) != 'v' || (name[n - 2] | 0x20) != 'c' || (name[n - 1] | 0x20) != 'd')
        return 0;
    return (int)(n - 4);
}

/* A cue with a single FILE that is not there: look for "<name>.bin" instead. */
static int use_stem_name(vcd_slot_t *v, const char *name, int stem)
{
    int bl = stem - v->dir_len;

    if (bl <= 0 || bl + 5 > VCD_NAMES_MAX)
        return -1;
    memcpy(v->names, name + v->dir_len, (size_t)bl);
    memcpy(v->names + bl, ".bin", 5);
    v->name_off[0] = 0;
    v->name_off[1] = (uint16_t)(bl + 5);
    return 0;
}

/* Reads the whole cue sheet into g_cue_buf. Returns 0 or a negative errno. */
static int read_cue(const vcd_ops_t *ops, const char *name, int stem, unsigned int *len)
{
    char path[VCD_PATH_MAX];
    uint32_t csize = 0;
    void *ch = NULL;
    unsigned int got = 0;
    int i, r = -ENOENT, n;

    for (i = 0; i < 2 && r == -ENOENT; i++) {
        memcpy(path, name, (size_t)stem);
        memcpy(path + stem, i == 0 ? ".cue" : ".CUE", 5);
        r = ops->open(path, &csize, &ch);
        if (ops->ci)
            break;
    }
    if (r < 0)
        return r;
    if (csize == 0 || csize > VCD_CUE_MAX) {
        ops->close(ch);
        return -EINVAL;
    }
    while (got < csize) {
        n = ops->read(ch, got, g_cue_buf + got, (int)(csize - got));
        if (n <= 0)
            break;
        got += (unsigned int)n;
    }
    ops->close(ch);
    if (got != csize)
        return -EIO;
    *len = got;
    return 0;
}

int vcd_virtual_open(const vcd_ops_t *ops, const char *name, void **out)
{
    char path[VCD_PATH_MAX];
    vcd_slot_t *v = NULL;
    uint64_t total = 0;
    unsigned int len = 0;
    int OldState, busy, stem, dir, i, r, rc;
    void *first = NULL;

    stem = stem_len(name);
    if (stem <= 0 || stem + 5 > VCD_PATH_MAX)
        return -ENOENT;

    CpuSuspendIntr(&OldState);
    busy = g_busy;
    g_busy = 1;
    CpuResumeIntr(OldState);
    if (busy)
        return -EBUSY;

    rc = read_cue(ops, name, stem, &len);
    if (rc < 0)
        goto out;
    r = vcd_cue_parse((const char *)g_cue_buf, len, &g_cue);
    if (r != VCD_OK) {
        VLOG("cue error %d\n", r);
        rc = -EINVAL;
        goto out;
    }

    v = slot_alloc();
    if (v == NULL) {
        rc = -EMFILE;
        goto out;
    }
    dir = stem;
    while (dir > 0 && name[dir - 1] != '/')
        dir--;
    memcpy(v->dir, name, (size_t)dir);
    v->dir[dir] = 0;
    v->dir_len = dir;
    v->nfiles  = g_cue.nfiles;
    memcpy(v->name_off, g_cue.name_off, sizeof(uint16_t) * (size_t)(g_cue.nfiles + 1));
    memcpy(v->names, g_cue.names, g_cue.name_off[g_cue.nfiles]);
    for (i = 0; i < VCD_HANDLES; i++) {
        v->hf[i]  = -1;
        v->h[i]   = NULL;
        v->age[i] = 0;
    }
    v->tick = 0;

    /* the size of every file (each one is opened); the first stays open for
     * the first read */
    for (i = 0; i < v->nfiles; i++) {
        uint32_t sz = 0;
        void *h = NULL;

        if (vd_path(v, i, path) != 0) {
            rc = -ENAMETOOLONG;
            goto fail;
        }
        r = ops->open(path, &sz, &h);
        if (r == -ENOENT && v->nfiles == 1 && use_stem_name(v, name, stem) == 0 &&
            vd_path(v, 0, path) == 0)
            r = ops->open(path, &sz, &h);
        if (r < 0) {
            VLOG("cannot open %s (%d)\n", path, r);
            rc = r;
            goto fail;
        }
        total += sz;
        if (sz == 0 || total > 0xFFFFFFFFu - VCD_HDR_SIZE) {
            VLOG("bad size of %s\n", path);
            ops->close(h);
            rc = -EINVAL;
            goto fail;
        }
        g_fsz[i]     = sz;
        v->fstart[i] = (uint32_t)(total - sz);
        if (i == 0)
            first = h;
        else
            ops->close(h);
    }
    v->fstart[v->nfiles] = (uint32_t)total;

    if (vcd_build_header(&g_cue, g_fsz, v->hdr) != VCD_OK) {
        VLOG("header error\n");
        rc = -EINVAL;
        goto fail;
    }

    v->ops      = ops;
    v->position = 0;
    v->size     = VCD_HDR_SIZE + (unsigned int)total;
    v->hf[0]    = 0;
    v->h[0]     = first;
    v->age[0]   = ++v->tick;
    first       = NULL;
    VLOG("virtual VCD: %s (%d tracks, %d files, image %u bytes)\n", name, g_cue.ntracks, v->nfiles,
         (unsigned)total);
    *out = v;
    rc   = 0;
    goto out;

fail:
    /* a file whose size was read stays open only in 'first' */
    if (first != NULL)
        ops->close(first);
    v->used = 0;
out:
    g_busy = 0;
    return rc;
}

/* The handle of file f: from the small cache of open files, or opened
 * (closing the one used least recently). NULL if it cannot be opened. */
static void *vcd_select(vcd_slot_t *v, int f)
{
    char path[VCD_PATH_MAX];
    uint32_t sz = 0;
    void *h = NULL;
    int i, e;

    for (i = 0; i < VCD_HANDLES; i++)
        if (v->hf[i] == f) {
            v->age[i] = ++v->tick;
            return v->h[i];
        }
    if (vd_path(v, f, path) != 0)
        return NULL;
    e = -1;
    for (i = 0; i < VCD_HANDLES; i++)
        if (v->hf[i] < 0) {
            e = i;
            break;
        }
    if (e < 0) {
        e = 0;
        for (i = 1; i < VCD_HANDLES; i++)
            if (v->age[i] < v->age[e])
                e = i;
    }
    if (v->ops->open(path, &sz, &h) != 0)
        return NULL;
    if (sz != v->fstart[f + 1] - v->fstart[f]) {      /* not the file the header was built for */
        v->ops->close(h);
        return NULL;
    }
    if (v->hf[e] >= 0)
        v->ops->close(v->h[e]);
    v->hf[e]  = f;
    v->h[e]   = h;
    v->age[e] = ++v->tick;
    return h;
}

int vcd_virtual_is_ours(const void *p)
{
    return (p >= (const void *)&g_slots[0] && p < (const void *)&g_slots[VCD_MAX_OPEN]);
}

int vcd_virtual_read(void *p, void *buffer, int size)
{
    vcd_slot_t *v = (vcd_slot_t *)p;
    uint8_t *out = (uint8_t *)buffer;
    unsigned int pos = v->position;
    int total = 0, n;

    if (size <= 0)
        return 0;

    /* the first VCD_HDR_SIZE bytes come from RAM: the header, then zeros */
    if (pos < VCD_HDR_SIZE) {
        unsigned int i, k = VCD_HDR_SIZE - pos;

        if (k > (unsigned int)size)
            k = (unsigned int)size;
        for (i = 0; i < k; i++)
            out[i] = (pos + i < VCD_HDR_BYTES) ? v->hdr[pos + i] : 0;
        total = (int)k;
        pos += k;
    }

    /* the rest from the .bin files, in cue order */
    while (total < size) {
        unsigned int o = pos - VCD_HDR_SIZE, take;
        void *h;
        int f = 0;

        if (o >= v->fstart[v->nfiles])
            break;                              /* past the end of the image */
        while (o >= v->fstart[f + 1])
            f++;
        take = v->fstart[f + 1] - o;
        if (take > (unsigned int)(size - total))
            take = (unsigned int)(size - total);
        h = vcd_select(v, f);
        if (h == NULL)
            break;
        n = v->ops->read(h, o - v->fstart[f], out + total, (int)take);
        if (n <= 0)
            break;
        total += n;
        pos += (unsigned int)n;
        if ((unsigned int)n < take)
            break;
    }
    v->position = pos;
    return total;
}

int vcd_virtual_lseek64(void *p, int64_t offset, int whence, int64_t *out_pos)
{
    vcd_slot_t *v = (vcd_slot_t *)p;
    int64_t np;

    if (whence == 0)
        np = offset;
    else if (whence == 1)
        np = (int64_t)v->position + offset;
    else if (whence == 2)
        np = (int64_t)v->size + offset;
    else
        return -EINVAL;
    if (np < 0 || np > 0xFFFFFFFFLL)
        return -EINVAL;
    v->position = (unsigned int)np;
    if (out_pos)
        *out_pos = np;
    return 0;
}

int vcd_virtual_close(void *p)
{
    vcd_slot_t *v = (vcd_slot_t *)p;
    int i;

    for (i = 0; i < VCD_HANDLES; i++)
        if (v->hf[i] >= 0) {
            v->ops->close(v->h[i]);
            v->hf[i] = -1;
        }
    v->used = 0;
    return 0;
}

#endif /* EMBER_BUILD */
