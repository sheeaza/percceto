#!/usr/bin/env python3
"""Parses a trace produced by libpftrace with the *real* Perfetto protobuf
schema (perfetto_trace_pb2) and asserts structural correctness: this is
the strongest available check short of dropping the file onto
ui.perfetto.dev, since it validates our hand-rolled encoder's bytes
against Google's own generated protobuf runtime.

Requires the `perfetto` Python package. Where that cannot be installed,
tools/dump_trace.c performs the same structural checks against the raw
wire format with no dependencies; the verify_trace tests in tools/CMakeLists.txt run
this one only when the module is importable.

Checks are relative to what the file contains rather than hard-coded
counts, because a buffer-buffered trace's size depends on the mode: a
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
    ap.add_argument(
        "--allow-drops",
        action="store_true",
        help="skip the per-track balance check for a window that lost events "
        "under load; the imbalance is still printed, and the other checks run",
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
            name = names.get(ev.name_iid, ev.name) if (ev.name_iid or ev.name) else None
            event_names_seen[track].add(name)
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
    assert unknown_track == 0, f"{unknown_track} events referenced an undeclared track"
    assert unresolved_name == 0, f"{unresolved_name} events used an unresolvable name_iid"
    # The library no longer emits categories. Asserting their absence
    # catches a half-reverted change that writes one without interning it.
    assert stray_cat == 0, f"{stray_cat} category fields in a trace that should have none"

    total_begins = sum(begins.values())
    total_ends = sum(ends.values())
    assert total_begins > 0, "trace contains no slices"

    if args.expect_complete:
        assert begins == ends, f"slice begin/end counts must balance per track: {dict(begins)} vs {dict(ends)}"
    else:
        # A window can open mid-slice (an end whose begin was recycled) or
        # close mid-slice, so allow a small imbalance per track. Dropped
        # events can leave more than that, so --allow-drops skips the check.
        if not args.allow_drops:
            for uuid in set(begins) | set(ends):
                delta = abs(begins[uuid] - ends[uuid])
                assert delta <= 2, f"track {uuid} imbalance of {delta} is too large for a window"
        print(f"slice imbalance (expected for a window): {total_begins - total_ends}")

    # Interning must actually deduplicate: far more slice events than
    # distinct names.
    if total_begins > 10:
        assert total_begins > sum(len(names) for names in interned_names.values()), \
            "interning should deduplicate repeated names into fewer entries than occurrences"

    print("\nALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
