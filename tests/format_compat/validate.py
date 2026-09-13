"""Read cangaroo's sample traces back with readers that share no code with it.

tests/format_compat/main.cpp writes every export format (export.*, empty.*) and
TraceRecorder output split into 1 MB parts (recorder_*/). Each file is parsed
by an independent implementation of its format and compared with the frames
below, which are defined here again instead of being taken from anything
cangaroo wrote:

  candump, Vector ASC   python-can  (CanutilsLogReader, ASCReader)
  pcap, pcapng          scapy       (RawPcapReader, RawPcapNgReader) for the
                                    container; SocketCAN frames are decoded per
                                    <linux/can.h>
  MDF4                  asammdf

Any warning a reader logs counts as a failure: python-can, for instance, reads
a malformed ASC CAN FD line but only logs "DLC vs Data Length mismatch".

Usage: validate.py <directory written by trace_format_samples>
"""

import glob
import logging
import os
import sys
from dataclasses import dataclass

import can
from asammdf import MDF
from scapy.utils import RawPcapNgReader, RawPcapReader

# <linux/can.h>
CAN_EFF_FLAG = 0x80000000
CAN_RTR_FLAG = 0x40000000
CAN_ERR_FLAG = 0x20000000
CAN_ERR_BUSERROR = 0x00000080  # <linux/can/error.h>; python-can writes unclassified errors this way
CAN_ERR_DLC = 8
CANFD_BRS = 0x01
CAN_MTU = 16
CANFD_MTU = 72

LINKTYPE_CAN_SOCKETCAN = 227
BASE_US = 1_757_000_000_000_000
INTERFACE_NAMES = {1: "vcan0", 2: "vcan1"}
SPLIT_BYTES = 1024 * 1024
BULK_COUNT = 30000


@dataclass(frozen=True)
class Frame:
    ts_us: int
    iface: int
    can_id: int
    extended: bool = False
    rx: bool = True
    data: bytes = b""
    rtr: bool = False
    fd: bool = False
    brs: bool = False
    error: bool = False

    @property
    def canid_t(self):
        if self.error:
            # The sample error frame carries no specific error, so it is an
            # unspecified bus error class.
            return CAN_ERR_FLAG | CAN_ERR_BUSERROR
        return (self.can_id
                | (CAN_EFF_FLAG if self.extended else 0)
                | (CAN_RTR_FLAG if self.rtr else 0))

    @property
    def socketcan_payload(self):
        """can_frame data: error frames always carry the 8-byte error payload."""
        return bytes(CAN_ERR_DLC) if self.error else self.data


# Keep in sync with sampleFrames() in main.cpp.
SAMPLE_FRAMES = [
    Frame(BASE_US + 0, 1, 0x123, data=bytes.fromhex("1122334455667788")),
    Frame(BASE_US + 10_000, 1, 0x18DAF110, extended=True, rx=False, data=bytes.fromhex("AABBCC")),
    Frame(BASE_US + 20_000, 2, 0x7DF, data=bytes(4), rtr=True),
    Frame(BASE_US + 30_000, 2, 0x456, data=bytes(range(12)), fd=True, brs=True),
    Frame(BASE_US + 40_000, 1, 0x1ABC0001, extended=True, rx=False, data=bytes([0xA5] * 64), fd=True),
    Frame(BASE_US + 50_000, 1, 0x100),
    Frame(BASE_US + 60_000, 2, 0, error=True),
]


def bulk_frame(i):
    """Keep in sync with bulkFrame() in main.cpp."""
    return Frame(BASE_US + i * 1000, 1 + i % 2, 0x100 + i % 0x700, rx=(i % 3 != 0),
                 data=bytes((i * 31 + k * 7) & 0xFF for k in range(8)))


failures = []


def check(label, got, want):
    if got != want:
        failures.append(f"{label}: got {got!r}, want {want!r}")


class WarningCollector(logging.Handler):
    def __init__(self):
        super().__init__(logging.WARNING)
        self.messages = []

    def emit(self, record):
        self.messages.append(f"{record.name}: {record.getMessage()}")


warnings = WarningCollector()
for logger_name in ("can", "asammdf"):
    logging.getLogger(logger_name).addHandler(warnings)


def check_reader_warnings(label):
    if warnings.messages:
        failures.append(f"{label}: reader warnings: {warnings.messages[:5]}")
        warnings.messages.clear()


# --- python-can: candump and Vector ASC -------------------------------------

def check_python_can(label, msg, frame, *, channel, rx, ts_us):
    check(f"{label} error frame", msg.is_error_frame, frame.error)
    if not frame.error:
        check(f"{label} id", msg.arbitration_id, frame.can_id)
        check(f"{label} extended", msg.is_extended_id, frame.extended)
        check(f"{label} remote", msg.is_remote_frame, frame.rtr)
        check(f"{label} fd", msg.is_fd, frame.fd)
        if frame.fd:
            check(f"{label} brs", msg.bitrate_switch, frame.brs)
        if frame.rtr:
            check(f"{label} dlc", msg.dlc, len(frame.data))
        else:
            check(f"{label} data", bytes(msg.data), frame.data)
    if channel is not None:
        check(f"{label} channel", msg.channel, channel)
    if rx is not None:
        check(f"{label} rx", msg.is_rx, rx)
    if ts_us is not None:
        check(f"{label} timestamp", ts_us, frame.ts_us)


def validate_candump(path, frames):
    messages = list(can.CanutilsLogReader(path))
    check_reader_warnings(path)
    check(f"{path} frame count", len(messages), len(frames))
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    for i, (msg, frame) in enumerate(zip(messages, frames)):
        label = f"{path}[{i}]"
        # candump -L carries absolute timestamps and interface names, no direction.
        # python-can drops the interface name of error frames, so it is also
        # checked on the line itself.
        interface = INTERFACE_NAMES[frame.iface]
        check(f"{label} interface in line", lines[i].split()[1] if i < len(lines) else None, interface)
        check_python_can(label, msg, frame, channel=None if frame.error else interface, rx=None,
                         ts_us=round(msg.timestamp * 1_000_000))
    return len(messages)


def validate_asc(path, frames, channel_of):
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    check(f"{path} header", bool(lines) and lines[0].startswith("date "), True)
    check(f"{path} footer", lines[-1] if lines else None, "End TriggerBlock")

    messages = list(can.ASCReader(path))
    check_reader_warnings(path)
    check(f"{path} frame count", len(messages), len(frames))
    if messages and frames:
        t0 = messages[0].timestamp
        for i, (msg, frame) in enumerate(zip(messages, frames)):
            # ASC times are relative to the file's first frame; channels are 1-based
            # in the file and 0-based in python-can.
            relative = frames[0].ts_us + round((msg.timestamp - t0) * 1_000_000)
            check_python_can(f"{path}[{i}]", msg, frame, channel=channel_of[frame.iface] - 1, rx=frame.rx,
                             ts_us=relative)
    return len(messages)


# --- scapy containers, SocketCAN frames per <linux/can.h> --------------------

def check_socketcan(label, raw, frame):
    check(f"{label} frame size", len(raw), CANFD_MTU if frame.fd else CAN_MTU)
    if len(raw) not in (CAN_MTU, CANFD_MTU):
        return
    length = raw[4]
    check(f"{label} can_id", int.from_bytes(raw[0:4], "big"), frame.canid_t)
    check(f"{label} len", length, len(frame.socketcan_payload))
    if frame.fd:
        check(f"{label} flags", raw[5], CANFD_BRS if frame.brs else 0)
        check(f"{label} reserved", raw[6:8], b"\0\0")
    else:
        check(f"{label} pad/res/len8_dlc", raw[5:8], b"\0\0\0")
    check(f"{label} data", raw[8:8 + length], frame.socketcan_payload)
    check(f"{label} padding", raw[8 + length:], bytes(len(raw) - 8 - length))


def validate_pcap(path, frames):
    reader = RawPcapReader(path)
    check(f"{path} linktype", reader.linktype, LINKTYPE_CAN_SOCKETCAN)
    records = list(reader)
    reader.close()
    check(f"{path} frame count", len(records), len(frames))
    for i, ((raw, meta), frame) in enumerate(zip(records, frames)):
        check(f"{path}[{i}] timestamp", meta.sec * 1_000_000 + meta.usec, frame.ts_us)
        check(f"{path}[{i}] wirelen", meta.wirelen, len(raw))
        check_socketcan(f"{path}[{i}]", raw, frame)
    return len(records)


def read_pcapng(path):
    reader = RawPcapNgReader(path)
    records = list(reader)
    reader.close()
    return records


def check_pcapng_record(label, raw, meta, frame):
    name = meta.ifname.decode() if isinstance(meta.ifname, bytes) else meta.ifname
    check(f"{label} linktype", meta.linktype, LINKTYPE_CAN_SOCKETCAN)
    check(f"{label} interface", name, INTERFACE_NAMES[frame.iface])
    check(f"{label} tsresol", meta.tsresol, 1_000_000)
    check(f"{label} timestamp", (meta.tshigh << 32) | meta.tslow, frame.ts_us)
    check(f"{label} wirelen", meta.wirelen, len(raw))
    check_socketcan(label, raw, frame)


def validate_pcapng(path, frames):
    records = read_pcapng(path)
    check(f"{path} frame count", len(records), len(frames))
    for i, ((raw, meta), frame) in enumerate(zip(records, frames)):
        check_pcapng_record(f"{path}[{i}]", raw, meta, frame)
    return len(records)


# --- asammdf ------------------------------------------------------------------

def validate_mdf(path, frames):
    with MDF(path) as mdf:
        check(f"{path} version", mdf.version, "4.10")
        names = sorted(ch.name for group in mdf.groups for ch in group.channels)
        check(f"{path} channels", names, sorted(["t", "CAN_ID", "DLC", "Dir", "DataBytes"]))
        if names != sorted(["t", "CAN_ID", "DLC", "Dir", "DataBytes"]):
            return 0

        can_id = mdf.get("CAN_ID")
        dlc = mdf.get("DLC")
        direction = mdf.get("Dir")
        data = mdf.get("DataBytes")
        count = len(can_id.samples)
        check(f"{path} frame count", count, len(frames))
        if frames:
            start_us = round(mdf.header.start_time.timestamp() * 1_000_000)
            check(f"{path} start time", start_us, frames[0].ts_us)
        for i, frame in enumerate(frames[:count]):
            label = f"{path}[{i}]"
            check(f"{label} time", round(float(can_id.timestamps[i]) * 1_000_000), frame.ts_us - frames[0].ts_us)
            check(f"{label} CAN_ID", int(can_id.samples[i]), frame.canid_t)
            check(f"{label} DLC", int(dlc.samples[i]), len(frame.data))
            check(f"{label} Dir", int(direction.samples[i]), 0 if frame.rx else 1)
            check(f"{label} DataBytes", bytes(data.samples[i])[:len(frame.data)], frame.data)
    check_reader_warnings(path)
    return count


# --- recorder parts -----------------------------------------------------------

def validate_recorder(folder, fmt):
    ext = {"asc": "asc", "candump": "candump", "pcapng": "pcapng"}[fmt]
    parts = sorted(glob.glob(os.path.join(folder, f"*.{ext}")))
    check(f"{folder} part count >= 2", len(parts) >= 2, True)
    expected = [bulk_frame(i) for i in range(BULK_COUNT)]
    # Channel numbers are assigned by first appearance and kept across parts.
    channel_of = {1: 1, 2: 2}

    position = 0
    for path in parts:
        size = os.path.getsize(path)
        check(f"{path} size within split limit", size <= SPLIT_BYTES + 256, True)
        if fmt == "asc":
            messages = list(can.ASCReader(path))
            count = len(messages)
            validate_asc(path, expected[position:position + count], channel_of)
        elif fmt == "candump":
            count = len(list(can.CanutilsLogReader(path)))
            validate_candump(path, expected[position:position + count])
        else:
            count = len(read_pcapng(path))
            validate_pcapng(path, expected[position:position + count])
        check(f"{path} not empty", count > 0, True)
        position += count
    check(f"{folder} total frames", position, BULK_COUNT)
    return len(parts), position


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = sys.argv[1]
    sample_channels = {1: 1, 2: 2}

    for name, validate in [
        ("export.candump", lambda p, f: validate_candump(p, f)),
        ("export.asc", lambda p, f: validate_asc(p, f, sample_channels)),
        ("export.pcap", validate_pcap),
        ("export.pcapng", validate_pcapng),
        ("export.mf4", validate_mdf),
    ]:
        before = len(failures)
        count = validate(os.path.join(out, name), SAMPLE_FRAMES)
        empty_name = name.replace("export.", "empty.")
        validate(os.path.join(out, empty_name), [])
        status = "OK  " if len(failures) == before else "FAIL"
        print(f"{status} {name}: {count} frames; {empty_name}: valid empty file")

    for fmt in ("asc", "candump", "pcapng"):
        before = len(failures)
        folder = os.path.join(out, f"recorder_{fmt}")
        parts, frames = validate_recorder(folder, fmt)
        status = "OK  " if len(failures) == before else "FAIL"
        print(f"{status} recorder {fmt}: {frames} frames in {parts} parts")

    for failure in failures[:50]:
        print("  ", failure)
    if len(failures) > 50:
        print(f"   ... {len(failures) - 50} more")
    print("RESULT:", "OK" if not failures else f"{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
