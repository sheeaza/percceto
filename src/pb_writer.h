#ifndef PB_WRITER_H
#define PB_WRITER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Minimal, growable protobuf byte-buffer writer. No schema knowledge lives
 * here — just wire-format primitives (varint / tag / length-delimited /
 * fixed64 fields, and nested submessages). */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} pb_buf;

void pb_buf_init(pb_buf *b);
void pb_buf_free(pb_buf *b);
void pb_buf_reset(pb_buf *b); /* keep capacity, drop contents */

void pb_put_byte(pb_buf *b, uint8_t v);
void pb_put_bytes(pb_buf *b, const void *data, size_t len);
void pb_put_varint(pb_buf *b, uint64_t v);

/* wire types: 0 = varint, 1 = 64-bit, 2 = length-delimited, 5 = 32-bit */
void pb_put_tag(pb_buf *b, uint32_t field_no, unsigned wire_type);

/* field helpers: each writes tag + payload */
void pb_put_varint_field(pb_buf *b, uint32_t field_no, uint64_t v);
void pb_put_bool_field(pb_buf *b, uint32_t field_no, bool v);
void pb_put_string_field(pb_buf *b, uint32_t field_no, const char *s, size_t len);
void pb_put_fixed64_field(pb_buf *b, uint32_t field_no, uint64_t raw_bits);
void pb_put_double_field(pb_buf *b, uint32_t field_no, double v);

/* embeds `sub`'s bytes as a length-delimited submessage under field_no */
void pb_put_submessage_field(pb_buf *b, uint32_t field_no, const pb_buf *sub);

#endif /* PB_WRITER_H */
