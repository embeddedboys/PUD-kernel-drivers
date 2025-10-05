## System Info

| Distro | Kernel version |
| --- | --- |
| WSL | 6.6.87.2-microsoft-standard-WSL2+ |

install tools
```bash
sudo apt install git make gcc gcc-12 vim -y
```

clone and build
```bash
git clone https://github.com/embeddedboys/PUD-kernel-drivers.git
cd PUD-kernel-drivers
git checkout kernel-6.6
make
sudo insmod pud.ko
```
The default display backend is DRM.

upgrade WSL kernel version if needed
```
git clone https://github.com/microsoft/WSL2-Linux-Kernel.git --depth 1
cd WSL2-Linux-Kernel
make KCONFIG_CONFIG=Microsoft/config-wsl -j$(nproc)

ls -lh arch/x86/boot/bzImage
```

copy `bzImage` to your windows user dir and rename it with "kernel"
then create and edit `.wslconfig` like this
```
[wsl2]
kernel=C:\\Users\\your_username\\kernel
```

after that, stop the wsl by typing the following in Windows CMD:
```
wsl --shutdown
```

the new kernel will be actived the next time you start your distro

## Setup and Test Desktop

### Disable WSL X11 application forward
create and edit `.wslconfig` in your current windows user dir
```
[wsl2]
guiApplications=false
```

### Install Desktop
```bash
sudo apt --no-install-recommends install xorg xfce4 lightdm -y
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
