#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Load, unload and inspect the pud module on the machine the panel is plugged
# into (the board itself -- insmod/rmmod only exist there).
#
#   scripts/pud-load.sh load   [module options...]   e.g. input_only=1
#   scripts/pud-load.sh unload [--stop-dm]
#   scripts/pud-load.sh reload [--stop-dm] [module options...]
#   scripts/pud-load.sh status
#
# --stop-dm stops the running display manager (gdm, lightdm, sddm, ...; found by
# asking systemd, PUD_DM=<unit> overrides) around the unload.  Only needed when
# a session holds the DRM node open; the older --stop-gdm spelling still works.
#
# Module options worth knowing (see notes/architecture.md):
#   input_only=1          register only the touch input device, no DRM/fbdev
#                         node -- nothing holds the module, so rmmod is instant
#   report_mode=pointer   present the panel as an absolute pointer instead of a
#                         touchscreen (default touch)
#
# Environment:
#   PUD_KO=/path/to/pud.ko   module to load (default: the repo's ./pud.ko)
#   MODULE=pud               module name
#
# Root is required; the script re-executes itself through sudo.

MODULE=${MODULE:-pud}
HERE=$(cd "$(dirname "$0")" && pwd)
KO=${PUD_KO:-$HERE/../pud.ko}

# normalize, so messages do not show a "scripts/.." detour
if [ -d "$(dirname "$KO")" ]; then
    KO="$(cd "$(dirname "$KO")" && pwd)/$(basename "$KO")"
fi

# Usage and help never need root, so they come before the sudo re-exec.
usage() {
    awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 && !/^#/ { exit }' "$0"
}

case "${1:-}" in
    ""|-h|--help|help) usage; exit 0 ;;
esac

if [ "$(id -u)" -ne 0 ]; then
    exec sudo -- "$0" "$@"
fi

say()  { printf '%s\n' "$*"; }
die()  { printf 'pud-load: %s\n' "$*" >&2; exit 1; }

loaded() { lsmod | grep -q "^${MODULE} "; }
refcnt() { cat "/sys/module/${MODULE}/refcnt" 2>/dev/null || echo '-'; }

# Who is holding the DRM node open -- that is what keeps the module busy.
holders() {
    if command -v lsof >/dev/null 2>&1; then
        lsof /dev/dri/* 2>/dev/null | tail -n +2 | head -n 10
    fi
}

# The .ko has to be built for the running kernel, or insmod fails with
# "disagrees about version of symbol module_layout" (this is what the old
# hand-copied md5 check was protecting against, except it also caught a stale
# module that happened to match the kernel).
check_ko() {
    [ -f "$KO" ] || die "no module at $KO -- build it first (make modules KERN_DIR=...) or set PUD_KO"

    local vm krel md5
    vm=$(modinfo -F vermagic "$KO" 2>/dev/null | awk '{print $1}')
    krel=$(uname -r)
    md5=$(md5sum "$KO" | cut -d' ' -f1)

    say "module    $KO"
    say "  md5       $md5"
    say "  vermagic  $vm   (running kernel: $krel)"

    [ "$vm" = "$krel" ] || die "vermagic does not match the running kernel.
  Build against /lib/modules/$(uname -r)/build, or copy the .ko that goes with
  this kernel -- insmod would fail with 'disagrees about version of symbol
  module_layout'."
}

do_load() {
    check_ko

    if loaded; then
        say "${MODULE} is already loaded (refcnt $(refcnt))"
        say "use '$0 reload' to pick up a new .ko, or '$0 unload' first"
        return 0
    fi

    # insmod resolves nothing: the module's own dependencies have to be loaded
    # first or the kernel refuses with "Unknown symbol in module" (7.0 keeps the
    # fbdev client in the drm core and the DMA helpers in drm_dma_helper, both
    # of which pud.ko imports from).  modinfo knows the list.
    local deps
    deps=$(modinfo -F depends "$KO" 2>/dev/null | tr ',' ' ')
    if [ -n "$deps" ]; then
        say "modprobe $deps"
        modprobe -a $deps || die "could not load the module's dependencies: $deps"
    fi

    say "insmod $KO $*"
    if ! insmod "$KO" "$@"; then
        dmesg | grep -i "$MODULE" | tail -n 3 | sed 's/^/  /'
        die "insmod failed"
    fi
    sleep 1
    say ""
    do_status
}

# The display manager to stop around an unload: whatever systemd says is
# running, so this works on gdm, lightdm, sddm and friends alike.  PUD_DM
# overrides it for setups that run something else entirely.
dm_unit() {
    local u
    for u in ${PUD_DM:-} gdm gdm3 lightdm sddm lxdm xdm ly greetd; do
        [ -n "$u" ] || continue
        systemctl is-active --quiet "$u" 2>/dev/null && { printf '%s\n' "$u"; return 0; }
    done
    return 1
}

do_unload() {
    local stop_dm=0

    case "${1:-}" in
        --stop-dm|--stop-gdm) stop_dm=1 ;;
    esac

    if ! loaded; then
        say "${MODULE} is not loaded"
        return 0
    fi

    if rmmod "$MODULE" 2>/dev/null; then
        say "rmmod ok"
        return 0
    fi

    say "rmmod failed: the module is in use (refcnt $(refcnt))"
    holders
    say ""
    say "Something in userspace keeps /dev/dri/card* open (the session, or"
    say "logind on its behalf)."

    if [ "$stop_dm" -eq 0 ]; then
        say "Re-run with --stop-dm to stop the display manager around the unload:"
        say "  $0 unload --stop-dm"
        return 1
    fi

    local dm
    if ! dm=$(dm_unit); then
        say "no running display manager found to stop (tried gdm, lightdm, sddm, ...)"
        say "log out of the session, then 'rmmod $MODULE' -- rebooting without"
        say "loading the module is the last resort"
        return 1
    fi

    say "stopping $dm, unloading, restarting $dm ..."
    systemctl stop "$dm" || say "warning: could not stop $dm"
    local rc=0
    rmmod "$MODULE" || rc=$?
    systemctl start "$dm" || say "warning: $dm did not restart -- run 'systemctl start $dm'"

    if [ "$rc" -eq 0 ]; then
        say "rmmod ok ($dm restarted)"
    else
        say "rmmod still failed; reboot without loading the module as a last resort"
    fi
    return "$rc"
}

do_status() {
    say "--- lsmod ---"
    lsmod | grep "^${MODULE} " || say "(not loaded)"

    if [ -d "/sys/module/${MODULE}/parameters" ]; then
        say "--- parameters ---"
        for p in /sys/module/${MODULE}/parameters/*; do
            [ -e "$p" ] || continue
            say "  $(basename "$p")=$(cat "$p" 2>/dev/null)"
        done
    fi

    say "--- display nodes ---"
    # pud's connector shows up as cardN-USB-*; matching on that keeps the
    # board's own DRM cards out of the report.
    local found=0 n c
    for c in /sys/class/drm/card*-USB-*; do
        [ -e "$c/status" ] || continue
        say "  $c: $(cat "$c/status") $(cat "$c/enabled" 2>/dev/null)"
        found=1
    done
    for n in /sys/class/graphics/fb*; do
        [ -e "$n/name" ] || continue
        if [ "$(cat "$n/name")" = "pud-drmdrmfb" ]; then
            say "  $n = pud-drmdrmfb"
            found=1
        fi
    done
    [ "$found" -eq 1 ] || say "  (none -- loaded with input_only=1?)"

    say "--- input device ---"
    if grep -q "pud touch panel" /proc/bus/input/devices 2>/dev/null; then
        grep -A6 "pud touch panel" /proc/bus/input/devices | sed 's/^/  /'
    else
        say "  (none -- the device reports no touch controller, or input_only was not used)"
    fi

    say "--- dmesg ---"
    dmesg 2>/dev/null | grep -i "pud" | tail -n 12 | sed 's/^/  /' || true
}

cmd=${1:-}
[ $# -gt 0 ] && shift

case "$cmd" in
    load)   do_load "$@" ;;
    unload) do_unload "$@" ;;
    reload)
        stop_dm=""
        case "${1:-}" in
            --stop-dm|--stop-gdm) stop_dm="$1"; shift ;;
        esac
        if loaded; then
            do_unload $stop_dm || exit 1
        fi
        do_load "$@"
        ;;
    status) do_status ;;
    ""|-h|--help|help) usage ;;
    *) die "unknown command '$cmd' (load | unload | reload | status)" ;;
esac
