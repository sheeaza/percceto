# ringbuf_mpsc design notes

Status: **complete.** This document covers `reserve`, `submit`, `pop`, and
`advance_read_idx()` in both modes.

## Modes

The buffer has two modes, chosen at create time (`enum ringbuf_mode`):

- **Drop newest:** when the buffer is full, the new push is dropped.
- **Drop oldest:** when the buffer is full, the oldest unread message is
  dropped and overwritten by the new one.

---

## Algorithm

The producer side and the consumer side use the same slot protocol. The
drop-newest producer path is below. It is the same as the usual bounded MPMC
ring, with two changes: the producer never spins, and it claims with a CAS
instead of `fetch_add`.

**Reserve (producer, drop-newest)**

```
pos = write_idx.load(relaxed)
loop:
    idx = pos & mask
    slot = stamp[idx]
    seq = slot.load(acquire)
    if seq == pos:                              /* free for ticket pos */
        if write_idx.CAS(pos -> pos+1, relaxed, relaxed)
            return slot.data, ticket pos        /* claimed */
        continue                                /* pos reloaded by CAS */
    if seq < pos:                               /* previous lap not yet freed */
        return full                             /* no ticket consumed */
    pos = write_idx.load(relaxed)               /* pos was stale, retry */
```

The caller then writes the payload into `slot.data` with normal stores, and
calls submit.

**Submit (producer)**

```
slot.seq.store(pos + 1, release)
```

**Pop (consumer, drop-newest)**

```
pos = read_idx.load(relaxed)
idx = pos & mask
slot = stamp[idx]
if slot.seq.load(acquire) != pos + 1:          /* not yet published */
    return empty
read payload
slot.seq.store(pos + capacity, release)
read_idx.store(pos + 1, relaxed)
```

Here `stamp` is the per-slot `seq` array, `write_idx` is the producer tail,
and `read_idx` is the consumer head. In drop-newest mode the consumer is the only
writer of `read_idx`.

---

## Why CAS and not fetch_add

The textbook producer uses `tail.fetch_add(1)`. That claims a ticket
unconditionally, so a producer that finds the slot still occupied has already
consumed a ticket it cannot give back. The ring would then have a hole at that
position, and the consumer would wait on it forever.

Reserve has to be able to refuse. So it reads the slot first, and claims with a
CAS only when the slot is free for the ticket it is about to take. A failed
refusal leaves `write_idx` untouched. Retrying is therefore always safe.

The cost is one CAS per claim in place of one `fetch_add`, plus a retry loop
when another producer wins the race.

---

## Why the two sides of a claim do not need more than this

A producer may be preempted between reading `seq` and CASing `write_idx`. When
it resumes, `write_idx` has moved on, so its CAS fails. It cannot win a ticket
that was already handed out, so there is no ABA problem.

`write_idx` only increases, so the value `pos` cannot come back after a lost
race, except after wrapping `size_t`. The signed differences in the comparisons
keep the logic correct across that wrap.

---

## Why a `seq < pos` result means genuinely full

Example: all tickets 0 to 3 are published and unread.

```
slot:      0 1 2 3
stamp:     1 2 3 4
write_idx: 4
read_idx:  0
```

A producer reads `pos = 4`. Slot 0 holds `stamp = 1`, which is less than 4, so
reserve refuses. Nothing is lost: ticket 0 is still unread.

---

## What `seq < pos` can mean

Same state as above, with `pos = 4`. Slot 0 can hold either of two values:

Published, not yet popped (ticket 0 is in the slot):

```
slot:      0 1 2 3
stamp:     1 2 3 4
write_idx: 4
read_idx:  0
```

Claimed, not yet submitted (ticket 0 was reserved but the filler has not
submitted):

```
slot:      0 1 2 3
stamp:     0 2 3 4
write_idx: 4
read_idx:  0
```

Both refuse. The producer does not need to tell them apart.

---

## Overwrite mode (drop-oldest): reserve

Drop-oldest reserve has the same claim as drop-newest, but on a full ring it
evicts the oldest unread message instead of refusing. Eviction needs a per-slot
lock, because a consumer may be copying that slot out at the same moment.

**Reserve (producer, drop-oldest)**

```
pos = write_idx.load(relaxed)
loop:
    idx = pos & mask
    seq = stamp[idx].load(acquire)
    if seq > pos:                               /* stale, retry */
        pos = write_idx.load(relaxed)
        continue
    if seq == pos:                              /* free for ticket pos */
        if write_idx.CAS(pos -> pos+1, relaxed, relaxed)
            return slot.data, ticket pos
        continue
    /* full: seq is pos - capacity + 1 (unread) or pos - capacity (unsubmitted) */
    if !slot_lock[idx].CAS(0 -> 1, acquire)
        return false                            /* consumer is copying this slot */
    if actual = stamp[idx].CAS(pos - capacity + 1 -> pos) succeeds
        dropped_count++
        advance_read_idx(pos - capacity + 1)
    else if actual == pos - capacity            /* previous lap unsubmitted */
        slot_lock[idx].store(0, release)
        return false
    /* else the consumer already retired the victim; nothing is counted */
    if write_idx.CAS(pos -> pos+1, relaxed, relaxed)
        slot_lock[idx].store(0, release)
        return slot.data, ticket pos
    slot_lock[idx].store(0, release)
    pos = write_idx.load(relaxed)
```

The lock is released before the caller fills the slot, not after. It only
guards the victim's bytes while eviction is settled. The new message is
protected by the stamp: it reads as "reserved for pos", so no other producer
evicts it until it is submitted.

### Lock before the ticket CAS

The slot lock is taken before `write_idx` is claimed. The reason is that a won
ticket cannot be given back. Take the ticket first, and then fail the lock, and
the ticket is left with no slot filled behind it. The consumer waits on that
ticket forever and the ring wedges. Taking the lock on a tentative `pos` is
safe, because the `write_idx` CAS still checks that `pos` is current.

Example: tickets 0 to 3 are published, and a producer at `pos = 4` evicts the
oldest.

```
slot:      0 1 2 3
stamp:     1 2 3 4
write_idx: 4
read_idx:  0
```

The producer takes the lock on slot 0, swaps its stamp from `1` to `4` (ticket 0
is dropped, `dropped_count` goes to 1), moves `read_idx` to 1, claims ticket 4,
and releases the lock:

```
slot:      0 1 2 3
stamp:     4 2 3 4
write_idx:   5
read_idx:    1
```

### What the stamp CAS can see

The eviction CAS sees one of three states on slot `idx`:

- `pos - capacity + 1`: the victim is published. Evict it.
- `pos - capacity`: the victim is claimed but not submitted. Refuse. A filler may
  still be writing those bytes, so this slot is never evicted.
- `pos + 1`: the consumer already popped the victim. Nothing is counted, and the
  slot is free.

### An eviction stands even if the ticket CAS fails

If another producer wins `pos` after the eviction, this producer's victim is
still counted as dropped. The message really is gone, and the winner fills the
slot. Retrying with a fresh `pos` is safe.

---

## Submit (both modes)

```
stamp[pos & mask].store(pos + 1, release)
```

Submit takes no lock in either mode. Eviction only retires a slot whose stamp
reads as published, and it refuses a slot that is still unsubmitted. So an
eviction can never race a submit.

---

## Pop (drop-newest)

Shown in the Algorithm section above.

---

## Pop (drop-oldest)

The consumer and an evicting producer can both touch the same slot, so pop takes
the same slot lock that eviction does.

```
pos = read_idx.load(relaxed)
idx = pos & mask
if !slot_lock[idx].CAS(0 -> 1, acquire)
    return false                                /* producer evicting this slot */
if read_idx.load(acquire) != pos
    slot_lock[idx].store(0, release)
    return false                                /* evicted while we waited */
if stamp[idx].load(acquire) != pos + 1
    slot_lock[idx].store(0, release)
    return false                                /* not yet published */
read payload
stamp[idx].store(pos + capacity, release)       /* safe: we hold the lock */
advance_read_idx(pos + 1)
slot_lock[idx].store(0, release)
```

The consumer's stamp check already rejects a stale `pos`. Eviction always moves
the stamp off `pos + 1`, and nothing moves it back for that lap. So the
`read_idx` re-read under the lock is an early exit, not a requirement.

Example: pop reads `pos = 0` and waits for the lock on slot 0. Meanwhile a
producer evicts ticket 0, claims ticket 4, and submits it:

```
slot:      0 1 2 3
stamp:     5 2 3 4
write_idx:   5
read_idx:    1
```

Pop takes the lock, sees `stamp = 5` instead of `pos + 1 = 1`, and returns
false.

---

## advance_read_idx

```
advance_read_idx(target):
    cur = read_idx.load(relaxed)
    while target - cur > 0                      /* signed */
        if read_idx.CAS(cur -> target, release)
            return
```

`read_idx` has two writers in drop-oldest mode: the consumer when it receives a
message, and an evicting producer when it drops one. The slot lock serializes
two writers on the same slot, but these two write on different slots, so their
stores can arrive out of order.

Example: the consumer has received ticket 4 but not yet called
`advance_read_idx(5)`. A producer takes ticket 8 into slot 0, then ticket 9,
which evicts ticket 5 from slot 1 and advances `read_idx` to 6:

```
slot:      0 1 2 3
stamp:     8 9 7 8
write_idx:     10
read_idx:      6
```

If the consumer's store of 5 landed now, a plain store would move `read_idx`
back to 5, a message that was already dropped. The CAS loop only moves it
forward, so the late store does nothing.

---
