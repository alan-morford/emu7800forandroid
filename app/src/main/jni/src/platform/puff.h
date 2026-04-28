/*
 * puff.h
 * Minimal inflate (deflate decompression) interface.
 * From zlib contrib/puff (public domain).
 */
#ifndef PUFF_H
#define PUFF_H

/*
 * Decompress `source[0..*sourcelen-1]` into `dest[0..*destlen-1]`.
 * On return, *destlen is the number of bytes written to dest and
 * *sourcelen is the number of bytes consumed from source.
 * Returns 0 on success, negative on error, 1 if dest was too small.
 */
int puff(unsigned char *dest,         unsigned long *destlen,
         const unsigned char *source, unsigned long *sourcelen);

#endif /* PUFF_H */
