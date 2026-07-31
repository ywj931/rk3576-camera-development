# Minimal camera UART service

This service is the first userspace layer of the RK3576 camera UART control
path. It does not change the IMX586 driver or start a second RKAIQ context.

## Current UART settings

- Device: /dev/ttyS9
- Baud rate: 115200
- Data bits: 8
- Parity: none
- Stop bits: 1
- Flow control: none

## Build and test

Build on the RK3576 board:

    make -f Makefile.camera_uartd

Cross-build a deployable ARM64 executable on the SDK host:

    make -f Makefile.camera_uartd aarch64

Run the protocol self-test without opening the UART:

    make -f Makefile.camera_uartd check

The self-test rebuilds `camera_uartd` for the current SDK host before running
it.  Run the `aarch64` target again afterwards when preparing a board image.

Test the same parser through stdin/stdout:

    printf '$CAM,1,255,PING\n$CAM,2,0,GET_STATUS\n' | ./camera_uartd --stdio

Expected output:

    $ACK,1,255,PONG
    $ACK,2,0,GET_STATUS,UART=READY,CAMERA_BACKEND=NOT_CONNECTED

Run the service:

    ./camera_uartd

Use another UART device when a controller becomes available. Do not leave the
ttyS9 TX and RX pins physically shorted while camera_uartd is running, because
the service would receive its own response.

## Protocol version 0

Each command is one ASCII line terminated by LF or CRLF:

    $CAM,<sequence>,<camera_id>,<command>

Supported commands:

    $CAM,1,255,PING
    $CAM,2,0,GET_STATUS
    $CAM,3,1,GET_STATUS

camera_id 0 and 1 select the two cameras. camera_id 255 is the global ID and is
currently valid only for PING.

The camera backend is deliberately not connected in this version. GET_STATUS
reports CAMERA_BACKEND=NOT_CONNECTED instead of returning a fake online state.
CRC and the final external-controller framing should be added after the
controller-side protocol has been agreed.
