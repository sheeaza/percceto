"""Unit tests for pfraw2pb.

Run from the repo root, inside the cshell environment (it provides perfetto):
    python3 -m unittest discover -s tools -p 'test_pfraw2pb.py'

The raw files are built here from docs/raw_format.md. The output is decoded
with the generated Perfetto classes, the same ones the converter writes with.
"""
import io
import struct
import unittest

import pfraw2pb as pb
from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import Trace, TrackEvent

NAME_PTR = 0x1000
OTHER_PTR = 0x2000
UNIT_COUNT = 2  # PF_UNIT_COUNT in include/pftrace.h
SEQ_CLEARED = 1  # SEQ_INCREMENTAL_STATE_CLEARED in include/pftrace.h
# Event type numbers as written to the raw file (docs/raw_format.md).
RAW_SLICE_BEGIN, RAW_SLICE_END, RAW_INSTANT, RAW_COUNTER = 1, 2, 3, 4


def raw_file(*chunks):
    header = struct.pack("<8sIIIIQ", b"PFRAWv1\0", 1, 32, 7, 0, 0)
    body = b"".join(struct.pack("<II", kind, len(payload)) + payload
                    for kind, payload in chunks)
    return header + body


def name_entry(ptr, name):
    raw = name.encode()
    return struct.pack("<QI", ptr, len(raw)) + raw


def track_entry(uuid, parent, kind, pid, tid, unit, name):
    raw = name.encode()
    return struct.pack("<QQIiiIII64s", uuid, parent, kind, pid, tid, unit,
                       1 if raw else 0, len(raw), raw.ljust(64, b"\0"))


def event(ts, track, u, etype, flags=0):
    return struct.pack("<QQQII", ts, track, u, etype, flags)


def convert_bytes(data):
    """Runs the converter over an in-memory raw file. Returns (packets,
    stats, torn), where packets are generated TracePacket messages."""
    out = io.BytesIO()
    reader = pb.RawReader(io.BytesIO(data))
    conv = pb.Converter(out)
    for kind, payload, offset in reader.chunks():
        if kind == pb.CHUNK_NAMES:
            conv.names_chunk(payload)
        elif kind == pb.CHUNK_TRACKS:
            conv.tracks_chunk(payload)
        elif kind == pb.CHUNK_EVENTS:
            conv.events_chunk(payload)
    return Trace.FromString(out.getvalue()).packet, conv.stats, reader.torn_bytes


class ConvertTest(unittest.TestCase):
    def thread_tracks(self):
        return pb.CHUNK_TRACKS, track_entry(1, 0, pb.TRACK_THREAD, 42, 7, 0,
                                            "main")

    def test_slices_and_instant_keep_order_and_names(self):
        names = (pb.CHUNK_NAMES, name_entry(NAME_PTR, "compute"))
        events = (pb.CHUNK_EVENTS,
                  event(100, 1, NAME_PTR, RAW_SLICE_BEGIN)
                  + event(110, 1, NAME_PTR, RAW_INSTANT)
                  + event(120, 1, 0, RAW_SLICE_END))
        packets, stats, torn = convert_bytes(
            raw_file(self.thread_tracks(), names, events))

        self.assertEqual(torn, 0)
        self.assertEqual(stats["events"], 3)
        # Track descriptor first, then the three events.
        self.assertEqual(len(packets), 4)

        desc = packets[0]
        self.assertEqual(desc.sequence_flags, SEQ_CLEARED,
                         "first packet clears incremental state")
        self.assertEqual(desc.track_descriptor.uuid, 1)
        self.assertEqual(desc.track_descriptor.thread.tid, 7)
        self.assertEqual(desc.track_descriptor.thread.thread_name, "main")

        begin = packets[1]
        self.assertEqual(begin.timestamp, 100)
        entry = begin.interned_data.event_names[0]
        self.assertEqual((entry.iid, entry.name), (1, "compute"))
        self.assertEqual(begin.track_event.type, TrackEvent.TYPE_SLICE_BEGIN)
        self.assertEqual(begin.track_event.name_iid, 1)

        # The name is defined once: the instant reuses iid 1 with no
        # interned data of its own.
        instant = packets[2]
        self.assertEqual(len(instant.interned_data.event_names), 0)
        self.assertEqual(instant.track_event.name_iid, 1)

        end = packets[3].track_event
        self.assertEqual(end.type, TrackEvent.TYPE_SLICE_END)
        self.assertFalse(end.HasField("name_iid"))

    def test_counter_int_and_double(self):
        tracks = (pb.CHUNK_TRACKS, track_entry(5, 0, pb.TRACK_COUNTER, 0, 0,
                                               UNIT_COUNT, "depth"))
        bits = struct.unpack("<Q", struct.pack("<d", 2.5))[0]
        events = (pb.CHUNK_EVENTS,
                  event(1, 5, 7, RAW_COUNTER, pb.RF_HAS_VALUE)
                  + event(2, 5, bits, RAW_COUNTER,
                          pb.RF_HAS_VALUE | pb.RF_VALUE_IS_DOUBLE))
        packets, _, _ = convert_bytes(raw_file(tracks, events))

        self.assertEqual(packets[1].track_event.counter_value, 7)
        self.assertEqual(packets[2].track_event.double_counter_value, 2.5)

        counter = packets[0].track_descriptor
        self.assertEqual(counter.name, "depth")
        self.assertEqual(counter.counter.unit, UNIT_COUNT)

    def test_negative_counter_value_is_signed(self):
        tracks = (pb.CHUNK_TRACKS, track_entry(5, 0, pb.TRACK_COUNTER, 0, 0,
                                               UNIT_COUNT, "depth"))
        minus_three = (1 << 64) - 3  # two's-complement bits, as the ring stores
        events = (pb.CHUNK_EVENTS,
                  event(1, 5, minus_three, RAW_COUNTER, pb.RF_HAS_VALUE))
        packets, _, _ = convert_bytes(raw_file(tracks, events))
        self.assertEqual(packets[1].track_event.counter_value, -3)

    def test_torn_tail_is_reported_and_prior_events_kept(self):
        names = (pb.CHUNK_NAMES, name_entry(NAME_PTR, "compute"))
        events = (pb.CHUNK_EVENTS,
                  event(1, 1, NAME_PTR, RAW_SLICE_BEGIN)
                  + event(2, 1, 0, RAW_SLICE_END))
        data = raw_file(self.thread_tracks(), names, events)
        # The events chunk is 8 + 64 bytes; 5 are cut, so 67 remain of it.
        packets, stats, torn = convert_bytes(data[:-5])
        self.assertEqual(torn, 67)
        self.assertEqual(stats["events"], 0)
        self.assertEqual(len(packets), 1)  # descriptor only; events lost

    def test_event_on_undefined_track_is_rejected(self):
        data = raw_file((pb.CHUNK_EVENTS,
                         event(1, 99, 0, RAW_SLICE_END)))
        with self.assertRaisesRegex(pb.RawFormatError, "undefined track"):
            convert_bytes(data)

    def test_event_on_undefined_name_is_rejected(self):
        data = raw_file(self.thread_tracks(),
                        (pb.CHUNK_EVENTS,
                         event(1, 1, OTHER_PTR, RAW_SLICE_BEGIN)))
        with self.assertRaisesRegex(pb.RawFormatError, "undefined name"):
            convert_bytes(data)

    def test_conflicting_name_definition_is_rejected(self):
        data = raw_file(
            (pb.CHUNK_NAMES, name_entry(NAME_PTR, "a")),
            (pb.CHUNK_NAMES, name_entry(NAME_PTR, "b")))
        with self.assertRaisesRegex(pb.RawFormatError, "conflicting"):
            convert_bytes(data)

    def test_identical_repeat_definition_is_accepted(self):
        data = raw_file(
            self.thread_tracks(),
            (pb.CHUNK_NAMES, name_entry(NAME_PTR, "a")),
            (pb.CHUNK_NAMES, name_entry(NAME_PTR, "a")),
            (pb.CHUNK_EVENTS, event(1, 1, NAME_PTR, RAW_SLICE_BEGIN)))
        _, stats, _ = convert_bytes(data)
        self.assertEqual(stats["names"], 1)

    def test_bad_magic_is_rejected(self):
        data = b"NOTRAW00" + raw_file()[8:]
        with self.assertRaisesRegex(pb.RawFormatError, "bad magic"):
            pb.RawReader(io.BytesIO(data))


if __name__ == "__main__":
    unittest.main()
