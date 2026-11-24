# Line coverage of the fuzzed RRF functions, from the .gcov files that gcov wrote into the given directory. Lists every function of
# DataTransfer.cpp, SbcInterface.cpp, BinaryParser.cpp and StringHandle.cpp that the target compiles, and for the packet loop of
# SbcInterface::ExchangeData every case of the switch over SbcRequest.
#
#   python3 -I coverage.py <directory with .gcov files>

import os
import re
import sys

def parse(path):
    lines = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            parts = raw.split(':', 2)
            if len(parts) < 3:
                continue
            count, number, text = parts[0].strip(), parts[1].strip(), parts[2]
            if not number.isdigit() or number == '0':
                continue
            n = int(number)
            if count == '-':
                lines[n] = (None, text)
            elif count.startswith('#####') or count.startswith('====='):
                lines[n] = (0, text)
            else:
                lines[n] = (int(count.rstrip('*').replace(',', '')), text)
    return lines

def report(name, lines):
    numbers = sorted(lines)
    head = re.compile(r'^[A-Za-z].*?\b(\w+::~?\w+)\(')
    case = re.compile(r'^\t\tcase SbcRequest::(\w+):')
    regions = []
    current = None
    for n in numbers:
        text = lines[n][1].rstrip('\n')
        m = head.match(text)
        if m and not text.rstrip().endswith(';'):
            current = [m.group(1), n, n]
            regions.append(current)
            continue
        m = case.match(text)
        if m:
            current = ['  case ' + m.group(1), n, n]
            regions.append(current)
            continue
        if current is not None:
            current[2] = n
    total_exec = total_hit = 0
    for region, start, end in regions:
        executable = [lines[n][0] for n in numbers if start <= n <= end and lines[n][0] is not None]
        if not executable:
            continue
        hit = sum(1 for c in executable if c > 0)
        if not region.startswith('  case'):
            total_exec += len(executable)
            total_hit += hit
        print('%-60s %4d/%-4d %5.1f%%' % (region + ' (' + name + ':' + str(start) + ')', hit, len(executable), 100.0 * hit / len(executable)))
    if total_exec:
        print('%-60s %4d/%-4d %5.1f%%' % ('all functions above (' + name + ')', total_hit, total_exec, 100.0 * total_hit / total_exec))

def main():
    directory = sys.argv[1]
    for gcov in sorted(os.listdir(directory)):
        if gcov in ('DataTransfer.cpp.gcov', 'SbcInterface.cpp.gcov', 'BinaryParser.cpp.gcov', 'StringHandle.cpp.gcov'):
            report(gcov[:-5], parse(os.path.join(directory, gcov)))
            print()

main()
