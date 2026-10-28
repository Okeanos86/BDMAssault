/*
 * Copyright (c) 2026 Okeanos
 * Licensed under the Academic Free License version 2.0
 * See the LICENSE file; third-party components: docs/THIRD_PARTY_NOTICES.md
 */

/*
 * vcd_cue.c - cue sheet parser and POPStarter VCD header generator.
 * See vcd_cue.h. Header layout verified against the output of CUE2POPS v2.3.
 */
#include "vcd_cue.h"

typedef struct {
    const char *p, *e;
} cur_t;

static int is_ws(char c) { return c == ' ' || c == '\t'; }
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static void skip_ws(cur_t *c)
{
    while (c->p < c->e && is_ws(*c->p))
        c->p++;
}

/* Case-insensitive keyword followed by blank or end of line. */
static int match_kw(cur_t *c, const char *w)
{
    const char *s = c->p;

    while (*w) {
        if (s >= c->e || up(*s) != *w)
            return 0;
        s++;
        w++;
    }
    if (s < c->e && !is_ws(*s))
        return 0;
    c->p = s;
    return 1;
}

static int read_uint(cur_t *c, int maxdigits, int *v)
{
    int n = 0, x = 0;

    while (c->p < c->e && *c->p >= '0' && *c->p <= '9') {
        if (++n > maxdigits)
            return 0;
        x = x * 10 + (*c->p - '0');
        c->p++;
    }
    if (!n)
        return 0;
    *v = x;
    return 1;
}

static int expect_colon(cur_t *c)
{
    if (c->p < c->e && *c->p == ':') {
        c->p++;
        return 1;
    }
    return 0;
}

static int parse_line(cur_t *c, vcd_cue_t *out)
{
    skip_ws(c);
    if (c->p >= c->e)
        return VCD_OK;

    if (match_kw(c, "FILE")) {
        const char *s, *b;
        int n, i, used;

        skip_ws(c);
        if (c->p < c->e && *c->p == '"') {
            c->p++;
            s = c->p;
            while (c->p < c->e && *c->p != '"')
                c->p++;
            if (c->p >= c->e)
                return VCD_E_SYNTAX;
            n = (int)(c->p - s);
            c->p++;
        } else {
            s = c->p;
            while (c->p < c->e && !is_ws(*c->p))
                c->p++;
            n = (int)(c->p - s);
        }
        skip_ws(c);
        if (!match_kw(c, "BINARY"))
            return VCD_E_FILETYPE;
        /* keep the base name only: the file is looked for next to the cue */
        b = s;
        for (i = 0; i < n; i++)
            if (s[i] == '/' || s[i] == '\\')
                b = s + i + 1;
        n -= (int)(b - s);
        if (n <= 0)
            return VCD_E_SYNTAX;
        if (out->nfiles >= VCD_MAX_FILES)
            return VCD_E_TOOMANY;
        used = out->name_off[out->nfiles];
        if (used + n + 1 > VCD_NAMES_MAX)
            return VCD_E_TOOMANY;
        for (i = 0; i < n; i++)
            out->names[used + i] = b[i];
        out->names[used + n] = 0;
        out->nfiles++;
        out->name_off[out->nfiles] = (uint16_t)(used + n + 1);
        return VCD_OK;
    }

    if (match_kw(c, "TRACK")) {
        int no;
        vcd_track_t *t;

        skip_ws(c);
        if (!read_uint(c, 2, &no) || no < 1)
            return VCD_E_SYNTAX;
        skip_ws(c);
        if (out->nfiles == 0)
            return VCD_E_NOFILE;
        if (out->ntracks >= VCD_MAX_TRACKS)
            return VCD_E_TOOMANY;
        t = &out->t[out->ntracks];
        t->no = (uint8_t)no;
        t->file = (uint8_t)(out->nfiles - 1);
        t->i0 = -1;
        t->i1 = -1;
        if (match_kw(c, "MODE2/2352"))
            t->audio = 0;
        else if (match_kw(c, "AUDIO"))
            t->audio = 1;
        else
            return out->ntracks == 0 ? VCD_E_TRACK1 : VCD_E_TRACKTYPE;
        if (out->ntracks == 0 && t->audio)
            return VCD_E_TRACK1;
        out->ntracks++;
        return VCD_OK;
    }

    if (match_kw(c, "INDEX")) {
        int idx, mm, ss, ff;
        int32_t lba;

        if (out->ntracks == 0)
            return VCD_E_SYNTAX;
        skip_ws(c);
        if (!read_uint(c, 2, &idx))
            return VCD_E_SYNTAX;
        skip_ws(c);
        if (!read_uint(c, 3, &mm) || !expect_colon(c) || !read_uint(c, 2, &ss) ||
            !expect_colon(c) || !read_uint(c, 2, &ff) || ss >= 60 || ff >= 75)
            return VCD_E_SYNTAX;
        lba = mm * 4500 + ss * 75 + ff;
        if (lba + 150 >= 100 * 4500)    /* absolute minutes must stay 2 BCD digits */
            return VCD_E_SYNTAX;
        if (idx == 0)
            out->t[out->ntracks - 1].i0 = lba;
        else if (idx == 1)
            out->t[out->ntracks - 1].i1 = lba;
        return VCD_OK;
    }

    if (match_kw(c, "PREGAP") || match_kw(c, "POSTGAP"))
        return VCD_E_PREGAP;

    return VCD_OK;      /* REM, TITLE, PERFORMER, FLAGS, ISRC, CATALOG... */
}

int vcd_cue_parse(const char *text, unsigned int len, vcd_cue_t *out)
{
    const char *p = text, *end = text + len;
    int i, r;
    uint8_t has_track[VCD_MAX_FILES];

    out->nfiles = 0;
    out->name_off[0] = 0;
    out->ntracks = 0;

    if (len >= 3 && (uint8_t)p[0] == 0xEF && (uint8_t)p[1] == 0xBB && (uint8_t)p[2] == 0xBF)
        p += 3;                                 /* UTF-8 BOM */

    while (p < end) {
        cur_t c;

        c.p = p;
        while (p < end && *p != '\n' && *p != '\r')
            p++;
        c.e = p;
        while (p < end && (*p == '\n' || *p == '\r'))
            p++;
        r = parse_line(&c, out);
        if (r != VCD_OK)
            return r;
    }
    if (out->nfiles == 0)
        return VCD_E_NOFILE;
    if (out->ntracks == 0)
        return VCD_E_SYNTAX;
    for (i = 0; i < out->nfiles; i++)
        has_track[i] = 0;
    for (i = 0; i < out->ntracks; i++) {
        if (out->t[i].i1 < 0)
            return VCD_E_INDEX;
        has_track[out->t[i].file] = 1;
    }
    for (i = 0; i < out->nfiles; i++)
        if (!has_track[i])
            return VCD_E_SYNTAX;        /* a FILE without TRACK */
    return VCD_OK;
}

const char *vcd_cue_file(const vcd_cue_t *cue, int i)
{
    return cue->names + cue->name_off[i];
}

/* ------------------------------------------------------------- header */

static uint8_t bcd(uint32_t x) { return (uint8_t)(((x / 10) << 4) | (x % 10)); }

static void put_msf(uint8_t *d, uint32_t lba)
{
    d[0] = bcd(lba / 4500);
    d[1] = bcd((lba % 4500) / 75);
    d[2] = bcd(lba % 75);
}

int vcd_build_header(const vcd_cue_t *cue, const uint32_t *file_size, uint8_t *hdr)
{
    uint32_t start[VCD_MAX_FILES + 1];      /* first sector of each file */
    uint32_t n, lo;
    int k;
    const vcd_track_t *last;
    uint8_t ltype;

    if (cue->ntracks < 1 || cue->nfiles < 1)
        return VCD_E_SYNTAX;
    start[0] = 0;
    for (k = 0; k < cue->nfiles; k++) {
        if (file_size[k] == 0 || file_size[k] % VCD_SECT)
            return VCD_E_BINSIZE;
        if (start[k] > 0xFFFFFFFFu / VCD_SECT - file_size[k] / VCD_SECT)
            return VCD_E_TOOMANY;
        start[k + 1] = start[k] + file_size[k] / VCD_SECT;
    }
    n = start[cue->nfiles];

    for (k = 0; k < VCD_HDR_BYTES; k++)
        hdr[k] = 0;

    last = &cue->t[cue->ntracks - 1];
    ltype = last->audio ? 0x01 : 0x41;

    /* A0 / A1 / A2 entries (10 bytes each) */
    hdr[0x00] = 0x41; hdr[0x02] = 0xA0; hdr[0x07] = 0x01; hdr[0x08] = 0x20;
    hdr[0x0A] = ltype; hdr[0x0C] = 0xA1; hdr[0x11] = bcd(last->no);
    hdr[0x14] = ltype; hdr[0x16] = 0xA2;
    lo = n + 150;
    hdr[0x1A] = bcd(lo / 4500 / 100);
    hdr[0x1B] = bcd(lo / 4500 % 100);
    hdr[0x1C] = bcd(lo % 4500 / 75);
    hdr[0x1D] = bcd(lo % 75);

    /* one entry per track from 0x1E; times are absolute: the start of the
     * track's file in the image plus the INDEX time inside the file */
    for (k = 0; k < cue->ntracks; k++) {
        const vcd_track_t *t = &cue->t[k];
        uint8_t *e = hdr + 0x1E + 10 * k;
        uint32_t base = start[t->file];
        uint32_t fsect = start[t->file + 1] - base;
        int32_t rel = t->i0 >= 0 ? t->i0 : t->i1;
        uint32_t abs_start, abs_i1;

        if ((uint32_t)t->i1 >= fsect || (uint32_t)rel >= fsect)
            return VCD_E_INDEX;
        abs_start = base + (uint32_t)rel;
        abs_i1 = base + (uint32_t)t->i1;
        if (abs_i1 + 150 >= 100u * 4500u || abs_start + 150 >= 100u * 4500u)
            return VCD_E_SYNTAX;        /* minutes must stay 2 BCD digits */

        e[0] = t->audio ? 0x01 : 0x41;
        e[2] = bcd(t->no);
        put_msf(e + 3, abs_start + (k == 0 ? 0u : 150u));
        put_msf(e + 7, abs_i1 + 150u);
    }

    hdr[0x400] = 0x6B; hdr[0x401] = 0x48; hdr[0x402] = 0x6E; hdr[0x403] = 0x22;
    for (k = 0; k < 4; k++) {
        hdr[0x408 + k] = (uint8_t)(n >> (8 * k));
        hdr[0x40C + k] = (uint8_t)(n >> (8 * k));
    }
    return VCD_OK;
}
