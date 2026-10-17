#ifndef RINGBUF_COMMON_H
#define RINGBUF_COMMON_H

#include <stddef.h>
#include <stdint.h>

/**
 * enum ringbuf_mode - what a push does when the buffer is full
 * @RINGBUF_DROP_NEWEST: the push fails and the new message is discarded.
 *	Nothing already in the buffer is touched.
 * @RINGBUF_DROP_OLDEST: the push evicts the oldest unread message to make
 *	room, and counts it in the variant's dropped_count.
 * @RINGBUF_BLOCK: a push waits until a slot is free. Unused for now: the
 *	variants' create functions reject it and return NULL.
 */
enum ringbuf_mode {
    RINGBUF_DROP_NEWEST,
    RINGBUF_DROP_OLDEST,
    RINGBUF_BLOCK
};

/**
 * next_pow2 - round up to a power of two
 * @v: value to round
 *
 * Return: the smallest power of two >= @v, but never less than 2 -- the
 * mask-based slot indexing needs both properties -- or 0 if that would
 * overflow. Callers must check for 0.
 */
static inline size_t next_pow2(size_t v)
{
    if (v < 2)
        return 2;
    size_t p = 1;
    while (p < v) {
        if (p > SIZE_MAX / 2)
            return 0;
        p <<= 1;
    }
    return p;
}

/**
 * add_checked - add two sizes, detecting overflow
 * @a: first addend
 * @b: second addend
 *
 * Return: @a + @b, or 0 on overflow. 0 is never a legitimate total here
 * (the header alone is non-empty), so callers treat it as failure.
 */
static inline size_t add_checked(size_t a, size_t b)
{
    if (a > SIZE_MAX - b)
        return 0;
    return a + b;
}

/**
 * align_up - round up to an alignment boundary
 * @v: value to round
 * @align: alignment, which must be a power of two
 *
 * Return: @v rounded up to a multiple of @align, or 0 on overflow.
 */
static inline size_t align_up(size_t v, size_t align)
{
    size_t sum = add_checked(v, align - 1);
    if (sum == 0)
        return 0;
    return sum & ~(align - 1);
}

#endif /* RINGBUF_COMMON_H */
