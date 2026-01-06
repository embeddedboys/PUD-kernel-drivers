## System Info

| Distro                    | Kernel version    | Machine            |
| ------------------------- | ----------------- | ------------------ |
| Ubuntu 24.04.3 LTS x86_64 | 6.14.0-37-generic | ThinkPad E14 Gen 2 |

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

## FIXMEs:

- [ ] transfer buffer is on stack

```bash
[  372.572182] pud 1-4:1.0: [drm] fb1: pud-drmdrmfb frame buffer device
[  372.584656] ------------[ cut here ]------------
[  372.584668] transfer buffer is on stack
[  372.584696] WARNING: CPU: 4 PID: 203 at drivers/usb/core/hcd.c:1476 usb_hcd_map_urb_for_dma+0x463/0x4d0
```

- [ ] Part refresh
