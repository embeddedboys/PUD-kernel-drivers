#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Boot a throwaway VM around this repository (virtme-ng), load pud.ko inside it
# and hand over a root shell.  The driver therefore runs against a real kernel
# without ever being loaded on this machine: the guest shares this filesystem
# read-only, so nothing here can be damaged by a driver that crashes.
#
#   scripts/qemu.sh                    boot, load, interactive root shell
#   scripts/qemu.sh 'dmesg | grep pud' run one command instead of the shell
#
# Environment (all optional):
#   KERNEL_IMG=/path/vmlinuz-<rel>  guest kernel image.  Default: the running
#                                   kernel when a readable image for it exists,
#                                   otherwise the newest obtainable one (see
#                                   "kernel image" below).
#   PARAMS='report_mode=pointer'    module parameters passed to insmod
#   PASSTHROUGH=auto|1|0            hand the USB panel over to the guest
#                                   (default auto: only if it is plugged in)
#   KO=/path/to/pud.ko              use this module instead of building one
#   QEMU_OPTS='-smp 4'              extra arguments for vng/QEMU
#
# The module is built for the *guest* kernel, whose vermagic has to match --
# that overwrites the repository's ./pud.ko, and `make modules` rebuilds it for
# the running kernel afterwards.
#
# Kernel image: Ubuntu ships /boot/vmlinuz-* as root-only, so a guest kernel
# other than one we can read is extracted from the linux-image package in the
# apt cache (~/.cache/pud-kbuild/img/ caches the result).  To run the guest with
# the running kernel, make its image readable once:
#
#   sudo cp /boot/vmlinuz-$(uname -r) ~/.cache/pud-kbuild/img/

set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
CACHE=$HOME/.cache/pud-kbuild
VNG=$HERE/.venv/bin/vng
USB_ID=2e8a:0001

say() { printf '%s\n' "$*"; }
die() { printf 'qemu: %s\n' "$*" >&2; exit 1; }

# The header comment above is the help text.
case "${1:-}" in
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 && !/^#/ { exit }' "$0"; exit 0 ;;
esac

[ -x "$VNG" ] || die "no virtme-ng at $VNG.
  python3 -m venv $HERE/.venv && $HERE/.venv/bin/pip install virtme-ng"

# ---------------------------------------------------------------------------
# Which kernel the guest runs
#
# The module is built for whatever this picks, so it has to be a kernel whose
# headers are installed.  The running kernel wins when its image is readable;
# otherwise the newest candidate wins.  An image is usable when it is readable
# in /boot, already cached, or extractable from a linux-image package in the
# apt cache.
# ---------------------------------------------------------------------------
image_from_deb() {  # <rel> -> prints the image path, or nothing
    local rel=$1 deb img
    deb=$(ls /var/cache/apt/archives/linux-image-${rel}_*.deb 2>/dev/null | head -n 1)
    [ -n "$deb" ] || return 1
    img=$CACHE/img/vmlinuz-$rel
    say "  extracting $img from $(basename "$deb")" >&2
    mkdir -p "$CACHE/img"
    dpkg-deb --fsys-tarfile "$deb" | tar -xO "./boot/vmlinuz-$rel" > "$img.tmp"
    mv "$img.tmp" "$img"
    printf '%s\n' "$img"
}

kernel_image() {
    if [ -n "${KERNEL_IMG:-}" ]; then
        [ -r "$KERNEL_IMG" ] || die "KERNEL_IMG=$KERNEL_IMG is not readable"
        printf '%s %s\n' "${KERNEL_IMG##*vmlinuz-}" "$KERNEL_IMG"
        return
    fi

    local rel img running
    running=$(uname -r)
    # running kernel first, then newest first
    for rel in "$running" $(ls /lib/modules | grep -v "^$running$" | sort -rV); do
        [ -e "/lib/modules/$rel/build" ] || continue
        if [ -r "/boot/vmlinuz-$rel" ]; then
            img=/boot/vmlinuz-$rel
        elif [ -r "$CACHE/img/vmlinuz-$rel" ]; then
            img=$CACHE/img/vmlinuz-$rel
        else
            img=$(image_from_deb "$rel") || continue
        fi
        printf '%s %s\n' "$rel" "$img"
        return
    done

    die "no usable guest kernel image.

  Either make the running kernel's image readable:
    sudo cp /boot/vmlinuz-$running $CACHE/img/
  or install headers for a kernel whose package is in the apt cache and retry."
}

read -r KREL KIMG <<<"$(kernel_image)"
[ -n "$KREL" ] && [ -n "$KIMG" ] || die "could not pick a guest kernel"
[ -e "/lib/modules/$KREL/build" ] ||
    die "no headers for $KREL -- sudo apt install linux-headers-$KREL"

# ---------------------------------------------------------------------------
# Module for that kernel
# ---------------------------------------------------------------------------
ko=${KO:-$HERE/pud.ko}
if [ -z "${KO:-}" ]; then
    have=$(modinfo -F vermagic "$ko" 2>/dev/null | awk '{print $1}')
    if [ "$have" != "$KREL" ]; then
        say "building pud.ko for $KREL (was: ${have:-none})"
        make -C "$HERE" modules KERN_DIR="/lib/modules/$KREL/build" >/dev/null 2>&1 ||
            die "build failed -- run make modules KERN_DIR=/lib/modules/$KREL/build"
    fi
fi
[ -f "$ko" ] || die "no module at $ko"

# ---------------------------------------------------------------------------
# Hand the panel over to the guest when it is plugged in here
# ---------------------------------------------------------------------------
case "${PASSTHROUGH:-auto}" in
    0) pass=no ;;
    1) pass=yes ;;
    *)
        # lsusb hangs when the USB stack is wedged -- always bound it
        if timeout 5 lsusb -d "$USB_ID" >/dev/null 2>&1; then pass=yes; else pass=no; fi
        ;;
esac

qemu_opts=${QEMU_OPTS:-}
if [ "$pass" = yes ]; then
    qemu_opts="-device qemu-xhci -device usb-host,vendorid=0x2e8a,productid=0x0001 $qemu_opts"
fi

# ---------------------------------------------------------------------------
# What runs inside the guest
#
# A file, not --exec's command line: the guest cannot see this machine's /tmp
# (it gets its own), so the script lives where the guest can read it.
# ---------------------------------------------------------------------------
cmd=${1:-${CMD:-}}
guest=$CACHE/qemu-guest.sh
cat > "$guest" <<EOF
#!/bin/bash
# generated by scripts/qemu.sh
modprobe drm_dma_helper 2>/dev/null

insmod "$ko" ${PARAMS:-} || echo "pud: insmod failed"
sleep 1

# The first vendor request after the guest takes the USB device over times out
# (-110) on this emulated xHCI often enough to matter, and the driver then keeps
# its host-side defaults instead of the panel's own parameters.  The same
# request from the host works, and a reload a moment later always does, so this
# is the VM's quirk rather than the driver's -- one reload gives the session the
# real capability report.
if dmesg | tail -n 20 | grep -q "no capability report"; then
    echo "(capability report missed on the first probe -- reloading once)"
    rmmod pud
    sleep 3
    insmod "$ko" ${PARAMS:-}
    sleep 1
fi

echo
echo "--- dmesg ---"
dmesg | grep -i pud | tail -n 10
for c in /sys/class/drm/card*-USB-*; do
    [ -e "\$c/status" ] && echo "\$c: \$(cat "\$c/status") \$(cat "\$c/enabled" 2>/dev/null)"
done
for f in /sys/class/graphics/fb*; do
    [ -e "\$f/name" ] && [ "\$(cat "\$f/name")" = pud-drmdrmfb ] && echo "\$f = pud-drmdrmfb"
done
grep -q "pud touch panel" /proc/bus/input/devices && echo "input: pud touch panel"
echo "refcnt: \$(cat /sys/module/pud/refcnt 2>/dev/null || echo -)"
EOF

if [ -n "$cmd" ]; then
    cat >> "$guest" <<EOF

echo
$cmd
EOF
else
    cat >> "$guest" <<'EOF'

echo
echo "pud is loaded.  Exit this shell (or Ctrl-D) to power the VM off."
exec bash -i
EOF
fi

say "guest kernel: $KREL"
say "guest image : $KIMG"
say "module      : $ko${PARAMS:+ ($PARAMS)}"
[ -n "$cmd" ] && say "command     : $cmd"
[ "$pass" = yes ] && say "usb         : 2e8a:0001 handed to the guest" ||
    say "usb         : (nothing to pass through -- the module will load, nothing will probe)"
say ""

export PATH=$HERE/.venv/bin:$PATH
args=(--run "$KIMG" --user root --exec "bash $guest")
[ -n "$qemu_opts" ] && args+=(--qemu-opts="$qemu_opts")
exec "$VNG" "${args[@]}"
