/* Chunk pool and writers. See docs/chunk_buffers_design.md.
 *
 * Producers take the pool lock only when they rotate to a new chunk, once per
 * chunk_slots events. The drain takes it to detach or walk the committed FIFO.
 * On the per-event path nothing is written to shared state, apart from a relaxed
 * store to the thread's own writer counter. The writer lookup reads owner tags
 * of other slots while it probes; those are written only on bind and unbind. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "pftrace_internal.h"

static void commit_locked(struct pf_trace *t, struct pf_writer *w);

/* Gives back the calling thread's slot in @t and drops its reference. The
 * partial chunk is committed, so its records reach the drain without waiting
 * for the trace to finish. The slot is then free for another thread to bind,
 * and the reference is dropped last, since it may be the one that finishes
 * the trace. Runs on the thread's exit, on a second close, and from
 * pf_trace_close on the closing thread. */
static void unbind(struct pf_trace *t)
{
    pthread_setspecific(t->bind_key, NULL);

    struct pf_writer *w = pf_writer_find(t);
    if (w) {
        struct pf_chunk *spare = NULL;

        pthread_mutex_lock(&t->pool_lock);
        if (w->cur && w->used > 0)
            commit_locked(t, w);
        else {
            spare = w->cur;
            w->cur = NULL;
        }
        pthread_mutex_unlock(&t->pool_lock);
        pf_chunks_release(t, spare);

        /* The event count is kept, so stats still count this slot's events.
         * Bind resets the producer's fields for the next owner. */
        pthread_mutex_lock(&t->reg_lock);
        atomic_store_explicit(&w->owner, PF_OWNER_RELEASED, memory_order_release);
        pthread_mutex_unlock(&t->reg_lock);
    }
    pf_trace_unref(t);
}

void pf_unbind_thread(struct pf_trace *t)
{
    if (pthread_getspecific(t->bind_key) == t)
        unbind(t);
}

static void unbind_at_exit(void *v)
{
    unbind(v);
}

int pf_pool_init(struct pf_trace *t)
{
    uint32_t slots = (uint32_t)t->cfg.chunk_slots;
    size_t chunk_bytes = sizeof(struct pf_chunk) + slots * sizeof(struct pf_rec);
    size_t count = t->cfg.buffer_bytes / chunk_bytes;

    /* A writer needs a spare chunk while its full one is committed. */
    if (count < 2) {
        errno = EINVAL;
        return -1;
    }

    /* The writer table is a power of two, so a thread's probe start is a
     * mask rather than a division. */
    uint32_t writer_count = 1;
    while (writer_count < t->cfg.max_threads) {
        if (writer_count > UINT32_MAX / 2) {
            errno = EINVAL;
            return -1;
        }
        writer_count <<= 1;
    }

    t->pool_mem = calloc(count, chunk_bytes);
    if (!t->pool_mem) {
        errno = ENOMEM;
        return -1;
    }

    /* aligned_alloc, not calloc: the writers need a cache-line boundary, and
     * sizeof is a multiple of 64, as aligned_alloc requires. Zeroed by hand. */
    t->writers = aligned_alloc(64, writer_count * sizeof(*t->writers));
    if (!t->writers)
        goto free_pool;
    memset(t->writers, 0, writer_count * sizeof(*t->writers));

    if (pthread_key_create(&t->bind_key, unbind_at_exit) != 0)
        goto free_writers;

    /* Each chunk is linked onto the free list. chunk_bytes is a multiple of 8,
     * so every chunk's records stay 8-byte aligned. */
    for (size_t i = count; i-- > 0;) {
        struct pf_chunk *c = (struct pf_chunk *)((char *)t->pool_mem + i * chunk_bytes);
        c->next = t->free_list;
        t->free_list = c;
    }

    t->chunk_bytes = chunk_bytes;
    t->chunk_count = (uint32_t)count;
    t->chunk_slots = slots;
    t->writer_count = writer_count;
    t->watermark_chunks = (uint32_t)(count * t->cfg.watermark_pct / 100u);

    pthread_mutex_init(&t->pool_lock, NULL);
    pthread_mutex_init(&t->reg_lock, NULL);
    return 0;

free_writers:
    free(t->writers);
    t->writers = NULL;
free_pool:
    free(t->pool_mem);
    t->pool_mem = NULL;
    errno = ENOMEM;
    return -1;
}

void pf_pool_destroy(struct pf_trace *t)
{
    pthread_key_delete(t->bind_key);
    pthread_mutex_destroy(&t->reg_lock);
    pthread_mutex_destroy(&t->pool_lock);
    free(t->writers);
    free(t->pool_mem);
    t->writers = NULL;
    t->pool_mem = NULL;
}

struct pf_writer *pf_writer_bind(struct pf_trace *t)
{
    /* A thread past the slot limit is bound with no slot. It keeps its
     * reference and its key value, so it does not come back here as a new
     * binding on every event. */
    if (pthread_getspecific(t->bind_key) == t)
        return NULL;

    /* Taken even when no slot is free, so the thread's key always names a
     * live trace. */
    atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);
    pthread_setspecific(t->bind_key, t);

    uintptr_t self = pf_thread_id();
    uint32_t mask = t->writer_count - 1;
    struct pf_writer *w = NULL;

    /* Claim the first free slot on the probe chain from this thread's hash.
     * Every slot before it is in use, so it is never NONE, and the lookup
     * finds the claim. */
    pthread_mutex_lock(&t->reg_lock);
    uint32_t i = pf_writer_hash(self, mask);
    for (uint32_t n = 0; n < t->writer_count; n++, i = (i + 1) & mask) {
        uintptr_t owner = atomic_load_explicit(&t->writers[i].owner, memory_order_relaxed);
        if (owner == PF_OWNER_NONE || owner == PF_OWNER_RELEASED) {
            w = &t->writers[i];
            w->id = i;
            break;
        }
    }

    /* Fields owned by the producer are set here, before the owner tag makes
     * the slot visible to lookups. used == slots makes the first emit rotate
     * and take a chunk. */
    if (w) {
        w->cur = NULL;
        w->used = t->chunk_slots;
        w->seq = 0;
        atomic_store_explicit(&w->owner, self, memory_order_release);
    }
    pthread_mutex_unlock(&t->reg_lock);
    return w;
}

/* Links @w's full chunk onto the committed FIFO. The caller holds pool_lock.
 * The unlock that follows is the release that publishes the chunk's records
 * to the drain. */
static void commit_locked(struct pf_trace *t, struct pf_writer *w)
{
    struct pf_chunk *c = w->cur;

    c->used = w->used;
    c->next = NULL;
    if (t->committed_tail)
        t->committed_tail->next = c;
    else
        t->committed_head = c;
    t->committed_tail = c;
    t->committed_count++;
    w->cur = NULL;
}

/* Snapshot mode only: takes the oldest committed chunk to reuse. Its records
 * are overwritten, which snapshot mode does by design, so nothing is counted
 * as dropped. The caller holds pool_lock. */
static struct pf_chunk *steal_oldest_locked(struct pf_trace *t)
{
    struct pf_chunk *c = t->committed_head;
    if (!c)
        return NULL;

    t->committed_head = c->next;
    if (!t->committed_head)
        t->committed_tail = NULL;
    t->committed_count--;
    return c;
}

bool pf_writer_rotate(struct pf_trace *t, struct pf_writer *w)
{
    pthread_mutex_lock(&t->pool_lock);

    if (w->cur)
        commit_locked(t, w);

    struct pf_chunk *c = t->free_list;
    if (c)
        t->free_list = c->next;
    else if (t->cfg.mode == PF_MODE_SNAPSHOT)
        c = steal_oldest_locked(t);

    if (c) {
        c->next = NULL;
        c->used = 0;
        c->writer = w->id;
        c->seq = w->seq++;
    }
    w->cur = c;
    w->used = c ? 0 : t->chunk_slots;

    bool over_watermark = t->cfg.mode == PF_MODE_CONTINUOUS &&
                          t->committed_count >= t->watermark_chunks;
    pthread_mutex_unlock(&t->pool_lock);

    if (over_watermark)
        pf_drain_wake(t);
    if (!c)
        atomic_fetch_add_explicit(&t->dropped, 1, memory_order_relaxed);
    return c != NULL;
}

uint64_t pf_writers_events(struct pf_trace *t)
{
    uint64_t total = 0;
    pthread_mutex_lock(&t->reg_lock);
    for (uint32_t i = 0; i < t->writer_count; i++)
        total += atomic_load_explicit(&t->writers[i].events, memory_order_relaxed);
    pthread_mutex_unlock(&t->reg_lock);
    return total;
}

void pf_writer_flush_own_locked(struct pf_trace *t)
{
    struct pf_writer *w = pf_writer_find(t);
    if (!w || !w->cur || w->used == 0)
        return;
    commit_locked(t, w);
    w->used = t->chunk_slots;
}

/* A live partial chunk has next == NULL, like every chunk a writer holds, and
 * the owner never reads next. So a snapshot can borrow the link field for the
 * length of one walk. */
struct pf_chunk *pf_link_partials_locked(struct pf_trace *t)
{
    struct pf_chunk *head = t->committed_head;
    struct pf_chunk *tail = t->committed_tail;
    for (uint32_t i = 0; i < t->writer_count; i++) {
        struct pf_chunk *c = t->writers[i].cur;
        if (!c)
            continue;
        if (tail)
            tail->next = c;
        else
            head = c;
        tail = c;
    }
    return head;
}

void pf_unlink_partials_locked(struct pf_trace *t)
{
    if (t->committed_tail)
        t->committed_tail->next = NULL;
    for (uint32_t i = 0; i < t->writer_count; i++) {
        struct pf_chunk *c = t->writers[i].cur;
        if (c)
            c->next = NULL;
    }
}

struct pf_chunk *pf_committed_take(struct pf_trace *t)
{
    pthread_mutex_lock(&t->pool_lock);
    pf_writer_flush_own_locked(t);
    struct pf_chunk *head = t->committed_head;
    t->committed_head = NULL;
    t->committed_tail = NULL;
    t->committed_count = 0;
    pthread_mutex_unlock(&t->pool_lock);
    return head;
}

void pf_chunks_release(struct pf_trace *t, struct pf_chunk *head)
{
    if (!head)
        return;

    pthread_mutex_lock(&t->pool_lock);
    while (head) {
        struct pf_chunk *next = head->next;
        head->next = t->free_list;
        t->free_list = head;
        head = next;
    }
    pthread_mutex_unlock(&t->pool_lock);
}
