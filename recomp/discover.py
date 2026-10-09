"""Function discovery for Supervision cartridges.

    python -m watarakit.recomp.discover ROM.sv [--trace DIR] [--report]

A function is keyed by (slot, address): slot -1 for the fixed bank
($C000-$FFFF), slot k for code that runs in the window ($8000-$BFFF) with
bank k selected.  A function is everything control reaches from its entry
without a JSR: both ways at a branch, through JMP abs, and through an
indirect JMP to the targets the tracer saw; it stops at RTS, RTI, STP, BRK
and undefined opcodes (a walk into data).  Code reached by jumps is part of
every function that reaches it -- a JMP is never a C call, so a state
machine that jumps around cannot grow the C stack.

Where the bytes cannot be known statically the code is left to the
runtime: a transfer from fixed code into the window (which bank?), into RAM,
or an indirect target nobody saw.  At run time those go through `sv_call`,
which finds the native function for the bank selected then, or interprets.

Seeds: the three vectors; every JSR target the descent meets (from fixed
code into the window, in each bank where the tracer ran code at that
address); and, from a trace (`tracer/svtrace` output), every address that
was the target of a JSR or JMP.  A native function translates the bytes at
its address whatever discovery thought of them, so a seed in the wrong
place costs code, never correctness.
"""
import argparse
import os
import re
import sys

from .. import cart as cartmod
from .. import w65c02

FIXED = -1
R_OP, R_ARG, R_READ, R_JTGT = 1, 2, 4, 8


def slot_of(addr, fslot):
    """The slot an address of a function in `fslot` is read from, or None
    when it cannot be known (the window from fixed code, RAM, I/O)."""
    if addr >= 0xC000:
        return FIXED
    if addr >= 0x8000 and fslot != FIXED:
        return fslot
    return None


class Function:
    __slots__ = ('slot', 'entry', 'code', 'calls', 'indirect', 'problems')

    def __init__(self, slot, entry):
        self.slot, self.entry = slot, entry
        self.code = {}            # addr -> Insn
        self.calls = set()        # (slot, target) of JSRs, slot None if dynamic
        self.indirect = {}        # addr of JMP (abs)/(abs,X) -> [targets]
        self.problems = []

    @property
    def name(self):
        return fname(self.slot, self.entry)


def fname(slot, addr):
    return f'f_F_{addr:04X}' if slot == FIXED else f'f_B{slot}_{addr:04X}'


class Trace:
    """What svtrace saw: per ROM byte flags, and the indirect jumps."""

    def __init__(self, path, cart):
        self.cov = open(os.path.join(path, 'cov.bin'), 'rb').read()
        self.ind = {}             # (slot, site) -> {target}
        rep = open(os.path.join(path, 'report.txt')).read()
        for k, s, d in re.findall(r'^edge ([ix]) (\S+) -> (\S+) x\d+', rep, re.M):
            ss, sa = _parse_loc(s, cart)
            ds, da = _parse_loc(d, cart)
            if ss is None or ds is None:
                continue
            self.ind.setdefault((ss, sa), set()).add(da)
        self.cart = cart

    def flag(self, slot, addr):
        if addr >= 0xC000:
            o = self.cart.fixed * 0x4000 + addr - 0xC000
        elif addr >= 0x8000:
            o = slot * 0x4000 + addr - 0x8000
        else:
            return 0
        return self.cov[o]

    def entries(self):
        c = self.cart
        for o, f in enumerate(self.cov):
            if f & R_JTGT:
                b, off = divmod(o, 0x4000)
                if b == c.fixed:
                    yield FIXED, 0xC000 + off
                else:
                    yield b, 0x8000 + off


def _parse_loc(s, cart):
    m = re.match(r'(F|B(\d+)|RAM):([0-9A-F]{4})', s)
    if not m:
        return None, None
    a = int(m.group(3), 16)
    if m.group(1) == 'F':
        return FIXED, a
    if m.group(1) == 'RAM':
        return None, a
    return int(m.group(2)), a


class Program:
    def __init__(self, cart, trace=None):
        self.cart, self.trace = cart, trace
        self.funcs = {}           # (slot, entry) -> Function
        self.readers = {FIXED: cart.reader(cart.fixed)}
        for b in range(cart.nbanks):
            self.readers[b] = cart.reader(b)

    def decode(self, slot, addr):
        return w65c02.decode(self.readers[FIXED if slot is None else slot], addr)

    def window_banks(self, addr):
        """Banks where the tracer ran code at a window address."""
        if not self.trace:
            return []
        return [b for b in range(self.cart.nbanks) if self.trace.flag(b, addr) & R_OP]

    def walk(self, slot, entry):
        f = Function(slot, entry)
        work = [entry]
        while work:
            a = work.pop()
            if a in f.code:
                continue
            s = slot_of(a, slot)
            if s is None:
                continue
            ins = w65c02.decode(self.readers[s], a)
            op = ins.op
            if op.illegal or op.mnem == 'BRK':
                f.problems.append(a)
                continue
            f.code[a] = ins
            m = op.mnem
            nxt = (a + op.size) & 0xFFFF
            t = ins.target()
            if m in w65c02.BRANCHES:
                work += [t, nxt]
            elif m == 'BRA':
                work.append(t)
            elif m == 'JMP':
                if op.mode == 'abs':
                    work.append(t)
                else:
                    seen = set()
                    if self.trace:
                        seen = self.trace.ind.get((s, a), set())
                    f.indirect[a] = sorted(seen)
                    work += [x for x in seen if slot_of(x, slot) is not None]
            elif m == 'JSR':
                ts = slot_of(t, slot)
                f.calls.add((ts, t))
                work.append(nxt)
            elif m in ('RTS', 'RTI', 'STP'):
                pass
            else:
                work.append(nxt)
        return f

    def discover(self):
        c = self.cart
        seeds = [(FIXED, v) for v in c.vectors().values() if v >= 0xC000]
        for v in c.vectors().values():          # a vector into the window: bank 0 at reset
            if 0x8000 <= v < 0xC000:
                seeds += [(b, v) for b in (self.window_banks(v) or [0])]
        if self.trace:
            seeds += list(self.trace.entries())
        todo = list(dict.fromkeys(seeds))
        self.empty = set()
        while todo:
            key = todo.pop()
            if key in self.funcs or key in self.empty:
                continue
            slot, a = key
            if slot != FIXED and slot >= c.nbanks:
                continue
            f = self.walk(slot, a)
            if not f.code:              # an entry on data: left to the interpreter
                self.empty.add(key)
                continue
            self.funcs[key] = f
            for ts, t in f.calls:
                if ts is not None:
                    todo.append((ts, t))
                elif 0x8000 <= t < 0xC000:
                    todo += [(b, t) for b in self.window_banks(t)]
        return self

    def coverage(self):
        """Executed instruction starts (per the trace) that some function holds."""
        if not self.trace:
            return None
        held = set()
        for (slot, _), f in self.funcs.items():
            for a in f.code:
                held.add(self.cart.offset(a, slot if slot != FIXED else 0)
                         if a < 0xC000 else self.cart.offset(a))
        ran = [o for o, x in enumerate(self.trace.cov) if x & R_OP]
        return sum(1 for o in ran if o in held), len(ran)


def main(argv=None):
    ap = argparse.ArgumentParser(prog='watarakit.recomp.discover')
    ap.add_argument('rom')
    ap.add_argument('--trace')
    ap.add_argument('--report', action='store_true')
    a = ap.parse_args(argv)
    c = cartmod.Cart.load(a.rom)
    tr = Trace(a.trace, c) if a.trace else None
    p = Program(c, tr).discover()
    insns = sum(len(f.code) for f in p.funcs.values())
    uniq = len({(f.slot if a < 0xC000 else FIXED, a) for f in p.funcs.values() for a in f.code})
    print(f'{c.name}: {len(p.funcs)} functions, {insns} instructions emitted '
          f'({uniq} distinct)')
    cov = p.coverage()
    if cov:
        print(f'  traced instructions held by a function: {cov[0]}/{cov[1]}')
    probs = sum(len(f.problems) for f in p.funcs.values())
    ind = sum(len(f.indirect) for f in p.funcs.values())
    print(f'  indirect jump sites {ind}, walks into data {probs}')
    if a.report:
        for key in sorted(p.funcs, key=lambda k: (k[0], k[1])):
            f = p.funcs[key]
            print(f'  {f.name:14} {len(f.code):5} insns  calls {len(f.calls):3}'
                  + (f'  data at {",".join("%04X" % x for x in f.problems[:4])}' if f.problems else ''))


if __name__ == '__main__':
    sys.exit(main())
