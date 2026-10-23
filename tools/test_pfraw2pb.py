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


def raw_file(*chunks, clock=7, offset=0, boottime=0):
    header = struct.pack("<8sIIIIqq", b"PFRAWv1\0", 6, 32, clock, 0, offset,
                         boottime)
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


def event_copied(ts, track, name, etype):
    """A record whose name is copied after the head, padded to 8 bytes."""
    head = struct.pack("<QQQII", ts, track, len(name), etype, pb.RF_NAME_COPIED)
    return head + name + b"\0" * (-len(name) % 8)


def arg_entry(key_ptr, kind, value):
    """One argument of an argument section: key pointer, kind, 4 reserved
    bytes, then the value's 8 bytes. The value is already its bit pattern."""
    return struct.pack("<QIIQ", key_ptr, kind, 0, value & ((1 << 64) - 1))


def event_args(ts, track, u, etype, args, flags=0):
    """A record with an argument section after the head, as docs/raw_format.md
    lays it out: a count, 4 reserved bytes, then one entry per argument."""
    head = struct.pack("<QQQII", ts, track, u, etype, flags | pb.RF_HAS_ARGS)
    section = struct.pack("<II", len(args), 0)
    return head + section + b"".join(arg_entry(*a) for a in args)


def convert_bytes(data, keep_clock=False):
    """Runs the converter over an in-memory raw file. Returns (packets,
    stats, torn), where packets are generated TracePacket messages. The
    ClockSnapshot packet is left out unless keep_clock is true."""
    out = io.BytesIO()
    reader = pb.RawReader(io.BytesIO(data))
    conv = pb.Converter(out)
    conv.clock_snapshot(reader.clock_id, reader.clock_offset_ns,
                        reader.boottime_offset_ns)
    for kind, payload, offset in reader.chunks():
        if kind == pb.CHUNK_NAMES:
            conv.names_chunk(payload)
        elif kind == pb.CHUNK_KEYS:
            conv.keys_chunk(payload)
        elif kind == pb.CHUNK_TRACKS:
            conv.tracks_chunk(payload)
        elif kind == pb.CHUNK_EVENTS:
            conv.events_chunk(payload)
    packets = Trace.FromString(out.getvalue()).packet
    if not keep_clock:
        packets = [p for p in packets if not p.HasField("clock_snapshot")]
    return packets, conv.stats, reader.torn_bytes


class ConvertTest(unittest.TestCase):
    def thread_tracks(self):
        return pb.CHUNK_TRACKS, track_entry(1, 0, pb.TRACK_THREAD, 42, 7, 0,
                                            "main")

    def test_args_become_debug_annotations(self):
        # Two keys, the first used twice: the packet that first uses a key
        # defines it, and later packets name it by iid alone.
        keys = (pb.CHUNK_KEYS, name_entry(0x3000, "i") + name_entry(0x3008, "ratio"))
        int_bits = -2
        double_bits = struct.unpack("<Q", struct.pack("<d", 0.5))[0]
        events = (event_args(5, 1, NAME_PTR, RAW_SLICE_BEGIN,
                             [(0x3000, pb.ARG_INT, int_bits),
                              (0x3008, pb.ARG_DOUBLE, double_bits)])
                  + event_args(6, 1, NAME_PTR, RAW_INSTANT,
                               [(0x3000, pb.ARG_UINT, 9)]))
        data = raw_file(self.thread_tracks(),
                        (pb.CHUNK_NAMES, name_entry(NAME_PTR, "compute")),
                        keys, (pb.CHUNK_EVENTS, events))
        packets, stats, torn = convert_bytes(data)
        evs = [p for p in packets if p.HasField("track_event")]
        self.assertEqual(len(evs), 2)
        self.assertEqual(stats["keys"], 2)

        first = evs[0]
        self.assertEqual(len(first.track_event.debug_annotations), 2)
        ann_i, ann_ratio = first.track_event.debug_annotations
        self.assertEqual(ann_i.name_iid, 1)
        self.assertEqual(ann_i.int_value, -2)
        self.assertEqual(ann_ratio.name_iid, 2)
        self.assertEqual(ann_ratio.double_value, 0.5)
        defined = {d.iid: d.name for d in first.interned_data.debug_annotation_names}
        self.assertEqual(defined, {1: "i", 2: "ratio"})

        second = evs[1]
        ann, = second.track_event.debug_annotations
        self.assertEqual(ann.name_iid, 1)
        self.assertEqual(ann.uint_value, 9)
        self.assertEqual(len(second.interned_data.debug_annotation_names), 0)

    def test_args_with_copied_name_is_rejected(self):
        # The writer never sets both flags; the reader refuses the record.
        events = event_args(5, 1, NAME_PTR, RAW_SLICE_BEGIN,
                            [(0x3000, pb.ARG_UINT, 1)], flags=pb.RF_NAME_COPIED)
        data = raw_file(self.thread_tracks(),
                        (pb.CHUNK_KEYS, name_entry(0x3000, "i")),
                        (pb.CHUNK_EVENTS, events))
        with self.assertRaisesRegex(pb.RawFormatError, "both arguments and a copied name"):
            convert_bytes(data)

    def test_clock_snapshot_boottime_with_realtime_offset(self):
        offset = 1_700_000_000_000_000_000
        packets, _, _ = convert_bytes(raw_file(clock=7, offset=offset),
                                      keep_clock=True)
        snap = packets[0].clock_snapshot
        self.assertEqual(snap.primary_trace_clock, pb.ClockSnapshot.Clock.BOOTTIME)
        self.assertEqual([(c.clock_id, c.timestamp) for c in snap.clocks],
                         [(pb.ClockSnapshot.Clock.BOOTTIME, 0),
                          (pb.ClockSnapshot.Clock.REALTIME, offset)])

    def test_clock_snapshot_realtime_links_boottime(self):
        # Trace clock is REALTIME, so boot time is far below wall time: the
        # offset is negative, and the snapshot shifts every reading up to keep
        # the timestamps unsigned.
        boot = -1_700_000_000_000_000_000
        packets, _, _ = convert_bytes(raw_file(clock=0, offset=0,
                                               boottime=boot),
                                      keep_clock=True)
        snap = packets[0].clock_snapshot
        self.assertEqual(snap.primary_trace_clock, pb.ClockSnapshot.Clock.REALTIME)
        readings = {c.clock_id: c.timestamp for c in snap.clocks}
        self.assertEqual(readings, {pb.ClockSnapshot.Clock.REALTIME: -boot,
                                    pb.ClockSnapshot.Clock.BOOTTIME: 0})

    def test_clock_snapshot_monotonic_names_all_three(self):
        realtime, boot = 1_700_000_000_000_000_000, 5_000_000_000
        packets, _, _ = convert_bytes(raw_file(clock=1, offset=realtime,
                                               boottime=boot),
                                      keep_clock=True)
        snap = packets[0].clock_snapshot
        self.assertEqual(snap.primary_trace_clock,
                         pb.ClockSnapshot.Clock.MONOTONIC)
        self.assertEqual([(c.clock_id, c.timestamp) for c in snap.clocks],
                         [(pb.ClockSnapshot.Clock.MONOTONIC, 0),
                          (pb.ClockSnapshot.Clock.REALTIME, realtime),
                          (pb.ClockSnapshot.Clock.BOOTTIME, boot)])

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
        minus_three = (1 << 64) - 3  # two's-complement bits, as the record stores
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

    def test_track_rename_is_accepted_and_redeclared(self):
        data = raw_file(
            self.thread_tracks(),
            (pb.CHUNK_TRACKS, track_entry(1, 0, pb.TRACK_THREAD, 42, 7, 0,
                                          "renamed")),
            (pb.CHUNK_EVENTS, event(1, 1, 0, RAW_SLICE_END)))
        packets, stats, _ = convert_bytes(data)
        descs = [p.track_descriptor for p in packets
                 if p.HasField("track_descriptor")]
        self.assertEqual([d.thread.thread_name for d in descs],
                         ["main", "renamed"])
        self.assertEqual(stats["tracks"], 2)

    def test_track_redefinition_with_different_fields_is_rejected(self):
        data = raw_file(
            self.thread_tracks(),
            (pb.CHUNK_TRACKS, track_entry(1, 0, pb.TRACK_COUNTER, 42, 7, 0,
                                          "main")))
        with self.assertRaisesRegex(pb.RawFormatError, "redefined"):
            convert_bytes(data)

    def test_bad_magic_is_rejected(self):
        data = b"NOTRAW00" + raw_file()[8:]
        with self.assertRaisesRegex(pb.RawFormatError, "bad magic"):
            pb.RawReader(io.BytesIO(data))

    def test_dynamic_name_is_written_inline(self):
        events = (pb.CHUNK_EVENTS,
                  event_copied(100, 1, b"job 7 took 2.500000", RAW_SLICE_BEGIN))
        packets, stats, torn = convert_bytes(
            raw_file(self.thread_tracks(), events))
        self.assertEqual(torn, 0)
        self.assertEqual(packets[1].track_event.name, "job 7 took 2.500000")
        # Not interned: the name is per event, so no definition is written.
        self.assertEqual(len(packets[1].interned_data.event_names), 0)
        self.assertEqual(stats["events"], 1)

    def test_copied_names_of_mixed_lengths_keep_records_aligned(self):
        # 3, 8 and 5 bytes: the padding must put each head on the next record.
        events = (pb.CHUNK_EVENTS,
                  event_copied(1, 1, b"abc", RAW_INSTANT)
                  + event_copied(2, 1, b"12345678", RAW_INSTANT)
                  + event_copied(3, 1, b"xyzuv", RAW_INSTANT))
        packets, stats, torn = convert_bytes(
            raw_file(self.thread_tracks(), events))
        self.assertEqual(torn, 0)
        self.assertEqual([p.track_event.name for p in packets[1:]],
                         ["abc", "12345678", "xyzuv"])
        self.assertEqual(stats["events"], 3)

    def test_static_and_dynamic_names_mix(self):
        names = (pb.CHUNK_NAMES, name_entry(NAME_PTR, "compute"))
        events = (pb.CHUNK_EVENTS,
                  event(100, 1, NAME_PTR, RAW_SLICE_BEGIN)
                  + event_copied(110, 1, b"job 3", RAW_INSTANT))
        packets, _, _ = convert_bytes(
            raw_file(self.thread_tracks(), names, events))
        self.assertEqual(packets[1].track_event.name_iid, 1)
        self.assertEqual(packets[2].track_event.name, "job 3")

    def test_copied_name_over_limit_is_rejected(self):
        # 129 bytes: one past the 128 the writer copies.
        big = b"x" * 129
        data = raw_file(self.thread_tracks(),
                        (pb.CHUNK_EVENTS, event_copied(1, 1, big, RAW_INSTANT)))
        with self.assertRaisesRegex(pb.RawFormatError, "over 128"):
            convert_bytes(data)

    def test_copied_name_past_chunk_end_is_rejected(self):
        # The head says 20 name bytes, but the chunk ends after the head.
        head = struct.pack("<QQQII", 1, 1, 20, RAW_INSTANT, pb.RF_NAME_COPIED)
        data = raw_file(self.thread_tracks(), (pb.CHUNK_EVENTS, head))
        with self.assertRaisesRegex(pb.RawFormatError, "runs past"):
            convert_bytes(data)

if __name__ == "__main__":
    unittest.main()
