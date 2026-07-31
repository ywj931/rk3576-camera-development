#!/bin/sh

# Sourced by /usr/bin/usbdevice from /etc/usbdevice.d.
# Keep the existing USB network link available while adding two UVC outputs.
export USB_FUNCS="rndis uvc"
export UVC_INSTANCES="uvc.0 uvc.1"
export UVC_CNT=2
export UVC_START_DAEMON=0
export CAMERA_UVC_4000X3000_ONLY=1
export CAMERA_UVC_MJPEG_MAX_FRAME_SIZE=4194304
