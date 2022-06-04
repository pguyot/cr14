# CR14 RFID reader driver for Linux (Raspbian)

[![GitHub Super-Linter](https://github.com/pguyot/cr14/actions/workflows/super-linter.yml/badge.svg)](https://github.com/marketplace/actions/super-linter)

[![ARM Runner](https://github.com/pguyot/cr14/actions/workflows/arm-runner.yml/badge.svg)](https://github.com/marketplace/actions/arm-runner)

## Datasheet and technical documents

Chipset:
- [CR14](https://datasheet.octopart.com/CR14-MQP/1GE-STMicroelectronics-datasheet-10836722.pdf)


PICC:
- [SRIX4K](http://www.orangetags.com/wp-content/downloads/datasheet/STM/srix4k.pdf)
- [SRI512](https://www.advanide.de/wp-content/uploads/products/rfid/SRI512.pdf)
- [SRT512](https://www.advanide.de/wp-content/uploads/products/rfid/SRT512.pdf)
- [SRI4K](https://www.advanide.de/wp-content/uploads/products/rfid/SRI4K.pdf)
- [SRI2K](https://www.advanide.de/wp-content/uploads/products/rfid/SRI2K.pdf)
- [ST25TB512-AC](https://www.st.com/resource/en/datasheet/st25tb512-ac.pdf)
- [ST25TB04K](https://www.st.com/resource/en/datasheet/st25tb04k.pdf)
- [ST25TB512-AT](https://www.st.com/resource/en/datasheet/st25tb512-at.pdf)
- [ST25TB02K](https://www.st.com/resource/en/datasheet/st25tb02k.pdf)

This driver may be compatible with CRX14 as well.
Tested with CR14 and SRI512/SRT512 PICCs.

## Installation

Install requirements

    sudo apt-get install raspberrypi-kernel-headers

Clone source code.

    git clone https://github.com/pguyot/cr14

Compile and install with

    cd cr14
    make
    sudo make install

Makefile will automatically edit /boot/config.txt and add/enable if required the following params and overlays:

    dtparam=i2c_arm=on
    dtoverlay=cr14

You might want to review changes before rebooting.

Reboot.

## Interface

Driver creates device /dev/rfid0

Several modes are available, see the sample Python scripts in examples.

Simplest mode consists in opening device read-only. The CR14 will be polled every 0.5 second, printing detected tag UIDs preceeded by 'u' (UIDs are printed in little endian, LSB first).

More complex interactions are possible by opening device r/w and sending commands, for example to read or write EEPROM.
