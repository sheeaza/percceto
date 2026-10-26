# TODO

Performance and behaviour work on the chunk buffers. See
`docs/chunk_buffers_design.md` for the design and its known limits.

## 18. Fast path for the common protobuf event

A static name already defined in this chunk, no args, no value. Its size is
fixed except for the `ts`, `track` and `iid` varints, so a dedicated encoder
can skip `pf_pb_size` and the `struct pb_event` setup. Write varints a word at
a time rather than a byte at a time.

## 21. Drain writes chunks without a copy

`pb_drain_locked` copies every chunk into `t->out` before `write()`. A
`writev` over the chunk list removes the copy. The drain's tail is already 1 to
3 ns/event, so this mainly saves memory bandwidth. Low priority.

## 23. Counter values on a track that is not a counter track

`pf_counter_set_int` and `pf_counter_set_double` take any track uuid. Perfetto
accepts a counter value only on a counter track, and drops the rest with
`track_event_counter_invalid_track_uuid`. `tests/test_stress.c` set counters
on thread tracks, and every one of those values was lost on import (found by
the `import_*` tests).

Options: reject the call, or drop the event and count it. Either needs the
track's kind on the hot path, which today reads only the uuid. The track
table is behind `track_lock`, so the check would need a lock-free lookup, for
example a kind bit encoded in the uuid. Not decided.

## Notes

- Continuous mode has no timer flush. A thread's partial chunk is written when
  it unbinds (the thread exits, or closes the trace), or when it fills. Events
  still in a live thread's partial chunk are lost only if the process dies first.
- Encoding stays on the drain thread.
