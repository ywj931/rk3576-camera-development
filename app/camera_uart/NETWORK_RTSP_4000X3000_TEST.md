# RK3576 dual-camera network output

## 1. Current implementation

`camera_aiq_test` now has a network output backend in addition to the existing
UVC backend.  It consumes the camera0/camera1 NV12 callbacks, gives each camera
an independent one-frame queue and RK MPP H.264 encoder, and publishes both
sessions through one SDK `librtsp.a` server.

```text
camera0 V4L2 NV12 4000x3000
        |
        +--> UVC MJPEG 4000x3000@10fps
        |
        +--> MPP H.264 CBR 12Mbps 4000x3000@10fps
                         |
                         +--> RTSP :8554 /cam0

camera1 V4L2 NV12 4000x3000
        |
        +--> MPP H.264 CBR 12Mbps 4000x3000@10fps
                         |
                         +--> RTSP :8554 /cam1
```

The network backend does not write sensor registers and does not replace
RKAIQ.  Exposure, gain, IQ and capture remain in the existing camera chain.
The USB-to-Ethernet/RNDIS interface only supplies the IP path; the program
does not bind to a hard-coded interface name.

The capture node normally supplies about 30 fps.  MPP's `rc:fps_in_*` and
`rc:fps_out_*` fields configure rate control but do not discard application
input frames, so the backend also performs explicit application pacing.  It
encodes at most one newest frame per 100 ms and drops superseded frames from
the one-frame queue.  MPP PTS uses a continuous output-frame index rather than
the V4L2 sequence, because the selected input sequence normally advances by
about three for every 10 fps output frame.

4000x3000 requires a 16-line-aligned MPP input buffer (the aligned height is
3008).  The backend copies each captured Y and UV row into that stride before
calling MPP.  The H.264 level is set to 6 because 4000x3000 exceeds level
5.2's maximum frame-size macroblock count.

## 2. Build

On the SDK host:

```sh
cd /home/ywj/rk3576_sdk/TaishanPi-3-Linux/app/camera_uart
make -f Makefile.camera_aiq aarch64
file camera_aiq_test
```

The expected architecture is `ARM aarch64`.  The Makefile links the SDK RTSP
library from `external/common_algorithm/misc/lib/.../librtsp.a`.

## 3. Board-side start sequence

Copy the resulting executable to the board using the same deployment method
already used for `camera_aiq_test`, then make it executable.  Keep the existing
IQ directories and camera node arguments unchanged.  Start the program, and
in its command input use:

```text
stream-start all
net-start 0
net-start 1
net-status all
```

The output prints the URL templates `rtsp://<board-ip>:8554/cam0` and
`rtsp://<board-ip>:8554/cam1`.  The corresponding capture must be running
before `net-start CAMERA_ID`; starting network output alone does not open a
camera device.

To stop the stream without stopping camera capture:

```text
net-stop all
```

`status all` includes one `NET_STATUS` line per camera.

## 4. USB-to-Ethernet/RNDIS network check

On the board, find the address of the USB network interface (the name may be
`usb0`, `eth1`, or `enx...`):

```sh
ip -br addr
ip route
ss -lntp | grep 8554
```

Use that address in the RTSP URL.  For example, if the board address is
`192.168.55.1`:

```text
rtsp://192.168.55.1:8554/cam0
```

The host and board must be on the same USB-Ethernet/RNDIS link.  First verify
the link with `ping`; an RTSP failure caused by missing IP configuration is not
an encoder failure.

## 5. Host-side playback and acceptance checks

With `ffprobe` (replace the path with `cam1` for camera1):

```sh
ffprobe -rtsp_transport tcp rtsp://192.168.55.1:8554/cam0
```

With FFmpeg, save ten seconds without re-encoding:

```sh
ffmpeg -rtsp_transport tcp -i rtsp://192.168.55.1:8554/cam0 \
  -an -t 10 -c:v copy /tmp/cam0_rtsp.mkv
```

The acceptance conditions for this first network-output step are:

1. `net-start CAMERA_ID` returns `OK` and the corresponding
   `server_running=1`.
2. The corresponding `NET_STATUS submitted`, `encoded`, and `sent` increase
   while capture runs.
3. `encode_errors=0` and `rtsp_errors=0` during a ten-second test.
4. `ffprobe` reports H.264, 4000x3000, approximately 10 fps.
5. The saved ten-second file can be decoded on the host.

`queue_drops` can be non-zero on a slow board because the queue is deliberately
latest-frame-only; this keeps network output from blocking camera capture.

## 6. Verified board result (2026-07-24)

The camera0-only paced build tested at that stage was installed as
`/root/camera_uart/camera_aiq_test`.  Its SHA-256 was
`505bbf3d6a3f313075d01088523270924ec84adbcecbf8a61cec499c5b0e8323`.

- A ten-second sample added about 300 capture submissions and 99 encoded/sent
  H.264 frames.  This verifies the explicit 10 fps pacing.
- A longer run reached `frames=4227`, `sequence_drops=0`, `encoded=1377`,
  `sent=1376`, `encode_errors=0`, and `rtsp_errors=0`.
- A board-local GStreamer client completed RTSP OPTIONS, DESCRIBE, SETUP and
  PLAY over TCP, decoded the H.264 stream, and produced a valid 4000x3000 JPEG.
- Disconnecting the first client originally delivered `SIGPIPE` from
  `librtsp` and terminated the complete camera process.  The program now
  ignores `SIGPIPE`; two later client disconnects were logged as `peer closed`
  while camera capture and port 8554 remained running.
- `net-stop`, `stream-stop 0`, and `quit` all completed.  The original
  `rkaiq_3A.service` was restored to the active state after testing.

The decoded test image and logs are stored under
`test_results/20260724/network_rtsp/`.

The external PC-to-board network test is still blocked by the physical link,
not by RTSP.  On this test setup the board had `usb0=192.168.55.1/24`, but the
interface was `DOWN,NO-CARRIER`; the PC did not enumerate a USB Ethernet/RNDIS
interface.  PC ping had 100 percent loss and TCP port 8554 timed out.  The
board also had no entries in `/sys/bus/usb/devices`.  End-to-end acceptance
from section 5 must be repeated after the USB Ethernet device or RNDIS cable
actually enumerates and both ends show a carrier.

## 7. Camera1 and dual-stream board result (2026-07-24)

Camera1 and simultaneous camera0/camera1 output have now also been verified on
the board.  The tested capture mapping is camera0 `/dev/video22` and camera1
`/dev/video31`; the earlier `/dev/video-camera0` alias incorrectly resolved to
camera1 on this boot, and no `/dev/video-camera1` alias existed.

- Camera1 alone produced H.264 High Profile, 4000x3000 at 10 fps.  A five-second
  TCP RTSP probe received 51 packets, and FFmpeg decoded a valid 4000x3000 JPEG.
- With both encoders running, simultaneous TCP probes of `/cam0` and `/cam1`
  each received 51 packets in five seconds.  Both network status records kept
  `encode_errors=0` and `rtsp_errors=0`.
- `net-stop 1` removed only `/cam1`; `/cam0` remained available and delivered
  another 31 packets during a three-second probe.

The detailed result and decoded camera1 image are under
`test_results/20260724/network_rtsp_cam1/`.  The later UART command layer should
call the network start, stop and status APIs rather than writing sensor
registers.

The final dual-stream build, including the verified default capture nodes, is
installed at `/root/camera_uart/camera_aiq_test`.  Its SHA-256 is
`eeb96a37903dc9fb6ff81dcab3724e32d436822be4c96a098604d3588bd4d35c`.
