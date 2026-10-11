/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 *
 * Parts of the network handling derive from UDPBD and the UDPFS/UDPRDMA
 * protocol of Neutrino (rickgaiser), Academic Free License 3.0.
 */

/*
 * net_udpfs_client.c - minimal UDPFS client (see udpfs_client.h)
 *
 * Packet format VERIFIED against the real sources of rickgaiser/neutrino
 * (iop/udpfs/src/udprdma_packet.h for the C struct, pc/udpfs_server.py
 * Header/DataHeader classes pack/unpack for the definitive truth on the
 * bit layout): little-endian on both sides (IOP MIPS and PC server are
 * both little-endian), no network-byte-order conversion for the UDPRDMA
 * header itself - unlike the layers below (Ethernet/IP/UDP), which ARE
 * in network byte-order (ntohs/ntohl already used correctly in
 * net_ministack.c).
 *
 * IMPORTANT: discovery (DISCOVERY/INFORM) is NOT optional even with a
 * fixed server IP - the server uses two separate sockets: a fixed one
 * (0xF5F6) for discovery only, and one for data with a port chosen at
 * random by the operating system at each start, communicated to the
 * client only in the INFORM reply. With a fixed IP (UDPFSCONFIG.DAT) the
 * DISCOVERY goes directly to the server, otherwise it is broadcast; the
 * INFORM provides the data port, IP and MAC (then Ethernet unicast).
 * Three attempts within the allowed time; after that, a lazy retry on the
 * next request.
 */
#include <stdint.h>
#include <string.h>
#include <thbase.h>
#include <thevent.h>
#include <thsemap.h>
#include <smapregs.h>
#include <stdio.h>
#ifdef UDPFS_STATS
#include <sysclib.h>
#endif

#include "ministack.h"
#include "udpfs_client.h"
#include "udpfs_config.h"

int smap_is_initialized(void);

/* Wait for single-packet replies (OPEN, CLOSE, GETSTAT): sleep of
 * 1 ms after the first busy-poll iterations. */
#ifndef UDPFS_POLL_SLEEP_US
#define UDPFS_POLL_SLEEP_US 1000
#endif


/* ---------------------------------------------------------------- */
/* UDPRDMA header - see WARNING above */

#define UDPRDMA_TYPE_DISCOVERY 0
#define UDPRDMA_TYPE_INFORM    1
#define UDPRDMA_TYPE_DATA      2

#define UDPRDMA_FLAG_ACK 0x1
#define UDPRDMA_FLAG_FIN 0x2

/*
 * Format CONFIRMED against the real sources of rickgaiser/neutrino
 * (iop/udpfs/src/udprdma_packet.h and pc/udpfs_server.py, Header/DataHeader
 * pack/unpack): little-endian on both sides (IOP MIPS and PC server are
 * both little-endian, no network-byte-order conversion for THIS header -
 * unlike Ethernet/IP/UDP below, which remain in network byte-order,
 * ntohs/ntohl already correct in net_ministack.c).
 */
typedef union {
    uint16_t raw;
    struct {
        uint16_t packet_type : 4;   /* low bits: bits 0-3 */
        uint16_t seq_nr      : 12;  /* high bits: bits 4-15 */
    } __attribute__((packed)) f;
} udprdma_base_hdr_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t seq_nr_ack      : 12;  /* low bits: bits 0-11 */
        uint32_t flags           : 2;   /* bits 12-13 */
        uint32_t hdr_word_count  : 4;   /* bits 14-17 */
        uint32_t data_byte_count : 14;  /* high bits: bits 18-31 */
    } __attribute__((packed)) f;
} udprdma_data_hdr_t;

typedef struct {
    uint16_t service_id;
    uint16_t reserved;
} __attribute__((packed)) udprdma_disc_hdr_t;

#define UDPRDMA_SVC_UDPFS 0xF5F5

/* IMPORTANT NOTE: udp_packet_t (ministack.h) has a 2-byte "align" field
 * after the udp header, to 4-byte-align a GENERIC payload that follows -
 * but UDPRDMA wants its own base header (2 bytes) exactly at offset
 * 0x2A, right after udp, WITHOUT that align in between (verified on
 * udpbd_pkt_t in udpbd.c, which in fact does NOT use udp_packet_t but
 * redefines raw eth+ip+udp). Using udp_packet_t here would have shifted
 * our whole payload 2 bytes further on the wire - bug already hit and
 * fixed. */
typedef struct {
    eth_header_t eth;
    ip_header_t  ip;
    udp_header_t udp;
} __attribute__((packed)) raw_udp_hdr_t;

/* UDP packet + UDPRDMA header + payload space, to build the
 * requests in a single contiguous buffer (as udpbd_pkt_t does). */
typedef struct {
    raw_udp_hdr_t     hdr;
    udprdma_base_hdr_t base;
    udprdma_data_hdr_t data;
    uint8_t          payload[512];
} __attribute__((packed)) udpfs_pkt_t;

/* ---------------------------------------------------------------- */
/* UDPFS messages (see UDPFS.md - these layouts are documented with
 * explicit byte offsets, much more reliable than the UDPRDMA header
 * above) */

#define UDPFS_MSG_OPEN_REQ    0x10
#define UDPFS_MSG_OPEN_REPLY  0x11
#define UDPFS_MSG_CLOSE_REQ   0x12
#define UDPFS_MSG_CLOSE_REPLY 0x13
#define UDPFS_MSG_GETSTAT_REQ   0x1E
#define UDPFS_MSG_GETSTAT_REPLY 0x1F
#define UDPFS_MSG_RESULT_REPLY 0x26
#define UDPFS_MSG_BREAD_REQ   0x28

struct udpfs_open_req {
    uint8_t  msg_type;
    uint8_t  is_dir;
    uint16_t flags;
    uint32_t mode;
    /* path follows, null-terminated */
} __attribute__((packed));

struct udpfs_open_reply {
    uint8_t  msg_type;
    uint8_t  reserved[3];
    int32_t  handle;
    uint32_t mode;
    uint32_t size;
    uint32_t hisize;
    uint8_t  ctime[8];
    uint8_t  mtime[8];
} __attribute__((packed));

struct udpfs_close_req {
    uint8_t  msg_type;
    uint8_t  reserved[3];
    int32_t  handle;
} __attribute__((packed));

/* GETSTAT_REPLY: 48 byte, result @4, mode @8, attr @12, size @16,
 * hisize @20, ctime @24, atime @32, mtime @40 (see PackGetstatReply). */
struct udpfs_getstat_reply {
    uint8_t  msg_type;
    uint8_t  reserved[3];
    int32_t  result;
    uint32_t mode;
    uint32_t attr;
    uint32_t size;
    uint32_t hisize;
    uint8_t  ctime[8];
    uint8_t  atime[8];
    uint8_t  mtime[8];
} __attribute__((packed));

/* BREAD_REQ (16 bytes): 512-byte sectors for regular files, at most
 * UDPFS_MAX_SECTOR_READ per request. The reply is RESULT_REPLY (8-byte app
 * header in the first packet) followed by the data, over multiple packets. */
struct udpfs_bread_req {
    uint8_t  msg_type;
    uint8_t  reserved;
    uint16_t sector_count;
    int32_t  handle;
    uint32_t sector_nr_lo;
    uint32_t sector_nr_hi;
} __attribute__((packed));

struct udpfs_result_reply {
    uint8_t  msg_type;
    uint8_t  reserved[3];
    int32_t  result;
} __attribute__((packed));

/* ---------------------------------------------------------------- */
/* client state (one exchange at a time, no concurrency
 * expected in this minimal version) */

static udp_socket_t *g_socket = NULL;
static uint16_t g_data_port = 0; /* discovered via DISCOVERY/INFORM, see udpfs_discover() */

/* Server: fixed IP from UDPFSCONFIG.DAT (g_server_fixed=1) or discovered
 * with a broadcast DISCOVERY (IP and MAC taken from the INFORM). With the
 * MAC known, packets go to the server via Ethernet unicast, otherwise
 * (discovery only) via broadcast. */
static uint32_t g_server_ip = 0;
static int      g_server_fixed = 0;
static uint8_t  g_server_mac[6];
static volatile int g_server_mac_ok = 0;
static int g_unicast_off = 0;      /* 1 = unicast never got a reply: Ethernet broadcast only */
static int g_mac_proven = 0;       /* 1 = at least one request succeeded with the server MAC */
/* discovery diagnostics (written by the handler, printed by the thread) */
static volatile int g_dbg_inform_seen = 0;
static volatile uint32_t g_dbg_inform_ip = 0;
static volatile uint16_t g_dbg_inform_port = 0;

void udpfs_client_set_server_ip(uint32_t ip)
{
    g_server_ip = ip;
    g_server_fixed = 1;
}

static void udpfs_pkt_init(udp_packet_t *pkt, uint16_t port)
{
    udp_packet_init(pkt, g_server_ip, port);
    if (g_server_mac_ok)
        memcpy(pkt->eth.addr_dst, g_server_mac, 6);
}
static int g_net_sema = -1;              /* serializes requests: reply buffer and pkt are static */
static volatile int g_net_failed = 0;    /* 1 = network unavailable, fail immediately */
static volatile int g_ready = 0; /* 1 only after discovery has finished (success or failure) -
                                   * prevents fs_open (POPStarter thread) from proceeding while
                                   * net_init_thread is still inside udpfs_discover() */

/* NOTE: SetAlarm+WaitEventFlag/iSetEventFlag have been removed from
 * this file - even though their use matched the reference code
 * (udpbd.c) exactly, the behavior observed on real hardware was still
 * "timeout" even when the expected packet arrived and was correctly
 * recognized. Replaced with a simple volatile flag checked by polling
 * (DelayThread) - same pattern already used successfully for g_ready,
 * simpler and easier to diagnose. */
static volatile int g_discovered_flag = 0;
static volatile int g_disc_waiting = 0;   /* 1 only while udpfs_discover() waits for the INFORM:
                                           * an INFORM outside that window is ignored */

/* Request/reply protocol (see udprdma/session_rx.go and session_tx.go
 * of the server):
 *  - the seq_nr of the BASE header is the counter of WHO TRANSMITS, so
 *    client and server have two independent counters;
 *  - every server DATA carries in seq_nr_ack the last accepted client
 *    seq: it is the right field to recognize the reply to a
 *    request, not the base seq_nr. */
static volatile int g_waiting_seq = -1;   /* seq of the pending request; -1 = none: the handler drops everything */
static volatile int g_reply_seq   = -1;   /* seq_nr_ack of the last reply completed by the handler */
static volatile int g_nack_seq = -1;      /* CLIENT seq that the server declares it expects, received
                                           * with a NACK (out-of-sequence request); -1 = no NACK */
static uint16_t g_rx_srv_seq = 0;         /* seq_nr (server counter) of the reply, for the ACK */
static uint16_t g_srv_next_seq = 0;       /* next server seq we expect: needed for the NACK.
                                           * The server starts from 0 (new session or after ResetSession). */
static uint16_t g_tx_seq_nr = 0; /* OUTGOING sequence counter: must be incremented at every
                                   * DATA packet after the first OPEN_REQ, otherwise the server
                                   * treats a repeated seq_nr as an already-seen duplicate instead
                                   * of a new request (server log: "got previous packet
                                   * 0 (expected 1), acking" — the READ_REQ was ignored) */

/* Clock with 1 s resolution, incremented by the client's own alarm
 * (udpfs_tick_start / udpfs_tick_cb, further below). */
static volatile uint32_t g_tick_s = 0;

/* Discovery negative cache: after a failure, for UDPFS_DISC_RETRY_SECS
 * seconds requests fail immediately instead of waiting 1 s each. */
#define UDPFS_DISC_RETRY_SECS   30   /* server never found */
#define UDPFS_REDISC_RETRY_SECS 2    /* known server that stops replying */
static int      g_disc_backoff  = 0;
static uint32_t g_disc_retry_at = 0;
static int      g_rediscover    = 0;

/* Consecutive failed requests (all attempts): beyond the threshold only
 * one attempt is made per request until the server replies. */
#define UDPFS_FAIL_STREAK_MAX 3
static int g_fail_streak = 0;

/* filled by the receive handler */
static uint8_t  g_rx_apphdr[40];   /* app header (OPEN_REPLY or RESULT_REPLY) */
static uint32_t g_rx_apphdr_len;
static uint8_t  g_rx_data[4096] __attribute__((aligned(64)));   /* data read (OPEN/GETSTAT/CLOSE/READ) */
static uint32_t g_rx_data_len;

/* Multi-packet receive (BREAD). The caller sets destination and
 * size BEFORE enabling g_waiting_seq; the handler (SMAP thread)
 * fills the destination packet by packet, in server seq order,
 * and signals completion by writing g_reply_seq as for
 * single-packet replies. Only the handler writes these fields during
 * reception. */
static uint8_t * volatile g_bread_dst  = NULL;  /* NULL = no BREAD in progress */
static volatile uint32_t  g_bread_size = 0;     /* bytes expected */
static volatile uint32_t  g_bread_got  = 0;     /* bytes already received */
static volatile int       g_bread_first = 0;    /* 1 = first packet (with RESULT_REPLY) received */
static uint16_t           g_bread_next  = 0;    /* next expected server seq */
static uint8_t            g_bread_win   = 0;    /* packets since the last window ACK */
static uint8_t            g_bread_nacked = 0;   /* NACK already sent for g_bread_next */
static uint8_t            g_bread_dupcnt = 0;   /* duplicates received since the last accepted packet */
#define UDPFS_BREAD_ACK_WINDOW 6   /* the server keeps at most 8 packets in flight */

#define COMPILER_BARRIER() __asm__ volatile("" ::: "memory")

/* The seconds counter is generated by the client's own alarm
 * (same mechanism as the link check timer in net_smap.c, which uses the
 * same SetAlarm/USec2SysClock imports already present), so it does not
 * depend on changes in other files. The callback runs in interrupt context
 * and only increments the counter. */
static iop_sys_clock_t g_tick_clock;
/* Tenths of a second: real clock for waiting on the INFORM (g_tick_s is
 * derived from it, one second every 10). */
static volatile uint32_t g_tick_ds = 0;

#ifdef UDPFS_STATS
/* Diagnostic build: the alarm runs at 1 ms and also provides a
 * millisecond clock to measure the duration of requests and reads. */
static volatile uint32_t g_tick_ms = 0;
static uint32_t g_tick_ms_sub = 0;

static unsigned int udpfs_tick_cb(void *arg)
{
    (void)arg;
    g_tick_ms++;
    if ((g_tick_ms % 100) == 0)
        g_tick_ds++;
    if (++g_tick_ms_sub >= 1000) {
        g_tick_ms_sub = 0;
        g_tick_s++;
    }
    return g_tick_clock.lo;
}

static void udpfs_tick_start(void)
{
    USec2SysClock(1000, &g_tick_clock);
    SetAlarm(&g_tick_clock, udpfs_tick_cb, NULL);
}
#else
static unsigned int udpfs_tick_cb(void *arg)
{
    (void)arg;
    if ((++g_tick_ds % 10) == 0)
        g_tick_s++;
    return g_tick_clock.lo; /* re-arms the alarm with the same interval */
}

static void udpfs_tick_start(void)
{
    USec2SysClock(100000, &g_tick_clock);
    SetAlarm(&g_tick_clock, udpfs_tick_cb, NULL);
}
#endif

/* ACK (is_ack=1: seq_nr_ack = last server seq received) or NACK
 * (is_ack=0: seq_nr_ack = expected server seq) sent by the receive
 * handler. Own buffer: the waiting thread uses its own. Does not consume
 * an outgoing seq_nr. smap_transmit has its own semaphore and is already
 * used from this context (ARP replies in net_ministack.c). */
static void udpfs_rx_send_ack(uint16_t srv_seq, int is_ack)
{
    static struct {
        raw_udp_hdr_t      hdr;
        udprdma_base_hdr_t base;
        udprdma_data_hdr_t data;
    } __attribute__((packed, aligned(4))) a;

    udpfs_pkt_init((udp_packet_t *)&a.hdr, g_data_port);
    a.base.raw               = 0;
    a.base.f.packet_type     = UDPRDMA_TYPE_DATA;
    a.base.f.seq_nr          = g_tx_seq_nr;
    a.data.raw               = 0;
    a.data.f.seq_nr_ack      = srv_seq;
    a.data.f.flags           = is_ack ? UDPRDMA_FLAG_ACK : 0;
    a.data.f.hdr_word_count  = 0;
    a.data.f.data_byte_count = 0;
    udp_packet_send(g_socket, (udp_packet_t *)&a.hdr, sizeof(a.base) + sizeof(a.data));
}

/* One packet of a multi-packet reply (BREAD). The first has the RESULT_REPLY
 * app header (8 bytes), the others carry data only; data goes directly into
 * the caller's destination. Only the packet with the expected seq is accepted:
 * a seq further ahead (lost packet) makes us send ONE NACK, one further back
 * (duplicate after a retransmission) is ignored. ACK every
 * UDPFS_BREAD_ACK_WINDOW packets: the server keeps at most 8 in flight
 * and without an ACK waits 100 ms before retransmitting. */
static void udpfs_rx_bread(uint16_t pointer, udprdma_base_hdr_t base,
                           udprdma_data_hdr_t data, int seq_ack)
{
    USE_SMAP_REGS;
    uint32_t hdr_bytes = data.f.hdr_word_count * 4;
    uint32_t n = data.f.data_byte_count;
    uint32_t remaining, offset, i, words;
    uint16_t seq = base.f.seq_nr;
    uint8_t *dst;

    if (!g_bread_first) {
        if (hdr_bytes < sizeof(struct udpfs_result_reply)) {
            /* the first packet of the reply did not arrive */
            if (!g_bread_nacked) {
                g_bread_nacked = 1;
                udpfs_rx_send_ack(g_srv_next_seq, 0);
            }
            return;
        }
    } else {
        uint16_t diff = (uint16_t)((seq - g_bread_next) & 0xFFF);

        if (diff != 0) {
            if (diff < 2048) {
                /* packet further ahead: one is missing, one NACK per expected seq */
                if (!g_bread_nacked) {
                    g_bread_nacked = 1;
                    udpfs_rx_send_ack(g_bread_next, 0);
                }
            } else if ((g_bread_dupcnt++ & 3) == 0) {
                /* duplicate of an already received packet: the server is
                 * retransmitting because it did not see our window ACK
                 * (lost). A NACK is not enough (it does not advance its
                 * window): a cumulative ACK of the last received is needed.
                 * One every 4 duplicates (on the first and fifth of a burst of 8):
                 * right away, and a second one if that one is lost too. With a
                 * single ACK per burst, losing it stalled the transfer
                 * until the server gave up. */
                udpfs_rx_send_ack((uint16_t)((g_bread_next - 1) & 0xFFF), 1);
            }
            return;
        }
    }

    offset = pointer + 0x30;
    SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = offset;

    /* app header: only in the first packet, the rest is discarded */
    if (hdr_bytes) {
        uint32_t hw = hdr_bytes / 4;
        uint32_t raw32;

        if (!g_bread_first) {
            g_rx_apphdr_len = hdr_bytes > sizeof(g_rx_apphdr) ? sizeof(g_rx_apphdr) : hdr_bytes;
        }
        for (i = 0; i < hw; i++) {
            raw32 = SMAP_REG32(SMAP_R_RXFIFO_DATA);
            if (!g_bread_first && i * 4 + 4 <= sizeof(g_rx_apphdr))
                *((uint32_t *)(g_rx_apphdr + i * 4)) = raw32;
            offset += 4;
        }
    }

    /* data, directly into the destination */
    remaining = g_bread_size - g_bread_got;
    if (n > remaining)
        n = remaining;
    dst   = g_bread_dst + g_bread_got;
    words = n / 4;
    if (((uintptr_t)dst & 3) == 0) {
        uint32_t *d32 = (uint32_t *)dst;

        for (i = 0; i < words; i++) {
            d32[i] = SMAP_REG32(SMAP_R_RXFIFO_DATA);
            offset += 4;
        }
    } else {
        for (i = 0; i < words; i++) {
            uint32_t raw32;

            raw32 = SMAP_REG32(SMAP_R_RXFIFO_DATA);
            memcpy(dst + i * 4, &raw32, 4);
            offset += 4;
        }
    }
    if (n & 3) {
        uint32_t raw32;

        raw32 = SMAP_REG32(SMAP_R_RXFIFO_DATA);
        memcpy(dst + words * 4, &raw32, n & 3);
    }

    g_bread_got  += n;
    g_bread_next  = (uint16_t)((seq + 1) & 0xFFF);
    g_bread_first = 1;
    g_bread_nacked = 0;
    g_bread_dupcnt = 0;

    if ((data.f.flags & UDPRDMA_FLAG_FIN) || g_bread_got >= g_bread_size) {
        g_rx_srv_seq = seq;
        COMPILER_BARRIER();
        g_reply_seq = seq_ack;   /* last: see udpfs_rx_handler */
        return;
    }

    if (++g_bread_win >= UDPFS_BREAD_ACK_WINDOW) {
        g_bread_win = 0;
        udpfs_rx_send_ack(seq, 1);
    }
}

/* Handler called by the ministack when a packet arrives on our
 * port. 'pointer' is the raw offset into the SMAP hardware FIFO
 * (same mechanism as udpbd_isr in udpbd.c).
 *
 * Runs in the SMAP thread (HandleRxIntr is called after the
 * WaitEventFlag in net_smap.c), NOT in a real interrupt.
 *
 * ALWAYS returns -1: in that case HandleRxIntr moves RXFIFO_RD_PTR to the
 * end of the frame (pointer + rounded length). With 0 the pointer
 * stays where the last read left it, i.e. mid-frame. The
 * packet has already been copied into the client buffers anyway.
 *
 * Discards, WITHOUT touching any buffer, everything that is not the expected
 * reply: empty ACK/NACKs from the server, retransmissions of replies already
 * received, packets whose seq_nr_ack differs from the pending request,
 * packets not from the server's data port. */
static int udpfs_rx_handler(udp_socket_t *socket, uint16_t pointer, void *arg)
{
    USE_SMAP_REGS;
    uint32_t raw32;
    udprdma_base_hdr_t base;
    udprdma_data_hdr_t data;
    uint32_t app_words, i;
    uint32_t offset;
    uint16_t src_port;
    int seq_ack;

    (void)socket;
    (void)arg;

    /* base header (2 bytes, at offset 0x2A from the start of the
     * eth/ip/udp packet): same 32-bit read trick with 2 bytes of
     * padding used by udpbd_isr for the same hardware alignment
     * reason. */
    SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = pointer + 0x28;
    raw32 = SMAP_REG32(SMAP_R_RXFIFO_DATA);
    base.raw = (uint16_t)(raw32 >> 16);

    /* data header (4 bytes, at offset 0x2C - naturally 4-byte aligned) */
    SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = pointer + 0x2C;
    data.raw = SMAP_REG32(SMAP_R_RXFIFO_DATA);

    /* The source UDP port is at offset 0x22, not 4-byte aligned, and
     * a direct SMAP_REG16 there does not work on the FIFO hardware
     * (confirmed via hex dump: it read 2 bytes too early). Read 32
     * bits at the previous aligned offset (0x20) and extract the
     * 2 high bytes, same technique as the UDPRDMA base header at 0x2A.
     * Standard UDP field in network byte order, hence ntohs. */
    SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = pointer + 0x20;
    raw32 = SMAP_REG32(SMAP_R_RXFIFO_DATA);
    src_port = ntohs((uint16_t)(raw32 >> 16));

    if (base.f.packet_type == UDPRDMA_TYPE_INFORM) {
        /* The data port does NOT travel in the discovery/inform payload (there
         * is only service_id+reserved, always 0): it is the SOURCE port
         * of the packet, from the server's ephemeral data socket. Accepted
         * only while udpfs_discover() is waiting: an INFORM outside the
         * window (another host, network delay) must not overwrite the
         * data port in use. */
        /* During discovery the first INFORM is accepted. Outside the window
         * only if the data port is unknown: a late INFORM (network
         * not ready yet, slow server) still establishes the session;
         * with an active session it is discarded instead. */
        if (g_disc_waiting ? g_discovered_flag : (g_data_port != 0))
            return -1;
        if (src_port == 0)
            return -1;

        /* Sender IP and MAC from the frame: MAC at 6..11, IP at 26..29
         * (32-bit reads at the 4-aligned offsets 4, 8, 24, 28). */
        {
            uint32_t w4, w8, w24, w28, sip;

            SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = pointer + 0x04;
            w4  = SMAP_REG32(SMAP_R_RXFIFO_DATA);
            SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = pointer + 0x08;
            w8  = SMAP_REG32(SMAP_R_RXFIFO_DATA);
            SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = pointer + 0x18;
            w24 = SMAP_REG32(SMAP_R_RXFIFO_DATA);
            SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = pointer + 0x1C;
            w28 = SMAP_REG32(SMAP_R_RXFIFO_DATA);

            sip = (((w24 >> 16) & 0xFF) << 24) | (((w24 >> 24) & 0xFF) << 16) |
                  ((w28 & 0xFF) << 8) | ((w28 >> 8) & 0xFF);
            g_dbg_inform_seen++;
            g_dbg_inform_ip = sip;
            g_dbg_inform_port = src_port;
            /* With a fixed IP the DISCOVERY is directed at that host: only
             * it replies, so the sender IP is not checked (a server with
             * several addresses may reply from another one) and g_server_ip
             * stays the configured one. */
            if (!g_server_fixed) {
                if (sip == 0)
                    return -1;
                g_server_ip = sip;
            }

            g_server_mac[0] = (uint8_t)(w4 >> 16);
            g_server_mac[1] = (uint8_t)(w4 >> 24);
            g_server_mac[2] = (uint8_t)w8;
            g_server_mac[3] = (uint8_t)(w8 >> 8);
            g_server_mac[4] = (uint8_t)(w8 >> 16);
            g_server_mac[5] = (uint8_t)(w8 >> 24);
            g_mac_proven = 0;
            g_server_mac_ok = g_unicast_off ? 0 : 1;
        }

        g_data_port = src_port;
        g_discovered_flag = 1;
        return -1;
    }

    if (base.f.packet_type != UDPRDMA_TYPE_DATA)
        return -1;

    /* Pure ACK/NACKs from the server (no payload): no state to touch.
     * The server sends one for every packet received, duplicates
     * included, and a late one must not invalidate the good reply. */
    if (data.f.hdr_word_count == 0 && data.f.data_byte_count == 0) {
        /* A NACK (without the ACK flag) is the server's reply to a
         * request with an out-of-sequence seq_nr: seq_nr_ack is the seq it
         * expects. We only record it; it is up to the waiting thread to
         * resynchronize (see udpfs_send_and_wait). */
        if (!(data.f.flags & UDPRDMA_FLAG_ACK) && g_waiting_seq >= 0 &&
            g_data_port != 0 && src_port == g_data_port)
            g_nack_seq = (int)data.f.seq_nr_ack;
        return -1;
    }

    /* The reply to a request carries the client's seq in seq_nr_ack. */
    seq_ack = (int)data.f.seq_nr_ack;
    if (g_waiting_seq < 0 || seq_ack != g_waiting_seq)
        return -1;
    if (g_reply_seq == seq_ack)
        return -1; /* retransmission of an already completed reply */

    if (g_data_port == 0 || src_port != g_data_port)
        return -1;

    /* BREAD in progress: multi-packet reply, directly into the
     * caller's destination. */
    if (g_bread_dst != NULL) {
        udpfs_rx_bread(pointer, base, data, seq_ack);
        return -1;
    }

    /* All other requests: single-packet replies only (FIN).
     * A multi-packet reply (header+data over 1408 bytes) is not
     * handled here and must not corrupt the buffers. */
    if (!(data.f.flags & UDPRDMA_FLAG_FIN))
        return -1;

    offset = pointer + 0x30;

    /* 1. App Header extraction */
    app_words = data.f.hdr_word_count;
    g_rx_apphdr_len = app_words * 4;
    if (g_rx_apphdr_len > sizeof(g_rx_apphdr))
        g_rx_apphdr_len = sizeof(g_rx_apphdr);

    /* Explicit starting point: at this point the pointer was left
     * elsewhere by the header reads. It is the only setting,
     * then RXFIFO_DATA advances on its own. */
    SMAP_REG16(SMAP_R_RXFIFO_RD_PTR) = offset;

    for (i = 0; i < app_words; i++) {
        raw32 = SMAP_REG32(SMAP_R_RXFIFO_DATA);
        if (i * 4 + 4 <= sizeof(g_rx_apphdr)) {
            *((uint32_t*)(g_rx_apphdr + i * 4)) = raw32;
        }
        offset += 4;
    }

    /* 2. Data extraction */
    g_rx_data_len = data.f.data_byte_count;
    if (g_rx_data_len > sizeof(g_rx_data))
        g_rx_data_len = sizeof(g_rx_data);

    if (g_rx_data_len > 0) {
        uint32_t words = (g_rx_data_len + 3) / 4;
        uint32_t *dest_ptr = (uint32_t*)g_rx_data;
        for (i = 0; i < words; i++) {
            dest_ptr[i] = SMAP_REG32(SMAP_R_RXFIFO_DATA);
            offset += 4;
        }
    }

    g_rx_srv_seq = base.f.seq_nr;

    /* Completion is signaled LAST, by writing the request's seq: the
     * waiting thread compares it with its own, so a late completion
     * of an already abandoned request cannot be mistaken for that of
     * the next request. */
    COMPILER_BARRIER();
    g_reply_seq = seq_ack;
    return -1;
}

#define UDPFS_LINK_WAIT_US (4 * 1000 * 1000) /* max wait for the Ethernet link */
#define UDPFS_PROBE_US   (450 * 1000)   /* server restart probe: 3 DISCOVERY packets at 150 ms */

/* Real-time wait for the INFORM: the tenth-of-a-second counter
 * (g_tick_ds, hardware alarm) measures true time even if DelayThread
 * returns early (high-priority POPStarter threads). With a tenth
 * of a tick the wait is at most 100 ms longer. The iteration limit
 * is only a safety net in case the alarm is not running. */
static void udpfs_disc_wait(uint32_t us)
{
    uint32_t t0 = g_tick_ds;
    int32_t ds = (int32_t)(us / 100000u) + 1;
    uint32_t iters = us / 100u + 10, i;

    for (i = 0; i < iters && !g_discovered_flag; i++) {
        if ((int32_t)(g_tick_ds - t0) >= ds)
            break;
        DelayThread(10 * 1000);
    }
}

static int udpfs_discover(uint32_t timeout_us)
{
    static struct {
        raw_udp_hdr_t       hdr;
        udprdma_base_hdr_t base;
        udprdma_disc_hdr_t disc;
    } __attribute__((packed)) pkt;
    /* Boot discovery (long window): the first packet is often lost right
     * after the link comes up, so start with short intervals (250 ms) and
     * back off: 6 packets with weights 1,1,2,4,6,6 of the window. The short
     * windows (re-discovery, probe) send 3 equal ones. */
    static const uint8_t w_boot[] = {1, 1, 2, 4, 6, 6};
    static const uint8_t w_even[] = {1, 1, 1};
    const uint8_t *w = (timeout_us >= 3 * 1000 * 1000) ? w_boot : w_even;
    int ntries       = (timeout_us >= 3 * 1000 * 1000) ? (int)sizeof(w_boot) : (int)sizeof(w_even);
    uint32_t wsum    = (timeout_us >= 3 * 1000 * 1000) ? 20 : 3;
    uint32_t per_try_us;
    int tries;

    if (g_socket == NULL)
        return -1;

    g_discovered_flag = 0;
    g_dbg_inform_seen = 0;
    g_server_mac_ok = 0;      /* discovery always starts as an Ethernet broadcast */
    g_disc_waiting = 1;

    for (tries = 0; tries < ntries && !g_discovered_flag; tries++) {
        /* fixed IP: DISCOVERY directed at the server; otherwise broadcast */
        udp_packet_init((udp_packet_t *)&pkt.hdr,
                        g_server_fixed ? g_server_ip : IP_ADDR(255, 255, 255, 255), UDPFS_PORT);

        pkt.base.raw             = 0;
        pkt.base.f.packet_type   = UDPRDMA_TYPE_DISCOVERY;
        pkt.base.f.seq_nr        = 0;

        pkt.disc.service_id = UDPRDMA_SVC_UDPFS;
        pkt.disc.reserved   = 0;

        udp_packet_send(g_socket, (udp_packet_t *)&pkt.hdr, sizeof(pkt.base) + sizeof(pkt.disc));

        per_try_us = timeout_us / wsum * w[tries];
        if (per_try_us < 50 * 1000)
            per_try_us = 50 * 1000;
        udpfs_disc_wait(per_try_us);
    }

    g_disc_waiting = 0;

    if (g_discovered_flag)
        printf("UDPFS: INFORM from %d.%d.%d.%d port %d, server %d.%d.%d.%d, MAC %02x:%02x:%02x:%02x:%02x:%02x%s\n",
               (int)(g_dbg_inform_ip >> 24), (int)((g_dbg_inform_ip >> 16) & 255), (int)((g_dbg_inform_ip >> 8) & 255),
               (int)(g_dbg_inform_ip & 255), (int)g_dbg_inform_port,
               (int)(g_server_ip >> 24), (int)((g_server_ip >> 16) & 255), (int)((g_server_ip >> 8) & 255), (int)(g_server_ip & 255),
               g_server_mac[0], g_server_mac[1], g_server_mac[2], g_server_mac[3], g_server_mac[4], g_server_mac[5],
               g_server_mac_ok ? " (unicast)" : " (Ethernet broadcast)");
    else
        printf("UDPFS: no INFORM accepted after %d DISCOVERY packets (INFORMs seen: %d)\n", ntries, (int)g_dbg_inform_seen);
    printf("%s\n", udpfs_cfg_diag);
    printf("%s\n", udpfs_cfg_diag2);

    return g_discovered_flag ? 0 : -1;
}

void udpfs_client_init(void)
{
    udpfs_tick_start();

    g_socket = udp_bind(UDPFS_PORT, udpfs_rx_handler, NULL);

    /* smap_init() returns before the PHY is up (link negotiation takes
     * seconds after an IOP reset): DISCOVERY packets sent earlier are lost
     * and the failed discovery would then block requests for
     * UDPFS_DISC_RETRY_SECS. Wait for the link, in real time. */
    {
        uint32_t t0 = g_tick_ds;
        uint32_t i;

        for (i = 0; i < UDPFS_LINK_WAIT_US / 10000u + 10 && !smap_is_initialized(); i++) {
            if ((int32_t)(g_tick_ds - t0) >= (int32_t)(UDPFS_LINK_WAIT_US / 100000u) + 1)
                break;
            DelayThread(10 * 1000);
        }
    }


    UDPFS_MARK(UDPFS_M_LINK);

    /* Discovery is needed even with a fixed server IP: the server's data
     * port is assigned randomly by the operating system at every start and
     * is communicated only through the INFORM reply.
     * A single attempt, no retry (minimal choice). */
    if (!smap_is_initialized()) {
        /* No link (cable unplugged): nobody can answer, do not wait for a
         * discovery window. The first request after the retry delay tries
         * again. */
        g_disc_backoff  = 1;
        g_disc_retry_at = g_tick_s + UDPFS_DISC_RETRY_SECS;
    } else if (udpfs_discover(3 * 1000 * 1000) != 0) {
        /* Server absent at boot: the first requests fail immediately
         * (see negative cache in udpfs_send_and_wait). */
        g_disc_backoff  = 1;
        g_disc_retry_at = g_tick_s + UDPFS_DISC_RETRY_SECS;
    }


    UDPFS_MARK(UDPFS_M_DISC);

    /* g_ready must be set ONLY now, once discovery has finished (success or
     * failure): fs_open (called by POPStarter on a thread different from
     * this one) can fire at any moment, even while discovery is still
     * in progress. */
    g_ready = 1;
    UDPFS_MARK(UDPFS_M_READY);
    udpfs_marks_print();
}

/* Attempts per request and timeout. The timeout given by the caller is
 * that of the FIRST attempt and doubles at every subsequent attempt
 * (x1, x2, x4, x8). On attempts after the first, besides resending the
 * request, a NACK carrying the expected server seq is sent: if it was the
 * REPLY that got lost, the server retransmits it immediately from its own
 * buffer (onPeerNack -> retransmitFrom). Resending only the request is not
 * enough: the server recognizes it as a duplicate, sends an ACK and
 * re-executes nothing; it would only remember the reply when the FIN timer
 * expires (500 ms). */
#define UDPFS_RETRY_COUNT   4
#define UDPFS_TMO_FAST_US   (15 * 1000)   /* CLOSE, GETSTAT: 15+30+60+120 = 225 ms */
#define UDPFS_TMO_BREAD_US  (100 * 1000)  /* BREAD: maximum inactivity, 100+200+400+800 ms */
#ifndef UDPFS_BREAD_POLL_US
#define UDPFS_BREAD_POLL_US 100
#endif
#define UDPFS_TMO_META_US   (100 * 1000)  /* OPEN, GETSTAT: 100+200+400+800 = 1.5 s
                                           * (slow disk, files compressed on the server side) */

/* Outcome of udpfs_wait_reply */
#define UDPFS_WAIT_TIMEOUT 0
#define UDPFS_WAIT_REPLY   1
#define UDPFS_WAIT_RESYNC  2   /* the server expects a seq different from ours */

/* Waits for the handler to complete the reply to request 'seq', or for the
 * server to signal (NACK) that it expects a different seq.
 * Durations are estimates (DelayThread), not a clock. */
static int udpfs_wait_reply(int seq, uint32_t timeout_us)
{
    uint32_t waited_us = 0;
    uint32_t last_got = g_bread_got;
    int fast_spins = 0;

    while (waited_us < timeout_us) {
        if (g_reply_seq == seq)
            return UDPFS_WAIT_REPLY;
        if (g_nack_seq >= 0 && g_nack_seq != seq)
            return UDPFS_WAIT_RESYNC;

        if (g_bread_dst != NULL) {
            /* BREAD: the timeout is one of INACTIVITY (it restarts at every
             * progress) and the sleep is short to notice the end right away. */
            if (g_bread_got != last_got) {
                last_got = g_bread_got;
                waited_us = 0;
            }
            DelayThread(UDPFS_BREAD_POLL_US);
            waited_us += UDPFS_BREAD_POLL_US;
            continue;
        }

        /* Aggressive polling: the first ~50 cycles do not force 1ms of sleep,
         * to catch the server's ultra-fast replies without waiting for a
         * whole tick. */
        if (fast_spins < 50) {
            DelayThread(0);
            fast_spins++;
            waited_us += 50; /* rough estimate */
        } else {
            DelayThread(UDPFS_POLL_SLEEP_US);
            waited_us += UDPFS_POLL_SLEEP_US;
        }
    }
    return (g_reply_seq == seq) ? UDPFS_WAIT_REPLY : UDPFS_WAIT_TIMEOUT;
}

/* NACK: DATA packet with no payload and no ACK flag, with seq_nr_ack =
 * first server seq not received. It does not consume an outgoing seq_nr (like
 * ACKs: the server does not check the seq of control packets). */
static void udpfs_send_nack(void)
{
    static struct {
        raw_udp_hdr_t      hdr;
        udprdma_base_hdr_t base;
        udprdma_data_hdr_t data;
    } __attribute__((packed, aligned(4))) nack;

    udpfs_pkt_init((udp_packet_t *)&nack.hdr, g_data_port);
    nack.base.raw               = 0;
    nack.base.f.packet_type     = UDPRDMA_TYPE_DATA;
    nack.base.f.seq_nr          = g_tx_seq_nr;
    nack.data.raw               = 0;
    nack.data.f.seq_nr_ack      = (g_bread_dst != NULL && g_bread_first) ? g_bread_next : g_srv_next_seq;
    nack.data.f.flags           = 0;
    nack.data.f.hdr_word_count  = 0;
    nack.data.f.data_byte_count = 0;
    udp_packet_send(g_socket, (udp_packet_t *)&nack.hdr, sizeof(nack.base) + sizeof(nack.data));
}

#ifdef UDPFS_STATS
/* ------------------------------------------------------------------ */
/* Live statistics (only -DUDPFS_STATS, see Makefile).
 *
 * Once per second, at the end of a POPStarter read, the client sends the
 * server a GETSTAT whose "path" is the summary of the second just elapsed;
 * with the server in -verbose it appears in its log:
 *   GETSTAT "UDPFS_STATS t=412 c=150 b=307200 ms=2048 cmax=9 cavg=2 sc=0 r=450 rmax=4 sr=0 rt=0 rs=0 f=0": -5
 *   t    tick in seconds (a gap = no read completed in that time)
 *   c    POPStarter read calls (udpfs_stub_read)
 *   b    bytes delivered;   ms  maximum size requested in one call
 *   cmax maximum duration of a call (ms);  cavg average duration (ms)
 *   sc   calls longer than UDPFS_STATS_SLOW_CALL_MS
 *   r    completed network requests (including the report GETSTATs)
 *   rmax maximum duration of a request (ms)
 *   sr   requests longer than UDPFS_STATS_SLOW_REQ_MS
 *   gmax maximum pause (ms) between the end of a read call and the start
 *        of the next;  gavg average pause: time during which POPStarter is
 *        not waiting for this module
 *   rt   retransmissions (requests that needed more than one
 *        attempt);  rs  seq resynchronizations;  f  failed requests
 * Resolution 1 ms. The report GETSTAT is one extra request per
 * second, negligible but not zero. */
#ifndef UDPFS_STATS_SLOW_REQ_MS
#define UDPFS_STATS_SLOW_REQ_MS 5
#endif
#ifndef UDPFS_STATS_SLOW_CALL_MS
#define UDPFS_STATS_SLOW_CALL_MS 20
#endif

static uint32_t st_reqs, st_req_max, st_req_slow, st_retry, st_resync, st_fail;
static uint32_t st_calls, st_bytes, st_maxsz, st_call_max, st_call_sum, st_call_slow;
static uint32_t st_last_tick;
static uint32_t st_last_end, st_have_end, st_gap_max, st_gap_sum, st_gap_n;
uint32_t udpfs_stats_now_ms(void)
{
    return g_tick_ms;
}

static void udpfs_stats_req_end(uint32_t t0, int ok, int attempt, int resynced)
{
    uint32_t dur = g_tick_ms - t0;

    if (!ok) {
        st_fail++;
        return;
    }
    st_reqs++;
    if (dur > st_req_max)
        st_req_max = dur;
    if (dur > UDPFS_STATS_SLOW_REQ_MS)
        st_req_slow++;
    if (attempt > 0)
        st_retry++;
    if (resynced)
        st_resync++;
}

void udpfs_stats_read_done(uint32_t t0, uint32_t size, uint32_t got)
{
    uint32_t dur = g_tick_ms - t0;
    uint32_t tick = g_tick_s;

    /* pause between the end of the previous call and the start of this one: it is
     * the time during which POPStarter is NOT waiting for this module */
    if (st_have_end) {
        uint32_t gap = t0 - st_last_end;

        if (gap > st_gap_max)
            st_gap_max = gap;
        st_gap_sum += gap;
        st_gap_n++;
    }

    st_calls++;
    st_bytes += got;
    if (size > st_maxsz)
        st_maxsz = size;
    if (dur > st_call_max)
        st_call_max = dur;
    st_call_sum += dur;
    if (dur > UDPFS_STATS_SLOW_CALL_MS)
        st_call_slow++;

    if (tick != st_last_tick) {
        char msg[200];
        udpfs_stat_t st;
        uint32_t cavg = st_calls ? st_call_sum / st_calls : 0;

        sprintf(msg, "UDPFS_STATS t=%u c=%u b=%u ms=%u cmax=%u cavg=%u sc=%u r=%u rmax=%u sr=%u rt=%u rs=%u f=%u gmax=%u gavg=%u",
                (unsigned)tick, (unsigned)st_calls, (unsigned)st_bytes, (unsigned)st_maxsz,
                (unsigned)st_call_max, (unsigned)cavg, (unsigned)st_call_slow,
                (unsigned)st_reqs, (unsigned)st_req_max, (unsigned)st_req_slow,
                (unsigned)st_retry, (unsigned)st_resync, (unsigned)st_fail,
                (unsigned)st_gap_max, (unsigned)(st_gap_n ? st_gap_sum / st_gap_n : 0));
        if (strlen(msg) > 120) /* the path of a GETSTAT must stay under 128 bytes */
            msg[120] = 0;

        st_last_tick = tick;
        st_calls = st_bytes = st_maxsz = st_call_max = st_call_sum = st_call_slow = 0;
        st_reqs = st_req_max = st_req_slow = st_retry = st_resync = st_fail = 0;
        st_gap_max = st_gap_sum = st_gap_n = 0;

        udpfs_client_getstat(msg, &st); /* the report is the path: the server logs it */
    }

    st_last_end = g_tick_ms;   /* after the report, if any: does not count as a POPStarter pause */
    st_have_end = 1;
}
#define ST_DECL()          uint32_t st_t0 = g_tick_ms
#define ST_REQ_END(ok)    udpfs_stats_req_end(st_t0, (ok), attempt, resynced)
#else
#define ST_DECL()          ((void)0)
#define ST_REQ_END(ok)    ((void)0)
#endif

/* Sends 'payload' as a DATA packet
 * with FIN=1 and waits for the reply (data in g_rx_*). 'timeout_us' is the
 * timeout of the first attempt, see above. Returns 0 if the reply arrived,
 * -1 otherwise. */
static int udpfs_send_and_wait(const void *payload, uint16_t payload_len, uint32_t timeout_us)
{
    static udpfs_pkt_t pkt;
    uint16_t total, padded;
    uint16_t this_seq;
    int attempt, max_attempts, got = 0, resynced = 0, probed = 0;
    ST_DECL();

    if (g_net_failed)
        return -1;

    /* The wait for g_ready goes BEFORE the check on g_socket: if a
     * request arrives while net_init_thread is still initializing,
     * g_socket is still NULL and it used to fail immediately. */
    if (!g_ready) {
        int w = 0;
        while (!g_ready && !g_net_failed && w < 14 * 1000 * 1000) {
            DelayThread(1 * 1000);
            w += 1 * 1000;
        }
        if (!g_ready || g_net_failed) {
            return -1;
        }
    }

    if (g_socket == NULL)
        return -1;

    if (payload_len > sizeof(pkt.payload))
        return -1; /* before consuming a seq_nr */

    /* Lazy discovery: if the server was not there at boot (or did not reply
     * within 5 s) the data port is 0. Retry here, with a short timeout,
     * but no more than once every UDPFS_DISC_RETRY_SECS: with the server
     * off, a scan of several paths by POPStarter would cost
     * 1 s per path. No DATA packet has been sent yet, so
     * g_tx_seq_nr is still 0 and there is no need to reset it. */
    if (g_data_port == 0) {
        if (g_disc_backoff && (int32_t)(g_tick_s - g_disc_retry_at) < 0)
            return -1;
        if (udpfs_discover(1 * 1000 * 1000) != 0 || g_data_port == 0) {
            g_disc_backoff  = 1;
            g_disc_retry_at = g_tick_s + (g_rediscover ? UDPFS_REDISC_RETRY_SECS : UDPFS_DISC_RETRY_SECS);
            return -1;
        }
        g_disc_backoff = 0;
        g_rediscover   = 0;
    }

    udpfs_pkt_init((udp_packet_t *)&pkt.hdr, g_data_port);

    this_seq = g_tx_seq_nr;
    g_tx_seq_nr = (g_tx_seq_nr + 1) & 0xFFF;

    pkt.base.raw            = 0;
    pkt.base.f.packet_type  = UDPRDMA_TYPE_DATA;
    pkt.base.f.seq_nr       = this_seq;

    pkt.data.raw               = 0;
    pkt.data.f.seq_nr_ack      = 0;
    pkt.data.f.flags           = UDPRDMA_FLAG_FIN;
    pkt.data.f.hdr_word_count  = 0;
    pkt.data.f.data_byte_count = payload_len;

    memcpy(pkt.payload, payload, payload_len);

    total  = 2 + 4 + payload_len;
    padded = (total + 3) & ~3;

    /* With the server not replying for UDPFS_FAIL_STREAK_MAX requests,
     * a single attempt per request: the cost of a failure stays
     * bounded until it starts replying again. */
    max_attempts = (g_fail_streak >= UDPFS_FAIL_STREAK_MAX) ? 1 : UDPFS_RETRY_COUNT;

    /* Enable the handler BEFORE sending: a very fast reply
     * must not be dropped. g_reply_seq is reset only once
     * here, not at every attempt: a reply that arrived between two attempts
     * must not be lost. */
    g_reply_seq = -1;
    g_nack_seq  = -1;
    COMPILER_BARRIER();
    g_waiting_seq = this_seq;

    for (attempt = 0; attempt < max_attempts; attempt++) {
        int r;

        /* Server restart probe: two attempts without a reply may mean
         * that the server restarted with a new data port (the old one is
         * closed, only ICMP arrives). After ONE failed read POPS does not send
         * another, so rediscovery must happen here, inside the
         * same request. A short DISCOVERY: with the same port the server is
         * alive (only packet loss) and we carry on; with a different port
         * the session is new (seq 0, the server does not know our seq) and the
         * request starts over from scratch; with no reply we carry on as before. */
        if (attempt == 2 && !probed && g_data_port != 0) {
            uint16_t old_port = g_data_port;
            int old_mac_ok = g_server_mac_ok;
            int old_proven = g_mac_proven;

            probed = 1;
            if (udpfs_discover(UDPFS_PROBE_US) == 0 && g_data_port != old_port) {
                printf("UDPFS: server restarted (data port %d -> %d), new session\n", (int)old_port, (int)g_data_port);
                this_seq        = 0;
                g_tx_seq_nr     = 1;
                g_srv_next_seq  = 0;
                g_fail_streak   = 0;
                udpfs_pkt_init((udp_packet_t *)&pkt.hdr, g_data_port);
                pkt.base.f.seq_nr = 0;

                g_bread_got = 0;
                g_bread_first = 0;
                g_bread_win = 0;
                g_bread_nacked = 0;
                g_bread_dupcnt = 0;

                g_reply_seq = -1;
                g_nack_seq  = -1;
                COMPILER_BARRIER();
                g_waiting_seq = this_seq;
                attempt = -1;       /* the for restarts from 0 */
                continue;
            }
            /* same port or no reply: nothing has changed */
            g_mac_proven = old_proven;
            if (!g_discovered_flag)
                g_server_mac_ok = old_mac_ok;
        }

        /* The MAC learned from the INFORM never let a request through:
         * from the third attempt we go back to Ethernet broadcast (which works
         * with discovery) and unicast stays off. After a successful request
         * it is left alone: a failure is then just a missing network. */
        if (attempt == 2 && g_server_mac_ok && !g_mac_proven) {
            g_unicast_off = 1;
            g_server_mac_ok = 0;
            memset(((udp_packet_t *)&pkt.hdr)->eth.addr_dst, 0xff, 6);
            printf("UDPFS: no reply to unicast, falling back to Ethernet broadcast\n");
        }

        if (attempt > 0)
            udpfs_send_nack();

        udp_packet_send(g_socket, (udp_packet_t *)&pkt.hdr, padded);

        r = udpfs_wait_reply(this_seq, timeout_us << attempt);

        if (r == UDPFS_WAIT_REPLY) {
            got = 1;
            break;
        }

        if (r == UDPFS_WAIT_RESYNC) {
            int expected = g_nack_seq;

            g_nack_seq = -1;
            if (!resynced) {
                /* The server never received one or more previous requests
                 * (e.g. cable unplugged: their seq_nr was consumed anyway)
                 * and expects a lower seq. Without resynchronization every
                 * following request would be dropped with a NACK, forever.
                 * We resume from the expected seq and resend the request
                 * immediately; this resend does not consume an attempt. */
                this_seq    = (uint16_t)expected;
                g_tx_seq_nr = (this_seq + 1) & 0xFFF;
                pkt.base.f.seq_nr = this_seq;

                /* any BREAD in progress: start over from zero */
                g_bread_got = 0;
                g_bread_first = 0;
                g_bread_win = 0;
                g_bread_nacked = 0;
                g_bread_dupcnt = 0;

                g_reply_seq = -1;
                COMPILER_BARRIER();
                g_waiting_seq = this_seq;

                resynced = 1;
                attempt--;
            }
            /* Second resynchronization in the same request: the
             * attempt counts as lost. */
        }
    }

    /* From here the handler drops everything (duplicates included) and the
     * g_rx_* buffers stay stable while the caller reads them. */
    g_waiting_seq = -1;
    COMPILER_BARRIER();

    if (!got && g_reply_seq == (int)this_seq)
        got = 1; /* arrived at the last moment */

    if (!got) {
        if (g_fail_streak < 1000)
            g_fail_streak++;
        /* Two requests in a row without a reply: the server may have been
         * restarted (new data port) or changed address. We forget the
         * port: the next request redoes the discovery. */
        if (g_fail_streak >= 2 && g_data_port != 0) {
            g_data_port = 0;
            g_server_mac_ok = 0;
            g_disc_backoff = 0;
            g_rediscover = 1;
        }
        ST_REQ_END(0);
        return -1;
    }
    g_fail_streak = 0;
    if (g_server_mac_ok)
        g_mac_proven = 1;
    ST_REQ_END(1);

    /* The server numbers its own packets independently: the next one we
     * expect (for a possible NACK) is the one after the reply. */
    g_srv_next_seq = (g_rx_srv_seq + 1) & 0xFFF;

    /* Final ACK for the received reply. seq_nr_ack is the SERVER's seq. */
    {
        static struct {
            raw_udp_hdr_t      hdr;
            udprdma_base_hdr_t base;
            udprdma_data_hdr_t data;
        } __attribute__((packed)) ack;

        udpfs_pkt_init((udp_packet_t *)&ack.hdr, g_data_port);
        ack.base.raw               = 0;
        ack.base.f.packet_type     = UDPRDMA_TYPE_DATA;
        ack.base.f.seq_nr          = g_tx_seq_nr;
        ack.data.raw               = 0;
        ack.data.f.seq_nr_ack      = g_rx_srv_seq;
        ack.data.f.flags           = UDPRDMA_FLAG_ACK;
        ack.data.f.hdr_word_count  = 0;
        ack.data.f.data_byte_count = 0;
        udp_packet_send(g_socket, (udp_packet_t *)&ack.hdr, sizeof(ack.base) + sizeof(ack.data));
    }

    return 0;
}

void udpfs_client_preinit(void)
{
    iop_sema_t sema_param;

    if (g_net_sema >= 0)
        return;

    sema_param.attr    = 0;
    sema_param.initial = 1;
    sema_param.max     = 1;
    sema_param.option  = 0;
    g_net_sema = CreateSema(&sema_param);
}

void udpfs_client_set_failed(void)
{
    g_net_failed = 1;
}

static int udpfs_lock(void)
{
    if (g_net_failed || g_net_sema < 0)
        return -1;
    WaitSema(g_net_sema);
    return 0;
}

static void udpfs_unlock(void)
{
    SignalSema(g_net_sema);
}

static int udpfs_client_open_nl(const char *path, uint32_t *out_size)
{
    /* static, same reason as pkt in udpfs_send_and_wait */
    static uint8_t buf[8 + 128];
    struct udpfs_open_req *req = (struct udpfs_open_req *)buf;
    uint32_t path_len;
    struct udpfs_open_reply *reply;

    path_len = strlen(path);
    if (path_len >= 128)
        return -1;

    req->msg_type = UDPFS_MSG_OPEN_REQ;
    req->is_dir   = 0;
    req->flags    = 0; /* read-only */
    req->mode     = 0;
    memcpy(buf + 8, path, path_len + 1);

    if (udpfs_send_and_wait(buf, 8 + path_len + 1, UDPFS_TMO_META_US) != 0)
        return -1;

    if (g_rx_apphdr_len >= sizeof(struct udpfs_open_reply)) {
        reply = (struct udpfs_open_reply *)g_rx_apphdr;
    } else if (g_rx_data_len >= sizeof(struct udpfs_open_reply)) {
        /* alternative hypothesis: server sends OPEN_REPLY as "data"
         * (data_byte_count), not as "app header" (hdr_word_count) -
         * try this one too */
        reply = (struct udpfs_open_reply *)g_rx_data;
    } else {
        return -1;
    }

    if (reply->msg_type != UDPFS_MSG_OPEN_REPLY || reply->handle < 0)
        return -1;

    if (out_size)
        *out_size = reply->size;
    return reply->handle;
}

#define UDPFS_MAX_SECTOR_READ 128   /* 512-byte sectors per BREAD_REQ (64 KB) */

/* BREAD: reads 'count' 512-byte sectors starting at 'sector' directly
 * into 'buffer', with one request per 64 KB and the reply spread over several
 * packets (see udpfs_rx_bread). Returns the bytes read (fewer than requested
 * if the file ends first), -1 if the server does not reply, -2 if it replies
 * with an error (invalid handle, etc.). Does not depend on the file cursor. */
static int udpfs_client_bread_nl(int handle, uint32_t sector, uint32_t count, void *buffer)
{
    static struct udpfs_bread_req req __attribute__((aligned(4)));
    uint8_t *dst = (uint8_t *)buffer;
    uint32_t total = 0;

    while (count > 0) {
        uint32_t chunk = count > UDPFS_MAX_SECTOR_READ ? UDPFS_MAX_SECTOR_READ : count;
        uint32_t bytes = chunk * 512;
        struct udpfs_result_reply *rr;
        uint32_t n;
        int r;

        req.msg_type     = UDPFS_MSG_BREAD_REQ;
        req.reserved     = 0;
        req.sector_count = (uint16_t)chunk;
        req.handle       = handle;
        req.sector_nr_lo = sector;
        req.sector_nr_hi = 0;

        g_bread_got    = 0;
        g_bread_first  = 0;
        g_bread_win    = 0;
        g_bread_nacked = 0;
        g_bread_dupcnt = 0;
        g_bread_size   = bytes;
        COMPILER_BARRIER();
        g_bread_dst    = dst;   /* the handler uses them only with g_waiting_seq set (in udpfs_send_and_wait) */

        r = udpfs_send_and_wait(&req, sizeof(req), UDPFS_TMO_BREAD_US);

        g_bread_dst = NULL;
        if (r != 0)
            return total > 0 ? (int)total : -1;

        if (g_rx_apphdr_len < sizeof(struct udpfs_result_reply))
            return total > 0 ? (int)total : -1;
        rr = (struct udpfs_result_reply *)g_rx_apphdr;
        if (rr->msg_type != UDPFS_MSG_RESULT_REPLY || rr->result < 0)
            return total > 0 ? (int)total : -2;   /* error reply from the server (e.g. invalid handle) */

        n = (uint32_t)rr->result;
        if (n > g_bread_got)
            n = g_bread_got;
        total += n;
        if (n < bytes)
            break;          /* end of file */

        dst    += bytes;
        sector += chunk;
        count  -= chunk;
    }
    return (int)total;
}

/* ------------------------------------------------------------------ */
/* CLOSE and GETSTAT, plus the public wrappers that serialize requests.
 * The *_nl versions above use shared static buffers (pkt, g_rx_*):
 * two IOP threads entering together would overwrite each other. */

static int udpfs_client_close_nl(int handle)
{
    static struct udpfs_close_req req;
    struct udpfs_result_reply *reply;

    req.msg_type = UDPFS_MSG_CLOSE_REQ;
    memset(req.reserved, 0, sizeof(req.reserved));
    req.handle = handle;

    if (udpfs_send_and_wait(&req, sizeof(req), UDPFS_TMO_FAST_US) != 0)
        return -1;

    /* the server replies with SendData: 8 bytes as data, no app header */
    if (g_rx_data_len < sizeof(struct udpfs_result_reply))
        return -1;

    reply = (struct udpfs_result_reply *)g_rx_data;
    if (reply->msg_type != UDPFS_MSG_CLOSE_REPLY)
        return -1;

    return (reply->result < 0) ? reply->result : 0;
}

static int udpfs_client_getstat_nl(const char *path, udpfs_stat_t *st)
{
    static uint8_t buf[4 + 128];
    struct udpfs_getstat_reply *reply;
    uint32_t path_len = strlen(path);

    if (path_len >= 128)
        return -1;

    buf[0] = UDPFS_MSG_GETSTAT_REQ;
    buf[1] = buf[2] = buf[3] = 0;
    memcpy(buf + 4, path, path_len + 1);

    if (udpfs_send_and_wait(buf, 4 + path_len + 1, UDPFS_TMO_META_US) != 0)
        return -1;

    /* 48 bytes: they do not fit in g_rx_apphdr (40), they arrive as data */
    if (g_rx_data_len < sizeof(struct udpfs_getstat_reply))
        return -1;

    reply = (struct udpfs_getstat_reply *)g_rx_data;
    if (reply->msg_type != UDPFS_MSG_GETSTAT_REPLY)
        return -1;
    if (reply->result < 0)
        return reply->result;

    st->mode   = reply->mode;
    st->attr   = reply->attr;
    st->size   = reply->size;
    st->hisize = reply->hisize;
    memcpy(st->ctime, reply->ctime, 8);
    memcpy(st->atime, reply->atime, 8);
    memcpy(st->mtime, reply->mtime, 8);
    return 0;
}

int udpfs_client_open(const char *path, uint32_t *out_size)
{
    int res;
    if (udpfs_lock() != 0)
        return -1;
    res = udpfs_client_open_nl(path, out_size);
    udpfs_unlock();
    return res;
}

int udpfs_client_bread(int handle, uint32_t sector, uint32_t count, void *buffer)
{
    int res;
    if (udpfs_lock() != 0)
        return -1;
    res = udpfs_client_bread_nl(handle, sector, count, buffer);
    udpfs_unlock();
    return res;
}

int udpfs_client_close(int handle)
{
    int res;
    if (udpfs_lock() != 0)
        return -1;
    res = udpfs_client_close_nl(handle);
    udpfs_unlock();
    return res;
}

int udpfs_client_getstat(const char *path, udpfs_stat_t *st)
{
    int res;
    if (udpfs_lock() != 0)
        return -1;
    res = udpfs_client_getstat_nl(path, st);
    udpfs_unlock();
    return res;
}
