/* Chunk pool and writers. See docs/chunk_buffers_design.md.
 *
 * Producers take the pool lock only when they rotate to a new chunk, once per
 * chunk_slots events. The drain takes it to detach or walk the committed FIFO.
 * Nothing on the per-event path reads or writes shared state, apart from a
 * relaxed store to the thread's own writer counter. */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pftrace_internal.h"

__thread struct pf_thread_state pf_tls;

static pthread_key_t g_exit_key;
static pthread_once_t g_exit_key_once = PTHREAD_ONCE_INIT;

/* Trace indices in use. Guarded by g_index_lock, taken only at open and finish. */
static pthread_mutex_t g_index_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_index_used[PF_MAX_TRACES];

int pf_index_acquire(struct pf_trace *t)
{
    pthread_mutex_lock(&g_index_lock);
    for (uint32_t i = 0; i < PF_MAX_TRACES; i++) {
        if (!g_index_used[i]) {
            g_index_used[i] = true;
            t->index = i;
            pthread_mutex_unlock(&g_index_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_index_lock);
    errno = ENOSPC;
    return -1;
}

void pf_index_release(struct pf_trace *t)
{
    pthread_mutex_lock(&g_index_lock);
    g_index_used[t->index] = false;
    pthread_mutex_unlock(&g_index_lock);
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

/* Gives back the calling thread's slot for @t. The partial chunk is committed,
 * so its records reach the drain without waiting for the trace to finish. The
 * slot is cleared first, so nothing can find it while it is being released, and
 * the reference is dropped last, since it may be the one that finishes the trace.
 * Runs on rebind-free paths: pf_trace_close on the closing thread, and the
 * thread's exit. */
static void unbind(struct pf_trace *t)
{
    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    struct pf_writer *w = s->writer;
    s->trace = NULL;
    s->writer = NULL;
    s->track_uuid = 0;

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

        /* Bind resets the producer's fields, so the next owner starts clean.
         * The event count is kept, so stats still count this slot's events. */
        pthread_mutex_lock(&t->reg_lock);
        w->bound = false;
        pthread_mutex_unlock(&t->reg_lock);
    }
    pf_trace_unref(t);
}

void pf_unbind_thread(struct pf_trace *t)
{
    if (pf_tls.slots[t->index].trace == t)
        unbind(t);
}

/* One exit hook per thread, for every trace it is bound to. */
static void writer_exit(void *unused)
{
    (void)unused;
    for (uint32_t i = 0; i < PF_MAX_TRACES; i++) {
        struct pf_trace *t = pf_tls.slots[i].trace;
        if (t)
            unbind(t);
    }
}

static void make_exit_key(void)
{
    pthread_key_create(&g_exit_key, writer_exit);
}

/* A chunk's size, padded to a cache line so no two chunks share one. */
size_t pf_chunk_bytes(uint32_t slots)
{
    return roundup(sizeof(struct pf_chunk) + slots * PF_REC_HEAD_BYTES, (size_t)64);
}

int pf_pool_init(struct pf_trace *t)
{
    uint32_t slots = (uint32_t)t->cfg.chunk_slots;
    size_t chunk_bytes = pf_chunk_bytes(slots);
    size_t count = t->cfg.buffer_bytes / chunk_bytes;

    /* A writer needs a spare chunk while its full one is committed. */
    if (count < 2) {
        errno = EINVAL;
        return -1;
    }

    /* mmap, not malloc and memset: the kernel hands the pages over zeroed, so
     * there is no pass to clear them. MADV_POPULATE_WRITE maps them now, so no
     * producer takes a page fault on its first use of a chunk. Kernels before
     * 5.14 lack it, and touch each page instead. A mapping is page aligned, so
     * every chunk starts on a cache line. */
    size_t pool_bytes = count * chunk_bytes;
    t->pool_mem =
        mmap(NULL, pool_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t->pool_mem == MAP_FAILED) {
        t->pool_mem = NULL;
        errno = ENOMEM;
        return -1;
    }
    if (madvise(t->pool_mem, pool_bytes, MADV_POPULATE_WRITE) != 0) {
        long page = sysconf(_SC_PAGESIZE);
        for (size_t off = 0; off < pool_bytes; off += (size_t)page)
            ((volatile char *)t->pool_mem)[off] = 0;
    }

    /* aligned_alloc, not calloc: the writers need a cache-line boundary, and
     * sizeof is a multiple of 64, as aligned_alloc requires. Zeroed by hand. */
    t->writers = aligned_alloc(64, t->cfg.max_threads * sizeof(*t->writers));
    if (!t->writers)
        goto free_pool;
    memset(t->writers, 0, t->cfg.max_threads * sizeof(*t->writers));

    /* Each chunk is linked onto the free list. chunk_bytes is a multiple of 64,
     * so every chunk starts on a cache line and its records stay 8-byte aligned. */
    for (size_t i = count; i-- > 0;) {
        struct pf_chunk *c = (struct pf_chunk *)((char *)t->pool_mem + i * chunk_bytes);
        c->next = t->free_list;
        t->free_list = c;
    }
    atomic_init(&t->free_count, (uint32_t)count);

    t->chunk_bytes = chunk_bytes;
    t->chunk_count = (uint32_t)count;
    t->chunk_slots = slots;
    t->chunk_payload = slots * PF_REC_HEAD_BYTES;
    t->watermark_chunks = (uint32_t)(count * t->cfg.watermark_pct / 100u);

    pthread_mutex_init(&t->pool_lock, NULL);
    pthread_mutex_init(&t->reg_lock, NULL);
    return 0;

free_pool:
    munmap(t->pool_mem, pool_bytes);
    t->pool_mem = NULL;
    errno = ENOMEM;
    return -1;
}

void pf_pool_destroy(struct pf_trace *t)
{
    pthread_mutex_destroy(&t->reg_lock);
    pthread_mutex_destroy(&t->pool_lock);
    free(t->writers);
    munmap(t->pool_mem, (size_t)t->chunk_count * t->chunk_bytes);
    t->writers = NULL;
    t->pool_mem = NULL;
}

struct pf_writer *pf_writer_bind(struct pf_trace *t)
{
    struct pf_writer *w = NULL;

    /* Registered on every bind, NULL writer included, so the exit hook runs
     * and gives back the reference. */
    pthread_once(&g_exit_key_once, make_exit_key);
    pthread_setspecific(g_exit_key, &pf_tls);

    /* Taken even when no slot is free, so the slot always names a live trace. */
    atomic_fetch_add_explicit(&t->refs, 1, memory_order_relaxed);

    pthread_mutex_lock(&t->reg_lock);
    for (uint32_t i = 0; i < t->cfg.max_threads; i++) {
        if (!t->writers[i].bound) {
            w = &t->writers[i];
            w->bound = true;
            w->id = i;
            break;
        }
    }
    pthread_mutex_unlock(&t->reg_lock);

    /* Fields owned by the producer are set here, before the thread uses the
     * writer. used == slots makes the first emit rotate and take a chunk. */
    if (w) {
        w->cur = NULL;
        w->used = t->chunk_payload;
        w->seq = 0;
    }

    pf_tls.slots[t->index].trace = t;
    pf_tls.slots[t->index].writer = w;
    return w;
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
    /* A writer with no chunk while the pool is empty has nothing to commit and
     * nothing to take, so it drops without the lock. Otherwise every event of
     * every starved producer would take pool_lock, and slow the drain that is
     * freeing chunks. The count may be stale; the cost is one more drop. */
    if (!w->cur && t->cfg.mode == PF_MODE_CONTINUOUS &&
        atomic_load_explicit(&t->free_count, memory_order_relaxed) == 0) {
        pf_writer_drop(w);
        return false;
    }

    pthread_mutex_lock(&t->pool_lock);

    if (w->cur)
        commit_locked(t, w);

    struct pf_chunk *c = t->free_list;
    if (c) {
        t->free_list = c->next;
        atomic_store_explicit(&t->free_count,
                              atomic_load_explicit(&t->free_count, memory_order_relaxed) - 1,
                              memory_order_relaxed);
    } else if (t->cfg.mode == PF_MODE_SNAPSHOT) {
        c = steal_oldest_locked(t);
    }

    if (c) {
        c->next = NULL;
        c->used = 0;
        c->writer = w->id;
        c->seq = w->seq++;
        w->epoch++;
    }
    w->cur = c;
    w->used = c ? 0 : t->chunk_payload;

    bool over_watermark = t->cfg.mode == PF_MODE_CONTINUOUS &&
                          t->committed_count >= t->watermark_chunks;
    pthread_mutex_unlock(&t->pool_lock);

    if (over_watermark)
        pf_drain_wake(t);
    if (!c)
        pf_writer_drop(w);
    return c != NULL;
}

uint64_t pf_writers_events(struct pf_trace *t)
{
    uint64_t total = 0;
    pthread_mutex_lock(&t->reg_lock);
    for (uint32_t i = 0; i < t->cfg.max_threads; i++)
        total += atomic_load_explicit(&t->writers[i].events, memory_order_relaxed);
    pthread_mutex_unlock(&t->reg_lock);
    return total;
}

uint64_t pf_writers_dropped(struct pf_trace *t)
{
    uint64_t total = 0;
    pthread_mutex_lock(&t->reg_lock);
    for (uint32_t i = 0; i < t->cfg.max_threads; i++)
        total += atomic_load_explicit(&t->writers[i].dropped, memory_order_relaxed);
    pthread_mutex_unlock(&t->reg_lock);
    return total;
}

/* Continuous drain: commits the calling thread's partial chunk, so the drain
 * includes its last events. Caller holds pool_lock. */
static void pf_writer_flush_own_locked(struct pf_trace *t)
{
    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    if (s->trace != t || !s->writer)
        return;

    struct pf_writer *w = s->writer;
    if (!w->cur || w->used == 0)
        return;
    commit_locked(t, w);
    w->used = t->chunk_payload;
}

/* A live partial chunk has next == NULL, like every chunk a writer holds, and
 * the owner never reads next. So a snapshot can borrow the link field for the
 * length of one walk. */
struct pf_chunk *pf_link_partials_locked(struct pf_trace *t)
{
    struct pf_chunk *head = t->committed_head;
    struct pf_chunk *tail = t->committed_tail;
    for (uint32_t i = 0; i < t->cfg.max_threads; i++) {
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
    for (uint32_t i = 0; i < t->cfg.max_threads; i++) {
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
    uint32_t n = 0;
    while (head) {
        struct pf_chunk *next = head->next;
        head->next = t->free_list;
        t->free_list = head;
        head = next;
        n++;
    }
    atomic_store_explicit(&t->free_count,
                          atomic_load_explicit(&t->free_count, memory_order_relaxed) + n,
                          memory_order_relaxed);
    pthread_mutex_unlock(&t->pool_lock);
}
