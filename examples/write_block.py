#!/usr/bin/env python3

import os

# Example code demonstrating how to read and write single block.
# Read block #7 and write it back, as a counter.

rfid = os.open("/dev/rfid0", os.O_RDWR)
print("Waiting for a chip")
try:
    os.write(rfid, b"p")
    packet = os.read(rfid, 9)
    if packet[0] != ord("u"):
        print(f"Unexpected packet header {packet[0]}")
    else:
        # uid is in little endian
        uid_le = packet[1:]
        uid = bytearray(uid_le)
        uid.reverse()
        uid_str = ":".join("{:02x}".format(c) for c in uid)
        print(f"UID: {uid_str}")
        if uid[0] != 0xD0:
            print(f"Unexpected MSB, got {uid[0]}")
        os.write(rfid, b"r" + uid_le + b"\x07")
        packet = os.read(rfid, 1)
        while packet == b"u":
            os.read(rfid, 8)
            packet = os.read(rfid, 1)
        if packet == b"r":
            data = os.read(rfid, 4)
            counter = int.from_bytes(data, byteorder="little")
            print(f"Counter = {counter}, decrementing it")
            counter = counter - 1
            data = counter.to_bytes(4, byteorder="little", signed=counter < 0)
            os.write(rfid, b"w" + uid_le + b"\x07" + data)
            packet = os.read(rfid, 1)
            if packet == b"w":
                written_data = os.read(rfid, 4)
                if written_data != data:
                    print(
                        f"Data mismatch, got {written_data} but wrote {data}"
                    )
            else:
                print(f"Unexpected packet, got {packet}, expected w")
        else:
            print(f"Unexpected packet header {packet[0]}")
except KeyboardInterrupt:
    pass
os.close(rfid)
