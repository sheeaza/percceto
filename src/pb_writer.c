#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pb_writer.h"

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

void pb_buf_reserve(struct pb_buf *b, size_t extra)
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

void pb_put_bytes(struct pb_buf *b, const void *data, size_t len)
{
    pb_buf_reserve(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

void pb_put_varint(struct pb_buf *b, uint64_t v)
{
    uint8_t tmp[10];
    size_t n = 0;
    do {
        uint8_t byte = v & 0x7f;
        v >>= 7;
        if (v)
            byte |= 0x80;
        tmp[n++] = byte;
    } while (v);
    pb_put_bytes(b, tmp, n);
}
