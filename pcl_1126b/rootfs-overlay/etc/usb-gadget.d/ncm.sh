NCM_DEVICE_ADDRESS="${NCM_DEVICE_ADDRESS:-192.168.77.2/24}"

ncm_post_start_hook()
{
	count=0
	while ! ip link show usb0 >/dev/null 2>&1; do
		count=$((count + 1))
		[ "$count" -lt 50 ] || return 1
		sleep 0.1
	done
	ip link set usb0 up
	ip address flush dev usb0 scope global || true
	ip address add "$NCM_DEVICE_ADDRESS" dev usb0
}

ncm_pre_stop_hook()
{
	ip link show usb0 >/dev/null 2>&1 || return 0
	ip address flush dev usb0 scope global || true
	ip link set usb0 down || true
}
