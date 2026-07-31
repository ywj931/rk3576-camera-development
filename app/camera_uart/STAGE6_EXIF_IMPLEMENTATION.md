# Stage 6: Trigger-bound JPEG and EXIF

## Scope

Stage 6 is implemented in the existing camera_aiq_test executable. It does not add
a second board-side control application and does not change the UART protocol.

The data path is:

```text
V4L2 DQBUF -> trigger_frame_binder -> frame event callback
      -> asynchronous NV12 copy -> MPP JPEG encoder -> EXIF APP1 insertion
      -> atomic JPEG write + stage6_metadata.csv
```

A JPEG is accepted only when its v4l2_buffer.sequence has a trigger/frame binding.
Frames without a binding are counted as unbound_frames and are not saved.

## Commands

```text
photo-start CAMERA_ID OUTPUT_DIR
photo-stop CAMERA_ID
photo-status [all|0|1]
photo-offset CAMERA_ID SENSOR_RESPONSE_OFFSET_NS
```

photo-offset is the calibrated delay from a trigger edge to sensor exposure start.
Use zero only for the software-only simulator test. The actual value must be measured
with the real XVS source and an oscilloscope or logic analyzer.

## Software-only 2 Hz validation

This does not drive FSYNC_CAM. It validates the queueing, V4L2 sequence lookup,
JPEG encoding and EXIF/CSV creation while the cameras free-run.

```text
stream-start all
photo-offset 0 0
photo-offset 1 0
photo-start 0 /root/stage6/cam0
photo-start 1 /root/stage6/cam1
sync-bind-reset 1
sync-sim-start 2 20
wait 12000
sync-sim-stop
photo-stop 0
photo-stop 1
photo-status all
sync-bind-last
stream-stop all
```

Expected result: each camera writes trigger-bound JPEG files and one
stage6_metadata.csv. The frame counts need not be exactly 20 in simulator mode:
the simulator is not a physical trigger and binds the next available free-run frame.

## JPEG metadata

Each file contains TIFF/EXIF APP1 fields:

- DateTimeOriginal and SubSecTimeOriginal from the trigger realtime clock.
- ExposureTime from the latest RKAIQ exposure result.
- PhotographicSensitivity from the latest RKAIQ ISO result; an estimated ISO is
  marked in UserComment if unavailable.
- UserComment with camera ID, V4L2 frame sequence, trigger ID, monotonic/realtime
  trigger time, estimated exposure start/center, gain, ISO, response offset and
  source-quality flags.

Current formulas:

```text
exposure_start_realtime_ns = trigger_realtime_ns + sensor_response_offset_ns
exposure_center_realtime_ns = exposure_start_realtime_ns + exposure_us / 2
```

The current RKAIQ API supplies the latest applied exposure state, not a guaranteed
per-frame exposure record. This is explicitly recorded as
RKAIQ_LATEST_NOT_FRAME_BOUND in the metadata and must not be represented as final
per-frame exposure proof.

A simulator trigger has no UTC/PPS origin, so utc_valid=0 even though the EXIF fields
contain the Linux realtime clock. A future MCU UART PPS message must set source to
MCU_PPS and provide a valid realtime/PPS timestamp before DateTimeOriginal is treated
as an absolute, traceable capture time.

## Host-only checks

From this directory:

```bash
make -f Makefile.camera_aiq aarch64
make -f Makefile.camera_aiq check-stage6-host
```

The host check validates EXIF tag insertion and trigger-binder lookup using no camera,
UART, MPP device or board connection.

## Hardware acceptance still required

Stage 6 software validation is not physical synchronization acceptance. Before
release, complete all of the following with the real MCU:

1. Drive both IMX586 XVS pins from the common 1.8 V, idle-high, low-active MCU XVS.
2. Send matching trigger ID and PPS/NMEA time from the MCU over UART.
3. Measure XVS, camera 0 and camera 1 timing with an oscilloscope; calibrate each
   photo-offset from the measured response delay.
4. Prove one trigger yields one frame on each camera, with no duplicates or drops,
   over at least 1000 pulses at the required 2 Hz rate.
5. Bind trigger IDs to both frame sequences and report maximum/P99 camera-to-camera
   skew. Replace RKAIQ_LATEST_NOT_FRAME_BOUND when a per-frame exposure source is
   available.

Only after these checks can the EXIF timestamps and exposure timing be used as the
final camera data record.
