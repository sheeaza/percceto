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
     └─ open_next    link on the open-trace registry

    chunk: { next, used, writer, seq, recs[chunk_slots] }   (recs: struct pf_rec)

    chunk_bytes = header + chunk_slots * PF_PAYLOAD_BYTES

## Config

- `buffer_bytes`: total buffering budget. Default 8 MiB. Must hold two chunks.
- `chunk_slots`: records per chunk. Default 256. Chunk bytes follow from it.
- `max_threads`: writer slots. Default 256.

## Hot path

    emit:
      ts = now()
      w = writer for this trace (TLS cache; bound on first use)
      if w->used == chunk_slots: rotate (slow path, once per chunk)
      r = &w->cur->recs[w->used++]; fill the record
      w->events += 1   (relaxed; written only by this thread)

No shared state is read or written on the common path. The writer counter is
a relaxed store to a line only its owner writes, so stats can sum the counters
without the emit path ever contending with them.

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
  commit order, lock again and return the chunks to the free list.
- Snapshot: under the lock, walk the committed FIFO in place and write it. The
  lock stays held for the whole dump, so a producer that needs a chunk waits.

The dumping thread first commits its own partial chunk, so a dump includes its
latest events.

## Threads

- A thread's partial chunk is committed when the thread exits, if it is still
  bound to an open trace. A thread-exit destructor does this, after checking the
  open-trace registry, so it never touches a closed trace. Only the last trace
  the thread bound is covered. An earlier trace it used is committed at that
  trace's close.
- A thread past `max_threads` drops its events and counts them. Writer slots
  are not reclaimed when a thread exits, so a trace binds at most `max_threads`
  threads over its life.

## Close

Close removes the trace from the registry, stops the drain thread, commits every
writer's partial chunk, then drains and frees. Threads must stop emitting before
close.

## Known limits

- A live thread that has not filled its chunk and has not emitted since is not
  in a dump. Its last records appear once it emits again, fills its chunk, or
  exits. Only the dumping thread's own tail is included. Closing the gap needs
  a per-event shared store or a stall in the dump, and neither was chosen.
- Continuous mode has no timer flush. A low-rate thread's last events are lost
  only if the process dies before its chunk fills, exits, or the trace closes.

## Stats

- `events` counts records emitted into chunks, per writer, including records
  that a snapshot later overwrites. It matches the old ring's meaning.
- `dropped` counts events lost to a full pool, and events from threads past
  `max_threads`.

## Removed

The `pf_ring` backends (mpsc and zcopy), their per-backend build and tests, and
the ring benchmarks. The `ringbuf/` directory stays in the repo as its own
project; pftrace no longer uses it.
