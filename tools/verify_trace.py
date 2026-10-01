#!/usr/bin/env python3
"""Parses a trace produced by libpftrace with the *real* Perfetto protobuf
schema (perfetto_trace_pb2) and asserts structural correctness: this is
the strongest available check short of dropping the file onto
ui.perfetto.dev, since it validates our hand-rolled encoder's bytes
against Google's own generated protobuf runtime.

Requires the `perfetto` Python package. Where that cannot be installed,
tools/dump_trace.c performs the same structural checks against the raw
wire format with no dependencies; tools/run_checks.sh runs this one only
when the module is importable.

Checks are relative to what the file contains rather than hard-coded
counts, because a ring-buffered trace's size depends on the mode: a
snapshot holds a bounded recent window, while a continuous trace holds
everything. Pass --expect-complete for a trace that should balance
exactly (continuous mode, read from the start).
"""
import argparse
import sys
from collections import Counter, defaultdict

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import Trace, TrackEvent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument(
        "--expect-complete",
        action="store_true",
        help="require slices to balance exactly; only true for a continuous "
        "trace read from its beginning, never for a snapshot window",
    )
    args = ap.parse_args()

    with open(args.path, "rb") as f:
        data = f.read()

    trace = Trace.FromString(data)
    print(f"parsed OK: {len(trace.packet)} packets, {len(data)} bytes")

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
    assert uuids, "trace declares no tracks"

    begins = Counter()
    ends = Counter()
    counter_events = 0
    instant_events = 0
    event_names_seen = defaultdict(set)
    interned_names = {}
    interned_cats = {}
    unknown_track = 0
    unresolved_name = 0

    for p in trace.packet:
        # A state clear restarts interning for the sequence, which is how a
        # snapshot window stays self-contained.
        if p.sequence_flags & 1:
            interned_names.clear()
            interned_cats.clear()
        if p.HasField("interned_data"):
            for en in p.interned_data.event_names:
                interned_names[en.iid] = en.name
            for ec in p.interned_data.event_categories:
                interned_cats[ec.iid] = ec.name
        if not p.HasField("track_event"):
            continue
        ev = p.track_event
        if ev.track_uuid not in uuids:
            unknown_track += 1
        if ev.name_iid and ev.name_iid not in interned_names:
            unresolved_name += 1
        if ev.type == TrackEvent.TYPE_SLICE_BEGIN:
            begins[ev.track_uuid] += 1
            name = interned_names.get(ev.name_iid, ev.name) if (ev.name_iid or ev.name) else None
            event_names_seen[ev.track_uuid].add(name)
        elif ev.type == TrackEvent.TYPE_SLICE_END:
            ends[ev.track_uuid] += 1
        elif ev.type == TrackEvent.TYPE_COUNTER:
            counter_events += 1
        elif ev.type == TrackEvent.TYPE_INSTANT:
            instant_events += 1

    print(f"slice begins per track: {dict(begins)}")
    print(f"slice ends per track:   {dict(ends)}")
    print(f"counter events: {counter_events}")
    print(f"instant events: {instant_events}")
    print(f"interned event names (current generation): {interned_names}")
    print(f"interned event categories: {interned_cats}")

    # Every event must resolve against data present in this same file --
    # the property that actually breaks if a ring overwrites a descriptor
    # or an InternedData packet before it is written out.
    assert unknown_track == 0, f"{unknown_track} events referenced an undeclared track"
    assert unresolved_name == 0, f"{unresolved_name} events used an unresolvable name_iid"

    total_begins = sum(begins.values())
    total_ends = sum(ends.values())
    assert total_begins > 0, "trace contains no slices"

    if args.expect_complete:
        assert begins == ends, f"slice begin/end counts must balance per track: {dict(begins)} vs {dict(ends)}"
    else:
        # A window can open mid-slice (an end whose begin was recycled) or
        # close mid-slice, so allow a small imbalance per track.
        for uuid in set(begins) | set(ends):
            delta = abs(begins[uuid] - ends[uuid])
            assert delta <= 2, f"track {uuid} imbalance of {delta} is too large for a window"
        print(f"slice imbalance (expected for a window): {total_begins - total_ends}")

    # Interning must actually deduplicate: far more slice events than
    # distinct names.
    if total_begins > 10:
        assert total_begins > len(interned_names), \
            "interning should deduplicate repeated names into fewer entries than occurrences"

    print("\nALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
