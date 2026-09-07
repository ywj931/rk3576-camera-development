# RV1126B Camera Application

This directory contains the RV1126B camera application and its RK3588
transfer peer.

The main runtime is `camera_aiq_test`. It supports the RV1126B camera backend,
X-DAS UART control, GNSS/GPRMC plus PPS time synchronization, dual-camera
capture, vertical JPEG stitching, and reliable transfer to RK3588.

## RV1126B to RK3588 transfer

The photo path is USB CDC-NCM followed by TCP:

```text
RV1126B usb0: 192.168.77.2  -- USB CDC-NCM -->  RK3588: 192.168.77.1:46000
```

NCM provides the IP link. The application protocol is documented in
`NCM_TCP_PHOTO_TRANSFER_GUIDE.md` and
`CAMERA_USB_TRANSFER_PROTOCOL.md`.

Each photo transfer is:

```text
80-byte DIMG header + JPEG payload + 24-byte DACK acknowledgement
```

The sender deletes a local JPEG only after a matching `DACK/OK` with valid
length and SHA-256. Failed transfers remain queued for retry.

## Build

Build using the RV1126B SDK toolchain:

```sh
make -f Makefile.camera_aiq aarch64-package
make -f Makefile.camera_aiq check-xdas-uart
```

The full SDK overlay is required for the final image. NCM kernel/gadget
configuration is outside this application directory; see the NCM guide for
the required SDK files and addresses.

## Important files

- `camera_aiq_test.cpp`: RV1126B application entry and runtime commands.
- `camera_backend.cpp`, `capture_backend.cpp`: camera and V4L2 capture.
- `camera_photo_backend.cpp`: pair, JPEG, metadata, and transfer queue.
- `camera_transfer_client.cpp`: RV1126B TCP sender.
- `camera_transfer_receiver.cpp`: RK3588 TCP receiver.
- `stitch_transfer_worker.cpp`: retry and offline backlog recovery.
- `gnss_time_source.cpp`: GPRMC/PPS synchronization state machine.
- `xdas_camera_protocol.cpp`: X-DAS UART command protocol.
