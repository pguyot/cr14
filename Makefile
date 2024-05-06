# SPDX-License-Identifier: GPL-2.0
KERNELRELEASE ?= $(shell uname -r)

obj-m += cr14.o
dtbo-y += cr14.dtbo

targets += $(dtbo-y)

# Gracefully supporting the new always-y without cutting off older target with kernel 4.x
ifeq ($(firstword $(subst ., ,$(KERNELRELEASE))),4)
	always := $(dtbo-y)
else
	always-y := $(dtbo-y)
endif

all:
	make -C /lib/modules/$(KERNELRELEASE)/build M=$(PWD) modules

# dtbo rule is no longer available
ifeq ($(firstword $(subst ., ,$(KERNELRELEASE))),6)
all: cr14.dtbo

cr14.dtbo: cr14-overlay.dts
	dtc -I dts -O dtb -o $@ $<
endif

clean:
	make -C /lib/modules/$(KERNELRELEASE)/build M=$(PWD) clean

install: cr14.ko cr14.dtbo
	install -o root -m 755 -d /lib/modules/$(KERNELRELEASE)/kernel/input/misc/
	install -o root -m 644 cr14.ko /lib/modules/$(KERNELRELEASE)/kernel/input/misc/
	depmod -a $(KERNELRELEASE)
	install -o root -m 644 cr14.dtbo /boot/overlays/
	sed /boot/config.txt -i -e "s/^#dtparam=i2c_arm=on/dtparam=i2c_arm=on/"
	grep -q -E "^dtparam=i2c_arm=on" /boot/config.txt || printf "dtparam=i2c_arm=on\n" >> /boot/config.txt
	sed /boot/config.txt -i -e "s/^#dtoverlay=cr14/dtoverlay=cr14/"
	grep -q -E "^dtoverlay=cr14" /boot/config.txt || printf "dtoverlay=cr14\n" >> /boot/config.txt

.PHONY: all clean install
