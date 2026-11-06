#!/usr/bin/env python3
# Test of the boot arm delay (Meltingplot): while TaskLoop waits between DataTransfer::Init and the first header, TfrRdy is low
# and the SPI is disabled. Exchanges the SBC clocks then (the 4-byte BadResponse of a DSF that took Init's falling edge for
# its header response, or a whole header) reach nothing in RRF, and the first transfer after the delay is a clean one.
# Needs a harness built from sources with SbcBootArmDelay and runs it with -b, so that delay() blocks until T.
# Usage: python3 -I boot_delay_test.py <harness binary>...

import struct
import subprocess
import sys
import zlib

FORMAT_CODE, PROTOCOL_VERSION, SUCCESS = 0x5F, 7, 1


def dsf_header(seq, num_packets, data):
    head = struct.pack('<BBHHHI', FORMAT_CODE, num_packets, PROTOCOL_VERSION, seq, len(data), zlib.crc32(data))
    return head + struct.pack('<I', zlib.crc32(head))


def message(text):
    body = struct.pack('<HHHH', 17, 0, 8 + len(text), 0) + struct.pack('<IHH', 0x08, len(text), 0) + text.encode()
    return body + b'\0' * (-len(body) % 4)


def word(value):
    return struct.pack('<I', value)


def fields(reply):
    return dict(f.split('=', 1) for f in reply.split() if '=' in f)


def expect(f, **want):
    for k, v in want.items():
        assert f.get(k) == v, f'{k}={f.get(k)}, expected {v}: {f}'


class Harness:
    def __init__(self, path):
        self.p = subprocess.Popen([path, '-n', '-b'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        reply = self.p.stdout.readline().strip()
        print(f'  > {line[:60]}{"..." if len(line) > 60 else ""}\n  < {reply[:150]}')
        return reply

    def x(self, data):
        reply = self.cmd('X ' + (data.hex() if data else '-'))
        assert reply.startswith('R '), reply
        return bytes.fromhex(reply.split()[1]) if reply.split()[1] != '-' else b'', fields(reply)

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def run(path):
    print(path)
    h = Harness(path)

    # Boot: Init has driven TfrRdy low, TaskLoop waits in the boot arm delay
    expect(fields(h.cmd('NEW 0 ff')), state='ExchangingData', pin='0', wait='delay', conn='0', resets='0')

    # Exchanges during the delay reach the disabled SPI: DSF reads the underrun byte, RRF does not wake up
    reply, f = h.x(word(0xFEFEFEFE))
    assert reply == b'\xff' * 4, reply.hex()
    expect(f, state='ExchangingData', pin='0', wait='delay', resets='0')
    data = message('dsf:0')
    reply, f = h.x(dsf_header(1, 1, data))
    assert reply == b'\xff' * 16, reply.hex()
    expect(f, state='ExchangingData', pin='0', wait='delay', resets='0')

    # After the delay the first header is armed with TfrRdy high (usb1 waits SbcConnectionTimeout, usb0 without limit)
    f = fields(h.cmd('T'))
    expect(f, state='ExchangingHeader', pin='1', resets='0')
    assert f['wait'] in ('full', 'unlimited'), f

    # The first transfer is a clean one: nothing of the exchanges during the delay is left in RRF
    data = message('dsf:1')
    rrf, f = h.x(dsf_header(1, 1, data))
    fmt, _, _, rrf_seq, rrf_len, rrf_crc, _ = struct.unpack('<BBHHHII', rrf)
    assert fmt == FORMAT_CODE and rrf_seq == 1, rrf.hex()
    reply, f = h.x(word(SUCCESS))
    assert struct.unpack('<I', reply[:4])[0] == SUCCESS, reply.hex()
    reply, f = h.x(data + b'\0' * (max(len(data), rrf_len) - len(data)))
    assert zlib.crc32(reply[:rrf_len]) == rrf_crc, 'RRF data CRC'
    reply, f = h.x(word(SUCCESS))
    assert struct.unpack('<I', reply[:4])[0] == SUCCESS, reply.hex()
    expect(f, state='ExchangingHeader', pin='1', conn='1', resets='0', disc='0', got='dsf:1')
    h.close()
    print('  ok')


for binary in sys.argv[1:]:
    run(binary)
