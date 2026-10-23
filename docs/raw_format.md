# pftrace raw dump format

The raw dump is the on-disk output of the drain thread when it writes ring
records without encoding them. An offline tool (the Python parser) reads it
and produces the Perfetto protobuf trace.

This document is the contract between the writer (`src/pftrace.c`) and the
reader (the offline Python parser).

## Design goals

- The drain thread does no protobuf work and no per-event string handling.
  It copies records as they are and appends small sidecar entries. A record is
  fixed size unless its name is copied into it; see "Variable-size records".
- The file is append-only. Nothing already written is ever rewritten, so
  continuous mode and snapshot mode use the same format.
- A crash or truncation loses at most the last chunk.

## Conventions

- All integers are little-endian.
- All multi-byte fields are naturally aligned in the layouts below.
- `u32`, `u64` are unsigned fixed-width integers.

## File layout

```
+---------------------------+
| file header (fixed)       |  written once per file: at open, or per dump
+---------------------------+
| chunk                     |
| chunk                     |  appended over time, in order
| ...                       |
+---------------------------+
```

There is no index and no trailer in version 4. The reader scans chunks from
the start.

### File header

| Offset | Size | Field        | Value                                   |
|--------|------|--------------|-----------------------------------------|
| 0      | 8    | magic        | ASCII `PFRAWv1\0`                       |
| 8      | 4    | version      | `5`                                     |
| 12     | 4    | record_size  | `32` (bytes in the fixed head of an event record; see EVENTS payload) |
| 16     | 4    | clock_id     | The trace clock as defined by the writer's `<time.h>`: `CLOCK_REALTIME`, `CLOCK_MONOTONIC` or `CLOCK_BOOTTIME` (`7` on Linux for boot time) |
| 20     | 4    | flags        | `0` in version 4, reserved              |
| 24     | 8    | clock_offset | `CLOCK_REALTIME` minus the trace clock, in ns, as a signed 64-bit value. Sampled back to back when the header is written, so `wall = ts + clock_offset` gives Unix time in ns. `0` when the trace clock is `CLOCK_REALTIME`. |
| 32     | 8    | boottime_offset | `CLOCK_BOOTTIME` minus the trace clock, in ns, as a signed 64-bit value. Sampled back to back with the other two. `0` when the trace clock is `CLOCK_BOOTTIME`. Perfetto needs this link to the boot clock to place a proto trace whose primary clock is REALTIME. |

Total: 40 bytes. The header never changes after it is written.

The reader rejects the file if `magic` does not match, `version` is not
recognized, or `record_size` is not `32`. Version 4 files, with 48-byte records
and argument words, are not read by version 5 readers.

## Chunks

Every chunk starts with an 8-byte chunk header, followed by `len` payload
bytes.

| Offset | Size | Field | Meaning                                  |
|--------|------|-------|------------------------------------------|
| 0      | 4    | kind  | chunk kind, see below                    |
| 4      | 4    | len   | payload length in bytes, excluding header |

Chunks are written with one `write()` each, on an fd opened with `O_APPEND`.

### Chunk kinds

| Value | Name   | Payload                                          |
|-------|--------|--------------------------------------------------|
| 1     | NAMES  | sequence of name entries                         |
| 2     | TRACKS | sequence of track entries                        |
| 3     | EVENTS | event records, back to back; each is 32 bytes plus its copied name, if any |

Any other `kind` value is an error in version 4.

#### NAMES payload

A sequence of entries, each:

| Size | Field | Meaning                                                 |
|------|-------|---------------------------------------------------------|
| 8    | ptr   | the `name` pointer value from the event record (the key); never `0` |
| 4    | len   | string length in bytes, excluding any terminator        |
| len  | bytes | the string bytes, not NUL-terminated                    |

Each entry is followed by no padding. The next entry starts immediately.

The key is the pointer value as it appeared in the traced process. Pointers
are only meaningful within one run, which is fine because the sidecar is
written in the same run as the events that reference it.

#### TRACKS payload

A sequence of fixed-size entries, one per track descriptor:

| Size | Field       | Meaning                                    |
|------|-------------|--------------------------------------------|
| 8    | uuid        | track_uuid                                 |
| 8    | parent_uuid | parent track uuid, `0` if none             |
| 4    | kind        | `0` process, `1` thread, `2` counter       |
| 4    | pid         | signed                                     |
| 4    | tid         | signed                                     |
| 4    | unit        | `pf_unit`: `0` unspecified, `1` time ns, `2` count, `3` size bytes |
| 4    | has_name    | `1` if `name` is valid, else `0`           |
| 4    | name_len    | length of the name, `0` to `64`            |
| 64   | name        | name bytes, zero-padded                    |

Total: 104 bytes per entry. The payload length is therefore a multiple of 104.

The `name` field is capped at `PF_TRACK_NAME_MAX` (64) bytes.

#### EVENTS payload

Each record starts with the 32-byte head of `struct pf_rec` from
`src/pftrace_internal.h`, copied as is:

| Offset | Size | Field        | Meaning                                       |
|--------|------|--------------|-----------------------------------------------|
| 0      | 8    | ts           | timestamp, ns, on `clock_id` from the header  |
| 8      | 8    | track        | track_uuid                                    |
| 16     | 8    | u            | `name` pointer; `value` if `RF_HAS_VALUE`; name length in bytes if `RF_NAME_COPIED` |
| 24     | 4    | type         | `TYPE_*`                                      |
| 28     | 4    | flags        | `RF_*` bits                                   |

`type` is one of `1` slice begin, `2` slice end, `3` instant, `4` counter.

`RF_HAS_VALUE` (`0x1`) selects `value` in `u`. `RF_VALUE_IS_DOUBLE` (`0x2`)
means `value` holds an IEEE-754 double. `RF_NAME_COPIED` (`0x4`) is described
below. Other bits are reserved and zero.

The reader decodes `name` as a pointer key into the NAMES definitions seen
so far, unless `RF_NAME_COPIED` is set. A value of `0` means the event has no
name. The reader decodes `value` as a raw 64-bit value.

Version 5 has no argument words. Event arguments are formatted by the caller
before the event is emitted, so the name the reader sees is already final.

#### Copied names

When `RF_NAME_COPIED` is set, `u` holds the length `L` of the name, and the
name's bytes follow the head. They are not NUL-terminated. Zero bytes pad the
record to the next 8-byte boundary. The record is then `32 + pad8(L)` bytes
long, and the next record starts after the padding. `RF_HAS_VALUE` and
`RF_NAME_COPIED` are never both set.

A name is copied only when it is passed in as a dynamic string, not a static
literal. Static names keep the pointer form, so the NAMES sidecar still holds
them and their records stay 32 bytes.

## Variable-size records

Version 5 records are no longer a fixed size, which version 4 readers and the
header's `record_size` assumed. The trade-offs:

- Reading. A record's offset is known only after the records before it are
  walked. The reader cannot seek to record `n`. The drain and the snapshot walk
  already walk from the start of a chunk, so only the size arithmetic changes.
- Chunk fill. A record that does not fit the rest of its chunk moves to the
  next chunk, as protobuf entries already do. The unused tail of a chunk grows
  with the length of the names in it.
- Hot path. A copied name costs a `memcpy` of its bytes into the chunk on the
  producer. Static-name records cost the same as before.
- Size limit. A copied name is capped at `PF_DYN_NAME_MAX`, 128 bytes. A longer
  name is cut to 125 bytes and the last three bytes are `...`, so a reader can
  see the cut. The truncation is counted in the stats.
- Chunk capacity. `chunk_slots`, a record count, no longer bounds a chunk. The
  chunk is bounded by its payload size in bytes.
- Format version. The change bumps `version` to 5 and `record_size` to 32. A
  version 4 reader rejects a version 5 file, so the change is not silent.

## Ordering rules

1. A NAMES entry for a pointer is written before the first EVENTS chunk
   that references that pointer.
2. A TRACKS entry for a track is written before the first EVENTS chunk that
   references that track.
3. Both are satisfied by the drain thread writing any pending NAMES and
   TRACKS chunks before it writes the EVENTS chunk, under `out_lock`.

The reader may assume these rules hold. If an event references an unknown
name pointer or track uuid, the reader reports an error.

## Definitions

Names are immutable. A track's name may change, and nothing else about a
track may.

- For a name pointer, the first NAMES entry wins.
- For a track uuid, the first TRACKS entry defines the track. A later TRACKS
  entry with the same uuid renames it: the name and `has_name` are replaced
  by the new entry's values, in file order. Its `kind`, `parent_uuid`, `pid`,
  `tid` and `unit` must equal the first entry's. A later entry that differs
  in any of those is reported as an error.

A rename is written as a TRACKS entry, so it is covered by the same chunk
rules as the first definition.

## Torn tails

The reader stops at the first chunk that is incomplete:

- fewer than 8 bytes remain for a chunk header, or
- `len` bytes are not all present, or
- for EVENTS, a record's head or its copied name runs past the end of `len`, or
- for TRACKS, `len` is not a multiple of 104.

Everything before that point is valid. The reader reports the number of
bytes it ignored.

## Writer selection

The writer is chosen per trace by `pf_config.format`:

- `PF_FORMAT_PROTOBUF` (default): Perfetto protobuf, written by the drain.
- `PF_FORMAT_RAW`: this format.

A continuous raw file must be new or empty. `pf_trace_open` returns `EEXIST`
for a non-empty file, so a raw file is never mixed with another format. A
snapshot dump writes a new file each time, so it always starts empty.

## Snapshot mode

A snapshot dump writes the same format into its own file: the file header,
then the NAMES and TRACKS chunks needed by the snapshot, then one EVENTS chunk.
Interning starts empty in every file, so each file stands alone and needs no
special case in the reader.

## Open items

- Whether to add an optional index chunk at close, for faster random access.
  Not part of version 1.
- Whether to emit a chunk for dropped-event counts, so the reader can report
  drops. Not part of version 1.
