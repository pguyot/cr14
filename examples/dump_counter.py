#!/usr/bin/env python3

import os

# Example code demonstrating how to read multiple blocks.
# Print the counter values at blocks 5 and 6.

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
        os.write(rfid, b"R" + packet[1:] + b"\x02\x05\x06")
        packet = os.read(rfid, 1)
        while packet == b"u":
            os.read(rfid, 8)
            packet = os.read(rfid, 1)
        if packet == b"R":
            count = os.read(rfid, 1)
            if count[0] != 2:
                print(f"Unexpected block counts, got {count[0]}, expected 2")
            for x in range(5, 7):
                data = os.read(rfid, 4)
                counter_value = int.from_bytes(data, byteorder="little")
                print(f"{x} counter={counter_value} ({data.hex()})")
        else:
            print(f"Unexpected packet header {packet[0]}")
except KeyboardInterrupt:
    pass
os.close(rfid)
