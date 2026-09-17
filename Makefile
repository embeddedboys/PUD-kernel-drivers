
# Target kernel build tree. Pass it on the command line for other kernels:
#   make KERN_DIR=/path/to/linux KERN_OBJ_DIR=/path/to/objtree
KERN_DIR ?= /lib/modules/$(shell uname -r)/build

# Out-of-tree build directory (objtree) of KERN_DIR. Only needed when the
# kernel was built with O=, because the generated files required to link a
# module (scripts/module.lds, Module.symvers, ...) then live there instead of
# in the source tree. Leave empty for an in-tree built kernel.
KERN_OBJ_DIR ?=

# users kernel dir
# KERN_DIR=/home/user/linux

# Target architecture and cross compiler, override as needed
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

ifeq ($(KERN_OBJ_DIR),)
KBUILD_O :=
else
KBUILD_O := O=$(KERN_OBJ_DIR)
endif

MODULE_NAME:=pud

all: modules
	$(MAKE) -C tests/

modules:
	$(MAKE) -C $(KERN_DIR) $(KBUILD_O) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KERN_DIR) $(KBUILD_O) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules clean

test: all
	sudo rmmod $(MODULE_NAME).ko || true
	sudo insmod $(MODULE_NAME).ko || true

obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-y += usb.o jpegenc.o encoder.o rgb565_qoi.o fb.o drm.o input.o
