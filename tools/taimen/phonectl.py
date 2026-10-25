#!/usr/bin/env python3
"""Drive the phone's UI over its USB link (xnu 0304, TCP 5144 on 192.168.7.2).

    tools/taimen/phonectl.py tap X Y                 panel coordinates (1440 x 2880)
    tools/taimen/phonectl.py tapc X Y                screenshot (canvas, 1125 x 2436) coordinates
    tools/taimen/phonectl.py swipe X1 Y1 X2 Y2 [MS]  (swipec: canvas coordinates)
    tools/taimen/phonectl.py unlock | home           swipe up from the bottom edge
    tools/taimen/phonectl.py shot OUT.png            the 1125 x 2436 canvas iOS last drew
    tools/taimen/phonectl.py type TEXT | enter | backspace [N] | tab | escape   (xnu 0307)
    tools/taimen/phonectl.py key power|volup|voldown|home
    tools/taimen/phonectl.py end                     end the boot now, as taimenreboot would (xnu 0331)
    tools/taimen/phonectl.py run 'tapc 700 875' 'sleep 3' 'shot s.png' ...
The canvas is scaled 2880/2436 onto the panel and centred: panel x = 55 + 1.182 x, y = 1.182 y.
"""
import socket
import struct
import sys
import time
import zlib

PHONE, PORT = '192.168.7.2', 5144


def png(path, w, h, bgra):
    rows = bytearray()
    for y in range(h):
        row = bgra[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = row[2::4], row[1::4], row[0::4]
        rows += b'\0' + rgb

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))


class Phone:
    def __init__(self):
        self.s = socket.create_connection((PHONE, PORT), 5)
        self.s.settimeout(30)
        self.buf = b''

    def line(self):
        while b'\n' not in self.buf:
            b = self.s.recv(65536)
            if not b:
                raise ConnectionError('the phone closed the connection')
            self.buf += b
        line, _, self.buf = self.buf.partition(b'\n')
        return line.decode()

    def exact(self, n):
        while len(self.buf) < n:
            b = self.s.recv(1 << 20)
            if not b:
                raise ConnectionError('the phone closed the connection')
            self.buf += b
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def cmd(self, text):
        self.s.sendall(text.encode() + b'\n')
        r = self.line()
        if not r.startswith('OK'):
            raise RuntimeError(f'{text}: {r}')

    def shot(self, path):
        t = time.time()
        self.s.sendall(b'shot\n')
        r = self.line()
        if not r.startswith('SHOT '):
            raise RuntimeError(f'shot: {r}')
        w, h = map(int, r.split()[1:3])
        data = self.exact(w * h * 4)
        if self.line() != 'OK':
            raise RuntimeError('shot: no OK after the pixels')
        png(path, w, h, data)
        print(f'{path}: {w}x{h} in {time.time() - t:.1f} s')


SCALE, OFF_X = 2880 / 2436, 55


def panel(x, y):
    return round(OFF_X + float(x) * SCALE), round(float(y) * SCALE)


def run(p, words):
    op = words[0]
    if op == 'shot':
        p.shot(words[1])
    elif op in ('unlock', 'home'):
        p.cmd('swipe 720 2870 720 1300 250')
    elif op == 'sleep':
        time.sleep(float(words[1]))
    elif op in ('tap', 'swipe', 'enter', 'backspace', 'tab', 'escape', 'key', 'end'):
        p.cmd(' '.join(words))
    elif op == 'type':
        p.cmd('type ' + ' '.join(words[1:]))
    elif op == 'tapc':
        p.cmd('tap %d %d' % panel(words[1], words[2]))
    elif op == 'swipec':
        a, b = panel(words[1], words[2]), panel(words[3], words[4])
        p.cmd('swipe %d %d %d %d %s' % (a + b + (words[5] if len(words) > 5 else '250',)))
    else:
        sys.exit(__doc__)


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    p = Phone()
    if a[0] == 'run':
        for step in a[1:]:
            run(p, step.split())
    else:
        run(p, a)


if __name__ == '__main__':
    main()
