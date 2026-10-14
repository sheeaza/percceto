# ringbuf_mpsc design notes

Status: **draft, for review.** Written one function at a time. This section
covers the claim path in drop-newest mode (`mode = RINGBUF_DROP_NEWEST`),
`reserve_drop_newest()`. Later sections will cover overwrite mode, `submit`,
`pop`, and `advance_read_idx()`.

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
read_idx.store(pos + 1, release)
return payload
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

## Ordering

- **`seq.load(acquire)` in reserve** pairs with the consumer's
  `seq.store(pos + capacity, release)` in pop. When reserve sees the slot free,
  the consumer's read of the previous payload is complete, so the caller can
  write the slot without racing it. This is the ordering that carries the
  protocol.
- **`write_idx` loads and the CAS are relaxed.** Nothing reads `write_idx` to
  decide readiness. The CAS is an atomic read-modify-write on one location,
  so each ticket is won by exactly one producer. Ordering between producers'
  payload writes comes from `seq`, not from `write_idx`.
- **`seq.store(pos + 1, release)` in submit** pairs with the consumer's
  `seq.load(acquire)` in pop. The payload written by the producer is visible
  to the consumer before it reads it.

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

The producer's starting `pos` comes from a relaxed load, and it may be stale.
The refusal branch is only taken when `seq < pos` is observed on the slot
after that load. Ticket `pos - capacity` is not yet retired in that case.

To claim ticket `pos`, a producer needs the slot free for `pos`, which needs
ticket `pos - capacity` retired first. That has not happened. So at the moment
the slot is read, nobody holds ticket `pos`, and `write_idx` equals `pos`. The
refusal reflects the ring as it is at the read, not as it was at the start of
the loop, so it is not a false full.

---

## What `seq < pos` can mean

The slot is still holding the previous lap's ticket, `pos - capacity`. Either:

- that message is published but not yet popped, or
- that producer has claimed the ticket but not yet submitted.

The producer cannot tell these apart, and it does not need to. In both cases
the slot cannot be taken. The second case can last as long as the filler
takes, so in drop-newest mode reserve can refuse even when the consumer is idle.

---

## Interaction with other functions

- **`push`** is reserve, a `memcpy`, then submit. In drop-newest mode its
  behavior is the same as calling those three in sequence.
- **`submit`** publishes the slot. Its release is what `pop` acquires.
- **`pop`** frees the slot. Its release store is what reserve acquires.
  Lossless `pop` also advances `read_idx`; reserve never reads `read_idx`.

---

## Next section

`reserve_overwrite()`. It takes a per-slot lock before its CAS and can evict a
published message, so the order of the lock and the CAS is the main design
point there.
