# SPDX-License-Identifier: GPL-2.0
KERNELRELEASE ?= $(shell uname -r)

obj-m += cr14.o
dtbo-y += cr14.dtbo

targets += $(dtbo-y)
always-y := $(dtbo-y)

all:
	make -C /lib/modules/$(KERNELRELEASE)/build M=$(PWD) modules

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
