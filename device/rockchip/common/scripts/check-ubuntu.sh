#!/bin/bash -e

RK_SCRIPTS_DIR="${RK_SCRIPTS_DIR:-$(dirname "$(realpath "$0")")}"
RK_SDK_DIR="${RK_SDK_DIR:-$(realpath "$RK_SCRIPTS_DIR/../../../..")}"
RK_TOOLS_DIR="${RK_TOOLS_DIR:-$(realpath "$RK_SCRIPTS_DIR/../tools")}"
RK_UBUNTU_ARCH="${RK_UBUNTU_ARCH:-arm64}"
RK_UBUNTU_VERSION="${RK_UBUNTU_VERSION:-noble}"

if findmnt -fnu -o OPTIONS -T "$RK_SCRIPTS_DIR" | grep -qE "nodev"; then
	echo -e "\e[35m"
	echo "Please remount to allow creating devices on the filesystem:"
	echo "sudo mount -o remount,dev $(findmnt -fnu -o TARGET -T "$RK_SCRIPTS_DIR")"
	echo -e "\e[0m"
	exit 1
fi

if ! mke2fs -h 2>&1 | grep -wq "\-d"; then
	echo -e "\e[35m"
	echo "Your mke2fs is too old: $(mke2fs -V 2>&1 | head -n 1)"
	echo "Please update it:"
	"$RK_SCRIPTS_DIR/install-e2fsprogs.sh"
	echo -e "\e[0m"
	exit 1
fi

case "$RK_UBUNTU_ARCH" in
	arm64) QEMU_ARCH=aarch64 ;;
	armhf) QEMU_ARCH=arm ;;
	*)
		echo -e "\e[35m"
		echo "Unknown arch(RK_UBUNTU_ARCH): $RK_UBUNTU_ARCH!"
		echo -e "\e[0m"
		exit 1 ;;
esac

"$RK_SCRIPTS_DIR/check-package.sh" "qemu-$QEMU_ARCH-static(qemu-user-static)" \
	qemu-$QEMU_ARCH-static qemu-user-static

if ! [ -r /proc/sys/fs/binfmt_misc/qemu-$QEMU_ARCH ]; then
	echo -e "\e[34m"
	echo "The qemu-$QEMU_ARCH binfmt entry is not visible in this mount namespace."
	echo "The Ubuntu rootfs builder will verify ARM execution with a real chroot."
	echo -e "\e[0m"
fi

if [ "$RK_UBUNTU_MIRROR" ]; then
	"$RK_SCRIPTS_DIR/check-network.sh" "$RK_UBUNTU_MIRROR" \
		"the Ubuntu mirror source:\n$RK_UBUNTU_MIRROR"
fi
