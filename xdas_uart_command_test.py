#!/usr/bin/env python3
"""Build, validate, and optionally send X-DAS camera UART frames.

The live mode is intentionally conservative: it sends the version request by
default. Exposure commands are opt-in because they change camera state.
"""

from __future__ import annotations

import argparse
import os
import select
import sys
import termios
import time
from typing import Iterable, Optional, Sequence, Tuple


REQUEST_HEADER = 0xAA
RESPONSE_HEADER = 0x55
CRC_POLY = 0xD5


def crc8(data: Iterable[int]) -> int:
    """CRC-8/0xD5, initial value 0, MSB first."""
    crc = 0
    for value in data:
        crc ^= value & 0xFF
        for _ in range(8):
            crc = ((crc << 1) ^ CRC_POLY) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def request(command0: int, command1: int, payload: Sequence[int] = ()) -> bytes:
    body = bytes((REQUEST_HEADER, 0, command0, command1, *payload))
    if len(body) > 0xFF - 1:
        raise ValueError("request is too long")
    body = bytes((body[0], len(body) + 1, *body[2:]))
    return body + bytes((crc8(body),))


def format_frame(frame: bytes) -> str:
    return " ".join(f"{byte:02X}" for byte in frame)


def parse_response(frame: bytes) -> Tuple[int, int, int, bytes]:
    if len(frame) < 6 or frame[0] != RESPONSE_HEADER:
        raise ValueError("missing response header or frame too short")
    if frame[1] != len(frame):
        raise ValueError(f"length byte {frame[1]} != received {len(frame)}")
    if crc8(frame[:-1]) != frame[-1]:
        raise ValueError(
            f"bad CRC: calculated {crc8(frame[:-1]):02X}, received {frame[-1]:02X}"
        )
    return frame[2], frame[3], frame[4], frame[5:-1]


def read_response(fd: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        remaining = max(0.0, deadline - time.monotonic())
        ready, _, _ = select.select([fd], [], [], remaining)
        if not ready:
            break
        chunk = os.read(fd, 256)
        if not chunk:
            break
        received.extend(chunk)
        if len(received) >= 2:
            # Resynchronize if a USB/UART adapter delivered noise first.
            try:
                start = received.index(RESPONSE_HEADER)
            except ValueError:
                received.clear()
                continue
            if start:
                del received[:start]
            size = received[1]
            if size >= 6 and len(received) >= size:
                return bytes(received[:size])
    raise TimeoutError(f"no complete response within {timeout:.3f}s")


def write_all(fd: int, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        written = os.write(fd, data[offset:])
        if written <= 0:
            raise OSError("UART write returned no progress")
        offset += written


def configure_uart(fd: int) -> list:
    saved = termios.tcgetattr(fd)
    settings = termios.tcgetattr(fd)
    settings[0] &= ~(termios.IXON | termios.IXOFF | termios.IXANY)
    settings[1] &= ~termios.OPOST
    settings[2] &= ~(termios.CSIZE | termios.PARENB | termios.CSTOPB | termios.CRTSCTS)
    settings[2] |= termios.CS8 | termios.CLOCAL | termios.CREAD
    settings[3] &= ~(termios.ICANON | termios.ISIG | termios.IEXTEN | termios.ECHO)
    settings[4] = termios.B115200
    settings[5] = termios.B115200
    settings[6][termios.VMIN] = 0
    settings[6][termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW, settings)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return saved


def self_test() -> None:
    vectors = (
        ("version", request(0x00, 0x00), "AA 05 00 00 FF"),
        ("max-exposure-1/800", request(0x01, 0x17, (0x08,)), "AA 06 01 17 08 1D"),
        ("shutter-1/250", request(0x01, 0x19, (0x0F,)), "AA 06 01 19 0F 2B"),
        ("iso-3200", request(0x01, 0x1A, (0x06,)), "AA 06 01 1A 06 CA"),
    )
    for name, frame, expected in vectors:
        actual = format_frame(frame)
        if actual != expected:
            raise AssertionError(f"{name}: {actual} != {expected}")

    # A synthetic response checks dynamic version extraction without assuming
    # any product version string.
    fixture_payload = bytes((0x02,)) + b"fixture-version"
    fixture_body = bytes((RESPONSE_HEADER, 6 + len(fixture_payload), 0x00, 0x00, 0x00)) + fixture_payload
    fixture = fixture_body + bytes((crc8(fixture_body),))
    command0, command1, error, payload = parse_response(fixture)
    if (command0, command1, error, payload) != (0x00, 0x00, 0x00, fixture_payload):
        raise AssertionError("version response parser mismatch")
    if payload[0] != 0x02 or payload[1:].decode("ascii") != "fixture-version":
        raise AssertionError("version payload extraction mismatch")
    print("XDAS_UART_SELF_TEST_OK crc=CRC8_D5 vectors=version,max-exposure,shutter,iso response_crc=1")


def live_test(device: str, commands: Sequence[Tuple[str, bytes]], timeout: float) -> int:
    fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    saved: Optional[list] = None
    failures = 0
    try:
        saved = configure_uart(fd)
        for name, frame in commands:
            print(f"TX {name} bytes={len(frame)} frame={format_frame(frame)}")
            write_all(fd, frame)
            try:
                response = read_response(fd, timeout)
            except TimeoutError as error:
                failures += 1
                print(f"RX {name} TIMEOUT detail=\"{error}\"")
                continue
            print(f"RX {name} bytes={len(response)} frame={format_frame(response)}")
            try:
                command0, command1, error, payload = parse_response(response)
            except ValueError as error:
                failures += 1
                print(f"CHECK {name} FAIL detail=\"{error}\"")
                continue
            result = "PASS" if error == 0 else "NACK"
            print(
                f"CHECK {name} {result} cmd={command0:02X}{command1:02X} "
                f"error={error:02X} payload={format_frame(payload)}"
            )
            if name == "version" and error == 0:
                if not payload or payload[0] != 0x02:
                    failures += 1
                    print("CHECK version FAIL detail=missing type-02 payload")
                else:
                    print(f"VERSION {payload[1:].decode('ascii', errors='replace')}")
            expected_nack = name in {"udisk", "pps", "max_bad"}
            if error != 0 and not expected_nack:
                failures += 1
    finally:
        if saved is not None:
            termios.tcsetattr(fd, termios.TCSANOW, saved)
        os.close(fd)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="run offline frame/CRC checks")
    parser.add_argument("--device", help="UART device for live mode")
    parser.add_argument("--timeout", type=float, default=1.0, help="response timeout in seconds")
    parser.add_argument(
        "--suite",
        choices=("queries", "state", "all"),
        help="live command suite; queries are read-only, state changes are restored by order",
    )
    parser.add_argument("--shutter", type=lambda value: int(value, 0), metavar="SLOT", help="also send 01 19 SLOT")
    parser.add_argument("--iso", type=lambda value: int(value, 0), metavar="SLOT", help="also send 01 1A SLOT")
    args = parser.parse_args()

    if args.self_test or not args.device:
        self_test()
        if not args.device:
            return 0

    commands = [("version", request(0x00, 0x00))]
    if args.suite in ("queries", "all"):
        commands.extend(
            (
                ("mode", request(0x00, 0x02)),
                ("capacity", request(0x00, 0x03)),
                ("time", request(0x00, 0x04)),
                ("udisk", request(0x01, 0x14)),
                ("pps", request(0x01, 0x18, (1,))),
            )
        )
    if args.suite in ("state", "all"):
        commands.extend(
            (
                ("save_on", request(0x01, 0x11)),
                ("save_off", request(0x01, 0x12)),
                ("uvc", request(0x01, 0x15)),
                ("max_ok", request(0x01, 0x17, (8,))),
                ("max_bad", request(0x01, 0x17, (9,))),
                ("shutter_manual", request(0x01, 0x19, (0x0F,))),
                ("shutter_auto", request(0x01, 0x19, (0,))),
                ("iso_manual", request(0x01, 0x1A, (6,))),
                ("iso_auto", request(0x01, 0x1A, (0,))),
            )
        )
    if args.shutter is not None:
        if not 0 <= args.shutter <= 0xFF:
            parser.error("--shutter must be 0..255")
        commands.append(("shutter", request(0x01, 0x19, (args.shutter,))))
    if args.iso is not None:
        if not 0 <= args.iso <= 0xFF:
            parser.error("--iso must be 0..255")
        commands.append(("iso", request(0x01, 0x1A, (args.iso,))))
    return 1 if live_test(args.device, commands, args.timeout) else 0


if __name__ == "__main__":
    raise SystemExit(main())
