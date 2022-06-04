#!/usr/bin/env python3

import os

MODELS = {
    0x02: {
        (
            6,
            0b000011,
            "SRIX4K",
        ),  # http://www.orangetags.com/wp-content/downloads/datasheet/STM/srix4k.pdf
        (
            6,
            0b000110,
            "SRI512",
        ),  # http://www.advanide.com/wp-content/uploads/products/rfid/SRI512.pdf
        (
            6,
            0b001100,
            "SRT512",
        ),  # https://www.advanide.de/wp-content/uploads/products/rfid/SRT512.pdf
        (
            6,
            0b000111,
            "SRI4K",
        ),  # https://www.advanide.de/wp-content/uploads/products/rfid/SRI4K.pdf
        (
            6,
            0b001111,
            "SRI2K",
        ),  # https://www.advanide.de/wp-content/uploads/products/rfid/SRI2K.pdf
        (
            8,
            0x1B,
            "ST25TB512-AC",
        ),  # https://www.st.com/resource/en/datasheet/st25tb512-ac.pdf
        (
            8,
            0x1F,
            "ST25TB04K",
        ),  # https://www.st.com/resource/en/datasheet/st25tb04k.pdf
        (
            8,
            0x33,
            "ST25TB512-AT",
        ),  # https://www.st.com/resource/en/datasheet/st25tb512-at.pdf
        (
            8,
            0x3F,
            "ST25TB02K",
        ),  # https://www.st.com/resource/en/datasheet/st25tb02k.pdf
    }
}
MANUFACTURERS = {
    0x01: "Motorola",
    0x02: "ST Microelectronics",
    0x03: "Hitachi",
    0x04: "NXP Semiconductors",
    0x05: "Infineon Technologies",
    0x06: "Cylinc",
    0x07: "Texas Instruments Tag-it",
    0x08: "Fujitsu Limited",
    0x09: "Matsushita Electric Industrial",
    0x0A: "NEC",
    0x0B: "Oki Electric",
    0x0C: "Toshiba",
    0x0D: "Mitsubishi Electric",
    0x0E: "Samsung Electronics",
    0x0F: "Hyundai Electronics",
    0x10: "LG Semiconductors",
    0x16: "EM Microelectronic-Marin",
    0x1F: "Melexis",
    0x2B: "Maxim",
    0x33: "AMIC",
    0x44: "GenTag, Inc (USA)",
    0x45: "Invengo Information Technology Co.Ltd",
}

rfid = os.open("/dev/rfid0", os.O_RDONLY)
print("Exit with control-C")
while True:
    try:
        packet = os.read(rfid, 9)
        if packet[0] != ord("u"):
            print(f"Unexpected packet header {packet[0]}")
            break
        # uid is in little endian
        uid = bytearray(packet[1:])
        uid.reverse()
        uid_str = ":".join("{:02x}".format(c) for c in uid)
        print(f"UID: {uid_str}")
        if uid[0] != 0xD0:
            print(f"Unexpected MSB, got {uid[0]}")
        if uid[1] in MANUFACTURERS:
            manufacturer = MANUFACTURERS[uid[1]]
            print(f"Manufacturer: {manufacturer}")
        else:
            print(f"Manufacturer: unknown ({int(uid[1])})")
        serial_start = 2
        if uid[1] in MODELS:
            model_unknown = True
            for bits, model_id, model_str in MODELS[uid[1]]:
                model = uid[2]
                if bits < 8:
                    model = model >> (8 - bits)
                if model == model_id:
                    print(f"Model: {model_str}")
                    if bits < 8:
                        uid[2] = uid[2] & ~(model << (8 - bits))
                    else:
                        serial_start = 3
                    model_unknown = False
                    break
            if model_unknown:
                print(f"Model: unknown ({uid[2]})")
        serial = ":".join("{:02x}".format(c) for c in uid[serial_start:])
        print(f"Serial number: {serial}")
    except KeyboardInterrupt:
        break
os.close(rfid)
