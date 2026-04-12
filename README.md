# Kernel DRM drivers for Pico USB Display device

## System Info

| Distro                    | Kernel version    | Machine            |
| ------------------------- | ----------------- | ------------------ |
| Ubuntu 24.04.3 LTS x86_64 | 6.17.0-20-generic | ThinkPad E14 Gen 2 |

install dependencies

```bash
sudo apt install git make gcc vim -y
```

clone and build

```bash
git clone https://github.com/embeddedboys/PUD-kernel-drivers.git
cd PUD-kernel-drivers
git checkout 6.17.0-20-generic
make
sudo insmod pud.ko
```

The default display backend is DRM.

## More

### Setup a Raspberrypi Pico USB Display device

please refer to ...

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

