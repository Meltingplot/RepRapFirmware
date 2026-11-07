#!/usr/bin/env python3
# Test of a restart of RRF after an exchange and before the next one is armed (Meltingplot), as seen on the lab printer on
# 2026-11-07 after M122 P1001. SoftwareReset writes the reset data with the interrupts off while the DMA runs on, so DSF can
# complete the exchange RRF armed last (D) before the processor resets (RESET) and boots (NEW). DSF then goes on with its
# transfer: its header response hits the boot window and reads zeros, so it restarts the full transfer with the extended
# BadResponse, which lands in the first header the booted RRF arms.
# - "loop": RRF does not decode that word and answers BadHeaderChecksum. Both sides stay one exchange apart: DSF reads RRF's
#   BadHeaderChecksum as a header and the start of RRF's header as a response, as in the printer's log.
# - "in-step": RRF decodes it while not connected and retries the data response, and the next header exchange is in step.
# Needs sources with the boot arm delay and runs the harness with -b.
# Usage: python3 -I restart_test.py [--expect loop|in-step] <harness binary>...

import struct
import subprocess
import sys
import zlib

FORMAT_CODE, PROTOCOL_VERSION = 0x5F, 7
SUCCESS, BAD_HEADER_CHECKSUM = 1, 5
EXT = {SUCCESS: 0xFF00FF00, BAD_HEADER_CHECKSUM: 0xF0F0F0F0, 0xFEFEFEFE: 0xCCCCCCCC}
BAD_RESPONSE = 0xFEFEFEFE
ACTIVATE, ALL_CAPABILITIES = 0x4D50, 7


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


def u32(data):
    return struct.unpack('<I', data[:4])[0]


def fields(reply):
    return dict(f.split('=', 1) for f in reply.split() if '=' in f)


def expect(f, **want):
    for k, v in want.items():
        assert f.get(k) == v, f'{k}={f.get(k)}, expected {v}: {f}'


def rrf_header_fields(data):
    """formatCode, numPackets, protocolVersion, sequenceNumber, dataLength, crcData, and whether the header CRC is right"""
    fmt, packets, version, seq, length, crc_data, crc_header = struct.unpack('<BBHHHII', data[:16])
    return fmt, packets, version, seq, length, crc_data, crc_header == zlib.crc32(data[:12])


class Harness:
    def __init__(self, path):
        self.p = subprocess.Popen([path, '-n', '-b'], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def cmd(self, line):
        self.p.stdin.write(line + '\n')
        self.p.stdin.flush()
        reply = self.p.stdout.readline().strip()
        print(f'  > {line[:60]}{"..." if len(line) > 60 else ""}\n  < {reply[:150]}')
        return reply

    def exchange(self, command, data):
        reply = self.cmd(f'{command} ' + (data.hex() if data else '-'))
        assert reply.startswith('R '), reply
        return (bytes.fromhex(reply.split()[1]) if reply.split()[1] != '-' else b''), fields(reply)

    def x(self, data):
        return self.exchange('X', data)

    def d(self, data):
        return self.exchange('D', data)

    def close(self):
        self.p.stdin.close()
        self.p.wait()


def transfer(h, seq, data, num_packets, response, expect_rrf_seq, expect_rrf_response):
    """One full transfer. Returns the status after the last exchange"""
    rrf_header, f = h.x(dsf_header(seq, num_packets, data))
    fmt, _, _, rrf_seq, rrf_len, rrf_crc, crc_ok = rrf_header_fields(rrf_header)
    assert fmt == FORMAT_CODE and crc_ok and rrf_seq == expect_rrf_seq, f'RRF header {rrf_header.hex()}, expected seq {expect_rrf_seq:#06x}'
    reply, f = h.x(word(response))
    assert u32(reply) == expect_rrf_response, f'header response {u32(reply):#x}, expected {expect_rrf_response:#x}'
    reply, f = h.x(data + b'\0' * (max(len(data), rrf_len) - len(data)))
    assert zlib.crc32(reply[:rrf_len]) == rrf_crc, 'RRF data CRC'
    reply, f = h.x(word(response))
    assert u32(reply) == expect_rrf_response, f'data response {u32(reply):#x}, expected {expect_rrf_response:#x}'
    return f


def run(path, expected):
    print(path)
    h = Harness(path)

    # Boot and connect on the extended link: activation in transfer 1, RRF switches in transfer 2, DSF in transfer 3
    expect(fields(h.cmd('NEW 0 00')), state='ExchangingData', pin='0', wait='delay')
    expect(fields(h.cmd('T')), state='ExchangingHeader', pin='1')
    f = transfer(h, 1, dsf_packet(ACTIVATE, 0, word(ALL_CAPABILITIES)) + message(1, 'dsf:1'), 2, SUCCESS, 1, SUCCESS)
    expect(f, conn='1', got='dsf:1')
    transfer(h, 2, message(0, 'dsf:2'), 1, SUCCESS, ext_seq(2), EXT[SUCCESS])
    f = transfer(h, ext_seq(3), message(0, 'dsf:3'), 1, EXT[SUCCESS], ext_seq(3), EXT[SUCCESS])
    expect(f, state='ExchangingHeader', pin='1', conn='1', resets='0', got='dsf:3')

    # Transfer 4, both sides with data. DSF completes the header exchange while RRF's interrupts are off, then RRF resets
    data = message(0, 'dsf:4')
    header = dsf_header(ext_seq(4), 1, data)
    rrf_header, f = h.d(header)
    fmt, _, _, rrf_seq, _, _, crc_ok = rrf_header_fields(rrf_header)
    assert fmt == FORMAT_CODE and crc_ok and rrf_seq == ext_seq(4), rrf_header.hex()
    expect(f, state='ExchangingHeader', pin='1')
    expect(fields(h.cmd('RESET 00')), state='Boot', pin='1')

    # Boot: DataTransfer::Init drives TfrRdy low. DSF takes that edge for its header response, which reaches the disabled SPI.
    # The booted RRF has no packet for its first transfer, as on the printer (DSF logged code 0x0007005f)
    expect(fields(h.cmd('NEW 1 00')), state='ExchangingData', pin='0', wait='delay', conn='0')
    reply, f = h.x(word(EXT[SUCCESS]))
    assert reply == b'\0' * 4, f'DSF reads {reply.hex()} in the boot window'
    expect(f, state='ExchangingData', wait='delay')

    # DSF read 0x00000000 as the header response and restarts the full transfer. It carries data, so DSF sends the extended
    # BadResponse first, into the first header the booted RRF arms
    expect(fields(h.cmd('T')), state='ExchangingHeader', pin='1', conn='0')
    reply, f = h.x(word(EXT[BAD_RESPONSE]))
    assert reply[0] == FORMAT_CODE, reply.hex()
    if f['state'] == 'ExchangingHeaderResponse':
        outcome = 'loop'
        # RRF answered BadHeaderChecksum. DSF sends its header into RRF's response and reads that BadHeaderChecksum as a header,
        # then sends its own BadHeaderChecksum into RRF's header and reads the start of RRF's header as a response
        for _ in range(3):
            reply, f = h.x(header)
            assert reply == word(BAD_HEADER_CHECKSUM) + b'\0' * 12, f'DSF reads {reply.hex()} as the header'
            expect(f, state='ExchangingHeader', conn='0')
            reply, f = h.x(word(EXT[BAD_HEADER_CHECKSUM]))
            assert u32(reply) == 0x0007005F, f'DSF reads {reply.hex()} as the response'
            expect(f, state='ExchangingHeaderResponse', conn='0')
    else:
        outcome = 'in-step'
        # RRF took the word for a BadResponse and retries the data response with Success. DSF reads that as a header and asks
        # for the header again, RRF takes the start of DSF's header for a new transfer and asks for it again as well
        expect(f, state='ExchangingDataResponseRetry', conn='0')
        reply, f = h.x(header)
        assert reply == word(SUCCESS) + b'\0' * 12, f'DSF reads {reply.hex()} as the header'
        expect(f, state='ExchangingHeaderResponse')
        reply, f = h.x(word(EXT[BAD_HEADER_CHECKSUM]))
        assert reply == word(BAD_HEADER_CHECKSUM), f'DSF reads {reply.hex()} as the response'
        expect(f, state='ExchangingHeader')

        # In step: both headers arrive whole. RRF's regular sequence number 1 tells DSF about the restart, and the transfer
        # ends in a connection reset because DSF's number does not follow (no e-stop, RRF was not connected yet)
        rrf_header, f = h.x(header)
        fmt, _, _, rrf_seq, rrf_len, rrf_crc, crc_ok = rrf_header_fields(rrf_header)
        assert fmt == FORMAT_CODE and crc_ok and rrf_seq == 1, rrf_header.hex()
        expect(f, state='ExchangingHeaderResponse')
        reply, f = h.x(word(SUCCESS))
        assert u32(reply) == SUCCESS, reply.hex()
        reply, f = h.x(data + b'\0' * (max(len(data), rrf_len) - len(data)))
        assert zlib.crc32(reply[:rrf_len]) == rrf_crc, 'RRF data CRC'
        reply, f = h.x(word(SUCCESS))
        assert u32(reply) == SUCCESS, reply.hex()
        expect(f, state='ExchangingHeader', conn='0', disc='0', resets='1')
    h.close()
    print(f'  {outcome}')
    assert expected is None or outcome == expected, f'outcome {outcome}, expected {expected}'


args = sys.argv[1:]
expected = None
if len(args) >= 2 and args[0] == '--expect':
    expected = args[1]
    args = args[2:]
for binary in args:
    run(binary, expected)
