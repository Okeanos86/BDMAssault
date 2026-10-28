/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * vcd_cue.h - POPStarter VCD header built from a BIN/CUE.
 *
 * A VCD is a 1 MiB header followed by the raw MODE2/2352 disc image. Only the
 * first VCD_HDR_BYTES bytes of the header carry data; the rest is zero. Layout
 * follows CUE2POPS v2.3 (track 1 MODE2/2352, no PREGAP/POSTGAP commands).
 *
 * Unlike CUE2POPS, a cue sheet may name several .bin files (one per track, as
 * dumped by Redump): the image is their concatenation in cue order and the
 * INDEX times of each FILE are relative to that file.
 *
 * No dependencies: plain C, usable on the IOP and on a host (for tests).
 */
#ifndef VCD_CUE_H
#define VCD_CUE_H

#include <stdint.h>

#define VCD_HDR_SIZE   0x100000u   /* bytes before the disc image in the VCD */
#define VCD_HDR_BYTES  0x410       /* meaningful part of the header */
#define VCD_SECT       2352u
#define VCD_MAX_TRACKS 99
#define VCD_MAX_FILES  VCD_MAX_TRACKS   /* every file holds at least one track */
#ifndef VCD_NAMES_MAX
#define VCD_NAMES_MAX  8192        /* all file names together, with NULs (-D to change) */
#endif
#ifndef VCD_CUE_MAX
#define VCD_CUE_MAX    16384       /* largest cue sheet accepted (-D to change) */
#endif

#define VCD_OK            0
#define VCD_E_SYNTAX     -1
#define VCD_E_NOFILE     -2        /* no FILE line before the first TRACK */
#define VCD_E_TRACK1     -4        /* track 1 is not MODE2/2352 */
#define VCD_E_TRACKTYPE  -5        /* track type other than MODE2/2352, AUDIO */
#define VCD_E_PREGAP     -6        /* PREGAP/POSTGAP command */
#define VCD_E_INDEX      -7        /* track without INDEX 01, or outside its file */
#define VCD_E_TOOMANY    -8
#define VCD_E_BINSIZE    -9        /* file size not a multiple of 2352 */
#define VCD_E_FILETYPE   -10       /* FILE type is not BINARY */

typedef struct {
    uint8_t no;         /* track number as written in the cue */
    uint8_t audio;      /* 1 = AUDIO, 0 = MODE2/2352 */
    uint8_t file;       /* index of the FILE it belongs to */
    int32_t i0;         /* INDEX 00, sectors from the start of its file, -1 = none */
    int32_t i1;         /* INDEX 01, sectors from the start of its file */
} vcd_track_t;

typedef struct {
    int         nfiles;
    uint16_t    name_off[VCD_MAX_FILES + 1];    /* names[name_off[i]] = file i; [nfiles] = end */
    char        names[VCD_NAMES_MAX];           /* base names (directories stripped) */
    int         ntracks;
    vcd_track_t t[VCD_MAX_TRACKS];
} vcd_cue_t;

/* Parses a cue sheet (text, not necessarily NUL-terminated). 0 or VCD_E_*. */
int vcd_cue_parse(const char *text, unsigned int len, vcd_cue_t *out);

/* Base name of file i as written in the cue. */
const char *vcd_cue_file(const vcd_cue_t *cue, int i);

/* Builds the VCD_HDR_BYTES header. file_size[i] = size in bytes of the i-th
 * file (a multiple of 2352). 0 or VCD_E_*. */
int vcd_build_header(const vcd_cue_t *cue, const uint32_t *file_size, uint8_t *hdr);

#endif /* VCD_CUE_H */
