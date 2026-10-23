/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

#ifndef UDPFS_H
#define UDPFS_H

/* Registers the "mass" ioman device (served by the UDPFS server).
 * Returns 0 on success. */
int udpfs_init_driver(void);

#endif /* UDPFS_H */
