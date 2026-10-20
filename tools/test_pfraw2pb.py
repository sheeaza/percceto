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
    header = struct.pack("<8sIIIIqq", b"PFRAWv1\0", 4, 48, clock, 0, offset,
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


def event(ts, track, u, etype, flags=0, a0=0, a1=0):
    return struct.pack("<QQQII2Q", ts, track, u, etype, flags, a0, a1)


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
        # The events chunk is 8 + 96 bytes; 5 are cut, so 99 remain of it.
        packets, stats, torn = convert_bytes(data[:-5])
        self.assertEqual(torn, 99)
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

    def test_args_are_rendered_into_event_name(self):
        names = (pb.CHUNK_NAMES, name_entry(NAME_PTR, "job %d took %f"))
        bits = struct.unpack("<Q", struct.pack("<d", 2.5))[0]
        # Two args: arg0 is an int, arg1 is a double (RF_ARG1_IS_DOUBLE).
        flags = (2 << pb.RF_NARGS_SHIFT) | 0x20
        events = (pb.CHUNK_EVENTS,
                  event(100, 1, NAME_PTR, RAW_SLICE_BEGIN, flags, 7, bits))
        packets, _, _ = convert_bytes(
            raw_file(self.thread_tracks(), names, events))
        self.assertEqual(packets[1].track_event.name, "job 7 took 2.500000")

    def test_unsigned_and_hex_conversions(self):
        names = (pb.CHUNK_NAMES, name_entry(NAME_PTR, "n=%u x=%x 100%%"))
        flags = 2 << pb.RF_NARGS_SHIFT
        events = (pb.CHUNK_EVENTS,
                  event(100, 1, NAME_PTR, RAW_INSTANT, flags, 42, 255))
        packets, _, _ = convert_bytes(
            raw_file(self.thread_tracks(), names, events))
        self.assertEqual(packets[1].track_event.name, "n=42 x=ff 100%")

    def test_arg_count_mismatch_writes_bare_format_and_warns(self):
        names = (pb.CHUNK_NAMES, name_entry(NAME_PTR, "job %d"))
        flags = 2 << pb.RF_NARGS_SHIFT
        events = (pb.CHUNK_EVENTS,
                  event(100, 1, NAME_PTR, RAW_SLICE_BEGIN, flags, 7, 0))
        with self.assertLogs("pfraw2pb", level="WARNING") as logs:
            packets, stats, _ = convert_bytes(
                raw_file(self.thread_tracks(), names, events))
        self.assertEqual(packets[1].track_event.name, "job %d")
        self.assertEqual(stats["bad_format"], 1)
        self.assertIn("fewer placeholders", logs.output[0])

    def test_arg_type_mismatch_writes_bare_format_and_warns(self):
        names = (pb.CHUNK_NAMES, name_entry(NAME_PTR, "%d"))
        flags = (1 << pb.RF_NARGS_SHIFT) | 0x10  # arg0 is a double
        events = (pb.CHUNK_EVENTS,
                  event(100, 1, NAME_PTR, RAW_SLICE_BEGIN, flags, 0, 0))
        with self.assertLogs("pfraw2pb", level="WARNING") as logs:
            packets, stats, _ = convert_bytes(
                raw_file(self.thread_tracks(), names, events))
        self.assertEqual(packets[1].track_event.name, "%d")
        self.assertIn("given a double", logs.output[0])

    def test_width_flags_and_precision(self):
        bits = struct.unpack("<Q", struct.pack("<d", 2.5))[0]
        self.assertEqual(
            pb.format_args("[%8d][%-6d][%+d][%08.3f][%.2e]",
                           [(5, False), (3, False), (3, False),
                            (bits, True), (bits, True)]),
            "[       5][3     ][+3][0002.500][2.50e+00]")

    def test_char_and_pointer(self):
        self.assertEqual(
            pb.format_args("%c%c p=%p P=%8p",
                           [(104, False), (105, False),
                            (4096, False), (255, False)]),
            "hi p=0x1000 P=    0xff")

    def test_hash_flag_is_rejected(self):
        with self.assertRaises(pb.RawFormatError):
            pb.format_args("%#x", [(255, False)])

    def test_name_length_limit_matches_C(self):
        # 127 characters fit; 128 do not, as in PF_FORMAT_NAME_MAX.
        self.assertEqual(len(pb.format_args("x" * 127, [])), 127)
        with self.assertRaisesRegex(pb.RawFormatError, "longer than 127"):
            pb.format_args("x" * 128, [])


if __name__ == "__main__":
    unittest.main()
