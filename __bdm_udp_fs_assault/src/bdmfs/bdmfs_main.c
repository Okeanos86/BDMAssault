#include <bdm.h>
#include <intrman.h>
#include <irx.h>
#include <loadcore.h>
#include <stdio.h>
#include <sysmem.h>
#include <cdvdman.h>
#include <thbase.h>

#include "include/ff.h"
#include "include/fs_driver.h"
#include "net_init.h"
#include "udpfs_client.h"
#include "udpfs_config.h"
#include "udpfs_stub.h"

//#define DEBUG  //comment out this line when not debugging
#include "include/module_debug.h"

#define MAJOR_VER 1
#define MINOR_VER 4

// IRX_ID("bdmff", MAJOR_VER, MINOR_VER); //because were fusing two IRX

static struct file_system g_fs = {
    .priv = NULL,
    .name = "fatfs",
    .connect_bd = connect_bd,
    .disconnect_bd = disconnect_bd,
};

/* Max time the network thread waits for the USB drive to be mounted before
 * it gives up on the DAT files and uses the defaults. */
#define UDPFS_USB_WAIT_POLLS 200 /* x 50 ms = 10 s */

/* The config files live on the USB drive (mass:/SYS-CONF/). Reading them needs
 * the volume mounted by the "mass" driver; mounted_bd is set before f_mount
 * runs, so also wait for FatFs to report a mounted file system. Pure polling
 * of driver state: no I/O, no module loading. */
static int usb_wait_mounted(void)
{
    int i;

    for (i = 0; i < UDPFS_USB_WAIT_POLLS; i++) {
        if (fatfs_fs_driver_get_mounted_bd_from_index(0) != NULL && fs_driver_mount_info[0].fatfs.fs_type != 0)
            return 1;
        DelayThread(50 * 1000);
    }
    return 0;
}

static void net_init_thread(void *unused)
{
    int res;
    (void)unused;

    res = net_dev9_init();
    UDPFS_MARK(UDPFS_M_DEV9);
    if (res != 0) {
        /* Without this, g_ready stays 0 forever and every fs_open that
         * tries the UDPFS fallback waits for the whole timeout (14 s) before
         * failing, instead of exiting right away as per the requirement
         * "if not found in either, exit without errors". */
        udpfs_client_set_failed();
        return;
    }

    res = smap_init(0, NULL);
    UDPFS_MARK(UDPFS_M_SMAP);
    if (res != 0) {
        udpfs_client_set_failed();
        return;
    }

    /* Configuration (mass:/SYS-CONF/IPCONFIG.DAT and UDPFSCONFIG.DAT): wait for
     * the USB drive, then read both files. Without a drive, or without files,
     * the defaults are used. The result is printed by udpfs_client_init once
     * the tty is up. */
    usb_wait_mounted();
    UDPFS_MARK(UDPFS_M_MC);
    udpfs_config_apply(udpfs_cfg_read_stash_or_usb);
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

int bdmfs_fatfs_start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    UDPFS_MARK(UDPFS_M_START);
    /* memory card config: first load reads it, second load gets it from IOP RAM */
    udpfs_stash_init();
#if defined(DEBUG)
    udpfs_dbg_capture(argc, argv);
#endif

    // The network semaphore must exist BEFORE the "mass" driver is
    // connected (bdm_connect_fs, below): from that moment fs_open can
    // arrive at any time on another thread. udpfs_client_preinit()
    // only creates the semaphore (instant, no network I/O).
    udpfs_client_preinit();

    // Start the real DEV9 hardware + the network (SMAP/ministack) in a
    // separate thread: it must NEVER block the POPStarter boot if
    // the network is unavailable or slow to respond. It starts first so that
    // dev9/smap init and the Ethernet link negotiation overlap with the
    // USB mount. The thread never loads modules and never touches the memory
    // card (it is not available after the POPStarter IOP reset); it reads the
    // DAT files from the USB drive once it is mounted.
    net_init_start();

    printf("BDM_ASSAULT: starting Fatfs side\n");

    // initialize the file system driver
    if (InitFS() != 0) {
        M_DEBUG("Error initializing FatFs driver!\n");
        return MODULE_NO_RESIDENT_END;
    }

    // Connect to block device manager
    bdm_connect_fs(&g_fs);

    // return resident
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
