#!/usr/bin/env python3
"""Send a file reliably over a Linux UART using CRC32, ACK and retries."""
import argparse
import hashlib
import os
import select
import struct
import sys
import termios
import time
import zlib

HELLO = b"RFH1"
PACKET = b"RFP1"
END = b"RFE1"
CHUNK = 1024
RETRIES = 20
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

def write_wire(fd, data):
    """Escape every raw 'q' so Rockchip's serial FIQ detector cannot see 'fiq'."""
    encoded = bytearray()
    for byte in data:
        if byte in (ESCAPE, FIQ_TRIGGER_LAST):
            encoded.append(ESCAPE)
            encoded.append(byte ^ 0x20)
        else:
            encoded.append(byte)
    write_all(fd, encoded)

def read_exact(fd, count, timeout):
    out = bytearray()
    deadline = time.monotonic() + timeout
    while len(out) < count:
        left = deadline - time.monotonic()
        if left <= 0 or not select.select([fd], [], [], left)[0]:
            raise TimeoutError
        data = os.read(fd, count - len(out))
        if data:
            out.extend(data)
    return bytes(out)

def wait_reply(fd, wanted, seq=None, timeout=4.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            code = read_exact(fd, 1, deadline - time.monotonic())
        except TimeoutError:
            return False
        if code not in (b"H", b"A", b"N", b"E"):
            continue
        if code in (b"A", b"N"):
            try:
                number = struct.unpack("<I", read_exact(fd, 4, 1.0))[0]
            except TimeoutError:
                continue
            if seq is not None and number != seq:
                continue
            return code == wanted
        if code == wanted:
            return True
    return False

def send_file(device, path, baud, skip_handshake=False):
    size = os.path.getsize(path)
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for data in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(data)
    expected = digest.digest()
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY)
    try:
        configure_tty(fd, baud)
        termios.tcflush(fd, termios.TCIOFLUSH)
        if not skip_handshake:
            hello = HELLO + struct.pack("<Q", size) + expected
            for _ in range(RETRIES):
                write_wire(fd, hello)
                if wait_reply(fd, b"H"):
                    break
            else:
                raise RuntimeError("receiver did not acknowledge handshake")
        sent = 0
        seq = 0
        next_report = 1024 * 1024
        with open(path, "rb") as source:
            for data in iter(lambda: source.read(CHUNK), b""):
                packet = PACKET + struct.pack("<IHI", seq, len(data), zlib.crc32(data)) + data
                for _ in range(RETRIES):
                    write_wire(fd, packet)
                    if wait_reply(fd, b"A", seq):
                        break
                else:
                    raise RuntimeError("packet %d failed after retries" % seq)
                sent += len(data)
                seq += 1
                if sent >= next_report or sent == size:
                    print("RFT_SEND %d/%d bytes (%.1f%%)" %
                          (sent, size, sent * 100.0 / size), flush=True)
                    next_report += 1024 * 1024
        trailer = END + struct.pack("<IQ", seq, size)
        for _ in range(RETRIES):
            write_wire(fd, trailer)
            if wait_reply(fd, b"E", timeout=10.0):
                print("RFT_SEND_DONE size=%d sha256=%s" %
                      (size, expected.hex()), flush=True)
                return
        raise RuntimeError("receiver did not confirm final checksum")
    finally:
        os.close(fd)

def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--break":
        fd = os.open(sys.argv[2], os.O_RDWR | os.O_NOCTTY)
        termios.tcsendbreak(fd, 0)
        os.close(fd)
        return
    if len(sys.argv) == 3 and sys.argv[1] == "--abort-receive":
        fd = os.open(sys.argv[2], os.O_RDWR | os.O_NOCTTY)
        configure_tty(fd, 1500000)
        termios.tcflush(fd, termios.TCIOFLUSH)
        write_all(fd, END + struct.pack("<IQ", 0, 0))
        time.sleep(1.0)
        os.close(fd)
        return
    if len(sys.argv) == 3 and sys.argv[1] == "--probe-nack":
        fd = os.open(sys.argv[2], os.O_RDWR | os.O_NOCTTY)
        configure_tty(fd, 1500000)
        data = read_exact(fd, 5, 15.0)
        print("UART_RX_HEX=" + data.hex())
        if data[:1] in (b"A", b"N"):
            print("UART_REPLY code=%s seq=%d" % (data[:1].decode(), struct.unpack("<I", data[1:])[0]))
        os.close(fd)
        return

    parser = argparse.ArgumentParser()
    parser.add_argument("device")
    parser.add_argument("file")
    parser.add_argument("--baud", type=int, default=1500000)
    parser.add_argument("--skip-handshake", action="store_true")
    args = parser.parse_args()
    send_file(args.device, args.file, args.baud, args.skip_handshake)

if __name__ == "__main__":
    main()
