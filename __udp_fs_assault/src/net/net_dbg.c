/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * net_dbg.c - DEBUG builds only: how POPStarter loads this module.
 *
 * Records, from _start, the arguments the module was given (POPStarter passes
 * only argv[0] = "LBbyEE"); udpfs_dbg_print() prints them once the UDP tty is
 * up (lines printed earlier are lost). Read-only: it changes nothing.
 */
#include <stdio.h>
#include <sysclib.h>
#include <thbase.h>

#include "udpfs_config.h"

static int g_argc;
static char g_arg[4][44];

void udpfs_dbg_capture(int argc, char *argv[])
{
    int i;

    g_argc = argc;
    for (i = 0; i < 4; i++) {
        g_arg[i][0] = 0;
        if (i < argc && argv != NULL && argv[i] != NULL)
            strncpy(g_arg[i], argv[i], sizeof(g_arg[i]) - 1);
        g_arg[i][sizeof(g_arg[i]) - 1] = 0;
    }
}

void udpfs_dbg_print(void)
{
    int i;

    printf("UDPFS: args argc=%d\n", g_argc);
    for (i = 0; i < 4 && i < g_argc; i++)
        printf("UDPFS: arg%d [%s]\n", i, g_arg[i]);
}
