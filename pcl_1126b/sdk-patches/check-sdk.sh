#!/bin/bash -e

RK_SCRIPTS_DIR="${RK_SCRIPTS_DIR:-$(dirname "$(realpath "$0")")}"
RK_SDK_DIR="${RK_SDK_DIR:-$RK_SCRIPTS_DIR/../../../..}"
RK_OWNER="${RK_OWNER:-$(stat --format %U "$RK_SDK_DIR")}"
RK_OWNER_UID="${RK_OWNER_UID:-$(stat --format %u "$RK_SDK_DIR")}"

if [ "$(id -u)" -ne 0 ] && [ "$RK_OWNER_UID" -ne "$(id -u)" ]; then
	echo -e "\e[35m"
	echo "ERROR: Current user is not the owner of SDK source!"
	echo "Please change owner of SDK code:"
	echo "sudo chown -h -R $(id -un):$(id -un) $RK_SDK_DIR/"
	if ! [ "$RK_OWNER" = UNKNOWN ]; then
		echo "Or switch to user($RK_OWNER):"
		echo "su - $RK_OWNER"
	fi
	echo -e "\e[0m"
	exit 1
fi

supported_filesystem()
{
	case "$1" in
		ext* | zfs | xfs | f2fs | btrfs) return 0 ;;
		*) return 1 ;;
	esac
}

sdk_filesystem=$(findmnt -fnu -o FSTYPE -T "$RK_SCRIPTS_DIR")
if [ "$sdk_filesystem" = overlay ]; then
	overlay_options=$(findmnt -fnu -o OPTIONS -T "$RK_SCRIPTS_DIR")
	upper_dir=$(echo "$overlay_options" | sed -n 's/.*upperdir=\([^,]*\).*/\1/p')
	work_dir=$(echo "$overlay_options" | sed -n 's/.*workdir=\([^,]*\).*/\1/p')
	upper_filesystem=$(findmnt -fnu -o FSTYPE -T "$upper_dir")
	work_filesystem=$(findmnt -fnu -o FSTYPE -T "$work_dir")
	if [ -z "$upper_dir" ] || [ -z "$work_dir" ] ||
		! supported_filesystem "$upper_filesystem" ||
		! supported_filesystem "$work_filesystem"; then
		echo "ERROR: OverlayFS upper/work directories need a supported Linux filesystem."
		exit 1
	fi
elif ! supported_filesystem "$sdk_filesystem"; then
	echo -e "\e[35m"
	echo "Please move SDK source code into an ext4 partition."
	echo -e "\e[0m"
	exit 1
fi

if ! which python3 >/dev/null 2>&1; then
	echo -e "\e[35m"
	echo "Your python3 is missing"
	echo "Please install it:"
	"$RK_SCRIPTS_DIR/install-python3.sh"
	echo -e "\e[0m"
	exit 1
fi

"$RK_SCRIPTS_DIR/check-package.sh" rsync
"$RK_SCRIPTS_DIR/check-package.sh" gcc
"$RK_SCRIPTS_DIR/check-package.sh" g++
