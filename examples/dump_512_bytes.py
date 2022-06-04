#!/usr/bin/env python3

import os

# Example code demonstrating how to read multiple blocks.
# Dumps the first 512 bytes (typically every byte for 512 bytes PICCs)

rfid = os.open("/dev/rfid0", os.O_RDWR)
print("Waiting for a chip")
try:
    os.write(rfid, b"p")
    packet = os.read(rfid, 9)
    if packet[0] != ord("u"):
        print(f"Unexpected packet header {packet[0]}")
    else:
        # uid is in little endian
        uid = bytearray(packet[1:])
        uid.reverse()
        uid_str = ":".join("{:02x}".format(c) for c in uid)
        print(f"UID: {uid_str}")
        if uid[0] != 0xD0:
            print(f"Unexpected MSB, got {uid[0]}")
        os.write(
            rfid,
            b"R"
            + packet[1:]
            + b"\x10\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F",
        )
        packet = os.read(rfid, 1)
        while packet == b"u":
            os.read(rfid, 8)
            packet = os.read(rfid, 1)
        if packet == b"R":
            count = os.read(rfid, 1)
            if count[0] != 16:
                print(f"Unexpected block counts, got {count[0]}, expected 16")
            for x in range(0, count[0]):
                data = os.read(rfid, 4)
                data = bytearray(data)
                data.reverse()
                data_str = ":".join("{:02x}".format(c) for c in data)
                print(f"{x} {data_str}")
        else:
            print(f"Unexpected packet header {packet[0]}")
except KeyboardInterrupt:
    pass
os.close(rfid)
