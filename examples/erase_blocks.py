#!/usr/bin/env python3

import os

# Example code demonstrating how to write several blocks in a row.
# Write FFFFFFFF to blocks 7 to 9 (that may be affected by other scripts)

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
        erased_data = b"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
        os.write(rfid, b"W" + uid_le + b"\x03\x07\x08\x09" + erased_data)
        packet = os.read(rfid, 1)
        while packet == b"u":
            os.read(rfid, 8)
            packet = os.read(rfid, 1)
        if packet == b"W":
            count = os.read(rfid, 1)
            if count[0] != 3:
                print(f"Unexpected count, got {count[0]}, expected 3")
            else:
                data = os.read(rfid, 12)
                if data != erased_data:
                    print(
                        f"Data mismatch, got {data.hex()} but wrote {erased_data.hex()}"
                    )
                else:
                    print("Erased blocks 7 to 9 (wrote FFFFFFFF)")
        else:
            print(f"Unexpected packet header {packet[0]}")
except KeyboardInterrupt:
    pass
os.close(rfid)
