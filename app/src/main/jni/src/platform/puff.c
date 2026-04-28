/*
 * puff.c
 * Minimal inflate (deflate decompression) — from zlib contrib/puff.
 * Public domain.
 *
 * Bug fixed in decode(): the canonical Huffman match condition was:
 *   code - h->count[len] < first + h->count[len]  (wrong: too wide, matches
 *   code < first + 2*count, causing false matches and error -10)
 * Correct condition:
 *   code - count < first  (i.e. code < first + count)
 */

#include <string.h>
#include "puff.h"

#define MAXBITS    15
#define MAXLCODES  286
#define MAXDCODES  30
#define MAXCODES   (MAXLCODES + MAXDCODES)
#define FIXLCODES  288

struct state {
    unsigned char       *out;
    unsigned long        outlen;
    unsigned long        outcnt;
    const unsigned char *in;
    unsigned long        inlen;
    unsigned long        incnt;
    int                  bitbuf;
    int                  bitcnt;
};

struct huffman {
    short *count;
    short *symbol;
};

static int bits(struct state *s, int need)
{
    long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt == s->inlen) return -1;
        val |= (long)(s->in[s->incnt++]) << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf  = (int)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
}

static int decode(struct state *s, const struct huffman *h)
{
    int len, code, first, count, index;

    code = first = index = 0;
    for (len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if (b < 0) return b;
        code |= b;
        count = h->count[len];
        if (code - count < first)           /* correct canonical Huffman condition */
            return h->symbol[index + (code - first)];
        index += count;
        first  = (first + count) << 1;
        code <<= 1;
    }
    return -10;
}

static int construct(struct huffman *h, const short *length, int n)
{
    int symbol, len, left;
    short offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++)
        offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0)
            h->symbol[offs[length[symbol]]++] = (short)symbol;
    return left;
}

static int stored(struct state *s)
{
    unsigned len;

    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return -2;
    len  = s->in[s->incnt++];
    len |= (unsigned)s->in[s->incnt++] << 8;
    if (s->in[s->incnt++] != (~len & 0xff) ||
        s->in[s->incnt++] != ((~len >> 8) & 0xff))
        return -2;
    if (s->incnt + len > s->inlen) return -2;
    if (s->outcnt + len > s->outlen) return 1;
    while (len--)
        s->out[s->outcnt++] = s->in[s->incnt++];
    return 0;
}

static const short lens[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const short lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const short dists[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};
static const short dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int codes(struct state *s,
                 const struct huffman *lencode,
                 const struct huffman *distcode)
{
    int symbol;
    do {
        symbol = decode(s, lencode);
        if (symbol < 0) return symbol;
        if (symbol < 256) {
            if (s->outcnt == s->outlen) return 1;
            s->out[s->outcnt++] = (unsigned char)symbol;
        } else if (symbol > 256) {
            unsigned dist;
            int ext, len;
            symbol -= 257;
            if (symbol >= 29) return -10;
            ext = bits(s, lext[symbol]);
            if (ext < 0) return ext;
            len = lens[symbol] + ext;

            symbol = decode(s, distcode);
            if (symbol < 0) return symbol;
            ext = bits(s, dext[symbol]);
            if (ext < 0) return ext;
            dist = (unsigned)dists[symbol] + (unsigned)ext;
            if (dist > s->outcnt) return -11;
            if (s->outcnt + (unsigned)len > s->outlen) return 1;
            while (len--) {
                s->out[s->outcnt] = s->out[s->outcnt - dist];
                s->outcnt++;
            }
        }
    } while (symbol != 256);
    return 0;
}

static int fixed(struct state *s)
{
    static short lencnt[MAXBITS + 1], lensym[FIXLCODES];
    static short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    static struct huffman lencode  = {lencnt, lensym};
    static struct huffman distcode = {distcnt, distsym};
    static int virgin = 1;

    if (virgin) {
        short lengths[FIXLCODES];
        int sym;
        for (sym = 0; sym <  144; sym++) lengths[sym] = 8;
        for (; sym <  256; sym++) lengths[sym] = 9;
        for (; sym <  280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        virgin = 0;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic(struct state *s)
{
    static const short order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    int nlen, ndist, ncode, index, err;
    short lengths[MAXCODES];
    short lencnt[MAXBITS + 1], lensym[MAXLCODES];
    short distcnt[MAXBITS + 1], distsym[MAXDCODES];
    struct huffman lencode  = {lencnt, lensym};
    struct huffman distcode = {distcnt, distsym};

    nlen  = bits(s, 5);  if (nlen  < 0) return nlen;   nlen  += 257;
    ndist = bits(s, 5);  if (ndist < 0) return ndist;  ndist += 1;
    ncode = bits(s, 4);  if (ncode < 0) return ncode;  ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return -3;

    for (index = 0; index < 19; index++) lengths[index] = 0;
    for (index = 0; index < ncode; index++) {
        int b = bits(s, 3);
        if (b < 0) return b;
        lengths[order[index]] = (short)b;
    }

    {
        short llcnt[MAXBITS + 1], llsym[19];
        struct huffman lenlencode = {llcnt, llsym};
        err = construct(&lenlencode, lengths, 19);
        if (err < 0) return -4;

        index = 0;
        while (index < nlen + ndist) {
            int sym = decode(s, &lenlencode);
            if (sym < 0) return sym;
            if (sym < 16) {
                lengths[index++] = (short)sym;
            } else if (sym == 16) {
                short prev;
                int rep;
                if (index == 0) return -5;
                prev = lengths[index - 1];
                rep  = bits(s, 2);
                if (rep < 0) return rep;
                rep += 3;
                while (rep-- && index < nlen + ndist)
                    lengths[index++] = prev;
            } else if (sym == 17) {
                int rep = bits(s, 3);
                if (rep < 0) return rep;
                rep += 3;
                while (rep-- && index < nlen + ndist)
                    lengths[index++] = 0;
            } else {
                int rep = bits(s, 7);
                if (rep < 0) return rep;
                rep += 11;
                while (rep-- && index < nlen + ndist)
                    lengths[index++] = 0;
            }
        }
    }

    err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return -6;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return -6;

    return codes(s, &lencode, &distcode);
}

int puff(unsigned char *dest, unsigned long *destlen,
         const unsigned char *source, unsigned long *sourcelen)
{
    struct state s;
    int last, type, err;

    s.out    = dest;
    s.outlen = *destlen;
    s.outcnt = 0;
    s.in     = source;
    s.inlen  = *sourcelen;
    s.incnt  = 0;
    s.bitbuf = 0;
    s.bitcnt = 0;

    do {
        last = bits(&s, 1);  if (last < 0) { err = last; goto done; }
        type = bits(&s, 2);  if (type < 0) { err = type; goto done; }
        switch (type) {
        case 0:  err = stored(&s);  break;
        case 1:  err = fixed(&s);   break;
        case 2:  err = dynamic(&s); break;
        default: err = -1;
        }
        if (err != 0) goto done;
    } while (!last);
    err = 0;
done:
    *destlen   = s.outcnt;
    *sourcelen = s.incnt;
    return err;
}
