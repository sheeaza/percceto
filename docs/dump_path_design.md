# Dump file path and name

Status: implemented. Notes on the implementation are at the end.

## Problem

Today `pf_config.path` is a single full file path. `pf_trace_open()` opens it
once with `O_WRONLY | O_CREAT | O_APPEND` and keeps the fd for the life of the
trace. Every `pf_trace_dump()` in snapshot mode appends to that same file.

Snapshot dumps can happen at any point in the program, from any thread, any
number of times. Appending them all to one file makes it hard to tell dumps
apart, and a raw-format snapshot file cannot hold a second header at all.

## Design

Split the output location into a directory (`path`) and a file name (`name`).
The name is bound at a different point depending on mode.

| | Continuous | Snapshot |
|---|---|---|
| `path` (directory) | given at open, fixed | given at open, fixed |
| `name` (file name) | given at open, fixed | given at each dump, required |
| Output files | one file, for the life of the trace | one new file per dump |
| File opened | at open, held, `O_APPEND` | at each dump, `O_TRUNC` |
| Existing file | extended (`O_APPEND`) | overwritten |

### Continuous mode

- `path` and `name` are read at `pf_trace_open()`. The resulting file is
  `path/name`.
- The file is opened once, at open, and held until close. Its name does not
  change across program exit or across any number of drains.
- `O_APPEND` stays, so a continuous trace can still be read while it is written.
- A name passed to `pf_trace_dump()` in this mode is rejected with `EINVAL`.
  Dumps in continuous mode only drain into the open file.

### Snapshot mode

- `path` is read at `pf_trace_open()`. No file is created at open. The output
  file is created by the first dump, so an unused snapshot trace leaves nothing
  on disk.
- `name` is not in `pf_config`. It is passed to each dump:

  ```c
  PF_API int pf_trace_dump(struct pf_trace *t, const char *name);
  PF_API int pf_trace_flush(struct pf_trace *t, const char *name);
  ```

  `name` must be non-NULL. A NULL name returns `EINVAL`.

- The dump does not write to `path/name` as given. It writes to a unique file:

  ```
  path/<name>-<UTC timestamp, ms>-<seq>.pftrace
  e.g.  traces/frame_stall-20261018T143007.512Z-0003.pftrace
  ```

  - **Timestamp:** `CLOCK_REALTIME`, UTC, to the millisecond, in ISO-8601
    basic form. It separates dumps across runs and across time.
  - **Sequence:** a per-trace `uint32` in `struct pf_trace`. It starts at 0 at
    open and increments once per snapshot dump. It is only touched under
    `out_lock`, so no atomics. It separates two dumps from the same trace that
    land in the same millisecond. It is not persisted, so it does not need to
    survive a restart.

- Overwrite: if the generated file already exists, it is overwritten (`O_TRUNC`).
  With the timestamp and sequence this only happens when two processes dump the
  same name in the same millisecond. It is not expected in practice.
- `pf_trace_close()` in snapshot mode does not dump. The caller dumps explicitly
  before closing. This is a behavior change from today, where close writes a
  final dump.

### Format interaction

- **Protobuf:** each snapshot file is self-contained already. The dump re-emits
  tracks at the head, and interning restarts from scratch per dump (see
  `src/pf_pb_drain.c`). Nothing changes beyond the file handling.
- **Raw:** each snapshot file gets its own header. `raw_write_header()` runs
  once per file, not once per trace. The "file must be empty" check (`EEXIST`)
  now applies only in continuous mode. Snapshot mode overwrites instead.

## Config struct

`struct pf_config` changes from `const char *path` to two fields:

```c
const char *path;   /* directory the output goes in */
const char *name;   /* continuous: the file name. snapshot: unused, must be NULL */
```

Open validation:

- `path` must be non-NULL and name an existing, writable directory. Checked at
  open so a bad path fails at open, not on the first dump.
- Continuous mode: `name` must be non-NULL.
- Snapshot mode: `name` must be NULL, so a caller cannot pass one name at open
  and a different one at dump.
- Names are a single path component. A `/` in a name is `EINVAL`, both at open
  and at dump.

`pf_config_init()` still sets everything to defaults and leaves `path` and
`name` NULL.

## Implementation sketch

Files affected:

- `include/pftrace.h`: `pf_config` fields, `pf_trace_dump` and `pf_trace_flush`
  signatures and docs, close docs.
- `src/pftrace.c`: open validation; open-time `open()` only in continuous mode;
  per-dump open in `pf_trace_dump` and `pf_trace_flush`; file name generation;
  close no longer dumps in snapshot mode.
- `src/pftrace_internal.h`: `fd` becomes per-dump in snapshot mode; `dump_seq`
  counter.
- `src/pf_raw.c`: header written per file; `EEXIST` check gated on
  continuous mode.
- `docs/raw_format.md`: the header is written once per file, not once per trace.
- `examples/example_basic.c`, `examples/example_threads.c`: `path` is a
  directory; snapshot passes a name to dump.
- `tests/test_raw_roundtrip.c`: `refuses_existing` is continuous-only now; add
  a snapshot case that dumps twice and checks two distinct files.
- `tests/test_noalloc.c`: dump with a generated name must not allocate. Name
  generation uses stack buffers.
- `tools/dump_trace.c`, `tools/import_trace.py`: no format change, but any
  scripts that expect a single snapshot file need updating.

## Open questions

None outstanding.

## Implementation notes

- `example_basic` takes the name from `pf_trace_dump` and renames the dump to
  `demo_snapshot.pftrace`, so the `demo_snapshot` checks in
  `tools/CMakeLists.txt` still run.
- Tests that read a snapshot take the name from `pf_trace_dump`, join it to
  the per-test directory, and move the file to the fixed name the CMake consumers use.
- `pf_trace_dump` and `pf_trace_flush` return the name they wrote through a
  caller buffer, `file` and `size`, without the directory. A too-small buffer
  fails with `ENAMETOOLONG` before anything is written.
- The raw header check still runs on every file. Each snapshot file is opened
  with `O_TRUNC`, so the check always sees an empty file.
