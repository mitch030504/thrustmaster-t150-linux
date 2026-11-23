#!/bin/sh
# Uninstall hid-t150 on Linux, or ht150 on FreeBSD.

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

os=$(uname -s)
case "$os" in
FreeBSD)
	exec "$SCRIPT_DIR/freebsd/uninstall.sh" "$@"
	;;
Linux)
	;;
*)
	echo "Unsupported OS: $os (Linux and FreeBSD only)" >&2
	exit 1
	;;
esac

if [ -z "${BASH_VERSION:-}" ]; then
	if command -v bash >/dev/null 2>&1; then
		exec bash "$SCRIPT_DIR/uninstall.sh" "$@"
	fi
	echo "bash is required on Linux." >&2
	exit 1
fi

set -euo pipefail

if [ "${EUID}" -ne 0 ]; then
	echo "You are not running this script as root!"
	exit 1
fi

VERSION=1.2.0
helper=/usr/lib/udev/hid-t150-rebind
if { command -v pacman >/dev/null && pacman -Qo "$helper" >/dev/null 2>&1; } ||
   { command -v rpm >/dev/null && rpm -qf "$helper" >/dev/null 2>&1; }; then
	echo "Remove the installed package through the package manager." >&2
	exit 1
fi

rm -f /etc/udev/rules.d/10-t150.rules /etc/udev/rules.d/99-zz-t150-permissions.rules \
	/etc/modules-load.d/hid-t150.conf
udevadm control --reload
if [ -d /sys/module/hid_t150 ]; then
	rmmod hid_t150
fi

if command -v dkms >/dev/null && dkms status -m t150 -v "$VERSION" | grep -q .; then
	dkms remove -m t150 -v "$VERSION" --all
fi
for file in /usr/src/t150-"$VERSION"/hid-t150/* /usr/src/t150-"$VERSION"/common/* \
	/usr/src/t150-"$VERSION"/dkms.conf /usr/src/t150-"$VERSION"/Makefile; do
	[ ! -f "$file" ] || rm "$file"
done
rmdir /usr/src/t150-"$VERSION"/{hid-t150,build,common} /usr/src/t150-"$VERSION" 2>/dev/null || true

marker=/var/lib/t150-control/manual-kernel
if [ -f "$marker" ]; then
	kernel=$(cat "$marker")
	if [[ "$kernel" =~ ^[a-zA-Z0-9._+-]+$ ]]; then
		for suffix in '' .zst .xz .gz; do
			rm -f "/lib/modules/$kernel/extra/hid-t150/hid-t150.ko$suffix"
		done
		depmod -a "$kernel"
	fi
	rm "$marker"
fi

rm -f "$helper" /usr/bin/t150-preset /usr/bin/t150-control \
	/usr/share/applications/io.github.mitch030504.T150Control.desktop \
	/usr/lib/sysusers.d/t150-control.conf

modprobe hid-generic
shopt -s nullglob
for dev in /sys/bus/hid/devices/0003:044F:B677.* /sys/bus/hid/devices/0003:044F:B67F.*; do
	[ -L "$dev/driver" ] || printf '%s' "${dev##*/}" > /sys/bus/hid/drivers/hid-generic/bind
done
echo "hid-t150 uninstalled. Wheel settings and profiles were preserved."
