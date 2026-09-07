# RV1126B Runtime Health Logging

`camera_aiq_test` writes runtime messages to standard output and standard
error. The `S60pcl-camera` init script redirects both streams to
`/var/log/pcl-camera.log`.

For every daemon mode (`--capture-daemon`, `--daemon`, `--uvc-daemon`, or
`--all-daemon`), the program writes `RUNTIME_HEALTH` once at startup and then
every 30 seconds for each configured camera. The record contains:

- RKAIQ online/start state, AIQ error code, and selected IQ directory.
- Capture running state, frame count, measured frame rate, sequence drops, save
  queue drops, save failures, errno, and the failure stage.
- UVC enable/host-stream state and errors.
- HTTP output enable/client count and errors.
- JPEG/EXIF save enable/count and errors.

`health=DEGRADED` identifies the failed subsystem in `reason=`. In particular,
`frame_stalled` means a previously running capture produced no additional frame
during the 30-second health interval. Immediate V4L2 failures are emitted as
`CAPTURE_STREAM_ERROR` with one of `open`, `querycap`, `capability`,
`set_format`, `request_buffers`, `query_buffer`, `mmap`, `queue_buffer`,
`stream_on`, `poll`, `poll_revents`, `dqbuf`, `dqbuf_index`, or `qbuf`.

The interactive `capture-status all` command exposes the same current capture
failure in `last_errno` and `last_error_stage`.

## Dual Camera IQ Audit

The program creates one RKAIQ context per camera and enables Rockchip
multi-camera concurrency when `--camera-count 2` is selected. It also owns one
V4L2 capture thread per camera. This is software-level parallel capture, not a
single AIQ context time-shared between cameras.

At startup, `CAMERA_RUNTIME_CONFIG` and `CAMERA_IQ_LAYOUT` state whether the
two IQ paths are `per_camera` or `shared_read_only`. A shared path is valid only
for the same immutable tuning file; it is not independent tuning. The current
RV1126B overlay contains only one IQ JSON under `/etc/iqfiles` and its default
configuration uses `PCL_CAMERA_COUNT=1`. Do not claim two-camera production
stability until the second sensor, its independent `/etc/iqfiles/cam1` tuning
file, V4L2/params nodes, and both 30-second health records have been verified
on the board.

Suggested board check after configuring two cameras:

```sh
tail -f /var/log/pcl-camera.log
grep 'RUNTIME_HEALTH' /var/log/pcl-camera.log | tail -n 4
```

Do not launch a second `camera_aiq_test` process while the service owns both
RKAIQ contexts. Use the existing control interface to request status when one
is configured, or inspect the daemon health records above.

For a stable two-camera run, both records must remain `health=OK`, frame counts
must increase on both cameras, and `sequence_drops`, `save_queue_drops`, and
`last_errno` must remain unchanged at zero.
