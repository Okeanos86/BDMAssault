/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * udpfs_main.c - module entry point of the UDPFS-only "mass" driver:
 * starts the network in its own thread (it must never block the loader) and
 * registers the "mass" device. No USB, no block devices, no FatFs.
 */
#include <bdm.h>
#include <intrman.h>
#include <irx.h>
#include <loadcore.h>
#include <stdio.h>
#include <sysmem.h>
#include <thbase.h>

#include "net_init.h"
#include "udpfs_client.h"
#include "udpfs_config.h"
#include "udpfs.h"

#define MAJOR_VER 1
#define MINOR_VER 0

IRX_ID("udp_fs_assault", MAJOR_VER, MINOR_VER);

extern struct irx_export_table _exp_bdm;

static void net_init_thread(void *unused)
{
    int res;
    (void)unused;

    res = net_dev9_init();
    UDPFS_MARK(UDPFS_M_DEV9);
    if (res != 0) {
        /* Without this g_ready stays 0 and every request waits for the whole
         * timeout before failing. */
        udpfs_client_set_failed();
        return;
    }

    res = smap_init(0, NULL);
    UDPFS_MARK(UDPFS_M_SMAP);
    if (res != 0) {
        udpfs_client_set_failed();
        return;
    }

    /* Configuration: only what the memory card provided (read in _start, or
     * carried over the IOP reset in IOP RAM); otherwise the defaults. */
    UDPFS_MARK(UDPFS_M_MC);
    udpfs_config_apply(udpfs_cfg_read_stash);
    udpfs_client_init();
}

static void net_init_start(void)
{
    iop_thread_t param;
    int tid;

    param.attr      = TH_C;
    param.thread    = net_init_thread;
    param.priority  = 0x40;
    param.stacksize = 0x2000;
    param.option    = 0;

    tid = CreateThread(&param);
    if (tid >= 0)
        StartThread(tid, NULL);
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    UDPFS_MARK(UDPFS_M_START);

    /* The "bdm" library (empty stubs, see udpfs_bdm_stub.c): a USB
     * block-device driver loaded after this module imports it. */
    if (RegisterLibraryEntries(&_exp_bdm) != 0)
        return MODULE_NO_RESIDENT_END;

    /* memory card config: first load reads it, second load gets it from IOP RAM */
    udpfs_stash_init();
#if defined(DEBUG)
    udpfs_dbg_capture(argc, argv);
#endif

    /* The network semaphore must exist BEFORE the "mass" driver is added:
     * from then on fs_open can arrive at any time on another thread. */
    udpfs_client_preinit();
    net_init_start();

    printf("UDPFS: starting\n");
    if (udpfs_init_driver() != 0)
        return MODULE_NO_RESIDENT_END;
    return MODULE_RESIDENT_END;
}

void *malloc(int size)
{
    void *result;
    int OldState;

    CpuSuspendIntr(&OldState);
    result = AllocSysMemory(ALLOC_FIRST, size, NULL);
    CpuResumeIntr(OldState);

    return result;
}

void free(void *ptr)
{
    int OldState;

    CpuSuspendIntr(&OldState);
    FreeSysMemory(ptr);
    CpuResumeIntr(OldState);
}
