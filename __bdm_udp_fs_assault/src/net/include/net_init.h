#ifndef NET_INIT_H
#define NET_INIT_H

/* net_dev9_init() - src/net/net_dev9.c (formerly _start of ps2dev9.c,
 * renamed: here it is not the module entry point). Initializes
 * the real DEV9 hardware. Returns 0 on success. */
int net_dev9_init(void);

/* smap_init() - src/net/net_smap.c. Initializes the network adapter
 * (SMAP) and brings up the IP/ARP/UDP ministack. argc/argv are not used
 * internally (verified in the source), safe to call it with
 * smap_init(0, NULL). Returns 0 on success. */
int smap_init(int argc, char *argv[]);

/* Non-zero once the link is up and the MAC enabled. */
int smap_is_initialized(void);

#endif /* NET_INIT_H */
