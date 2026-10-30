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


def server_name(hello):
    """the SNI host name of a TLS ClientHello record, or None"""
    try:
        if hello[0] != 0x16 or hello[5] != 1:
            return None
        p = 5 + 4 + 2 + 32                      # record and handshake headers, version, random
        p += 1 + hello[p]                       # session id
        p += 2 + int.from_bytes(hello[p:p + 2], 'big')  # cipher suites
        p += 1 + hello[p]                       # compression methods
        end = p + 2 + int.from_bytes(hello[p:p + 2], 'big')
        p += 2
        while p + 4 <= end:
            kind, size = int.from_bytes(hello[p:p + 2], 'big'), int.from_bytes(hello[p + 2:p + 4], 'big')
            p += 4
            if kind == 0 and hello[p + 2] == 0:  # server_name, its first entry a host name
                n = int.from_bytes(hello[p + 3:p + 5], 'big')
                return hello[p + 5:p + 5 + n].decode('ascii')
            p += size
    except (IndexError, UnicodeDecodeError):
        pass
    return None


async def via_ipv4(addr, port, r, w, why):
    """A CONNECT to an IPv6 address the PC cannot reach (WSL has no IPv6 route; the phone resolves AAAA
    records through 1.1.1.1: 228 of 235 failures on 2026-10-30). It is accepted, and the server its TLS
    ClientHello names is reached over IPv4. Returns the upstream streams, a label and the ClientHello."""
    w.write(b'HTTP/1.1 200 Connection established\r\n\r\n')
    await w.drain()
    hello = await asyncio.wait_for(r.read(16384), 10)
    while 5 <= len(hello) < 5 + int.from_bytes(hello[3:5], 'big'):
        more = await asyncio.wait_for(r.read(16384), 10)
        if not more:
            break
        hello += more
    name = server_name(hello)
    if not name:
        raise OSError(f'{why}; no server name in its first {len(hello)} bytes')
    ur, uw = await asyncio.wait_for(asyncio.open_connection(name, port, family=socket.AF_INET), 20)
    return ur, uw, f'{name} (for {addr})', hello


async def handle(head, r, w):
    """head: the app's request head, up to the blank line"""
    line, _, rest = head.partition(b'\r\n')
    try:
        method, target, version = line.split(b' ', 2)
        if method == b'CONNECT':
            host, _, port = target.decode().rpartition(':')
            host, port = host.strip('[]'), int(port)
            try:
                ur, uw = await asyncio.wait_for(asyncio.open_connection(host, port), 20)
            except OSError as e:
                if ':' not in host:
                    raise
                ur, uw, host, hello = await via_ipv4(host, port, r, w, e)
                stats['up'] += len(hello)
                uw.write(hello)
                await uw.drain()
            else:
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
