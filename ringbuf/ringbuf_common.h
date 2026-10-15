#ifndef RINGBUF_COMMON_H
#define RINGBUF_COMMON_H

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

#endif /* RINGBUF_COMMON_H */
