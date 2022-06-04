#!/usr/bin/env python3

import os

# Example code demonstrating how to read and write several blocks in a row.
# Read blocks #7, #8 and #9 and swap them.

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
        os.write(rfid, b"R" + uid_le + b"\x03\x07\x08\x09")
        packet = os.read(rfid, 1)
        while packet == b"u":
            os.read(rfid, 8)
            packet = os.read(rfid, 1)
        if packet == b"R":
            count = os.read(rfid, 1)
            if count[0] != 3:
                print(f"Unexpected count, got {count[0]}, expected 3")
            else:
                block7 = os.read(rfid, 4)
                block8 = os.read(rfid, 4)
                block9 = os.read(rfid, 4)
                os.write(
                    rfid,
                    b"W"
                    + uid_le
                    + b"\x03\x07\x08\x09"
                    + block8
                    + block9
                    + block7,
                )
                packet = os.read(rfid, 1)
                if packet == b"W":
                    count = os.read(rfid, 1)
                    if count[0] != 3:
                        print(f"Unexpected count, got {count[0]}, expected 3")
                    else:
                        written7 = os.read(rfid, 4)
                        written8 = os.read(rfid, 4)
                        written9 = os.read(rfid, 4)
                        if written7 != block8:
                            print(
                                f"Data mismatch, got {written7.hex()}"
                                f" but wrote {block8.hex()}"
                            )
                        if written8 != block9:
                            print(
                                f"Data mismatch, got {written7.hex()}"
                                f" but wrote {block8.hex()}"
                            )
                        if written9 != block7:
                            print(
                                f"Data mismatch, got {written7.hex()}"
                                f" but wrote {block8.hex()}"
                            )
                        print(f"New 7: {written7.hex()}")
                        print(f"New 8: {written8.hex()}")
                        print(f"New 9: {written9.hex()}")
                else:
                    print(f"Unexpected packet, got {packet.hex()}, expected w")
        else:
            print(f"Unexpected packet header {packet[0]}")
except KeyboardInterrupt:
    pass
os.close(rfid)
