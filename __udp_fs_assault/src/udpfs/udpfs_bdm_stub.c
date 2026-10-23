/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * udpfs_bdm_stub.c - empty "bdm" library.
 *
 * The module this one replaces exported the BDM (Block Device Manager)
 * library, which the USB block-device drivers (usbmass_bd.irx, ...) import.
 * POPStarter / Ember may still load such a driver: without these exports the
 * loader refuses it and the program gives up. Here nothing is ever connected:
 * every call is accepted and ignored, so a USB driver that finds no drive (or
 * finds one) changes nothing: "mass:" is served by the UDPFS server only.
 */
#include <bdm.h>

void bdm_connect_bd(struct block_device *bd)
{
    (void)bd;
}

void bdm_disconnect_bd(struct block_device *bd)
{
    (void)bd;
}

void bdm_connect_fs(struct file_system *fs)
{
    (void)fs;
}

void bdm_disconnect_fs(struct file_system *fs)
{
    (void)fs;
}

void bdm_get_bd(struct block_device **pbd, unsigned int count)
{
    unsigned int i;

    for (i = 0; i < count; i++)
        pbd[i] = NULL;
}

void bdm_RegisterCallback(bdm_cb cb)
{
    (void)cb;
}
