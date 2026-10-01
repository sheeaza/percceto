#!/usr/bin/env python3
"""Validates one trace with a protoc-generated protobuf runtime.

Driven by tools/check_schema.sh, which generates perfetto_min_pb2 from
tools/perfetto_min.proto and passes the trace path in $TRACE. The point is
that nothing here shares code with src/ -- the bytes are decoded by Google's
generated runtime, so a bug in our encoder cannot be masked by a matching
bug in our own decoder (tools/dump_trace.c has that weakness by nature).
"""
import os
import sys

from perfetto_min_pb2 import Trace, TrackEvent


def main():
    path = os.environ["TRACE"]
    with open(path, "rb") as f:
        data = f.read()

    trace = Trace.FromString(data)
    print(f"  parsed {len(trace.packet)} packets / {len(data)} bytes")

    # Round-trip: re-encoding and re-parsing must preserve the packet count,
    # which fails if our framing is subtly wrong in a way the first parse
    # tolerated.
    again = Trace.FromString(trace.SerializeToString())
    assert len(again.packet) == len(trace.packet), (
        f"round-trip changed packet count: {len(trace.packet)} -> {len(again.packet)}"
    )

    uuids = set()
    names = {}
    cats = {}
    begins = ends = instants = counters = 0
    unknown_track = unresolved_name = unresolved_cat = 0

    for p in trace.packet:
        # SEQ_INCREMENTAL_STATE_CLEARED restarts interning, which is what
        # makes each snapshot window independently readable.
        if p.sequence_flags & 1:
            names.clear()
            cats.clear()
        if p.HasField("track_descriptor"):
            uuids.add(p.track_descriptor.uuid)
        if p.HasField("interned_data"):
            for e in p.interned_data.event_names:
                names[e.iid] = e.name
            for e in p.interned_data.event_categories:
                cats[e.iid] = e.name
        if not p.HasField("track_event"):
            continue

        ev = p.track_event
        if ev.track_uuid not in uuids:
            unknown_track += 1
        if ev.name_iid and ev.name_iid not in names:
            unresolved_name += 1
        for iid in ev.category_iids:
            if iid not in cats:
                unresolved_cat += 1
        if ev.type == TrackEvent.TYPE_SLICE_BEGIN:
            begins += 1
        elif ev.type == TrackEvent.TYPE_SLICE_END:
            ends += 1
        elif ev.type == TrackEvent.TYPE_INSTANT:
            instants += 1
        elif ev.type == TrackEvent.TYPE_COUNTER:
            counters += 1

    print(f"  tracks={len(uuids)} begin={begins} end={ends} "
          f"instant={instants} counter={counters}")

    # The properties a ring buffer can actually break: a descriptor or an
    # InternedData packet overwritten before it reached the file leaves
    # events in it dangling.
    assert unknown_track == 0, f"{unknown_track} events on undeclared tracks"
    assert unresolved_name == 0, f"{unresolved_name} unresolvable name_iids"
    assert unresolved_cat == 0, f"{unresolved_cat} unresolvable category iids"
    assert uuids, "no tracks declared"
    assert begins > 0, "no slices in trace"

    print("  OK: every event resolves against this file alone")
    return 0


if __name__ == "__main__":
    sys.exit(main())
