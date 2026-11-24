#!/usr/bin/env python3
# Writes the stack frame size of every function of a firmware ELF, from its DWARF call frame information: the largest CFA offset of
# the function (CFA = SP + N in the rows of its FDE) is the stack that the function itself uses, the same figure that -fstack-usage
# writes as "static" into the .su files. The name is the demangled symbol name normalised as board_stack.cpp normalises the names of
# the host build (Normalise there), so that the stack model can give each host function the frame of the same function on the board.
#
#   python3 -I board-frames.py <llvm-dwarfdump --debug-frame output> <llvm-nm -C --defined-only output> > board-stack-usage.tsv
#
# The Makefile target "stack-table" runs it on the MB6HC build of the RRF commit under test.

import re
import sys


def strip_balanced(s, open_char, close_char):
    out = []
    depth = 0
    for c in s:
        if c == open_char:
            depth += 1
        elif c == close_char and depth > 0:
            depth -= 1
        elif depth == 0:
            out.append(c)
    return ''.join(out)


def normalise(name):
    # Keep in step with Normalise() in board_stack.cpp
    name = name.split(' [with ')[0]
    name = re.sub(r"\{lambda\([^{}]*\)#\d+\}", "{lambda}", name)        # GCC demangler
    name = re.sub(r"'lambda\d*'\([^()]*\)", "{lambda}", name)            # LLVM demangler
    name = name.replace('operator()', 'operator@call')
    name = strip_balanced(name, '(', ')')
    name = strip_balanced(name, '<', '>')
    name = name.strip()
    if name.endswith(' const'):
        name = name[:-6]
    name = name.split(' ')[-1]
    if name.endswith('::operator@call'):
        name = name[:-len('::operator@call')]
    return name


def main():
    frames = {}                                     # start address -> largest CFA offset
    pc = None
    with open(sys.argv[1]) as f:
        for line in f:
            m = re.search(r'FDE cie=\S+ pc=([0-9a-f]+)\.\.\.([0-9a-f]+)', line)
            if m:
                pc = int(m.group(1), 16)
                frames.setdefault(pc, 0)
                continue
            if pc is not None:
                m = re.search(r'CFA=SP\+(\d+)', line)
                if m:
                    frames[pc] = max(frames[pc], int(m.group(1)))
    table = {}
    with open(sys.argv[2]) as f:
        for line in f:
            parts = line.rstrip('\n').split(' ', 2)
            if len(parts) != 3 or parts[1] not in ('T', 't', 'W', 'w'):
                continue
            addr = int(parts[0], 16) & ~1
            if addr == 0 or addr not in frames:
                continue
            key = normalise(parts[2])
            table[key] = max(table.get(key, 0), frames[addr])
    print('# Stack frame sizes of the board firmware, by normalised function name (board-frames.py); largest frame of all functions with the name')
    for key in sorted(table):
        print('%s\t%d' % (key, table[key]))


main()
