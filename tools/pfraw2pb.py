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
import struct
import sys
from collections import Counter

from perfetto.protos.perfetto.trace.perfetto_trace_pb2 import (
    Trace,
    TrackDescriptor,
    TrackEvent,
)

MAGIC = b"PFRAWv1\0"
VERSION = 1
HEADER = struct.Struct("<8sIIIIQ")  # magic, version, record_size, clock, flags, reserved
CHUNK_HEADER = struct.Struct("<II")  # kind, len
NAME_HEADER = struct.Struct("<QI")  # ptr, len
TRACK_ENTRY = struct.Struct("<QQIiiIII64s")  # 104 bytes
EVENT = struct.Struct("<QQQII")  # 32 bytes: ts, track, u, type, flags

RECORD_SIZE = EVENT.size
TRACK_SIZE = TRACK_ENTRY.size
assert RECORD_SIZE == 32 and TRACK_SIZE == 104

CHUNK_NAMES = 1
CHUNK_TRACKS = 2
CHUNK_EVENTS = 3

# Event type numbers in the raw file, from docs/raw_format.md. They are the
# raw format's own values, so they map to Perfetto's enum explicitly.
RAW_TYPE_TO_PERFETTO = {
    1: TrackEvent.TYPE_SLICE_BEGIN,
    2: TrackEvent.TYPE_SLICE_END,
    3: TrackEvent.TYPE_INSTANT,
    4: TrackEvent.TYPE_COUNTER,
}

RF_HAS_VALUE = 0x1
RF_VALUE_IS_DOUBLE = 0x2

TRACK_PROCESS = 0
TRACK_THREAD = 1
TRACK_COUNTER = 2

SEQ_INCREMENTAL_STATE_CLEARED = 1
SEQ_NEEDS_INCREMENTAL_STATE = 2

UINT64_MASK = (1 << 64) - 1
INT64_SIGN = 1 << 63


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
        head = f.read(HEADER.size)
        if len(head) != HEADER.size:
            raise RawFormatError("file shorter than the 32-byte header")
        magic, version, record_size, clock_id, flags, _ = HEADER.unpack(head)
        if magic != MAGIC:
            raise RawFormatError("bad magic: not a raw pftrace file")
        if version != VERSION:
            raise RawFormatError(f"unsupported version {version}")
        if record_size != RECORD_SIZE:
            raise RawFormatError(f"record_size {record_size}, want {RECORD_SIZE}")
        if flags != 0:
            raise RawFormatError(f"reserved flags set: {flags:#x}")
        self.clock_id = clock_id

    def chunks(self):
        offset = HEADER.size
        while True:
            head = self._f.read(CHUNK_HEADER.size)
            if not head:
                return
            if len(head) < CHUNK_HEADER.size:
                self.torn_bytes = len(head)
                return
            kind, length = CHUNK_HEADER.unpack(head)
            payload = self._f.read(length)
            if len(payload) < length:
                self.torn_bytes = CHUNK_HEADER.size + len(payload)
                return
            yield kind, payload, offset
            offset += CHUNK_HEADER.size + length


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

    def names_chunk(self, payload):
        p = 0
        while p < len(payload):
            if len(payload) - p < NAME_HEADER.size:
                raise RawFormatError("truncated name entry")
            ptr, nlen = NAME_HEADER.unpack_from(payload, p)
            p += NAME_HEADER.size
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
            (uuid, parent, kind, pid, tid, unit, has_name, name_len,
             raw_name) = TRACK_ENTRY.unpack_from(payload, off)
            if uuid in self._tracks:
                raise RawFormatError(f"track {uuid} defined twice")
            if name_len > 64:
                raise RawFormatError("track name longer than 64 bytes")
            name = raw_name[:name_len].decode("utf-8", errors="replace")
            track = {"kind": kind, "uuid": uuid, "parent": parent, "pid": pid,
                     "tid": tid, "unit": unit, "has_name": bool(has_name),
                     "name": name}
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
            raise RawFormatError("events chunk is not a multiple of 32 bytes")
        batch = Trace()
        for ts, track, u, etype, flags in EVENT.iter_unpack(payload):
            if track not in self._tracks:
                raise RawFormatError(f"event refers to undefined track {track}")
            if etype not in RAW_TYPE_TO_PERFETTO:
                raise RawFormatError(f"unknown event type {etype}")

            packet = self._packet(batch, SEQ_NEEDS_INCREMENTAL_STATE)
            packet.timestamp = ts
            ev = packet.track_event
            ev.type = RAW_TYPE_TO_PERFETTO[etype]
            ev.track_uuid = track

            if flags & RF_HAS_VALUE:
                if flags & RF_VALUE_IS_DOUBLE:
                    ev.double_counter_value = struct.unpack(
                        "<d", struct.pack("<Q", u))[0]
                else:
                    # The ring stores the two's-complement bits of an int64.
                    ev.counter_value = u - (1 << 64) if u & INT64_SIGN else u
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


def convert(src, dst):
    """Converts src (a path) to dst (a path). Returns the stats Counter and
    the number of torn bytes at the end of the input."""
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        reader = RawReader(fin)
        conv = Converter(fout)
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
