#include "pb_writer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pb_die(const char *msg)
{
    fprintf(stderr, "pftrace: fatal: %s\n", msg);
    abort();
}

void pb_buf_init(struct pb_buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void pb_buf_free(struct pb_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void pb_buf_reset(struct pb_buf *b)
{
    b->len = 0;
}

static void pb_reserve(struct pb_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return;
    size_t new_cap = b->cap ? b->cap * 2 : 64;
    while (new_cap < b->len + extra)
        new_cap *= 2;
    uint8_t *new_data = realloc(b->data, new_cap);
    if (!new_data)
        pb_die("out of memory");
    b->data = new_data;
    b->cap = new_cap;
}

void pb_put_byte(struct pb_buf *b, uint8_t v)
{
    pb_reserve(b, 1);
    b->data[b->len++] = v;
}

void pb_put_bytes(struct pb_buf *b, const void *data, size_t len)
{
    pb_reserve(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

void pb_put_varint(struct pb_buf *b, uint64_t v)
{
    while (v >= 0x80) {
        pb_put_byte(b, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    pb_put_byte(b, (uint8_t)v);
}

void pb_put_tag(struct pb_buf *b, uint32_t field_no, unsigned wire_type)
{
    pb_put_varint(b, ((uint64_t)field_no << 3) | wire_type);
}

void pb_put_varint_field(struct pb_buf *b, uint32_t field_no, uint64_t v)
{
    pb_put_tag(b, field_no, 0);
    pb_put_varint(b, v);
}

void pb_put_bool_field(struct pb_buf *b, uint32_t field_no, bool v)
{
    pb_put_varint_field(b, field_no, v ? 1 : 0);
}

void pb_put_string_field(struct pb_buf *b, uint32_t field_no, const char *s,
                         size_t len)
{
    pb_put_tag(b, field_no, 2);
    pb_put_varint(b, len);
    pb_put_bytes(b, s, len);
}

void pb_put_fixed64_field(struct pb_buf *b, uint32_t field_no,
                          uint64_t raw_bits)
{
    pb_put_tag(b, field_no, 1);
    /* protobuf fixed64 is little-endian on the wire; every platform this
     * library targets (x86_64/arm64 Linux) is little-endian, so a raw
     * byte copy is correct without manual byte-swapping. */
    pb_put_bytes(b, &raw_bits, 8);
}

void pb_put_double_field(struct pb_buf *b, uint32_t field_no, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, 8);
    pb_put_fixed64_field(b, field_no, bits);
}

void pb_put_submessage_field(struct pb_buf *b, uint32_t field_no,
                             const struct pb_buf *sub)
{
    pb_put_tag(b, field_no, 2);
    pb_put_varint(b, sub->len);
    pb_put_bytes(b, sub->data, sub->len);
}
