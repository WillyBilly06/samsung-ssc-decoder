"""Reset the ESP32 and capture its console. Usage: python serial_log.py COM7 seconds [out.log] [--no-reset] [--baud=N]"""
import sys
import time

import serial

port, secs = sys.argv[1], float(sys.argv[2])
out = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].startswith('--') else None
baud = next((int(a.split('=')[1]) for a in sys.argv if a.startswith('--baud=')), 921600)
s = serial.Serial()
s.port, s.baudrate, s.timeout = port, baud, 0.2
s.dtr = False  # deassert before open: with defaults, opening the port resets the board
s.rts = False
s.open()
if '--no-reset' not in sys.argv:
    s.dtr = False
    s.rts = True   # EN low
    time.sleep(0.1)
    s.rts = False  # release EN, boot normally (IO0 high)
end = time.time() + secs
buf = bytearray()
while time.time() < end:
    buf += s.read(4096)
s.close()
text = buf.decode('utf-8', 'replace')
if out:
    open(out, 'a', encoding='utf-8').write(text)
print(text[-12000:])
