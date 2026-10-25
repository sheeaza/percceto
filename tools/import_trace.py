#!/usr/bin/env python3
"""Imports a trace with trace_processor, Perfetto's own importer, and checks
that it kept what the file holds.

verify_trace.py checks the bytes against the schema, which a trace can pass
while the importer still throws packets away or misplaces them: a packet on a
sequence whose incremental state was never cleared, a counter value on a track
that is not a counter track, a name iid with no definition, a clock snapshot
it rejects. Those show up only in trace_processor's stats or in the imported
times, so this check reads the stats, compares the slices each thread's track
imported with the track events the file carries for it, and with --expect-ts
compares the imported times with the ones the events were stamped with.

Needs the perfetto Python package and trace_processor_shell on PATH. When the
shell is missing the check is skipped with a warning, and exits with
SKIP_CODE, which the ctest that runs it maps to a skip.
"""
import argparse
import shutil
import sys

SKIP_CODE = 77

# Importer stats that mean events from the file were lost or misread.
LOSS_STATS = (
    "packet_skipped_seq_needs_incremental_state_invalid",
    "interned_data_skipped_incremental_state_invalid",
    "track_event_counter_invalid_track_uuid",
    "track_event_parser_errors",
    "track_event_tokenizer_errors",
    "interned_data_tokenizer_errors",
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument(
        "--min-threads",
        type=int,
        default=1,
        help="fail unless at least this many thread tracks imported slices",
    )
    ap.add_argument(
        "--expect-ts",
        metavar="FILE",
        help="fail unless the imported slice timestamps are exactly those in "
        "FILE, one per line, in any order",
    )
    args = ap.parse_args()

    shell = shutil.which("trace_processor_shell")
    if shell is None:
        print(
            "warning: trace_processor_shell not found on PATH; install it "
            "(Perfetto's trace processor) to run this check",
            file=sys.stderr,
        )
        return SKIP_CODE

    from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import Trace, TrackEvent
    from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig

    with open(args.path, "rb") as f:
        trace = Trace.FromString(f.read())
    slice_types = (TrackEvent.TYPE_SLICE_BEGIN, TrackEvent.TYPE_INSTANT)
    in_file = sum(
        1 for p in trace.packet if p.HasField("track_event") and p.track_event.type in slice_types
    )

    tp = TraceProcessor(trace=args.path, config=TraceProcessorConfig(bin_path=shell))
    failed = False

    for r in tp.query(
        "select name, value from stats where value > 0 and "
        "(severity in ('error', 'data_loss') or name in (%s)) order by name"
        % ", ".join("'%s'" % s for s in LOSS_STATS)
    ):
        print(f"FAIL: importer stat {r.name} = {r.value}")
        failed = True

    imported = list(tp.query("select count(*) n from slice"))[0].n
    print(f"slices: {imported} imported, {in_file} begins and instants in the file")
    if imported != in_file:
        print(f"FAIL: importer kept {imported} of {in_file} slices")
        failed = True

    threads = list(
        tp.query(
            "select th.name, count(*) n from slice s "
            "join thread_track tt on s.track_id = tt.id join thread th using (utid) "
            "group by tt.utid order by th.name"
        )
    )
    for r in threads:
        print(f"  thread {r.name}: {r.n} slices")
    if len(threads) < args.min_threads:
        print(f"FAIL: {len(threads)} threads imported slices, expected at least {args.min_threads}")
        failed = True

    if args.expect_ts:
        with open(args.expect_ts) as f:
            want = sorted(int(line) for line in f if line.strip())
        got = sorted(r.ts for r in tp.query("select ts from slice"))
        wrong = sum(1 for a, b in zip(got, want) if a != b) + abs(len(got) - len(want))
        print(f"timestamps: {len(got)} imported, {len(want)} expected, {wrong} differ")
        if wrong:
            print("FAIL: imported slice timestamps differ from the ones the events were stamped with")
            failed = True

    tp.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
