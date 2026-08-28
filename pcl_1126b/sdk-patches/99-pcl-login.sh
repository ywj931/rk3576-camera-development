#!/bin/bash -e

TARGET_DIR="$(realpath "$1")"

# This hook is shared by the SDK, so limit it to the PCL camera image.
[ "${RK_KERNEL_DTS_NAME:-}" = "rv1126b-pcl-camera-v1" ] || exit 0

INITTAB="$TARGET_DIR/etc/inittab"
SHADOW="$TARGET_DIR/etc/shadow"
ROOT_SHADOW='root:$1$R5mt1IGf$rWWiAmf8WYBWyKtKlyi0g1:18628::::::'

[ -f "$INITTAB" ]
[ -f "$SHADOW" ]

# ttyFIQ0 is the console on this board. A ttyS0 respawn entry floods the
# console because the device does not exist.
sed -i '/^ttyS0::respawn:/d' "$INITTAB"

# Use the password record generated and verified by passwd on the target.
sed -i "s#^root:.*#${ROOT_SHADOW}#" "$SHADOW"

grep -q '^ttyFIQ0::respawn:' "$INITTAB"
! grep -q '^ttyS0::respawn:' "$INITTAB"
grep -qxF "$ROOT_SHADOW" "$SHADOW"
