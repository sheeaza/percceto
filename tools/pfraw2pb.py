#!/usr/bin/env python3
"""Converts a raw pftrace dump (docs/raw_format.md) to a Perfetto protobuf
trace.

This is the offline half of PF_FORMAT_RAW: the drain copies ring records as
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
import re
import sys
from collections import Counter

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import (
    ClockSnapshot,
    Trace,
    TrackDescriptor,
    TrackEvent,
)

MAGIC = b"PFRAWv1\0"
VERSION = 3


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
    _pack_ = 1
    _fields_ = [
        ("ts", ctypes.c_uint64),
        ("track", ctypes.c_uint64),
        ("u", ctypes.c_uint64),
        ("type", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("a0", ctypes.c_uint64),
        ("a1", ctypes.c_uint64),
    ]


RECORD_SIZE = ctypes.sizeof(Event)
TRACK_SIZE = ctypes.sizeof(TrackEntry)
assert RECORD_SIZE == 48 and TRACK_SIZE == 104


class _Bits(ctypes.Union):
    _fields_ = [("bits", ctypes.c_uint64), ("double", ctypes.c_double)]


def bits_to_double(bits):
    """Reads a 64-bit word as an IEEE double, the way the ring stores it."""
    return _Bits(bits=bits).double

CHUNK_NAMES = 1
CHUNK_TRACKS = 2
CHUNK_EVENTS = 3

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
RF_NARGS_SHIFT = 2
RF_NARGS_MASK = 0x3 << 2
RF_ARG_IS_DOUBLE = (0x10, 0x20)

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
        if hdr.record_size != RECORD_SIZE:
            raise RawFormatError(f"record_size {hdr.record_size}, want {RECORD_SIZE}")
        if hdr.flags != 0:
            raise RawFormatError(f"reserved flags set: {hdr.flags:#x}")
        self.clock_id = hdr.clock_id
        self.clock_offset_ns = hdr.clock_offset_ns

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

    def clock_snapshot(self, clock_id, offset_ns):
        """Writes the ClockSnapshot the C writer emits at each restart. The raw
        header keeps only REALTIME minus the trace clock, so the trace clock
        reads 0 and REALTIME reads the offset. Perfetto maps time by the
        difference of the two readings, so this gives the writer's mapping."""
        if clock_id not in RAW_CLOCK_TO_PERFETTO:
            raise RawFormatError(f"clock_id {clock_id} is not a supported clock")
        if offset_ns < 0:
            raise RawFormatError(f"clock offset {offset_ns} is negative")
        primary = RAW_CLOCK_TO_PERFETTO[clock_id]
        # Every event packet names this clock, see events_chunk().
        self._clock = primary
        batch = Trace()
        # Not via _packet(): the snapshot has no incremental state, so the
        # first-packet flag stays with the first track descriptor.
        packet = batch.packet.add()
        packet.trusted_packet_sequence_id = 1
        snap = packet.clock_snapshot
        clock = snap.clocks.add()
        clock.clock_id = primary
        clock.timestamp = 0
        if primary != ClockSnapshot.Clock.REALTIME:
            clock = snap.clocks.add()
            clock.clock_id = ClockSnapshot.Clock.REALTIME
            clock.timestamp = offset_ns
        snap.primary_trace_clock = primary
        self._flush(batch)

    def names_chunk(self, payload):
        p = 0
        head_size = ctypes.sizeof(NameHeader)
        while p < len(payload):
            if len(payload) - p < head_size:
                raise RawFormatError("truncated name entry")
            hdr = NameHeader.from_buffer_copy(payload, p)
            ptr, nlen = hdr.ptr, hdr.length
            p += head_size
            if len(payload) - p < nlen:
                raise RawFormatError("name runs past its chunk")
            name = payload[p:p + nlen].decode("utf-8", errors="replace")
            p += nlen
            known = self._names.get(ptr)
            if known is None:
                self._names[ptr] = name
                self.stats["names"] += 1
            elif known != name:
                raise RawFormatError(f"conflicting definition for name {ptr:#x}")

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
        if len(payload) % RECORD_SIZE:
            raise RawFormatError(
                f"events chunk is not a multiple of {RECORD_SIZE} bytes")
        batch = Trace()
        for off in range(0, len(payload), RECORD_SIZE):
            rec = Event.from_buffer_copy(payload, off)
            ts, track, u, etype, flags = rec.ts, rec.track, rec.u, rec.type, rec.flags
            a0, a1 = rec.a0, rec.a1
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

            nargs = (flags & RF_NARGS_MASK) >> RF_NARGS_SHIFT
            if flags & RF_HAS_VALUE:
                if flags & RF_VALUE_IS_DOUBLE:
                    ev.double_counter_value = bits_to_double(u)
                else:
                    # The ring stores the two's-complement bits of an int64.
                    ev.counter_value = u - (1 << 64) if u & INT64_SIGN else u
            elif nargs:
                # The name is a format; its args are rendered per event, so
                # the text is written inline rather than interned.
                if u not in self._names:
                    raise RawFormatError(f"event refers to undefined name {u:#x}")
                args = [(a0, bool(flags & RF_ARG_IS_DOUBLE[0])),
                        (a1, bool(flags & RF_ARG_IS_DOUBLE[1]))][:nargs]
                try:
                    ev.name = format_args(self._names[u], args)
                except RawFormatError as err:
                    # Same fallback as the C drain: the bare format is
                    # written, so the event is kept and the mismatch shows.
                    log.warning("event at ts %d: %s; writing the bare format",
                                ts, err)
                    ev.name = self._names[u]
                    self.stats["bad_format"] += 1
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
SPEC_RE = re.compile(r"([-+ 0]*)(\d{0,3})(?:\.(\d{1,3}))?(ll|l)?([diuxXoceEfFgGp])")
# Longest rendered name, NUL included, as in PF_FORMAT_NAME_MAX in C.
FORMAT_NAME_MAX = 128


def format_args(fmt, args):
    """Renders a format with its args. args is a list of (bits, is_double).
    Raises RawFormatError on any mismatch, or if the result is too long."""
    out = []
    k = 0
    i = 0
    while i < len(fmt):
        if fmt[i] != "%":
            out.append(fmt[i])
            i += 1
            continue
        if fmt[i + 1:i + 2] == "%":
            out.append("%")
            i += 2
            continue
        m = SPEC_RE.match(fmt, i + 1)
        if not m or k >= len(args):
            raise RawFormatError(f"format {fmt!r}: unsupported or extra conversion")
        flags, width, prec, lenmod, conv = m.groups()
        i = m.end()
        bits, is_double = args[k]
        k += 1
        out.append(_render_arg(fmt, flags, width, prec, lenmod, conv,
                               bits, is_double))
    if k != len(args):
        raise RawFormatError(f"format {fmt!r} has fewer placeholders than args")
    text = "".join(out)
    if len(text) >= FORMAT_NAME_MAX:
        raise RawFormatError(f"format {fmt!r}: result is longer than {FORMAT_NAME_MAX - 1}")
    return text


def _render_arg(fmt, flags, width, prec, lenmod, conv, bits, is_double):
    def bad(why):
        return RawFormatError(f"format {fmt!r}: {why}")

    if conv in "uxXo" and ("+" in flags or " " in flags):
        raise bad(f"%{conv} takes no sign flag")
    if conv in "cp":
        if "+" in flags or " " in flags or "0" in flags or prec or lenmod:
            raise bad(f"%{conv} takes only '-' and a width")
    if conv in "fFeEgG" and lenmod:
        raise bad(f"%{conv} takes no length modifier")
    spec_prec = f".{prec}" if prec else ""

    if conv in "diuxXo":
        if is_double:
            raise bad(f"%{conv} given a double")
        if conv in "di":
            value = bits - (1 << 64) if bits & INT64_SIGN else bits
            return ("%" + flags + width + spec_prec + "d") % value
        return ("%" + flags + width + spec_prec + conv) % bits
    if conv == "c":
        if is_double or not 1 <= bits <= 127:
            raise bad("%c needs an int from 1 to 127")
        return ("%" + flags + width + "c") % chr(bits)
    if conv == "p":
        if is_double:
            raise bad("%p given a double")
        text = "0x%x" % bits
        w = int(width) if width else 0
        return text.ljust(w) if "-" in flags else text.rjust(w)
    # Float conversions.
    if not is_double:
        raise bad(f"%{conv} given an int")
    value = bits_to_double(bits)
    return ("%" + flags + width + spec_prec + conv) % value


def convert(src, dst):
    """Converts src (a path) to dst (a path). Returns the stats Counter and
    the number of torn bytes at the end of the input."""
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        reader = RawReader(fin)
        conv = Converter(fout)
        conv.clock_snapshot(reader.clock_id, reader.clock_offset_ns)
        for kind, payload, offset in reader.chunks():
            try:
                if kind == CHUNK_NAMES:
                    conv.names_chunk(payload)
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
