/* Prototype for the shared intern table: what a name lookup costs with the
 * current per-writer cache against one frozen table shared by every writer.
 *
 * The current cache (pf_pb_lookup in src/pf_pb_encode.c) scans a writer's 32
 * slots and compares pointers. The prototype builds a global table once, from
 * the static names, and looks a name up by hashing its address. Both tables
 * hold the same 16 literals. This measures the lookup alone, on one thread,
 * with no encoding and no chunks. It also checks that both return the same
 * iid for every name, and that an unknown pointer misses in both.
 *
 * Standalone: nothing from the library is linked.
 */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EVENTS 20000000u
#define NAMES 16u
#define WRITER_SLOTS 32u
#define GLOBAL_CAP 64u /* power of two, well above NAMES */

static const char *const kNames[NAMES] = {
    "parse", "decode", "lookup", "encode", "validate", "fetch", "merge", "sort",
    "hash",  "copy",   "filter", "reduce", "index",    "flush", "wait",  "notify",
};

/* The same slot shape as struct pb_intern_slot. */
struct writer_slot {
    const char *key;
    size_t len;
    uint32_t epoch;
};

struct global_slot {
    const char *key; /* NULL when empty */
    uint32_t iid;
};

static struct writer_slot writer_tab[WRITER_SLOTS];
static struct global_slot global_tab[GLOBAL_CAP];

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Multiplicative hash of the address. The top bits pick the slot. */
static size_t global_hash(const char *p)
{
    return (size_t)(((uintptr_t)p * 0x9E3779B97F4A7C15ull) >> 58) & (GLOBAL_CAP - 1);
}

/* Builds the global table once, before any writer uses it. Frozen after this:
 * nothing writes to it again, so readers need no lock. */
static void global_init(void)
{
    for (unsigned i = 0; i < NAMES; i++) {
        size_t idx = global_hash(kNames[i]);
        while (global_tab[idx].key)
            idx = (idx + 1) & (GLOBAL_CAP - 1);
        global_tab[idx].key = kNames[i];
        global_tab[idx].iid = i + 1;
    }
}

/* Returns the iid of @name, or 0 if it is not in the table. */
static uint32_t global_lookup(const char *name)
{
    size_t idx = global_hash(name);
    for (unsigned probe = 0; probe < GLOBAL_CAP; probe++) {
        const struct global_slot *s = &global_tab[idx];
        if (!s->key)
            return 0;
        if (s->key == name)
            return s->iid;
        idx = (idx + 1) & (GLOBAL_CAP - 1);
    }
    return 0;
}

/* Fills the writer's cache with the same names, in the same order. */
static void writer_init(void)
{
    for (unsigned i = 0; i < NAMES; i++) {
        writer_tab[i].key = kNames[i];
        writer_tab[i].len = strlen(kNames[i]);
        writer_tab[i].epoch = 1;
    }
}

/* Mirrors pf_pb_lookup's hit path: scan for the pointer, return its slot. */
static int writer_lookup(const char *name)
{
    for (int i = 0; i < (int)WRITER_SLOTS; i++) {
        if (writer_tab[i].key == name)
            return i;
    }
    return -1;
}

static int check_tables(void)
{
    int bad = 0;
    for (unsigned i = 0; i < NAMES; i++) {
        uint32_t g = global_lookup(kNames[i]);
        int w = writer_lookup(kNames[i]);
        if (g != i + 1 || w != (int)i) {
            printf("mismatch on %s: global %u, writer slot %d\n", kNames[i], g, w);
            bad++;
        }
    }
    /* A pointer that is not a registered literal must miss in both. The copy
     * is a different address with the same contents. */
    char other[16];
    strcpy(other, "parse");
    if (global_lookup(other) != 0 || writer_lookup(other) != -1) {
        printf("unknown pointer was found\n");
        bad++;
    }
    return bad;
}

int main(void)
{
    global_init();
    writer_init();

    int bad = check_tables();
    if (bad) {
        printf("FAIL: %d table check(s) failed\n", bad);
        return 1;
    }

    /* The checksum keeps the optimizer from dropping the lookups. */
    uint64_t sum = 0;

    uint64_t t0 = now_ns();
    for (uint32_t e = 0; e < EVENTS; e++)
        sum += (uint64_t)(writer_lookup(kNames[e % NAMES]) + 1);
    uint64_t t1 = now_ns();

    for (uint32_t e = 0; e < EVENTS; e++)
        sum += global_lookup(kNames[e % NAMES]);
    uint64_t t2 = now_ns();

    printf("per-writer scan: %.2f ns/lookup\n", (double)(t1 - t0) / EVENTS);
    printf("global hash:     %.2f ns/lookup\n", (double)(t2 - t1) / EVENTS);
    printf("checksum %llu\n", (unsigned long long)sum);
    return 0;
}
