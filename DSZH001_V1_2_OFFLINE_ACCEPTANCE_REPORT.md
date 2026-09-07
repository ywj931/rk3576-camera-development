# DSZH001 v1.2 Offline Acceptance Report

Date: 2026-08-31

Scope: offline regression only. No serial device was opened, no board was
connected, no system time was changed, and no reboot was requested.

## Command Matrix

| Command | Offline result | Assertions exercised |
| --- | --- | --- |
| `00 00` version | PASS | Exact type/string payload, ACK CRC, unexpected payload NACK. |
| `00 02` mode | PASS | Exact mode payload, ACK CRC, unexpected payload NACK. |
| `00 03` capacity | PASS | Exact LE payload, unexpected payload NACK, injected backend failure NACK. |
| `00 04` time query | PASS | Exact type/timestamp payload and unexpected payload NACK. |
| `01 04` reboot | PASS (mock) | Scheduling callback success, invalid payload NACK, injected scheduling failure NACK. No reboot was performed. |
| `01 11` save start | PASS (mock) | Two-camera callback execution, invalid camera NACK, camera-1 failure rollback and NACK. |
| `01 12` save stop | PASS (mock) | Two-camera stop callback execution and injected backend failure NACK. |
| `01 14` UDISK | PASS (unsupported by design) | `0xff` response and CRC verified; no UVC-stop inference. |
| `01 15` UVC | PASS (mock) | All-camera callback target, invalid camera NACK, injected backend failure NACK. |
| `01 16` set time | PASS (mock) | Strict timestamp payload, callback value, invalid-length NACK, injected failure NACK. No host/board clock was changed. |
| `01 17` max exposure | PASS (mock) | Slot 8 callback, empty/out-of-range payload NACK, injected backend failure NACK. |
| `01 18` PPS frequency | PASS (unsupported by design) | `0xff` response and CRC verified; no XVS retiming inference. |
| `01 19` shutter extension | PASS (mock) | Callback slot, empty/out-of-range NACK, injected backend failure NACK. |
| `01 1A` ISO extension | PASS (mock) | Callback slot, empty/out-of-range NACK, injected backend failure NACK. |

Every valid matrix frame verified response header, response length, echoed command,
error code, response CRC-8/0xD5, and successful payload. The PTY test verifies
fragmented input, repeated requests, a bad-CRC NACK, and post-error recovery;
the frame parser therefore provides the shared CRC/error-path coverage for all
commands.

## Executed Regression

| Test | Result |
| --- | --- |
| `check-xdas-uart` | PASS: command matrix, exact reference vectors, mock callbacks, save rollback, PTY recovery. |
| `check-control-uart-host`, `check-xvs-uart` | PASS. |
| `check-time-sync`, `check-gnss-time` | PASS. |
| `check-stage6-host`, `check-stage7-host`, `check-imx586-v4l2-metadata` | PASS. |
| `check-camera-transfer` | PASS. |
| ARM64 clean full build | PASS: full `camera_aiq_test` linked with the Buildroot ARM64 toolchain in a temporary directory. Existing ATK UVC warnings only. |
| QEMU ARM64 `xdas_camera_protocol_test` | PASS. |
| QEMU ARM64 `camera_aiq_test --xdas-protocol-self-test` | PASS. QEMU reports no MPP device-tree SoC, expected without hardware. |
| `check-xdas-save-rockchip` | BLOCKED: host build lacks `rk_mpi.h`; it was not claimed as passed. |

## Source Consistency

The changed upper files and their `source-overrides/camera_uart` counterparts
compare byte-for-byte: service, backend, runtime binding, protocol test, and
implementation matrix. The overlay test also passed under ARM64 QEMU.

`sdk` is empty. No `update.img` was generated.

## Hardware-Only Verification

The UART integration branch must verify these on the target, and this report
does not mark them as done: physical UART framing at 115200 8N1; real UVC gadget
start and dual-camera rollback; JPEG save and storage error propagation; RKAIQ
maximum-exposure application on both sensors; privilege-gated `clock_settime`;
reboot ACK-before-reboot timing; and PPS/UDISK behavior remaining unsupported.
