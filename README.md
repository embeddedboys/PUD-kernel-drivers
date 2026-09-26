## System Info

| Distro | Kernel version | Machine |
| --- | --- | --- |
| Debian GNU/Linux 13 (trixie) | 6.12.47+rpt-rpi-2712 | Raspberry Pi 5 Model B |

`kernel-6.12` is the upstream line and the branch the commands below check out.
The deployment branch for this project's RK3588 board is `rk-6.1.172` -- the
board's kernel, the vendor source tree and its headers package are all 6.1.172
-- and it carries `AGENTS.md` and `notes/`; both of its build modes are in
[notes/build-and-test.md](./notes/build-and-test.md).

install tools
```bash
sudo apt install git make gcc vim -y
```

clone and build
```bash
git clone https://github.com/embeddedboys/PUD-kernel-drivers.git
cd PUD-kernel-drivers
git checkout kernel-6.12
make
sudo insmod pud.ko
```
The default display backend is DRM.

`make qemu` boots a throwaway VM (virtme-ng, installed in `.venv/`) with the
module loaded and leaves a root shell there, so the driver can be exercised
without ever loading it on this machine. `PARAMS=`, `CMD=` and `PASSTHROUGH=`
tune it; see [notes/build-and-test.md](./notes/build-and-test.md) section C.

## Load / unload

On the machine the panel is plugged into (`insmod`/`rmmod` only exist there), use
the helper instead of a hand-written script: it refuses to load a module built for
a different kernel (the `vermagic` mismatch that used to cost a debugging round)
and handles a desktop holding the DRM node when unloading.

```bash
scripts/pud-load.sh load                       # display + touch
scripts/pud-load.sh load input_only=1          # touch only: rmmod always succeeds
scripts/pud-load.sh load input_only=1 report_mode=pointer
scripts/pud-load.sh load initial_mode=1        # light the panel with no userspace
scripts/pud-load.sh status                     # parameters, nodes, input device, dmesg
scripts/pud-load.sh unload [--stop-gdm]        # --stop-gdm: stop gdm, rmmod, start gdm
scripts/pud-load.sh reload input_only=1        # new .ko or different options
```

`PUD_KO=/path/to/pud.ko` selects a different module (default: the repo's
`./pud.ko`). See [notes/architecture.md](./notes/architecture.md) for what
`input_only` and `report_mode` do.

## Setup and Test Desktop

### Install Desktop
```bash
sudo apt --no-install-recommends install xorg xfce4 lightdm -y
sudo apt install dbus-x11 -y
sudo apt install lightdm-gtk-greeter -y
```

start xfce4 with root user:
```bash
sudo startxfce4
```

start xfce4 via lightdm
```bash
sudo lightdm -d
```

## More

### Useful commands during development

disable and enable cursor blink
```bash
sudo sh -c "echo 0 > /sys/class/graphics/fbcon/cursor_blink"
sudo sh -c "echo 1 > /sys/class/graphics/fbcon/cursor_blink"
```

vtconsole ubind and bind (This is useful when you removing activing fb driver)
```bash
sudo sh -c "echo 0 > /sys/class/vtconsole/vtcon1/bind"
sudo sh -c "echo 1 > /sys/class/vtconsole/vtcon1/bind"
```

mplayer output to fbdev
```bash
mplayer -vo fbdev2 -vf scale=480:320 xxx.mp4
```

## Development notes

Design notes and pitfall write-ups for maintainers live in [`notes/`](./notes/):

- [Architecture and code map](./notes/architecture.md)
- [USB protocol](./notes/usb-protocol.md) (authoritative field definitions)
- [Display and refresh policy](./notes/display-and-refresh.md) (damage, QOI/RLE, band splitting)
- [Build and test](./notes/build-and-test.md) (objtree and headers, both 6.1.172)
- [Pitfalls](./notes/pitfalls.md) (DMA buffers, `transfer buffer is on stack`, vmalloc, swiotlb)
