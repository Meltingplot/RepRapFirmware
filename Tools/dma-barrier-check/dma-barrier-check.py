#!/usr/bin/env python3
"""Lists the places where the SAME70 firmware can start an XDMAC channel without a memory barrier on the way there, and fails for
each one that allowlist.txt does not name. See README.md.

A start is a store to XDMAC_GE, the global channel enable register of the XDMAC. The store is ordered when every path from the
entry of its function to it runs a dsb or dmb, or a call to a function that runs one on every path through it. Otherwise every
direct call of the function must be ordered in the same way in its caller, and so on up the callers. A chain of callers
without a barrier is a finding. The check reads the machine code of the linked firmware with the objdump of the Arm GNU
toolchain, through the program model of Tools/alloc-check (control flow, jump tables, calls, functions that do not return).

    dma-barrier-check.py [--allowlist FILE] [--update] [--keys] [--verbose] [--root DIR] ELF...
"""
import argparse
import collections
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True                                  # no __pycache__ in Tools/alloc-check
_spec = importlib.util.spec_from_file_location('alloc_check', os.path.join(HERE, '..', 'alloc-check', 'alloc-check.py'))
ac = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ac)

# XDMAC_GE of the SAME70 (CoreN2G src/SAM4S_4E_E70/asf/sam/utils/cmsis/same70/include/instance/xdmac.h:47), which
# xdmac_channel_enable writes (src/SAM4S_4E_E70/asf/sam/drivers/xdmac/xdmac.h:362)
XDMAC_GE = 0x4007801C
BARRIERS = ('dsb', 'dmb')
CALLER_DEPTH = 8                                                # how far up the callers the search for the barrier goes
STORE_RE = re.compile(r'^' + ac.REG + r', \[' + ac.REG + r'(?:, #(-?\d+))?\]$')
IMM_RE = re.compile(r'^#(-?\d+)')
ALIAS = {'sb': 'r9', 'sl': 'r10', 'fp': 'r11', 'ip': 'r12'}
CORE_REGS = {f'r{i}' for i in range(13)} | {'lr'}
CALL_CLOBBERED = ('r0', 'r1', 'r2', 'r3', 'r12', 'lr')
# Instructions whose first operand the processor does not write
NO_WRITE = re.compile(r'^(str|stm|push|cmp|cmn|tst|teq|b|cb|it|nop|dsb|dmb|isb|vst|vpush|vcmp|vmsr|msr|svc|bkpt|udf|pl[di]|'
                      r'tb[bh]|cps|wf[ie]|sev|\.)')


def reg(name):
    return ALIAS.get(name, name)


def operands(ops):
    """The operands, split at the commas outside [] and {}, without the comment."""
    ops = ops.split('\t@')[0].split(' @ ')[0].strip()
    parts, depth, cur = [], 0, ''
    for ch in ops:
        depth += ch in '[{'
        depth -= ch in ']}'
        if ch == ',' and depth == 0:
            parts.append(cur.strip())
            cur = ''
        else:
            cur += ch
    return parts + ([cur.strip()] if cur.strip() else [])


def ge_stores(prog, f):
    """The indices in f of the stores to XDMAC_GE. The check follows the constants in the registers in address order: literal
    loads, mov, movw/movt, and add/sub of an immediate. A call forgets the registers that it may change."""
    known, stores = {}, []
    for k, (addr, mn, ops) in enumerate(f.insns):
        base = mn.split('.')[0]
        parts = operands(ops)
        if base == 'str':
            m = STORE_RE.match(', '.join(parts[:2]))
            if m and reg(m.group(2)) in known and (known[reg(m.group(2))] + int(m.group(3) or 0)) & 0xFFFFFFFF == XDMAC_GE:
                stores.append(k)
            if m and parts[-1].endswith('!'):
                known.pop(reg(m.group(2)), None)
            continue
        if base in ('bl', 'blx'):
            for r in CALL_CLOBBERED:
                known.pop(r, None)
            continue
        if base.startswith(('ldm', 'pop')):
            for r in re.findall(r'\b(r\d+|sb|sl|fp|ip|lr)\b', ops):
                known.pop(reg(r), None)
            continue
        if NO_WRITE.match(mn) or not parts:
            continue
        dest = reg(parts[0].rstrip('!'))
        if dest not in CORE_REGS:
            continue
        value = None
        if base == 'ldr' and '[pc' in ops:
            m = ac.LITERAL_RE.match(ops)
            value = prog.elf.word(int(m.group(2), 16)) if m else None
        elif mn in ('mov', 'movs', 'mov.w', 'movw') and len(parts) == 2:
            if parts[1].startswith('#'):
                value = int(IMM_RE.match(parts[1]).group(1)) & 0xFFFFFFFF
            elif reg(parts[1]) in known and mn != 'movw':
                value = known[reg(parts[1])]
        elif mn == 'movt' and len(parts) == 2 and dest in known:
            value = (known[dest] & 0xFFFF) | ((int(IMM_RE.match(parts[1]).group(1)) & 0xFFFF) << 16)
        elif mn.split('.')[0] in ('add', 'adds', 'addw', 'sub', 'subs', 'subw') and parts[-1].startswith('#') \
                and len(parts) in (2, 3):
            src = reg(parts[1]) if len(parts) == 3 else dest
            if src in known:
                imm = int(IMM_RE.match(parts[-1]).group(1))
                value = (known[src] + (imm if base.startswith('add') else -imm)) & 0xFFFFFFFF
        if value is None:
            known.pop(dest, None)
        else:
            known[dest] = value
        if len(parts) >= 2 and base.startswith('ldrd'):
            known.pop(reg(parts[1]), None)
    return stores


class Analysis:
    """Which functions run a barrier on every path through them, and where in a function a barrier has run on every path."""

    def __init__(self, prog):
        self.prog = prog
        self.callers = collections.defaultdict(list)            # callee name -> [(caller, index of the call)]
        for f in prog.funcs:
            for k, callee, _ in f.calls:
                if callee in prog.by_name and callee != f.name:
                    self.callers[callee].append((f, k))
        self.always = set()
        # Only a function that has a barrier or calls one directly or through other calls can run one on every path. Repeat until
        # nothing changes: each round can add the callers of the functions that the previous one added
        candidates = set()
        todo = [f.name for f in prog.funcs if any(self.is_barrier(f, k) for k in range(len(f.insns)))]
        while todo:
            n = todo.pop()
            if n in candidates:
                continue
            candidates.add(n)
            todo += [c.name for c, _ in self.callers.get(n, [])]
        changed = True
        while changed:
            changed = False
            for n in candidates:
                f = prog.by_name.get(n)
                if f is None or n in self.always or n in prog.noreturn:
                    continue
                _, exits = self.flow(f)
                if exits and all(exits):
                    self.always.add(n)
                    changed = True

    @staticmethod
    def is_barrier(f, k):
        return f.insns[k][1].split('.')[0] in BARRIERS

    def flow(self, f):
        """state[k] is True when a barrier has run on every path from the entry of f to instruction k, False when not, None when
        no path reaches it. exits holds the same for each way out of f (return, tail call, jump through a register)."""
        prog, n = self.prog, len(f.insns)
        state, exits = [None] * n, []
        if not n:
            return state, exits
        state[0] = False
        work = [0]

        def barrier_call(callee):
            return callee is not None and callee in self.always

        while work:
            k = work.pop()
            s = state[k]
            what, cond, detail = f.ops[k]
            out = True if (self.is_barrier(f, k) and not cond) else s
            succ = []
            if what in ('next', 'flag'):
                succ = [k + 1]
            elif what == 'call':
                if not (detail in prog.noreturn and not cond):
                    out = out or (not cond and barrier_call(detail))
                    succ = [k + 1]
            elif what == 'icall':
                succ = [k + 1]
            elif what == 'branch':
                if detail is None:
                    exits.append(s)                             # a target the model does not know: as if the function ended
                else:
                    succ = [detail]
                if cond:
                    succ.append(k + 1)
            elif what == 'table':
                succ = list(detail) + ([k + 1] if cond else [])
            elif what in ('return', 'itail'):
                exits.append(s)
                succ = [k + 1] if cond else []
            elif what == 'tail':
                if detail not in prog.noreturn:
                    exits.append(s or barrier_call(detail))
                succ = [k + 1] if cond else []
            elif what == 'stop':
                succ = [k + 1] if cond else []
            for j in succ:
                if j is None or j >= n:
                    continue
                new = out if state[j] is None else (state[j] and out)
                if new != state[j]:
                    state[j] = new
                    work.append(j)
        return state, exits

    def unordered(self, f, k, path, depth, memo):
        """The chains of callers (outermost last) on which the instruction k of f can run without a barrier before it."""
        if f.name not in memo:
            memo[f.name] = self.flow(f)
        state, _ = memo[f.name]
        if state[k]:
            return []
        callers = [(c, j) for c, j in self.callers.get(f.name, []) if c.name not in path]
        if not callers or depth == 0:
            return [path + [f.name]]
        found = []
        for c, j in callers:
            found += self.unordered(c, j, path + [f.name], depth - 1, memo)
        return found


def scan(elf, root, tools):
    """Returns {key: [(address, function, chain, source locations)]} and the number of starts."""
    prog = ac.Program(elf, tools['objdump'], tools['nm'])
    if 'xdmac_configure_transfer' not in prog.by_name:
        sys.exit(f'error: {elf} has no xdmac_configure_transfer, so it is not a SAME70 firmware that this check knows')
    an = Analysis(prog)
    memo, found, starts = {}, collections.OrderedDict(), []
    for f in prog.funcs:
        for k in ge_stores(prog, f):
            starts.append((f, k))
            for chain in an.unordered(f, k, [], CALLER_DEPTH, memo):
                key = ' <- '.join(ac.short(n) for n in chain)
                found.setdefault(key, []).append((f.insns[k][0], f.name, chain))
    where = ac.locate(elf, sorted({a for sites in found.values() for a, _, _ in sites} | {f.insns[k][0] for f, k in starts}),
                      tools['addr2line'], root)
    result = collections.OrderedDict((key, [(a, fn, chain, where.get(a, [])) for a, fn, chain in sites])
                                     for key, sites in found.items())
    return result, [(f, f.insns[k][0], where.get(f.insns[k][0], [])) for f, k in starts]


def location(loc, addr):
    return loc[-1][1] if loc else f'0x{addr:x}'


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('elf', nargs='+', help='linked SAME70 firmware ELF files with debug information (DEBUG_FLAGS=-g)')
    ap.add_argument('--allowlist', help='findings that may stay, default none')
    ap.add_argument('--update', action='store_true', help='rewrite the allowlist with the findings, keeping comments')
    ap.add_argument('--keys', action='store_true', help='print the keys of the findings only')
    ap.add_argument('--verbose', action='store_true', help='also print every start that the check found, and the allowed findings')
    ap.add_argument('--root', default=os.path.normpath(os.path.join(HERE, '..', '..')),
                    help='directory that the source locations are made relative to, default the RepRapFirmware directory')
    ap.add_argument('--objdump', default='arm-none-eabi-objdump')
    ap.add_argument('--nm', default='arm-none-eabi-nm')
    ap.add_argument('--addr2line', default='arm-none-eabi-addr2line')
    args = ap.parse_args()
    tools = {'objdump': args.objdump, 'nm': args.nm, 'addr2line': args.addr2line}
    root = os.path.abspath(args.root)

    header, allowed = ac.read_allowlist(args.allowlist)
    keys, new = collections.OrderedDict(), 0
    for elf in args.elf:
        result, starts = scan(elf, root, tools)
        if args.verbose:
            for f, addr, loc in starts:
                print(f'{location(loc, addr)}: note: XDMAC channel start in {ac.short(f.name)} (0x{addr:x}, {os.path.basename(elf)})')
        for key, sites in result.items():
            keys.setdefault(key, [])
            if args.keys:
                continue
            addr, _, chain, loc = sites[0]
            if key in allowed:
                if args.verbose:
                    print(f'{location(loc, addr)}: allowed: {key}')
                continue
            new += 1
            inl = ' in '.join(f'{ac.short(fn)} {fl}' for fn, fl in loc)
            print(f'{location(loc, addr)}: error: XDMAC channel started without a memory barrier before it: {key}: '
                  f'0x{addr:x} {inl} ({os.path.basename(elf)}). Run Cache::FlushBeforeDMASend on the buffer before the start')

    if args.keys:
        for key in sorted(keys):
            print(key)
        return 0
    if args.update:
        with open(args.allowlist, 'w') as fh:
            for line in header:
                fh.write(line + '\n')
            for key in sorted(keys):
                comment = allowed.get(key, '')
                fh.write(f'{key}  # {comment}\n' if comment else f'{key}\n')
        return 0
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
