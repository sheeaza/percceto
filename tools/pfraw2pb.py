#!/usr/bin/env python3
"""Converts a raw pftrace dump (docs/raw_format.md) to a Perfetto protobuf
trace.

This is the offline half of PF_FORMAT_RAW: the drain copies records as
they are, and this turns them into the same Trace the protobuf writer
produces. The protobuf classes come from the perfetto package, which
ships the generated schema.

Each input chunk becomes one serialized Trace. A Trace is a repeated
packet field, so the output is a sequence of Trace messages and parses as
one.

Usage:
    pfraw2pb.py input.pfraw [-o output.pftrace]
"""
import argparse
import ctypes
import logging
import sys
from collections import Counter

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import (
    ClockSnapshot,
    Trace,
    TrackDescriptor,
    TrackEvent,
)

MAGIC = b"PFRAWv1\0"
VERSION = 6


# On-disk layouts, little-endian and unpadded, as in docs/raw_format.md.
class Header(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("magic", ctypes.c_uint8 * 8),  # a c_char array would stop at the NUL
        ("version", ctypes.c_uint32),
        ("record_size", ctypes.c_uint32),
        ("clock_id", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("clock_offset_ns", ctypes.c_int64),  # REALTIME - clock (ns)
        ("boottime_offset_ns", ctypes.c_int64),  # BOOTTIME - clock (ns)
    ]


class ChunkHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("kind", ctypes.c_uint32),
        ("length", ctypes.c_uint32),
    ]


class NameHeader(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("ptr", ctypes.c_uint64),
        ("length", ctypes.c_uint32),
    ]


class TrackEntry(ctypes.LittleEndianStructure):
    _pack_ = 1
    _fields_ = [
        ("uuid", ctypes.c_uint64),
        ("parent", ctypes.c_uint64),
        ("kind", ctypes.c_uint32),
        ("pid", ctypes.c_int32),
        ("tid", ctypes.c_int32),
        ("unit", ctypes.c_uint32),
        ("has_name", ctypes.c_uint32),
        ("name_len", ctypes.c_uint32),
        ("name", ctypes.c_char * 64),
    ]


class Event(ctypes.LittleEndianStructure):
    # The fixed head of a record. An argument section follows it when
    # RF_HAS_ARGS, then a copied name when RF_NAME_COPIED.
    _pack_ = 1
    _fields_ = [
        ("ts", ctypes.c_uint64),
        ("track", ctypes.c_uint64),
        ("u", ctypes.c_uint64),
        ("type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
    ]


class ArgsHead(ctypes.LittleEndianStructure):
    # The argument section's head: the count, then 4 reserved bytes.
    _pack_ = 1
    _fields_ = [
        ("count", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
    ]


class ArgEntry(ctypes.LittleEndianStructure):
    # One argument: its key's pointer, its kind, 4 reserved bytes, and the
    # value's 8 bytes.
    _pack_ = 1
    _fields_ = [
        ("key", ctypes.c_uint64),
        ("kind", ctypes.c_uint32),
        ("reserved", ctypes.c_uint32),
        ("value", ctypes.c_uint64),
    ]


RECORD_HEAD_BYTES = ctypes.sizeof(Event)
ARGS_HEAD_BYTES = ctypes.sizeof(ArgsHead)
ARG_BYTES = ctypes.sizeof(ArgEntry)
TRACK_SIZE = ctypes.sizeof(TrackEntry)
assert RECORD_HEAD_BYTES == 32 and TRACK_SIZE == 104
assert ARGS_HEAD_BYTES == 8 and ARG_BYTES == 24
# A copied name is padded to 8 bytes and capped, as in the C library.
REC_ALIGN = 8
DYN_NAME_MAX = 128
# The argument kinds, from PF_ARG_* in include/pftrace.h.
ARG_INT, ARG_UINT, ARG_DOUBLE = 0, 1, 2


class _Bits(ctypes.Union):
    _fields_ = [("bits", ctypes.c_uint64), ("double", ctypes.c_double)]


def bits_to_double(bits):
    """Reads a 64-bit word as an IEEE double, the way the buffer stores it."""
    return _Bits(bits=bits).double

CHUNK_NAMES = 1
CHUNK_TRACKS = 2
CHUNK_EVENTS = 3
CHUNK_KEYS = 4

RF_HAS_VALUE = 0x1
# raw format's own values, so they map to Perfetto's enum explicitly.
# Event type numbers in the raw file, from docs/raw_format.md. They are the
# raw format's own values, so they map to Perfetto's enum explicitly.
RAW_TYPE_TO_PERFETTO = {
    1: TrackEvent.TYPE_SLICE_BEGIN,
    2: TrackEvent.TYPE_SLICE_END,
    3: TrackEvent.TYPE_INSTANT,
    4: TrackEvent.TYPE_COUNTER,
}

# Linux clock ids the raw header can carry, mapped to Perfetto's builtin clocks.
RAW_CLOCK_TO_PERFETTO = {
    0: ClockSnapshot.Clock.REALTIME,  # CLOCK_REALTIME
    1: ClockSnapshot.Clock.MONOTONIC,  # CLOCK_MONOTONIC
    7: ClockSnapshot.Clock.BOOTTIME,  # CLOCK_BOOTTIME
}

RF_HAS_VALUE = 0x1
RF_VALUE_IS_DOUBLE = 0x2
RF_NAME_COPIED = 0x4
RF_HAS_ARGS = 0x8

TRACK_PROCESS = 0
TRACK_THREAD = 1
TRACK_COUNTER = 2

# Fields a later TRACKS entry for the same uuid must not change; its name is
# the only thing a rename may replace.
TRACK_IDENTITY = ("kind", "uuid", "parent", "pid", "tid", "unit")

SEQ_INCREMENTAL_STATE_CLEARED = 1
SEQ_NEEDS_INCREMENTAL_STATE = 2

UINT64_MASK = (1 << 64) - 1
INT64_SIGN = 1 << 63

log = logging.getLogger("pfraw2pb")


class RawFormatError(Exception):
    """The file breaks the format. Raised with the byte offset where it did."""


# --- Reading ---------------------------------------------------------------


class RawReader:
    """Reads a raw file front to back, one chunk at a time.

    Iterating yields (kind, payload, offset) for every complete chunk. A
    chunk cut short at the end of the file is not yielded; its size is left
    in torn_bytes, which is how a crash mid-write shows up.
    """

    def __init__(self, f):
        self._f = f
        self.torn_bytes = 0
        size = ctypes.sizeof(Header)
        head = f.read(size)
        if len(head) != size:
            raise RawFormatError("file shorter than the 32-byte header")
        hdr = Header.from_buffer_copy(head)
        if bytes(hdr.magic) != MAGIC:
            raise RawFormatError("bad magic: not a raw pftrace file")
        if hdr.version != VERSION:
            raise RawFormatError(f"unsupported version {hdr.version}")
        if hdr.record_size != RECORD_HEAD_BYTES:
            raise RawFormatError(f"record_size {hdr.record_size}, want {RECORD_HEAD_BYTES}")
        if hdr.flags != 0:
            raise RawFormatError(f"reserved flags set: {hdr.flags:#x}")
        self.clock_id = hdr.clock_id
        self.clock_offset_ns = hdr.clock_offset_ns
        self.boottime_offset_ns = hdr.boottime_offset_ns

    def chunks(self):
        head_size = ctypes.sizeof(ChunkHeader)
        offset = ctypes.sizeof(Header)
        while True:
            head = self._f.read(head_size)
            if not head:
                return
            if len(head) < head_size:
                self.torn_bytes = len(head)
                return
            chunk = ChunkHeader.from_buffer_copy(head)
            payload = self._f.read(chunk.length)
            if len(payload) < chunk.length:
                self.torn_bytes = head_size + len(payload)
                return
            yield chunk.kind, payload, offset
            offset += head_size + chunk.length


# --- Converting ------------------------------------------------------------


class Converter:
    """Turns chunks into TracePacket messages, one Trace per chunk.

    Definitions must come before the records that use them, as the format
    requires. Names are interned on first use, the way pftrace.c does it.
    """

    def __init__(self, out):
        self._out = out
        self._names = {}  # ptr -> str
        self._iids = {}  # ptr -> iid, for names already written
        self._keys = {}  # ptr -> str, for argument keys
        self._key_iids = {}  # ptr -> iid, for keys already written
        self._tracks = {}  # uuid -> parsed track entry
        self._first = True
        # Until clock_snapshot() names the trace clock, packets use BOOTTIME,
        # the clock Perfetto assumes for a packet with no clock id.
        self._clock = ClockSnapshot.Clock.BOOTTIME
        self.stats = Counter()

    def _packet(self, batch, seq_flags=0):
        # The first packet of the stream clears any incremental state a
        # reader would otherwise assume.
        packet = batch.packet.add()
        packet.trusted_packet_sequence_id = 1
        if self._first:
            seq_flags |= SEQ_INCREMENTAL_STATE_CLEARED
            self._first = False
        if seq_flags:
            packet.sequence_flags = seq_flags
        self.stats["packets"] += 1
        return packet

    def _flush(self, batch):
        if batch.packet:
            self._out.write(batch.SerializeToString())

    def clock_snapshot(self, clock_id, offset_ns, boottime_offset_ns):
        """Writes the ClockSnapshot the C writer emits at each restart. The raw
        header keeps the trace clock's offsets to REALTIME and to BOOTTIME.
        Perfetto maps time by the differences between readings, so each reading
        is set relative to the trace clock, which reads 0. An offset can be
        negative (boot time is far below wall time when the trace clock is
        REALTIME), so every reading is shifted up by one common base to keep the
        timestamps unsigned. The differences, and so the mapping, are unchanged."""
        if clock_id not in RAW_CLOCK_TO_PERFETTO:
            raise RawFormatError(f"clock_id {clock_id} is not a supported clock")
        primary = RAW_CLOCK_TO_PERFETTO[clock_id]
        # Every event packet names this clock, see events_chunk().
        self._clock = primary
        readings = {primary: 0}
        if primary != ClockSnapshot.Clock.REALTIME:
            readings[ClockSnapshot.Clock.REALTIME] = offset_ns
        # Perfetto links BOOTTIME to the trace clock for a proto trace, so the
        # snapshot always names it. See pftrace_pb.c emit_clock_snapshot().
        if primary != ClockSnapshot.Clock.BOOTTIME:
            readings[ClockSnapshot.Clock.BOOTTIME] = boottime_offset_ns
        base = max(0, -min(readings.values()))
        batch = Trace()
        # Not via _packet(): the snapshot has no incremental state, so the
        # first-packet flag stays with the first track descriptor.
        packet = batch.packet.add()
        packet.trusted_packet_sequence_id = 1
        snap = packet.clock_snapshot
        for clock, ts in readings.items():
            entry = snap.clocks.add()
            entry.clock_id = clock
            entry.timestamp = ts + base
        snap.primary_trace_clock = primary
        self._flush(batch)


    def names_chunk(self, payload):
        self._define(payload, self._names, "name")

    def keys_chunk(self, payload):
        self._define(payload, self._keys, "key")

    def _define(self, payload, table, what):
        p = 0
        head_size = ctypes.sizeof(NameHeader)
        while p < len(payload):
            if len(payload) - p < head_size:
                raise RawFormatError(f"truncated {what} entry")
            hdr = NameHeader.from_buffer_copy(payload, p)
            ptr, nlen = hdr.ptr, hdr.length
            p += head_size
            if len(payload) - p < nlen:
                raise RawFormatError(f"{what} runs past its chunk")
            text = payload[p:p + nlen].decode("utf-8", errors="replace")
            p += nlen
            known = table.get(ptr)
            if known is None:
                table[ptr] = text
                self.stats[what + "s"] += 1
            elif known != text:
                raise RawFormatError(f"conflicting definition for {what} {ptr:#x}")

    def tracks_chunk(self, payload):
        if len(payload) % TRACK_SIZE:
            raise RawFormatError("tracks chunk is not a multiple of 104 bytes")
        batch = Trace()
        for off in range(0, len(payload), TRACK_SIZE):
            entry = TrackEntry.from_buffer_copy(payload, off)
            if entry.name_len > 64:
                raise RawFormatError("track name longer than 64 bytes")
            name = entry.name[:entry.name_len].decode("utf-8", errors="replace")
            track = {"kind": entry.kind, "uuid": entry.uuid, "parent": entry.parent,
                     "pid": entry.pid, "tid": entry.tid, "unit": entry.unit,
                     "has_name": bool(entry.has_name), "name": name}
            uuid = entry.uuid
            prev = self._tracks.get(uuid)
            if prev is not None:
                # A rename: only the name may change.
                if any(prev[k] != track[k] for k in TRACK_IDENTITY):
                    raise RawFormatError(
                        f"track {uuid} redefined with different fields")
            self._tracks[uuid] = track
            self._packet(batch).track_descriptor.CopyFrom(
                self._track_descriptor(track))
            self.stats["tracks"] += 1
        self._flush(batch)

    def _track_descriptor(self, t):
        desc = TrackDescriptor(uuid=t["uuid"])
        if t["parent"]:
            desc.parent_uuid = t["parent"]
        if t["kind"] == TRACK_PROCESS:
            desc.process.pid = t["pid"]
            if t["has_name"]:
                desc.process.process_name = t["name"]
        elif t["kind"] == TRACK_THREAD:
            desc.thread.pid = t["pid"]
            desc.thread.tid = t["tid"]
            if t["has_name"]:
                desc.thread.thread_name = t["name"]
        elif t["kind"] == TRACK_COUNTER:
            desc.counter.unit = t["unit"]
            if t["has_name"]:
                desc.name = t["name"]
        else:
            raise RawFormatError(f"unknown track kind {t['kind']}")
        return desc

    def events_chunk(self, payload):
        batch = Trace()
        off = 0
        while off < len(payload):
            if len(payload) - off < RECORD_HEAD_BYTES:
                raise RawFormatError("events chunk ends inside a record head")
            rec = Event.from_buffer_copy(payload, off)
            off += RECORD_HEAD_BYTES
            ts, track, u, etype, flags = rec.ts, rec.track, rec.u, rec.type, rec.flags
            if flags & RF_HAS_ARGS and flags & RF_NAME_COPIED:
                raise RawFormatError("record has both arguments and a copied name")

            # The argument section follows the head when RF_HAS_ARGS: a count,
            # then one entry per argument. A copied name comes after it.
            args = []
            if flags & RF_HAS_ARGS:
                if len(payload) - off < ARGS_HEAD_BYTES:
                    raise RawFormatError("argument section runs past the events chunk")
                count = ArgsHead.from_buffer_copy(payload, off).count
                off += ARGS_HEAD_BYTES
                if len(payload) - off < count * ARG_BYTES:
                    raise RawFormatError("argument entries run past the events chunk")
                for _ in range(count):
                    args.append(ArgEntry.from_buffer_copy(payload, off))
                    off += ARG_BYTES

            # A copied name follows the head: @u is its length, and the bytes
            # are padded to REC_ALIGN.
            copied = None
            if flags & RF_NAME_COPIED:
                if u > DYN_NAME_MAX:
                    raise RawFormatError(f"copied name of {u} bytes is over {DYN_NAME_MAX}")
                padded = (u + REC_ALIGN - 1) & ~(REC_ALIGN - 1)
                if len(payload) - off < padded:
                    raise RawFormatError("copied name runs past the events chunk")
                copied = bytes(payload[off:off + u]).decode("utf-8", "replace")
                off += padded

            if track not in self._tracks:
                raise RawFormatError(f"event refers to undefined track {track}")
            if etype not in RAW_TYPE_TO_PERFETTO:
                raise RawFormatError(f"unknown event type {etype}")

            packet = self._packet(batch, SEQ_NEEDS_INCREMENTAL_STATE)
            packet.timestamp = ts
            packet.timestamp_clock_id = self._clock
            ev = packet.track_event
            ev.type = RAW_TYPE_TO_PERFETTO[etype]
            ev.track_uuid = track

            # Each argument is a debug annotation. Its key is interned on first
            # use, as the name is, so the packet carrying it defines the key.
            for arg in args:
                if arg.key not in self._keys:
                    raise RawFormatError(f"argument refers to undefined key {arg.key:#x}")
                key_iid = self._key_iids.get(arg.key)
                if key_iid is None:
                    key_iid = len(self._key_iids) + 1
                    self._key_iids[arg.key] = key_iid
                    packet.interned_data.debug_annotation_names.add(
                        iid=key_iid, name=self._keys[arg.key])
                ann = ev.debug_annotations.add()
                ann.name_iid = key_iid
                if arg.kind == ARG_INT:
                    # The buffer stores the two's-complement bits of an int64.
                    ann.int_value = arg.value - (1 << 64) if arg.value & INT64_SIGN else arg.value
                elif arg.kind == ARG_UINT:
                    ann.uint_value = arg.value
                elif arg.kind == ARG_DOUBLE:
                    ann.double_value = bits_to_double(arg.value)
                else:
                    raise RawFormatError(f"unknown argument kind {arg.kind}")

            if flags & RF_HAS_VALUE:
                if flags & RF_VALUE_IS_DOUBLE:
                    ev.double_counter_value = bits_to_double(u)
                else:
                    # The buffer stores the two's-complement bits of an int64.
                    ev.counter_value = u - (1 << 64) if u & INT64_SIGN else u
            elif copied is not None:
                # A caller-built name differs per event, so it is written inline
                # rather than interned.
                ev.name = copied
            elif u:
                if u not in self._names:
                    raise RawFormatError(f"event refers to undefined name {u:#x}")
                iid = self._iids.get(u)
                if iid is None:
                    iid = len(self._iids) + 1
                    self._iids[u] = iid
                    packet.interned_data.event_names.add(
                        iid=iid, name=self._names[u])
                ev.name_iid = iid
            self.stats["events"] += 1
        self._flush(batch)


# One conversion: "%[flags][width][.prec][len]conv". Mirrors parse_spec() and
# render_arg() in src/pftrace_pb.c, so both writers agree on every name.
def convert(src, dst):
    """Converts src (a path) to dst (a path). Returns the stats Counter and
    the number of torn bytes at the end of the input."""
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        reader = RawReader(fin)
        conv = Converter(fout)
        conv.clock_snapshot(reader.clock_id, reader.clock_offset_ns,
                            reader.boottime_offset_ns)
        for kind, payload, offset in reader.chunks():
            try:
                if kind == CHUNK_NAMES:
                    conv.names_chunk(payload)
                elif kind == CHUNK_KEYS:
                    conv.keys_chunk(payload)
                elif kind == CHUNK_TRACKS:
                    conv.tracks_chunk(payload)
                elif kind == CHUNK_EVENTS:
                    conv.events_chunk(payload)
                else:
                    raise RawFormatError(f"unknown chunk kind {kind}")
            except RawFormatError as e:
                raise RawFormatError(f"chunk at byte {offset}: {e}") from None
        return conv.stats, reader.torn_bytes


def main(argv=None):
    logging.basicConfig(format="%(levelname)s: %(message)s")
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="default: input with .pftrace")
    args = ap.parse_args(argv)

    output = args.output or args.input.rsplit(".", 1)[0] + ".pftrace"
    try:
        stats, torn = convert(args.input, output)
    except (RawFormatError, OSError) as e:
        print(f"pfraw2pb: {args.input}: {e}", file=sys.stderr)
        return 1

    print(f"pfraw2pb: {stats['events']} events, {stats['tracks']} tracks, "
          f"{stats['names']} names -> {output}", file=sys.stderr)
    if torn:
        print(f"pfraw2pb: warning: {torn} torn bytes at end of input were "
              f"ignored", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
