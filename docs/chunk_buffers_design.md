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
     ├─ writers[]    power-of-two slots, claimed on a thread's first emit
     └─ refs         owner's reference, plus one per bound thread

    chunk: { next, used (atomic), writer, seq, recs[chunk_slots] }   (recs: struct pf_rec)

    chunk_bytes = header + chunk_slots * PF_PAYLOAD_BYTES

## Config

- `buffer_bytes`: total buffering budget. Default 8 MiB. Must hold two chunks.
- `chunk_slots`: records per chunk. Default 256. Chunk bytes follow from it.
- `max_threads`: writer slots. Default 256. Rounded up to a power of two.

## Hot path

    emit:
      ts = now()
      w = this thread's writer in this trace (probe the owner tags; claimed on first use)
      if w->used == chunk_slots: rotate (slow path, once per chunk)
      r = &w->cur->recs[w->used++]; fill the record
      w->cur->used = w->used   (release; publishes the record to a snapshot)
      w->events += 1   (relaxed; written only by this thread)

The common path takes no lock and reads no shared state. The writer lookup
reads no TLS: it hashes the thread's identity (the thread pointer) to a start
slot and probes for the slot whose owner tag is that identity. The probe reads
the owner tag, which sits on a line of its own and is written only on bind and
unbind, so it does not contend with the producers. The two stores go to lines
only the owner writes: the chunk's `used`, which a snapshot dump reads, and the
writer counter, which stats sums.

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

- A thread holds a reference to each trace it has bound to, from its first emit
  into that trace until it unbinds. A thread can be bound to several traces at
  once, and each one holds a writer slot and a reference. Unbinding commits the
  thread's partial chunk for that trace, frees the slot, then drops the
  reference. Each trace has a pthread key whose value is the trace while the
  thread holds a reference. Its destructor unbinds at thread exit, so a thread
  that exits without closing the trace still hands its records over.
- A thread past `max_threads` drops its events and counts them. It still holds a
  reference, so the trace stays open until it exits. A thread's slot is freed
  when it exits (after its partial chunk is committed), so a later thread can
  bind it. `max_threads` limits threads bound at once, not over the trace's life.
- A thread past the limit pays for its miss on every event: the lookup probes the
  whole table, then it checks its key. That is the cost of the overflow path,
  not of the common one.

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

Only the reference count guards the trace. The emit path reads only the trace's
own writer table, so it never touches a pointer that could outlive the trace.
The thread's key value holds its reference, and that is what keeps the trace
alive while the thread is bound.

## Known limits

- A thread that never exits keeps the trace open, and its partial chunk is not
  written. A process that exits without its threads exiting loses those events.
  This is the cost of close returning early.
- Continuous mode has no timer flush. A low-rate thread's last events are lost
  only if the process dies before its chunk fills, its thread exits, or the
  trace's last reference goes.
- The simple API's thread track uuid is still one per thread, not per trace.
  See TODO.md item 7.

## Risks

- Process exit with live threads. The file is final only when the last thread
  exits. If the process ends first, the partial chunks of those threads are not
  written, and nothing else flushes them. Before this change, close wrote them.
- Close returns early. A caller that reads the file right after close, while a
  thread is still bound, sees an incomplete file. A bound thread may also keep
  emitting after close, so its events reach the file only at its exit.
- A thread bound to two traces holds a reference in each, so each trace stays
  open until that thread exits or unbinds from it. Before, binding a second
  trace released the first, so close of the first could finish it at once.
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
the ring benchmarks. The `ringbuf/` directory stays in the repo as its own
project; pftrace no longer uses it.
