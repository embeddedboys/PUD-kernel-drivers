
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

# ---------------------------------------------------------------------------
# Building on the board itself
#
# A vendor kernel-headers package ships the kernel's host tools (fixdep,
# modpost, ...) prebuilt for the machine the kernel was built on -- in practice
# an x86-64 build server.  kbuild runs them straight out of the headers tree,
# which the package owns as root, so on an arm64 board a plain `make` dies with
#
#     /bin/sh: 1: scripts/basic/fixdep: Exec format error
#
# and there is nowhere writable to rebuild them: the tree is read-only, and
# asking kbuild to rebuild the tools itself would first regenerate
# include/config/auto.conf, which a headers package cannot do (it ships no
# Kconfig files).
#
# So when the tools cannot run on this host we keep a user-writable copy of the
# headers tree under $(HOME)/.cache/pud-kbuild/ and rebuild only those tools
# there, using the commands kbuild recorded in its own .cmd files (so the flags
# stay the kernel's).  KERN_DIR itself is never written to.  Cross builds, where
# the tools run on the build host, do not need any of this.
# ---------------------------------------------------------------------------
host_machine := $(shell uname -m)

# ELF e_machine of the machine doing the build, from the two bytes at offset 18
# (little-endian) of any ELF file -- no binutils or file(1) needed.
host_elf := $(strip $(if $(filter aarch64,$(host_machine)),b700, \
            $(if $(filter x86_64,$(host_machine)),3e00, \
            $(if $(filter armv6l armv7l,$(host_machine)),2800, \
            $(if $(filter riscv64,$(host_machine)),f300,)))))

tools_elf := $(strip $(shell od -An -tx1 -j18 -N2 $(KERN_DIR)/scripts/basic/fixdep 2>/dev/null | tr -d ' '))
kernel_release := $(shell cat $(KERN_DIR)/include/config/kernel.release 2>/dev/null || uname -r)
NATIVE_KERN_DIR := $(HOME)/.cache/pud-kbuild/$(kernel_release)
# KERN_DIR is usually /lib/modules/$(uname -r)/build, i.e. a symlink; copying it
# has to resolve to the real tree or we would be writing through the link.
KERN_SRC := $(realpath $(KERN_DIR))

# Same architecture (or the tools are missing): build against a tree we own.
ifneq ($(tools_elf),$(host_elf))
KBUILD_KERN_DIR := $(NATIVE_KERN_DIR)
else
KBUILD_KERN_DIR := $(KERN_DIR)
endif

MODULE_NAME:=pud

all: modules
	$(MAKE) -C tests/

modules: $(if $(filter $(NATIVE_KERN_DIR),$(KBUILD_KERN_DIR)),$(NATIVE_KERN_DIR))
	$(MAKE) -C $(KBUILD_KERN_DIR) $(KBUILD_O) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules
	@$(MAKE) --no-print-directory compile_commands.json

clean:
	$(MAKE) -C $(KBUILD_KERN_DIR) $(KBUILD_O) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules clean

# ---------------------------------------------------------------------------
# One-time preparation of that writable copy.  Cached per kernel release;
# `rm -rf $(NATIVE_KERN_DIR)` to have it redone (after a kernel update, say).
# The kernel dir keeps its own module version, so the copy has to be a real one
# (cp -a) rather than hard links -- rebuilding a tool must not touch the
# original, root-owned file.
# ---------------------------------------------------------------------------
$(NATIVE_KERN_DIR):
	@echo "  PREP    $@"
	@echo "          $(KERN_DIR) ships $(if $(tools_elf),ELF machine $(tools_elf),no) host tools; this host is $(host_machine)"
	rm -rf $@.tmp
	mkdir -p $(dir $@)
	cp -a $(KERN_SRC) $@.tmp
	@# the package's directories are root-owned and read-only for everyone, a
	@# mode cp -a faithfully reproduces -- we have to write the tools into them
	chmod -R u+w $@.tmp
	@cd $@.tmp && set -e; for c in \
		scripts/basic/.fixdep.cmd \
		scripts/mod/.modpost.o.cmd \
		scripts/mod/.file2alias.o.cmd \
		scripts/mod/.sumversion.o.cmd \
		scripts/mod/.mk_elfconfig.cmd \
		scripts/mod/.modpost.cmd \
		scripts/mod/.elfconfig.h.cmd ; do \
		cmd=$$(sed -n 's/^cmd_[^:]*:= //p' $$c); \
		[ -n "$$cmd" ] || { echo "no build command recorded in $$c"; exit 1; }; \
		echo "  HOSTCC  $$cmd"; \
		sh -c "$$cmd"; \
	done; \
	for t in scripts/basic/fixdep scripts/mod/modpost; do \
		[ -x $$t ] || { echo "$$t was not built"; exit 1; }; \
	done
	mv $@.tmp $@
	@echo "  PREP    done, host tools now native for $(host_machine)"

test: all
	sudo rmmod $(MODULE_NAME).ko || true
	sudo insmod $(MODULE_NAME).ko || true

# ---------------------------------------------------------------------------
# Editor support.  kbuild records one .<obj>.cmd per object, and the kernel
# ships the tool that turns those into a compilation database, so every build
# refreshes compile_commands.json for clangd (see .clangd).  Generated, not
# committed.
# ---------------------------------------------------------------------------
GEN_COMPILE_COMMANDS := $(KBUILD_KERN_DIR)/scripts/clang-tools/gen_compile_commands.py

.PHONY: compile_commands.json
compile_commands.json:
	@if [ -f "$(GEN_COMPILE_COMMANDS)" ]; then \
		python3 "$(GEN_COMPILE_COMMANDS)" -d $(CURDIR) -o $(CURDIR)/$@ && \
			echo "  CC-DB   $@"; \
	else \
		echo "  CC-DB   skipped, $(GEN_COMPILE_COMMANDS) is not there"; \
	fi

obj-m += $(MODULE_NAME).o
$(MODULE_NAME)-y += usb.o jpegenc.o encoder.o rgb565_qoi.o rgb565_rle.o fb.o drm.o input.o
