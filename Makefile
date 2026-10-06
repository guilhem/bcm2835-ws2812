# SPDX-License-Identifier: GPL-2.0-only
ifneq ($(KERNELRELEASE),)
obj-m := bcm2835-ws2812.o
else
KDIR ?= /lib/modules/$(shell uname -r)/build
DTC ?= dtc
HOSTCC ?= cc
DESTDIR ?=
OVERLAYDIR ?= /boot/firmware/overlays

.PHONY: all modules overlay check clean install
all: modules overlay
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
overlay: bcm2835-ws2812.dtbo
bcm2835-ws2812.dtbo: bcm2835-ws2812-overlay.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<
check:
	mkdir -p build
	$(HOSTCC) -std=c99 -Wall -Wextra -Werror -pedantic tests/encode.c -o build/check-encode
	./build/check-encode
clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f bcm2835-ws2812.dtbo build/check-encode
install: modules overlay
	$(MAKE) -C $(KDIR) M=$(CURDIR) INSTALL_MOD_PATH=$(DESTDIR) modules_install
	install -D -m 0644 bcm2835-ws2812.dtbo $(DESTDIR)$(OVERLAYDIR)/bcm2835-ws2812.dtbo
endif
