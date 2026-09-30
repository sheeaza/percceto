#!/usr/bin/env python3
"""Parses a trace produced by libpftrace with the *real* Perfetto protobuf
schema (perfetto_trace_pb2) and asserts structural correctness: this is
the strongest available check in this sandbox short of dropping the file
onto ui.perfetto.dev, since it validates our hand-rolled encoder's bytes
against Google's own generated protobuf runtime.
"""
import sys
from collections import Counter, defaultdict

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import Trace, TrackEvent


def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <trace-file>", file=sys.stderr)
        return 1

    path = sys.argv[1]
    with open(path, "rb") as f:
        data = f.read()

    trace = Trace.FromString(data)
    print(f"parsed OK: {len(trace.packet)} packets, {len(data)} bytes")

    track_descriptors = [p.track_descriptor for p in trace.packet if p.HasField("track_descriptor")]
    print(f"track descriptors: {len(track_descriptors)}")
    for td in track_descriptors:
        kind = "process" if td.HasField("process") else \
               "thread" if td.HasField("thread") else \
               "counter" if td.HasField("counter") else "plain"
        print(f"  uuid={td.uuid} parent={td.parent_uuid} kind={kind} name={td.name!r}")
    assert len(track_descriptors) == 4, "expected 4 tracks (process, 2 threads, 1 counter)"

    # slice begin/end balance, per track
    begins = Counter()
    ends = Counter()
    counter_events = 0
    event_names_seen = defaultdict(set)  # track -> set of resolved names, to sanity check interning
    interned_names = {}
    interned_cats = {}

    for p in trace.packet:
        if p.HasField("interned_data"):
            for en in p.interned_data.event_names:
                interned_names[en.iid] = en.name
            for ec in p.interned_data.event_categories:
                interned_cats[ec.iid] = ec.name
        if not p.HasField("track_event"):
            continue
        ev = p.track_event
        if ev.type == TrackEvent.TYPE_SLICE_BEGIN:
            begins[ev.track_uuid] += 1
            name = interned_names.get(ev.name_iid, ev.name) if (ev.name_iid or ev.name) else None
            event_names_seen[ev.track_uuid].add(name)
        elif ev.type == TrackEvent.TYPE_SLICE_END:
            ends[ev.track_uuid] += 1
        elif ev.type == TrackEvent.TYPE_COUNTER:
            counter_events += 1

    print(f"slice begins per track: {dict(begins)}")
    print(f"slice ends per track:   {dict(ends)}")
    print(f"counter events: {counter_events}")
    print(f"interned event names: {interned_names}")
    print(f"interned event categories: {interned_cats}")

    assert begins == ends, "slice begin/end counts must balance per track"
    assert counter_events == 6, "expected 6 counter samples (3 int + 3 double)"

    # interning dedup check: "compute"/"parse"/"render" repeat 3x each on the
    # main thread but must resolve to a single EventName interned entry each.
    assert len(interned_names) == 4, f"expected 4 distinct interned names (compute/parse/render/work_item), got {interned_names}"
    assert len(interned_cats) == 2, f"expected 2 distinct interned categories (app/worker), got {interned_cats}"

    total_slice_events = sum(begins.values())
    assert total_slice_events == 12, f"expected 12 slice-begin events (3x compute+parse+render + 3x work_item), got {total_slice_events}"
    assert total_slice_events > len(interned_names), "interning should deduplicate repeated names into fewer entries than occurrences"

    print("\nALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
