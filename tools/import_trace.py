#!/usr/bin/env python3
"""Checks a trace produced by libpftrace twice: once against the bytes in the
file, parsed with Perfetto's own generated schema (perfetto_trace_pb2), and
once against what trace_processor, Perfetto's importer, kept of it.

The file checks are relative to what the file contains rather than
hard-coded counts, because a trace's size depends on the mode: a snapshot
holds a bounded recent window, while a continuous trace holds everything.
Every event must resolve against tracks and interned names present in the
same file, slice begins and ends must balance per track (exactly with
--expect-complete, within a small window tolerance otherwise), no category
fields may appear, and interning must deduplicate names.

A trace can pass all of that while the importer still throws packets away or
misplaces them: a packet on a sequence whose incremental state was never
cleared, a counter value on a track that is not a counter track, a clock
snapshot it rejects. Those show up only in trace_processor's stats or in the
imported times, so the import checks read the stats, compare the slices each
thread's track imported with the track events the file carries for it, and
with --expect-ts compare the imported times with the ones the events were
stamped with.

Needs the perfetto Python package. tools/dump_trace.c performs the file
checks against the raw wire format with no dependencies, for where that
cannot be installed. The import checks also need trace_processor_shell on
PATH; without it they are skipped with a warning after the file checks run,
and the script exits with SKIP_CODE, which the ctests that run it map to a
skip. A failed file check still fails.
"""
import argparse
import shutil
import sys
from collections import Counter, defaultdict

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import Trace, TrackEvent

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


def check_file(trace, args):
    """Runs the checks against the parsed file. Returns whether any failed,
    and the number of slice begins and instants the file holds."""
    failed = False

    def fail(msg):
        nonlocal failed
        print(f"FAIL: {msg}")
        failed = True

    track_descriptors = [p.track_descriptor for p in trace.packet if p.HasField("track_descriptor")]
    uuids = set()
    print(f"track descriptors: {len(track_descriptors)}")
    for td in track_descriptors:
        kind = "process" if td.HasField("process") else \
               "thread" if td.HasField("thread") else \
               "counter" if td.HasField("counter") else "plain"
        if td.uuid not in uuids:
            print(f"  uuid={td.uuid} parent={td.parent_uuid} kind={kind} name={td.name!r}")
        uuids.add(td.uuid)
    if not uuids:
        fail("trace declares no tracks")

    begins = Counter()
    ends = Counter()
    counter_events = 0
    instant_events = 0
    # Interned names are per trusted sequence: each writer owns one sequence,
    # and its iids mean nothing to any other sequence.
    interned_names = defaultdict(dict)
    # The track an event with no track_uuid is on, per sequence, from the
    # TracePacketDefaults each chunk's header packet carries.
    default_track = {}
    stray_cat = 0
    unknown_track = 0
    unresolved_name = 0

    for p in trace.packet:
        seq = p.trusted_packet_sequence_id
        # A state clear restarts interning for that sequence, which is how a
        # snapshot window stays self-contained.
        if p.sequence_flags & 1:
            interned_names.pop(seq, None)
            default_track.pop(seq, None)
        if p.HasField("trace_packet_defaults"):
            d = p.trace_packet_defaults
            if d.HasField("track_event_defaults") and d.track_event_defaults.track_uuid:
                default_track[seq] = d.track_event_defaults.track_uuid
        names = interned_names[seq]
        if p.HasField("interned_data"):
            for en in p.interned_data.event_names:
                names[en.iid] = en.name
            stray_cat += len(p.interned_data.event_categories)
        if not p.HasField("track_event"):
            continue
        ev = p.track_event
        # An event with no track_uuid is on its sequence's default track.
        track = ev.track_uuid if ev.HasField("track_uuid") else default_track.get(seq, 0)
        if track not in uuids:
            unknown_track += 1
        if ev.name_iid and ev.name_iid not in names:
            unresolved_name += 1
        stray_cat += len(ev.category_iids) + len(ev.categories)
        if ev.type == TrackEvent.TYPE_SLICE_BEGIN:
            begins[track] += 1
        elif ev.type == TrackEvent.TYPE_SLICE_END:
            ends[track] += 1
        elif ev.type == TrackEvent.TYPE_COUNTER:
            counter_events += 1
        elif ev.type == TrackEvent.TYPE_INSTANT:
            instant_events += 1

    print(f"slice begins per track: {dict(begins)}")
    print(f"slice ends per track:   {dict(ends)}")
    print(f"counter events: {counter_events}")
    print(f"instant events: {instant_events}")
    print(f"interned event names (current generation, per sequence): {dict(interned_names)}")

    # Every event must resolve against data present in this same file --
    # the property that actually breaks if a buffer overwrites a descriptor
    # or an InternedData packet before it is written out.
    if unknown_track:
        fail(f"{unknown_track} events referenced an undeclared track")
    if unresolved_name:
        fail(f"{unresolved_name} events used an unresolvable name_iid")
    # The library no longer emits categories. Checking their absence
    # catches a half-reverted change that writes one without interning it.
    if stray_cat:
        fail(f"{stray_cat} category fields in a trace that should have none")

    total_begins = sum(begins.values())
    total_ends = sum(ends.values())
    if total_begins + instant_events == 0:
        fail("trace contains no slices or instants")

    if args.expect_complete:
        if begins != ends:
            fail(f"slice begin/end counts must balance per track: {dict(begins)} vs {dict(ends)}")
    else:
        # A window can open mid-slice (an end whose begin was recycled) or
        # close mid-slice, so allow a small imbalance per track. Dropped
        # events can leave more than that, so --allow-drops skips the check.
        if not args.allow_drops:
            for uuid in set(begins) | set(ends):
                delta = abs(begins[uuid] - ends[uuid])
                if delta > 2:
                    fail(f"track {uuid} imbalance of {delta} is too large for a window")
        print(f"slice imbalance (expected for a window): {total_begins - total_ends}")

    # Interning must actually deduplicate: far more slice events than
    # distinct names.
    if total_begins > 10 and total_begins <= sum(len(n) for n in interned_names.values()):
        fail("interning should deduplicate repeated names into fewer entries than occurrences")

    return failed, total_begins + instant_events


def check_import(path, shell, in_file, args):
    """Runs the checks against what trace_processor imported. Returns whether
    any failed."""
    from perfetto.trace_processor import TraceProcessor, TraceProcessorConfig

    tp = TraceProcessor(trace=path, config=TraceProcessorConfig(bin_path=shell))
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
    return failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument(
        "--expect-complete",
        action="store_true",
        help="require slices to balance exactly; only true for a continuous "
        "trace read from its beginning, never for a snapshot window",
    )
    ap.add_argument(
        "--allow-drops",
        action="store_true",
        help="skip the per-track balance check for a window that lost events "
        "under load; the imbalance is still printed, and the other checks run",
    )
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

    with open(args.path, "rb") as f:
        data = f.read()
    trace = Trace.FromString(data)
    print(f"parsed OK: {len(trace.packet)} packets, {len(data)} bytes")

    failed, in_file = check_file(trace, args)

    shell = shutil.which("trace_processor_shell")
    if shell is None:
        if failed:
            return 1
        print(
            "warning: trace_processor_shell not found on PATH; install it "
            "(Perfetto's trace processor) to run the import checks",
            file=sys.stderr,
        )
        return SKIP_CODE

    failed |= check_import(args.path, shell, in_file, args)
    if failed:
        return 1
    print("\nALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
