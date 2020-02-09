# SPDX-License-Identifier: GPL-2.0
obj-m += cr14.o
dtbo-y += cr14.dtbo

targets += $(dtbo-y)
always  := $(dtbo-y)
kernel_img_gzip_offset := $(shell grep -m 1 -abo 'uncompression error' /boot/kernel.img | cut -d ':' -f 1)
kernel_img_gzip_offset := $(shell expr $(kernel_img_gzip_offset) + 20)
kernel_version := $(shell dd if=/boot/kernel.img skip=$(kernel_img_gzip_offset) iflag=skip_bytes of=/dev/stdout | zgrep -aPom1 'Linux version \K\S+')

all:
	make -C /lib/modules/$(kernel_version)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(kernel_version)/build M=$(PWD) clean

install: cr14.ko cr14.dtbo
	install -o root -m 755 -d /lib/modules/$(kernel_version)/kernel/input/misc/
	install -o root -m 644 cr14.ko /lib/modules/$(kernel_version)/kernel/input/misc/
	depmod -a $(kernel_version)
	install -o root -m 644 cr14.dtbo /boot/overlays/
	sed /boot/config.txt -i -e "s/^#dtparam=i2c_arm=on/dtparam=i2c_arm=on/"
	grep -q -E "^dtparam=i2c_arm=on" /boot/config.txt || printf "dtparam=i2c_arm=on\n" >> /boot/config.txt
	sed /boot/config.txt -i -e "s/^#dtoverlay=cr14/dtoverlay=cr14/"
	grep -q -E "^dtoverlay=cr14" /boot/config.txt || printf "dtoverlay=cr14\n" >> /boot/config.txt

.PHONY: all clean install
