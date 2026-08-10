#!/bin/sh

# Sourced by /usr/bin/usbdevice from /etc/usbdevice.d.
# Keep the existing USB network link available while adding two UVC outputs.
export USB_FUNCS="rndis uvc"
# Allocate the two UVC streaming endpoints before RNDIS.  On RK3576, putting
# RNDIS first leaves the second UVC stream on EP7, where it starts but never
# completes frame transfers.  UVC-first uses EP2/EP4 and keeps RNDIS working.
export USB_FUNC_ORDER="uvc rndis uac ntb ums mtp acm"
export USB_PRODUCT_ID="0x0017"
export UVC_INSTANCES="uvc.0 uvc.1"
export UVC_CNT=2
export UVC_START_DAEMON=0
export CAMERA_UVC_4000X3000_ONLY=1
export CAMERA_UVC_MJPEG_MAX_FRAME_SIZE=4194304

rndis_post_start_hook()
{
	count=0
	while ! ip link show usb0 >/dev/null 2>&1; do
		count=$((count + 1))
		[ "$count" -ge 50 ] && return 1
		sleep 0.1
	done
	ip address replace 192.168.55.1/24 dev usb0
	ip link set usb0 up
}
