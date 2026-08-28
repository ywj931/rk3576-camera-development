#!/usr/bin/env python3

import argparse
import base64
import binascii
import hashlib
import os
import secrets
import select
import sys
import termios
import time
from pathlib import Path


BAUD_RATE = 1_500_000
DEFAULT_TIMEOUT_SECONDS = 600


def configure_serial(fd: int) -> None:
    speed = getattr(termios, "B1500000", None)
    if speed is None:
        raise RuntimeError("host termios does not support 1500000 baud")

    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attrs[3] = 0
    attrs[4] = speed
    attrs[5] = speed
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


def build_remote_command(token: str) -> bytes:
    begin = f"__PCL_BEGIN_{token}__"
    end = f"__PCL_END_{token}__"
    error = f"__PCL_ERROR_{token}__"
    command = (
        "dmesg -n 1 2>/dev/null || true; "
        "f0=$(ls -t /userdata/dualshot/*cam0.jpg 2>/dev/null | head -1); "
        "f1=$(ls -t /userdata/dualshot/*cam1.jpg 2>/dev/null | head -1); "
        "for f in \"$f0\" \"$f1\"; do "
        f"if [ ! -f \"$f\" ]; then printf '\\n{error}|missing_jpeg\\n'; continue; fi; "
        "name=$(basename \"$f\"); size=$(wc -c < \"$f\"); "
        "sha=$(sha256sum \"$f\" | cut -d' ' -f1); "
        f"printf '\\n{begin}|%s|%s|%s\\n' \"$name\" \"$size\" \"$sha\"; "
        "base64 \"$f\"; "
        f"printf '{end}\\n'; "
        "done; dmesg -n 7 2>/dev/null || true"
    )
    return ("\n" + command + "\n").encode("ascii")


def receive_files(
    fd: int, output_dir: Path, token: str, timeout_seconds: int
) -> list[Path]:
    begin = f"__PCL_BEGIN_{token}__|"
    end = f"__PCL_END_{token}__"
    error = f"__PCL_ERROR_{token}__|"
    deadline = time.monotonic() + timeout_seconds
    line_buffer = bytearray()
    output = None
    output_path = None
    temporary_path = None
    expected_size = 0
    expected_sha = ""
    actual_size = 0
    digest = hashlib.sha256()
    received: list[Path] = []

    def abort_partial() -> None:
        nonlocal output
        if output is not None:
            output.close()
            output = None
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass

    while len(received) < 2:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            abort_partial()
            raise TimeoutError("serial photo transfer timed out")

        readable, _, _ = select.select([fd], [], [], min(1.0, remaining))
        if not readable:
            continue
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            continue
        if not chunk:
            continue
        line_buffer.extend(chunk)

        while b"\n" in line_buffer:
            raw_line, _, rest = line_buffer.partition(b"\n")
            line_buffer = bytearray(rest)
            line = raw_line.rstrip(b"\r").decode("ascii", errors="ignore")

            if output is None:
                if line.startswith(error):
                    raise FileNotFoundError(
                        "board did not find both cam0/cam1 JPEG files"
                    )
                if not line.startswith(begin):
                    continue
                fields = line.split("|", 3)
                if len(fields) != 4:
                    raise ValueError("invalid serial file header")
                file_name = os.path.basename(fields[1])
                if not file_name or not file_name.lower().endswith(".jpg"):
                    raise ValueError("invalid incoming JPEG file name")
                expected_size = int(fields[2].strip())
                expected_sha = fields[3].strip().lower()
                if expected_size <= 0 or len(expected_sha) != 64:
                    raise ValueError("invalid incoming file metadata")

                output_path = output_dir / file_name
                temporary_path = output_dir / f".{file_name}.part"
                output = temporary_path.open("wb")
                actual_size = 0
                digest = hashlib.sha256()
                print(
                    f"RECEIVING name={file_name} size={expected_size} "
                    f"sha256={expected_sha}",
                    flush=True,
                )
                continue

            if line == end:
                output.flush()
                os.fsync(output.fileno())
                output.close()
                output = None
                actual_sha = digest.hexdigest()
                if actual_size != expected_size or actual_sha != expected_sha:
                    temporary_path.unlink(missing_ok=True)
                    raise ValueError(
                        "file verification failed: "
                        f"size={actual_size}/{expected_size} "
                        f"sha256={actual_sha}/{expected_sha}"
                    )
                temporary_path.replace(output_path)
                received.append(output_path)
                print(
                    f"RECEIVED_OK path={output_path} size={actual_size} "
                    f"sha256={actual_sha}",
                    flush=True,
                )
                output_path = None
                temporary_path = None
                continue

            try:
                decoded = base64.b64decode(line, validate=True)
            except binascii.Error as exc:
                abort_partial()
                raise ValueError(f"invalid Base64 data: {exc}") from exc
            output.write(decoded)
            digest.update(decoded)
            actual_size += len(decoded)

    return received


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Receive the latest cam0/cam1 JPEGs from an already logged-in "
            "RV1126B shell over its 1.5 Mbaud console UART."
        )
    )
    parser.add_argument("serial_device", help="host serial device, e.g. /dev/ttyUSB0")
    parser.add_argument("output_dir", type=Path)
    parser.add_argument(
        "--timeout", type=int, default=DEFAULT_TIMEOUT_SECONDS, help="seconds"
    )
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    token = secrets.token_hex(8)
    fd = os.open(
        args.serial_device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK
    )
    try:
        configure_serial(fd)
        os.write(fd, build_remote_command(token))
        files = receive_files(fd, args.output_dir, token, args.timeout)
    finally:
        os.close(fd)

    print(f"SERIAL_TRANSFER_OK files={len(files)}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        print(f"SERIAL_TRANSFER_FAILED reason={exc}", file=sys.stderr)
        raise SystemExit(1)
