#ifndef PB_WRITER_H
#define PB_WRITER_H

#include <stddef.h>
#include <stdint.h>

/* Growable byte buffer that trace output is staged in before write(). Schema
 * encoding is done by nanopb into src/perccetto.pb.c; this only provides the
 * storage and the varint length prefixes that frame each packet. */

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
 * pb_buf_reserve() - Ensure room for @extra more bytes past @len.
 * @b: Buffer to grow.
 * @extra: Bytes that will be appended.
 *
 * Aborts on allocation failure, matching the rest of the library.
 */
void pb_buf_reserve(struct pb_buf *b, size_t extra);

/**
 * pb_put_bytes() - Append raw bytes verbatim.
 * @b: Buffer to append to.
 * @data: Bytes to copy.
 * @len: Number of bytes.
 */
void pb_put_bytes(struct pb_buf *b, const void *data, size_t len);

/**
 * pb_put_varint() - Append a value as a protobuf varint.
 * @b: Buffer to append to.
 * @v: Value to encode.
 */
void pb_put_varint(struct pb_buf *b, uint64_t v);

#endif /* PB_WRITER_H */
