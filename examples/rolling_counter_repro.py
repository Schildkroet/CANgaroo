"""
Rolling-counter decoding bug reproduction / regression test.

Load examples/rolling_counter_repro.dbc (ships alongside this script) and this
script will periodically send synthetic CAN FD frames for message
TestMsg_A (0x100) with:
  - ChecksumByte   : fixed sanity-check byte (0x08), byte 0 of the payload
  - RollingCounter : a 4-bit rolling counter that increments 0..14 each frame
                     (bits 11..8 - the low nibble of byte 1 - a big-endian/
                     Motorola nibble starting mid-byte, which is exactly the
                     case that used to decode as a constant 0 before the
                     endianness fix)
  - StatusBits     : fixed value (0)
  - all other bytes: 0x00

The payload bytes are built by hand and sent as raw data (no cangaroo.encode()
/ DBC-based injection involved), so this exercises only the DECODE path
(BusMessage::extractRawSignal()) that CANgaroo uses when showing signals in
the Monitor/Signals view - independent of the encode()/inject path.

Usage:
  1. Add examples/rolling_counter_repro.dbc to Measurement Setup > Databases.
  2. Start the measurement (a real interface or a virtual one, e.g. vcan0,
     is fine - this only needs a loopback/self-receive to verify decoding).
  3. Paste this script into the Script window and click Run.
  4. Watch RollingCounter in the Monitor / Signals view - it should count
     0, 1, 2, ... 14, 0, 1, ... every 100 ms instead of staying at 0.
"""
import cangaroo
import time

INTERFACE_ID = 0     # change to match your setup
MESSAGE_ID   = 0x100  # TestMsg_A
PAYLOAD_LEN  = 32     # TestMsg_A is a 32-byte CAN FD frame
INTERVAL_S   = 0.1
COUNTER_MAX  = 14     # per DBC: RollingCounter range is [0|14]

# ---- show available interfaces ----
for iface in cangaroo.interfaces():
    print(f"Interface {iface['id']}: {iface['name']}")

counter = 0
print(f"Sending 0x{MESSAGE_ID:03X} every {INTERVAL_S * 1000:.0f} ms with an "
      f"incrementing RollingCounter (0..{COUNTER_MAX}). Stop the script to end.\n")

try:
    while True:
        # Build the raw payload by hand instead of using cangaroo.encode():
        #   byte 0            = ChecksumByte (bits 7..0, byte-aligned)
        #   byte 1, low nibble = RollingCounter (bits 11..8)
        #   everything else    = 0x00
        payload = bytearray(PAYLOAD_LEN)
        payload[0] = 0x08              # ChecksumByte
        payload[1] = counter & 0x0F    # RollingCounter (low nibble of byte 1)

        msg = cangaroo.Message()
        msg.id = MESSAGE_ID
        msg.fd = True  # 32-byte payload -> needs CAN FD
        msg.set_data(bytes(payload))

        cangaroo.send(msg, interface_id=INTERFACE_ID)
        print(f"TX 0x{msg.id:03X}  RollingCounter={counter:<2}  "
              f"data={msg.get_data().hex(' ')}")

        counter = (counter + 1) % (COUNTER_MAX + 1)
        time.sleep(INTERVAL_S)
except KeyboardInterrupt:
    pass

print("Done.")

