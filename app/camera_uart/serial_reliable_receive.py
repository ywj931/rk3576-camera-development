#!/usr/bin/env python3
"""Receive a file reliably on a Rockchip FIQ console UART."""
import argparse
import hashlib
import os
import select
import struct
import termios
import time
import zlib

HELLO = b"RFH1"
PACKET = b"RFP1"
END = b"RFE1"
ESCAPE = 0x7D
FIQ_TRIGGER_LAST = ord("q")


def configure_tty(fd, baud):
    speed = getattr(termios, "B%d" % baud)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = speed | termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[3] = 0
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)


def write_all(fd, data):
    view = memoryview(data)
    while view:
        view = view[os.write(fd, view):]


def read_raw(fd, timeout):
    if not select.select([fd], [], [], timeout)[0]:
        raise TimeoutError
    data = os.read(fd, 1)
    if not data:
        raise EOFError
    return data[0]


def read_wire_exact(fd, count, timeout=30.0):
    """Decode the sender's byte stuffing while reading exactly count bytes."""
    result = bytearray()
    deadline = time.monotonic() + timeout
    while len(result) < count:
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError
        byte = read_raw(fd, left)
        if byte == ESCAPE:
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError
            byte = read_raw(fd, left) ^ 0x20
        result.append(byte)
    return bytes(result)


def find_magic(fd, allowed):
    window = bytearray()
    while True:
        window.extend(read_wire_exact(fd, 1))
        if len(window) > 4:
            del window[0]
        if len(window) == 4 and bytes(window) in allowed:
            return bytes(window)


def receive_file(path, baud):
    fd = 0
    original_tty = termios.tcgetattr(fd)
    temporary = path + ".part"
    configure_tty(fd, baud)
    termios.tcflush(fd, termios.TCIOFLUSH)
    try:
        while True:
            find_magic(fd, (HELLO,))
            size = struct.unpack("<Q", read_wire_exact(fd, 8))[0]
            expected_hash = read_wire_exact(fd, 32)
            if size > 512 * 1024 * 1024:
                continue
            write_all(fd, b"H")
            break

        expected_seq = 0
        received = 0
        with open(temporary, "wb") as output:
            while True:
                magic = find_magic(fd, (PACKET, END))
                if magic == PACKET:
                    seq, length, crc = struct.unpack("<IHI", read_wire_exact(fd, 10))
                    if length > 1024:
                        write_all(fd, b"N" + struct.pack("<I", expected_seq))
                        continue
                    payload = read_wire_exact(fd, length)
                    if seq < expected_seq:
                        write_all(fd, b"A" + struct.pack("<I", seq))
                        continue
                    if seq != expected_seq or zlib.crc32(payload) != crc:
                        write_all(fd, b"N" + struct.pack("<I", expected_seq))
                        continue
                    output.write(payload)
                    received += length
                    expected_seq += 1
                    write_all(fd, b"A" + struct.pack("<I", seq))
                    continue

                end_seq, end_size = struct.unpack("<IQ", read_wire_exact(fd, 12))
                if end_seq != expected_seq or end_size != size or received != size:
                    write_all(fd, b"N" + struct.pack("<I", expected_seq))
                    continue
                output.flush()
                os.fsync(output.fileno())
                break

        digest = hashlib.sha256()
        with open(temporary, "rb") as source:
            for data in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(data)
        actual_hash = digest.digest()
        if actual_hash != expected_hash:
            raise RuntimeError("final sha256 mismatch")
        os.replace(temporary, path)
        write_all(fd, b"E")
        print("\nRFT_RECEIVE_DONE size=%d sha256=%s path=%s" %
              (size, actual_hash.hex(), path), flush=True)
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, original_tty)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("path")
    parser.add_argument("--baud", type=int, default=1500000)
    args = parser.parse_args()
    receive_file(args.path, args.baud)


if __name__ == "__main__":
    main()
