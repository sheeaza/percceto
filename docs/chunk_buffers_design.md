# Chunk buffers

Per-thread chunks replace the shared ring. A producer writes records into a
chunk it owns, and touches shared state only when that chunk fills. The drain
walks committed chunks in commit order. It does not merge them: Perfetto
sorts by timestamp on import.

## Layout

    trace
     ├─ pool         one allocation at open: buffer_bytes / chunk_bytes chunks
     ├─ free list    chunks nobody holds
     ├─ committed    FIFO of full chunks, in commit order
     ├─ writers[]    max_threads slots, bound on a thread's first emit
     └─ refs         owner's reference, plus one per bound thread

    chunk: { next, used (bytes, atomic), writer, seq, bytes[chunk payload] }

    chunk payload = chunk_bytes - header

Each chunk's bytes hold whole entries: raw records, which are a 32-byte head
plus any copied name, or protobuf Trace.packet entries, which are variable
length. A chunk never holds part of an entry. See "Protobuf" and "Variable-size
raw records" below.

## Config

- `buffer_bytes`: total buffering budget. Default 8 MiB. Must hold two chunks.
- `chunk_slots`: chunk size in records. Default 256. Chunk bytes follow from it.
- `max_threads`: writer slots. Default 256.

## Hot path

    emit (raw):
      ts = now()
      w = this thread's writer for this trace (pf_tls slot by trace index; bound on first use)
      if w->used + rec_size > chunk_payload: rotate (slow path, once per chunk)
      r = &w->cur->bytes[w->used]; fill the record; w->used += rec_size
      w->cur->used = w->used   (release; publishes the record to a snapshot)
      w->events += 1   (relaxed; written only by this thread)

    rec_size = 32, or 32 + pad8(len) when the name is copied into the record

    emit (protobuf): the same, with the event encoded into the chunk, and the
      name's definition placed in the same chunk when the writer's cache needs it.

The common path takes no lock and reads no shared state. The two stores go to
lines only the owner writes: the chunk's `used`, which a snapshot dump reads,
and the writer counter, which stats sums.

## Variable-size raw records

A raw record is 32 bytes, plus its copied name when the name is dynamic. The
trade-offs are the same as for protobuf entries, and are set out in
`docs/raw_format.md` under "Variable-size records":

- A record that does not fit the rest of its chunk moves to the next chunk.
  The unused tail of a chunk therefore grows with the names it holds.
- A copied name costs a `memcpy` on the producer's hot path. Static names do
  not.
- A name longer than `PF_DYN_NAME_MAX` is cut to the cap, ending in `...`, and
  the cut is counted. A record larger than a whole chunk payload is dropped and counted,
  as protobuf entries are.
- `chunk_slots`, a record count, no longer bounds a chunk for raw. The chunk is
  bounded by its payload in bytes.

## Protobuf

Encoding happens on the emitting thread, so the drain copies bytes and does not
encode. Each event is one `Trace.packet`, encoded in one pass into the chunk. The
length of each submessage is computed from its fields before the packet is
written, so every byte is written once.

Interned names are per writer. A writer owns trusted sequence `id + 1`, and its
slot `i` is interned as iid `i + 1` in that sequence. The writer's cache holds 32
names, each stamped with the epoch of the chunk its definition went into. A
rotation advances the epoch, so a name used in a new chunk is defined again there.
Each chunk is therefore self-contained.

A record that does not fit the rest of its chunk moves to the next chunk; it is
never split. A record larger than a whole chunk is dropped and counted.

Typed arguments are protobuf debug annotations, written in the same packet as
the event, after its name. Their bytes count toward the record size, so an event
with arguments fills its chunk faster and moves to the next chunk sooner. The
rule above applies unchanged.

The track descriptors and the clock snapshot go on sequence 1 and are written by
the drain, not by producers.

## Commit and rotate

Rotate takes the pool lock:

1. commits the full chunk: sets `used`, appends it to the committed FIFO. The
   unlock publishes the records to the drain.
2. takes a chunk from the free list.
3. if the free list is empty:
   - continuous: the event is dropped and counted;
   - snapshot: the oldest committed chunk is stolen. Its records are
     overwritten, which snapshot mode does by design, so nothing is counted as
     dropped.

## Drain

- Continuous: under the lock, detach the committed FIFO. Unlock, write it in
  commit order, lock again and return the chunks to the free list. The
  dumping thread's own partial chunk is committed first.
- Snapshot: under the lock, link every writer's partial chunk after the
  committed FIFO and walk the chain once. The lock stays held for the whole
  dump, so a producer that needs a chunk waits. Partial chunks are not
  committed: their owners keep filling them, and the walk reads each one only
  up to its published `used`. The links are cleared after the walk, and
  `next` is NULL on every chunk a writer holds, so this changes nothing the
  owners read.

The walk reads a chunk's `used` once, when it enters the chunk. Records below
that count are final until the chunk is recycled, which needs the pool lock.

## Threads

- A thread holds a reference to each trace it is bound to, from its first emit
  into that trace until it unbinds. A thread can be bound to several traces at
  once: `pf_tls` has a slot per open trace, and each slot holds the trace and the
  thread's writer in it. Unbinding commits the thread's partial chunk for that
  trace, clears the slot, then drops the reference. One exit hook per thread
  unbinds every slot, so a thread that exits without closing a trace still hands
  its records over.
- `pf_tls` is indexed by the trace's index, claimed at open. At most
  `PF_MAX_TRACES` (8) traces can be open at once; open fails with `ENOSPC` past
  that.
- A thread past `max_threads` drops its events and counts them. It still holds a
  reference, so the trace stays open until it exits. A thread's slot is freed
  when it exits (after its partial chunk is committed), so a later thread can
  bind it. `max_threads` limits threads bound at once, not over the trace's life.

## Close and lifetime

`pf_trace_close` gives back the caller's reference. If the caller is itself
bound, its binding is unbound first, since a main thread never runs the exit
destructor. The last reference to go finishes the trace: it stops the drain
thread, writes what is buffered, closes the file and frees the buffers. Close
does not commit any other thread's chunk, so no producer is raced.

So the file is complete when the last reference goes, which is:

- at close, if no other thread is bound;
- otherwise, at the exit of the last bound thread. Close returns earlier, and
  such a thread may keep emitting until it exits.

Only the reference count guards the trace. A bound thread's cached pointer is
safe because the thread holds a reference, so the trace cannot be freed or its
address reused while the cache points at it. This needs no registry.

## Known limits

- A thread that never exits keeps the trace open, and its partial chunk is not
  written. A process that exits without its threads exiting loses those events.
  This is the cost of close returning early; see TODO.md item 7 for the
  one-trace-per-thread limit that comes with it.
- Continuous mode has no timer flush. A low-rate thread's last events are lost
  only if the process dies before its chunk fills, its thread exits, or the
  trace's last reference goes.

## Risks

- Process exit with live threads. The file is final only when the last thread
  exits. If the process ends first, the partial chunks of those threads are not
  written, and nothing else flushes them. Before this change, close wrote them.
- Close returns early. A caller that reads the file right after close, while a
  thread is still bound, sees an incomplete file. A bound thread may also keep
  emitting after close, so its events reach the file only at its exit.
- Rebinding unbinds the old trace. A thread that binds a second trace gives up
  its first, and its later events go to the second only. This is a behaviour
  change from the old last-bound-trace rule; see TODO.md item 7.
- Snapshot mode. Buffered events of a snapshot trace stay in memory until the
  last reference goes, and are lost then unless a dump was taken first. Close
  does not dump.
- A thread past `max_threads` still takes a reference, so it holds the trace
  open until it exits, even though it writes nothing.

## Stats

- `events` counts records emitted into chunks, per writer, including records
  that a snapshot later overwrites. It matches the old ring's meaning.
- `dropped` counts events lost to a full pool, and events from threads past
  `max_threads`.

## Removed

The `pf_ring` backends (mpsc and zcopy), their per-backend build and tests, and
the ring benchmarks. The `ringbuf/` directory moved out of this repo into its
own project; pftrace no longer uses it.
