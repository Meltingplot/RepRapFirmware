#!/usr/bin/env python3
# Smoke test of the harness: one clean full transfer with data in both directions, then timeouts.
# Usage: python3 -I smoke_test.py <harness binary>...

import struct
import subprocess
import sys
import zlib

FORMAT_CODE = 0x5F
PROTOCOL_VERSION = 7
SUCCESS = struct.pack('<I', 1)


def dsf_header(seq, num_packets, data):
    head = struct.pack('<BBHHHI', FORMAT_CODE, num_packets, PROTOCOL_VERSION, seq, len(data), zlib.crc32(data))
    return head + struct.pack('<I', zlib.crc32(head))


def dsf_packet(request, packet_id, payload):
    body = struct.pack('<HHHH', request, packet_id, len(payload), 0) + payload
    return body + b'\0' * (-len(body) % 4)


class Harness:
    def __init__(self, path):
        self.p = subprocess.Popen([path, '-n'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        reply = self.p.stdout.readline().strip()
        print(f'  > {line[:60]}{"..." if len(line) > 60 else ""}\n  < {reply}')
        return reply

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def fields(reply):
    return dict(f.split('=', 1) for f in reply.split() if '=' in f)


def expect(reply, **want):
    f = fields(reply)
    for k, v in want.items():
        if f.get(k) != v:
            raise AssertionError(f'{k}={f.get(k)}, expected {v}: {reply}')


def run(path):
    print(path)
    usb_variant = path.endswith('usb1')
    h = Harness(path)

    expect(h.cmd('NEW 0 ff'), state='ExchangingHeader', pin='1', conn='0', wait='full' if usb_variant else 'unlimited')

    # Transfer 1: DSF sends one message packet containing "dsf:1"
    dsf_data = dsf_packet(17, 0, struct.pack('<IHH', 0x08, 5, 0) + b'dsf:1')
    r = h.cmd('X ' + dsf_header(1, 1, dsf_data).hex())
    expect(r, state='ExchangingHeaderResponse', pin='0', wait='partial')
    rrf_header = bytes.fromhex(r.split()[1])
    fmt, npk, proto, seq, dlen, crc_data, crc_head = struct.unpack('<BBHHHII', rrf_header)
    assert (fmt, npk, proto, seq) == (FORMAT_CODE, 1, PROTOCOL_VERSION, 1), rrf_header.hex()
    assert crc_head == zlib.crc32(rrf_header[:12]), 'RRF header CRC'
    print(f'  RRF header: dataLength={dlen} (not padded to 4: {dlen % 4 != 0})')

    expect(h.cmd('X ' + SUCCESS.hex()), state='ExchangingData', pin='1')
    n = max(len(dsf_data), dlen)
    r = h.cmd('X ' + (dsf_data + b'\0' * (n - len(dsf_data))).hex())
    expect(r, state='ExchangingDataResponse', pin='0')
    rrf_data = bytes.fromhex(r.split()[1])[:dlen]
    assert zlib.crc32(rrf_data) == crc_data, 'RRF data CRC'
    request, pid, plen, resend = struct.unpack('<HHHH', rrf_data[:8])
    mtype, mlen, mpad = struct.unpack('<IHH', rrf_data[8:16])
    text = rrf_data[16:16 + mlen].decode()
    print(f'  RRF packet: request={request} id={pid} length={plen} resendPacketId={resend}, '
          f'MessageHeader type=0x{mtype:x} length={mlen}, text={text!r}')
    assert (request, text) == (3, 'rrf:1')

    expect(h.cmd('X ' + SUCCESS.hex()), state='ExchangingHeader', pin='1', conn='1', wait='full', got='dsf:1')

    # Transfer 2 times out: disconnect with e-stop and a full reset
    expect(h.cmd('T'), conn='0', disc='1', resets='1', pin='1', wait='full' if usb_variant else 'unlimited')
    if usb_variant:
        expect(h.cmd('T'), conn='0', disc='1', resets='2')
    else:
        assert h.cmd('T').startswith('E ')
    h.close()
    print('  ok')


for binary in sys.argv[1:]:
    run(binary)
