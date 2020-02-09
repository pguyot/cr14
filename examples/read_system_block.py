#!/usr/bin/env python3

import os

rfid = os.open("/dev/rfid0", os.O_RDWR)
print("Waiting for a chip")
try:
    os.write(rfid, b'p')
    packet = os.read(rfid, 9)
    if packet[0] != ord('u'):
        print(f"Unexpected packet header {packet[0]}")
    else:
        # uid is in little endian
        uid_le = packet[1:]
        uid_be = bytearray(uid_le)
        uid_be.reverse()
        uid_str = ":".join("{:02x}".format(c) for c in uid_be)
        print(f"UID: {uid_str}")
        if uid_be[0] != 0xD0:
            print(f"Unexpected MSB, got {uid_be[0]}")
        if uid_be[1] != 0x02:
            print(f"Not a STMicroelectronics chip, will read block 255 anyway")
        os.write(rfid, b'r' + uid_le + b'\xFF')
        packet = os.read(rfid, 1)
        while packet == b'u':
            os.read(rfid, 8)
            packet = os.read(rfid, 1)
        if packet == b'r':
            data = os.read(rfid, 4)
            data = bytearray(data)
            data.reverse()
            data_str = ":".join("{:02x}".format(c) for c in data)
            print(f"Data (read single): {data_str}")
        else:
            print(f"Unexpected packet header {packet[0]}")
        os.write(rfid, b'R' + uid_le + b'\x01\xFF')
        packet = os.read(rfid, 1)
        while packet == b'u':
            os.read(rfid, 8)
            packet = os.read(rfid, 1)
        if packet == b'R':
            count = os.read(rfid, 1)
            if count[0] != 1:
                print(f"Unexpected count, got {count}, expected 1")
            data = os.read(rfid, 4)
            data = bytearray(data)
            data.reverse()
            data_str = ":".join("{:02x}".format(c) for c in data)
            print(f"Data (read block): {data_str}")
        else:
            print(f"Unexpected packet header {packet[0]}")
except KeyboardInterrupt:
    pass
os.close(rfid)
