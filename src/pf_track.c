/* The track table: registration, lookup and renames. See struct track_def. */

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pftrace_internal.h"

void copy_track_name(char *dst, const char *name)
{
    size_t n = strnlen(name, PF_TRACK_NAME_MAX - 1);
    memcpy(dst, name, n);
    dst[n] = '\0';
}

static void set_track_name(struct track_def *td, const char *name)
{
    copy_track_name(td->name, name);
    td->has_name = true;
}

/* Appends a track. Caller holds track_lock. */
static uint64_t track_add_locked(struct pf_trace *t, enum track_kind kind, uint64_t parent_uuid,
                                 int32_t pid, int32_t tid, const char *name, enum pf_unit unit)
{
    if (t->track_count >= t->cfg.max_tracks)
        return 0;
    struct track_def *td = &t->tracks[t->track_count++];
    memset(td, 0, sizeof(*td));
    td->kind = kind;
    td->uuid = t->next_uuid++;
    td->parent_uuid = parent_uuid;
    td->pid = pid;
    td->tid = tid;
    td->unit = unit;
    if (name)
        set_track_name(td, name);
    return td->uuid;
}

uint64_t track_add(struct pf_trace *t, enum track_kind kind, uint64_t parent_uuid, int32_t pid,
                   int32_t tid, const char *name, enum pf_unit unit)
{
    pthread_mutex_lock(&t->track_lock);
    uint64_t uuid = track_add_locked(t, kind, parent_uuid, pid, tid, name, unit);
    pthread_mutex_unlock(&t->track_lock);
    return uuid;
}

void track_rename(struct pf_trace *t, uint64_t uuid, const char *name)
{
    pthread_mutex_lock(&t->track_lock);
    for (unsigned i = 0; i < t->track_count; i++) {
        struct track_def *td = &t->tracks[i];
        if (td->uuid != uuid)
            continue;
        set_track_name(td, name);
        td->name_version++;
        break;
    }
    pthread_mutex_unlock(&t->track_lock);
}

bool track_emit_due(struct pf_trace *t, unsigned i, unsigned first, struct track_def *out)
{
    pthread_mutex_lock(&t->track_lock);
    struct track_def *td = &t->tracks[i];
    bool due = i >= first || td->name_version != td->emitted_version;
    if (due) {
        *out = *td;
        td->emitted_version = td->name_version;
    }
    pthread_mutex_unlock(&t->track_lock);
    return due;
}

uint64_t pf_track_thread(struct pf_trace *t, int32_t pid, int32_t tid, const char *name)
{
    return track_add(t, TRACK_THREAD, t->process_uuid, pid, tid, name, PF_UNIT_UNSPECIFIED);
}

/* Whether @td's stored name is @name. Compares against the truncated form
 * stored by set_track_name(), so a name longer than the cap matches its own
 * stored copy. */
static bool track_name_eq(const struct track_def *td, const char *name)
{
    size_t n = strnlen(name, PF_TRACK_NAME_MAX - 1);
    return td->has_name && strlen(td->name) == n && memcmp(td->name, name, n) == 0;
}

/* Whether @uuid names a process or thread track, the only kinds a track may
 * be parented under. Tracks are never removed, so a uuid in range is valid. */
static bool track_can_parent(struct pf_trace *t, uint64_t uuid)
{
    bool ok = false;
    pthread_mutex_lock(&t->track_lock);
    for (unsigned i = 0; i < t->track_count; i++) {
        if (t->tracks[i].uuid != uuid)
            continue;
        ok = t->tracks[i].kind == TRACK_PROCESS || t->tracks[i].kind == TRACK_THREAD;
        break;
    }
    pthread_mutex_unlock(&t->track_lock);
    return ok;
}

/* Returns the uuid of the track of @kind under @parent_uuid named @name,
 * adding one with @pid, @tid and @unit if there is none. A NULL @name never
 * matches, so unnamed tracks are always new. A match with a different @unit
 * returns 0. Lookup and insert share one critical section, so two threads
 * registering the same name cannot both insert it. */
static uint64_t track_find_or_add(struct pf_trace *t, enum track_kind kind, uint64_t parent_uuid,
                                  int32_t pid, int32_t tid, const char *name, enum pf_unit unit)
{
    pthread_mutex_lock(&t->track_lock);
    uint64_t uuid = 0;
    bool found = false;
    for (unsigned i = 0; i < t->track_count && name; i++) {
        const struct track_def *td = &t->tracks[i];
        if (td->kind != kind || td->parent_uuid != parent_uuid || !track_name_eq(td, name))
            continue;
        /* Same name with a different unit is a conflict, not a match. */
        uuid = td->unit == unit ? td->uuid : 0;
        found = true;
        break;
    }
    if (!found)
        uuid = track_add_locked(t, kind, parent_uuid, pid, tid, name, unit);
    pthread_mutex_unlock(&t->track_lock);
    return uuid;
}

uint64_t pf_track_counter(struct pf_trace *t, uint64_t parent_uuid, const char *name,
                          enum pf_unit unit)
{
    /* 0 selects the process track. Any other parent must be a process or
     * thread track, not a counter. */
    if (parent_uuid && !track_can_parent(t, parent_uuid))
        return 0;
    uint64_t parent = parent_uuid ? parent_uuid : t->process_uuid;
    return track_find_or_add(t, TRACK_COUNTER, parent, 0, 0, name, unit);
}
