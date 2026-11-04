/* UTF-8 <-> codepoint conversion.
 *
 * Strict RFC 3629 decoding: overlong forms, surrogates, and values above
 * U+10FFFF are rejected and decoded as U+FFFD. On AArch64 the bulk paths use
 * NEON to process runs of ASCII 16 bytes at a time; the scalar versions are
 * exported so tests can check that both paths agree.
 */
#ifndef SCRIBODO_UTF8_H
#define SCRIBODO_UTF8_H

#include <stddef.h>
#include <stdint.h>

#define UTF8_REPLACEMENT 0xFFFDu

/* Decode `len` bytes into `out`, which must have room for `len` codepoints
 * (UTF-8 never yields more codepoints than bytes). Returns the number of
 * codepoints written. If `invalid_count` is non-NULL it receives the number
 * of invalid sequences that were replaced with U+FFFD. */
size_t utf8_decode(const uint8_t *src, size_t len, uint32_t *out, size_t *invalid_count);
size_t utf8_decode_scalar(const uint8_t *src, size_t len, uint32_t *out, size_t *invalid_count);

/* Number of bytes needed to encode `n` codepoints. Invalid codepoints are
 * counted as U+FFFD (3 bytes). */
size_t utf8_encoded_len(const uint32_t *cps, size_t n);

/* Encode `n` codepoints into `out` (sized by utf8_encoded_len). Returns the
 * number of bytes written. No terminator is appended. */
size_t utf8_encode(const uint32_t *cps, size_t n, uint8_t *out);
size_t utf8_encode_scalar(const uint32_t *cps, size_t n, uint8_t *out);

/* Encode a single codepoint; returns bytes written (1..4). */
int utf8_encode_one(uint32_t cp, uint8_t out[4]);

/* Decode one codepoint from `s` (with `avail` bytes remaining). Returns the
 * bytes consumed (>= 1 when avail > 0). Invalid input yields U+FFFD. */
int utf8_decode_one(const uint8_t *s, size_t avail, uint32_t *cp);

#endif
