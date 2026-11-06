#!/usr/bin/env python3
# Test of the link capabilities (Meltingplot): activation, the switch to the extended link, error-tolerant response codes,
# the phase guard, a resend by the sequence guard and the SBC leaving the extended link.
# Usage: python3 -I link_test.py <harness binary>...

import struct
import subprocess
import sys
import zlib

FORMAT_CODE = 0x5F
PROTOCOL_VERSION = 7
SUCCESS, BAD_HEADER_CHECKSUM = 1, 5
EXT = {1: 0xFF00FF00, 5: 0xF0F0F0F0, 6: 0xF00F0FF0, 0xFEFEFEFE: 0xCCCCCCCC}
ACTIVATE = 0x4D50
ALL_CAPABILITIES = 7


def ext_seq(n):
    return (0x7FF << 5) | (n & 31)


def dsf_header(seq, num_packets, data):
    head = struct.pack('<BBHHHI', FORMAT_CODE, num_packets, PROTOCOL_VERSION, seq, len(data), zlib.crc32(data))
    return head + struct.pack('<I', zlib.crc32(head))


def dsf_packet(request, packet_id, payload):
    body = struct.pack('<HHHH', request, packet_id, len(payload), 0) + payload
    return body + b'\0' * (-len(body) % 4)


def message(packet_id, text):
    return dsf_packet(17, packet_id, struct.pack('<IHH', 0x08, len(text), 0) + text.encode())


def word(value):
    return struct.pack('<I', value)


class Harness:
    def __init__(self, path):
        self.p = subprocess.Popen([path, '-n'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

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


def fields(reply):
    return dict(f.split('=', 1) for f in reply.split() if '=' in f)


def expect(f, **want):
    for k, v in want.items():
        assert f.get(k) == v, f'{k}={f.get(k)}, expected {v}: {f}'


def u32(data):
    return struct.unpack('<I', data[:4])[0]


def transfer(h, seq, data, num_packets, response, expect_rrf_seq, expect_rrf_response):
    """One full transfer. Returns RRF's data and the status after the last exchange"""
    rrf_header, f = h.x(dsf_header(seq, num_packets, data))
    fmt, _, _, rrf_seq, rrf_len, rrf_crc, _ = struct.unpack('<BBHHHII', rrf_header)
    assert fmt == FORMAT_CODE and rrf_seq == expect_rrf_seq, f'RRF seq {rrf_seq:#06x}, expected {expect_rrf_seq:#06x}'
    reply, f = h.x(word(response))
    assert u32(reply) == expect_rrf_response, f'header response {u32(reply):#x}, expected {expect_rrf_response:#x}'
    n = max(len(data), rrf_len)
    reply, f = h.x(data + b'\0' * (n - len(data)))
    rrf_data = reply[:rrf_len]
    assert zlib.crc32(rrf_data) == rrf_crc, 'RRF data CRC'
    reply, f = h.x(word(response))
    assert u32(reply) == expect_rrf_response, f'data response {u32(reply):#x}, expected {expect_rrf_response:#x}'
    return rrf_data, f


def run(path):
    print(path)
    h = Harness(path)
    expect(fields(h.cmd('NEW 0 ff')), state='ExchangingHeader')

    # Transfer 1: activation, regular numbers and codes on both sides
    data = dsf_packet(ACTIVATE, 0, word(ALL_CAPABILITIES)) + message(1, 'dsf:1')
    _, f = transfer(h, 1, data, 2, SUCCESS, 1, SUCCESS)
    expect(f, conn='1', resets='0', got='dsf:1')

    # Transfer 2: RRF has switched, DSF still sends regular numbers and codes
    _, f = transfer(h, 2, message(0, 'dsf:2'), 1, SUCCESS, ext_seq(2), EXT[SUCCESS])
    expect(f, conn='1', resets='0', got='dsf:2')

    # Transfer 3: DSF has switched too. Its codes arrive with 3 bit errors, and the header response starts with the phase guard:
    # DSF sends the start of a header where RRF expects a response, so RRF asks for the header again
    data = message(0, 'dsf:3')
    header = dsf_header(ext_seq(3), 1, data)
    rrf_header, f = h.x(header)
    assert struct.unpack('<H', rrf_header[4:6])[0] == ext_seq(3)
    reply, f = h.x(header[:4])
    assert u32(reply) == EXT[SUCCESS], hex(u32(reply))
    expect(f, state='ExchangingHeaderResponse')
    reply, f = h.x(word(EXT[SUCCESS] ^ 0x80000101))
    assert u32(reply) == EXT[BAD_HEADER_CHECKSUM], f'RA response {u32(reply):#x}'
    expect(f, state='ExchangingHeader')
    rrf_data3, f = transfer(h, ext_seq(3), data, 1, EXT[SUCCESS] ^ 0x00010101, ext_seq(3), EXT[SUCCESS])
    expect(f, conn='1', resets='0', got='dsf:3')

    # DSF repeats transfer 3 because it missed RRF's last data response: RRF sends transfer 3 again, then the waiting transfer 4
    rrf_header, f = h.x(dsf_header(ext_seq(3), 1, data))
    assert struct.unpack('<H', rrf_header[4:6])[0] == ext_seq(4)
    reply, f = h.x(word(EXT[SUCCESS]))
    assert u32(reply) == EXT[BAD_HEADER_CHECKSUM], f'sequence guard response {u32(reply):#x}'
    rrf_again, f = transfer(h, ext_seq(3), data, 1, EXT[SUCCESS], ext_seq(3), EXT[SUCCESS])
    assert rrf_again == rrf_data3, 'resent data differs'
    expect(f, state='ExchangingHeader', conn='1', resets='0', got='-')
    rrf_data4, f = transfer(h, ext_seq(4), message(0, 'dsf:4'), 1, EXT[SUCCESS], ext_seq(4), EXT[SUCCESS])
    assert rrf_data4 != rrf_data3
    expect(f, conn='1', resets='0', got='dsf:4')

    # DSF restarted: regular numbers and codes again. RRF leaves the extended link at once, answers with regular codes and resets
    # the connection because the sequence number does not follow
    _, f = transfer(h, 1, message(0, 'dsf:5'), 1, SUCCESS, ext_seq(5), SUCCESS)
    expect(f, conn='0', disc='1', resets='1')
    # Like in the port, the deactivation in the header phase does not clear the status bits, so the regular numbers go on from them
    rrf_header, f = h.x(dsf_header(1, 0, b''))
    rrf_seq = struct.unpack('<H', rrf_header[4:6])[0]
    assert rrf_seq == ext_seq(5) + 1, f'RRF seq after the reset {rrf_seq:#06x}'
    reply, f = h.x(word(SUCCESS))
    assert u32(reply) == SUCCESS, f'regular response after the reset {u32(reply):#x}'
    h.close()
    print('  ok')


for binary in sys.argv[1:]:
    run(binary)
