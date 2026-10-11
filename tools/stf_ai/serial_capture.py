#!/usr/bin/env python3
"""Capture the rng-bench probe's serial output from a real board to a file
(Pinboard #611). Windows or Linux; needs pyserial (py -m pip install pyserial).

  py serial_capture.py                      list the serial ports, then capture
  py serial_capture.py --port COM5 --seconds 120 --out 611-rng-bench-capture.bin

Start it, then reset the board and let attract run. Every second it prints the
bytes so far and how many bench packets (5A A6, slot, 3 bytes) it has seen, so
you can tell straight away whether anything is coming in.

It raises DTR and RTS once the port is open (--no-dtr / --no-rts to leave them
down). Many RS-422/RS-485 converters take their power from those lines; .NET's
SerialPort, which the PowerShell capture used, leaves both down.
"""
import argparse
import os
import sys
import time

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit('pyserial is missing: py -m pip install pyserial')

PACKET = b'\x5a\xa6'
PACKET_LEN = 6


def default_out():
    """The pinboard uploads folder when it exists, else the current one."""
    up = os.path.join(os.path.expanduser('~'), 'source', 'repos', 'ai',
                      'pinboard', 'data', 'uploads')
    name = '611-rng-bench-capture.bin'
    return os.path.join(up, name) if os.path.isdir(up) else name


def list_ports():
    ports = sorted(serial.tools.list_ports.comports(), key=lambda p: p.device)
    for p in ports:
        print(f'  {p.device:8} {p.description} [{p.hwid}]')
    if not ports:
        print('  (no serial ports found: is the adapter plugged in, and not '
              'attached to WSL with usbipd?)')
    return ports


def open_port(args):
    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.bytesize, ser.parity, ser.stopbits = 8, 'N', 1
    ser.xonxoff = ser.rtscts = ser.dsrdtr = False
    ser.timeout = 0.05
    ser.dtr = not args.no_dtr
    ser.rts = not args.no_rts
    ser.open()
    if hasattr(ser, 'set_buffer_size'):  # Windows only
        ser.set_buffer_size(rx_size=1 << 20, tx_size=4096)
    ser.reset_input_buffer()
    return ser


class Counter:
    """Counts bench packets in the stream as it arrives."""

    def __init__(self):
        self.tail = b''
        self.packets = 0
        self.last_slot = None

    def feed(self, data):
        buf = self.tail + data
        i = 0
        while True:
            j = buf.find(PACKET, i)
            if j < 0 or j + PACKET_LEN > len(buf):
                break
            self.packets += 1
            self.last_slot = buf[j + 2]
            i = j + PACKET_LEN
        self.tail = buf[max(i, len(buf) - PACKET_LEN + 1):]


def capture(ser, out, seconds):
    count = Counter()
    total = 0
    start = time.monotonic()
    next_report = start + 1
    with open(out, 'wb') as f:
        while time.monotonic() - start < seconds:
            data = ser.read(max(1, ser.in_waiting))
            if data:
                f.write(data)
                total += len(data)
                count.feed(data)
            now = time.monotonic()
            if now >= next_report:
                next_report = now + 1
                slot = '-' if count.last_slot is None else f'{count.last_slot:02X}'
                print(f'\r{now - start:5.0f} s  {total:8d} bytes  '
                      f'{count.packets:6d} packets  last slot {slot}  ',
                      end='', flush=True)
    print()
    return total, count.packets


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', help='e.g. COM5 (default: the only port there is)')
    ap.add_argument('--baud', type=int, default=2000000)
    ap.add_argument('--seconds', type=float, default=120)
    ap.add_argument('--out', default=default_out())
    ap.add_argument('--no-dtr', action='store_true', help='leave DTR down')
    ap.add_argument('--no-rts', action='store_true', help='leave RTS down')
    ap.add_argument('--list', action='store_true', help='list ports and stop')
    args = ap.parse_args()

    print('Serial ports:')
    ports = list_ports()
    if args.list:
        return
    if not args.port:
        if len(ports) != 1:
            sys.exit('Pick one with --port COMn.')
        args.port = ports[0].device

    try:
        ser = open_port(args)
    except (serial.SerialException, ValueError) as e:
        sys.exit(f'Could not open {args.port} at {args.baud}: {e}')
    print(f'{args.port} open at {args.baud} 8N1, DTR {"on" if ser.dtr else "off"}, '
          f'RTS {"on" if ser.rts else "off"}. Reset the board now.')
    print(f'Writing {args.out} for {args.seconds:.0f} s (Ctrl+C stops early).')
    try:
        total, packets = capture(ser, args.out, args.seconds)
    except KeyboardInterrupt:
        print('\nStopped.')
        total = os.path.getsize(args.out)
        packets = None
    finally:
        ser.close()
    print(f'{args.out}: {total} bytes'
          + ('' if packets is None else f', {packets} bench packets'))
    if total == 0:
        print('Nothing came in. Try --no-dtr / --no-rts, check the converter '
              'is powered and its TX/RX pairs, and that the game reached attract.')


if __name__ == '__main__':
    main()
