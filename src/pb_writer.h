#ifndef PB_WRITER_H
#define PB_WRITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal, growable protobuf byte-buffer writer. No schema knowledge lives
 * here — just wire-format primitives (varint / tag / length-delimited /
 * fixed64 fields, and nested submessages). */

struct pb_buf {
    uint8_t *data;
    size_t len;
    size_t cap;
};

/**
 * pb_buf_init() - Initialize an empty, zero-capacity buffer.
 * @b: Buffer to initialize.
 */
void pb_buf_init(struct pb_buf *b);

/**
 * pb_buf_free() - Free a buffer's backing storage.
 * @b: Buffer to free.
 */
void pb_buf_free(struct pb_buf *b);

/**
 * pb_buf_reset() - Drop a buffer's contents while keeping its capacity.
 * @b: Buffer to reset.
 */
void pb_buf_reset(struct pb_buf *b);

/**
 * pb_put_byte() - Append a single raw byte.
 * @b: Destination buffer.
 * @v: Byte to append.
 */
void pb_put_byte(struct pb_buf *b, uint8_t v);

/**
 * pb_put_bytes() - Append raw bytes verbatim.
 * @b: Destination buffer.
 * @data: Bytes to append.
 * @len: Number of bytes to append.
 */
void pb_put_bytes(struct pb_buf *b, const void *data, size_t len);

/**
 * pb_put_varint() - Append a value as a protobuf varint.
 * @b: Destination buffer.
 * @v: Value to encode.
 */
void pb_put_varint(struct pb_buf *b, uint64_t v);

/**
 * pb_put_tag() - Append a protobuf field tag.
 * @b: Destination buffer.
 * @field_no: Field number.
 * @wire_type: Wire type (0 = varint, 1 = 64-bit, 2 = length-delimited,
 *      5 = 32-bit).
 */
void pb_put_tag(struct pb_buf *b, uint32_t field_no, unsigned wire_type);

/**
 * pb_put_varint_field() - Append a tag followed by a varint payload.
 * @b: Destination buffer.
 * @field_no: Field number.
 * @v: Value to encode.
 */
void pb_put_varint_field(struct pb_buf *b, uint32_t field_no, uint64_t v);

/**
 * pb_put_bool_field() - Append a tag followed by a bool payload.
 * @b: Destination buffer.
 * @field_no: Field number.
 * @v: Value to encode.
 */
void pb_put_bool_field(struct pb_buf *b, uint32_t field_no, bool v);

/**
 * pb_put_string_field() - Append a tag followed by a length-delimited
 *      string payload.
 * @b: Destination buffer.
 * @field_no: Field number.
 * @s: String bytes to encode.
 * @len: Number of bytes in @s.
 */
void pb_put_string_field(struct pb_buf *b, uint32_t field_no, const char *s,
                         size_t len);

/**
 * pb_put_fixed64_field() - Append a tag followed by a raw 64-bit payload.
 * @b: Destination buffer.
 * @field_no: Field number.
 * @raw_bits: Bits to encode, written little-endian.
 */
void pb_put_fixed64_field(struct pb_buf *b, uint32_t field_no,
                          uint64_t raw_bits);

/**
 * pb_put_double_field() - Append a tag followed by a double payload.
 * @b: Destination buffer.
 * @field_no: Field number.
 * @v: Value to encode.
 */
void pb_put_double_field(struct pb_buf *b, uint32_t field_no, double v);

/**
 * pb_put_submessage_field() - Embed a submessage's bytes as a
 *      length-delimited field.
 * @b: Destination buffer.
 * @field_no: Field number.
 * @sub: Encoded submessage to embed.
 */
void pb_put_submessage_field(struct pb_buf *b, uint32_t field_no,
                             const struct pb_buf *sub);

#endif /* PB_WRITER_H */
