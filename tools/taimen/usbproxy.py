#!/usr/bin/env python3
"""The PC end of the phone's HTTP proxy: internet for iOS over the USB link without admin rights.

The phone's netsvcd (tools/phone/netsvcd.c) makes 127.0.0.1:8080 iOS's HTTP and HTTPS proxy and
splices each app connection onto one of the idle connections this agent keeps open to
192.168.7.2:5142. The PC only connects out, so no firewall rule or NAT is needed. On each one
used, this is an ordinary HTTP proxy: CONNECT host:port is tunneled (HTTPS), absolute-URI
requests are forwarded (plain HTTP). Runs from WSL or any Python 3.8+.

    tools/taimen/usbproxy.py [--phone 192.168.7.2] [--pool 24] [--minutes N]
    tools/taimen/usbproxy.py --status           netsvcd's state (port 5143) and exit
"""
import argparse
import asyncio
import socket
import sys
import time
import urllib.parse

AGENT_PORT, STATUS_PORT = 5142, 5143
stats = {'requests': 0, 'failed': 0, 'up': 0, 'down': 0}


def log(msg):
    print(time.strftime('%H:%M:%S'), msg, flush=True)


async def pipe(r, w, key):
    try:
        while True:
            b = await r.read(65536)
            if not b:
                break
            stats[key] += len(b)
            w.write(b)
            await w.drain()
    except (ConnectionError, OSError, asyncio.CancelledError):
        pass
    finally:
        try:
            w.close()
        except Exception:
            pass


async def handle(head, r, w):
    """head: the app's request head, up to the blank line"""
    line, _, rest = head.partition(b'\r\n')
    try:
        method, target, version = line.split(b' ', 2)
        if method == b'CONNECT':
            host, _, port = target.decode().rpartition(':')
            host, port = host.strip('[]'), int(port)
            ur, uw = await asyncio.wait_for(asyncio.open_connection(host, port), 20)
            w.write(b'HTTP/1.1 200 Connection established\r\n\r\n')
            await w.drain()
        else:
            u = urllib.parse.urlsplit(target.decode())
            host, port = u.hostname, u.port or 80
            path = (u.path or '/') + ('?' + u.query if u.query else '')
            ur, uw = await asyncio.wait_for(asyncio.open_connection(host, port), 20)
            hdrs = [h for h in rest.split(b'\r\n') if h and not h.lower().startswith(b'proxy-')]
            uw.write(b' '.join([method, path.encode(), version]) + b'\r\n' + b'\r\n'.join(hdrs) + b'\r\n\r\n')
            await uw.drain()
    except (OSError, ValueError, asyncio.TimeoutError) as e:
        stats['failed'] += 1
        log(f'{line[:100].decode(errors="replace")}: {e.__class__.__name__} {e}')
        try:
            w.write(b'HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
            await w.drain()
            w.close()
        except Exception:
            pass
        return
    stats['requests'] += 1
    log(f'{method.decode()} {host}:{port}')
    await asyncio.gather(pipe(r, uw, 'up'), pipe(ur, w, 'down'))


async def slot(phone):
    """one idle connection to the phone at a time; a replacement opens as soon as one is used"""
    said = False
    while True:
        try:
            r, w = await asyncio.wait_for(asyncio.open_connection(phone, AGENT_PORT), 5)
        except (OSError, asyncio.TimeoutError):
            if not said:
                log(f'waiting for the phone ({phone}:{AGENT_PORT})')
                said = True
            await asyncio.sleep(2)
            continue
        if said:
            log(f'connected to {phone}:{AGENT_PORT}')
            said = False
        sock = w.get_extra_info('socket')
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # the phone resets without closing anything: keepalive finds the dead idle connections
        # (a rebooted phone answers the probes with RST) and the slot opens a new one
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        for opt, val in (('TCP_KEEPIDLE', 10), ('TCP_KEEPINTVL', 5), ('TCP_KEEPCNT', 3)):
            if hasattr(socket, opt):
                sock.setsockopt(socket.IPPROTO_TCP, getattr(socket, opt), val)
        try:
            head = await r.readuntil(b'\r\n\r\n')
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, OSError):
            w.close()
            await asyncio.sleep(0.5)
            continue
        asyncio.ensure_future(handle(head, r, w))


async def report():
    while True:
        await asyncio.sleep(60)
        log(f'{stats["requests"]} requests ({stats["failed"]} failed), '
            f'{stats["up"] / 1e6:.2f} MB up, {stats["down"] / 1e6:.2f} MB down')


async def main(a):
    tasks = [asyncio.ensure_future(slot(a.phone)) for _ in range(a.pool)] + [asyncio.ensure_future(report())]
    if a.minutes:
        await asyncio.sleep(a.minutes * 60)
        log('time is up')
    else:
        await asyncio.gather(*tasks)


def status(phone):
    with socket.create_connection((phone, STATUS_PORT), 5) as s:
        data = b''
        while True:
            b = s.recv(65536)
            if not b:
                break
            data += b
    sys.stdout.write(data.decode(errors='replace'))


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--phone', default='192.168.7.2')
    ap.add_argument('--pool', type=int, default=24, help='idle connections kept open (default 24)')
    ap.add_argument('--minutes', type=float, default=0, help='stop after this long (default: never)')
    ap.add_argument('--status', action='store_true', help="print netsvcd's state and exit")
    a = ap.parse_args()
    if a.status:
        status(a.phone)
    else:
        try:
            asyncio.run(main(a))
        except KeyboardInterrupt:
            pass
