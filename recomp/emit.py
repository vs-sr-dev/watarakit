"""W65C02 -> C++, against watarakit/runtime/sv.h.

One discovered function becomes one C++ function over `Sv& c`, with the
interpreter's semantics instruction for instruction (`sv_core.cpp`'s
`step`): the lockstep self-test holds the two together frame by frame.

* every instruction under a label `L_xxxx`, in address order, with a
  `goto` where control goes on to an instruction that is not the next one
  emitted (overlapping instructions, code shared from elsewhere);
* each instruction charges its cycles (SV_CYC) before it runs, a taken
  branch one more -- the same clock as the interpreter's;
* safe points where the interpreter has them: SV_POLL before a JSR (with
  the JSR's address), after a transfer to an address at or below its own
  (with the target), after WAI;
* JSR pushes the return address and calls the callee -- directly when it
  is a known native function of the same bank, else through `sv_call`
  (the bank chosen at run time; the interpreter for what is not native) --
  and checks that it came back (`c.resume`, and S as it was): otherwise
  the function returns and its callers unwind;
* RTS and RTI leave the address they popped in `c.resume` and return;
* a JMP (abs) is a `switch` over the targets the tracer saw, `sv_call`
  for the rest (a continuation: the code there returns for us);
* in a function of a window bank, the code checks after a write to $2026,
  a call or a safe point that its bank is still selected, and leaves the
  rest to `sv_call` if not.
"""
from .. import w65c02
from .discover import FIXED, fname, slot_of

CYC = [
    7, 6, 2, 1, 5, 3, 5, 5, 3, 2, 2, 1, 6, 4, 6, 5, 2, 5, 5, 1, 5, 4, 6, 5, 2, 4, 2, 1, 6, 4, 6, 5,
    6, 6, 2, 1, 3, 3, 5, 5, 4, 2, 2, 1, 4, 4, 6, 5, 2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 2, 1, 4, 4, 6, 5,
    6, 6, 2, 1, 3, 3, 5, 5, 3, 2, 2, 1, 3, 4, 6, 5, 2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 3, 1, 8, 4, 6, 5,
    6, 6, 2, 1, 3, 3, 5, 5, 4, 2, 2, 1, 6, 4, 6, 5, 2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 4, 1, 6, 4, 6, 5,
    3, 6, 2, 1, 3, 3, 3, 5, 2, 2, 2, 1, 4, 4, 4, 5, 2, 6, 5, 1, 4, 4, 4, 5, 2, 5, 2, 1, 4, 5, 5, 5,
    2, 6, 2, 1, 3, 3, 3, 5, 2, 2, 2, 1, 4, 4, 4, 5, 2, 5, 5, 1, 4, 4, 4, 5, 2, 4, 2, 1, 4, 4, 4, 5,
    2, 6, 2, 1, 3, 3, 5, 5, 2, 2, 2, 3, 4, 4, 6, 5, 2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 3, 3, 4, 4, 7, 5,
    2, 6, 2, 1, 3, 3, 5, 5, 2, 2, 2, 1, 4, 4, 6, 5, 2, 5, 5, 1, 4, 4, 6, 5, 2, 4, 4, 1, 4, 4, 7, 5]

COND = {'BPL': '!c.fn', 'BMI': 'c.fn', 'BVC': '!c.fv', 'BVS': 'c.fv',
        'BCC': '!c.fc', 'BCS': 'c.fc', 'BNE': '!c.fz', 'BEQ': 'c.fz'}
REG = {'A': 'c.a', 'X': 'c.x', 'Y': 'c.y'}


def h(v, w=4):
    return f'0x{v:0{w}X}'


class Operand:
    """How an instruction reaches its operand: `pre` (statements that fix
    the address), `rd` (an expression), `wr(x)` (a statement)."""

    def __init__(self, ins):
        m, a = ins.op.mode, ins.arg
        self.pre = ''
        self.lv = None
        if m == 'imm':
            self.rd = h(a, 2)
            return
        if m in ('zp', 'zpr'):
            self.lv = f'c.ram[{h(a & 0xFF, 2)}]'
        elif m == 'zpx':
            self.lv = f'c.ram[(uint8_t)({h(a, 2)} + c.x)]'
        elif m == 'zpy':
            self.lv = f'c.ram[(uint8_t)({h(a, 2)} + c.y)]'
        elif m == 'abs':
            if a < 0x2000:
                self.lv = f'c.ram[{h(a)}]'
            elif 0x4000 <= a < 0x6000:
                self.lv = f'c.vram[{h(a - 0x4000)}]'
            elif 0x2000 <= a < 0x4000:
                self.rd = f'sv_io_r(c, {h(a)})'
                self.wr = lambda x: f'sv_io_w(c, {h(a)}, {x});'
                return
            else:
                self.rd = f'sv_rd(c, {h(a)})'
                self.wr = lambda x: f'sv_wr(c, {h(a)}, {x});'
                return
        elif m in ('absx', 'absy'):
            r = 'c.x' if m == 'absx' else 'c.y'
            if a + 0xFF < 0x2000:
                self.lv = f'c.ram[{h(a)} + {r}]'
            elif 0x4000 <= a and a + 0xFF < 0x6000:
                self.lv = f'c.vram[{h(a - 0x4000)} + {r}]'
            else:
                self._ea(f'(uint16_t)({h(a)} + {r})')
                return
        elif m == 'indy':
            self._ea(f'(uint16_t)(sv_zpw(c, {h(a, 2)}) + c.y)')
            return
        elif m == 'indx':
            self._ea(f'sv_zpw(c, (uint8_t)({h(a, 2)} + c.x))')
            return
        elif m == 'zpi':
            self._ea(f'sv_zpw(c, {h(a, 2)})')
            return
        else:
            raise ValueError(m)
        self.rd = self.lv
        self.wr = lambda x: f'{self.lv} = {x};'

    def _ea(self, expr):
        self.pre = f'const uint16_t ea = {expr}; '
        self.rd = 'sv_rd(c, ea)'
        self.wr = lambda x: f'sv_wr(c, ea, {x});'


def stmt(ins):
    """C++ for an instruction that does not transfer control."""
    m, mode = ins.op.mnem, ins.op.mode
    if m in ('LDA', 'LDX', 'LDY'):
        o = Operand(ins)
        return f'{{ {o.pre}sv_nz(c, {REG[m[2]]} = {o.rd}); }}'
    if m in ('STA', 'STX', 'STY', 'STZ'):
        o = Operand(ins)
        return f'{{ {o.pre}{o.wr("0" if m == "STZ" else REG[m[2]])} }}'
    if m in ('ADC', 'SBC'):
        o = Operand(ins)
        return f'{{ {o.pre}sv_{m.lower()}(c, {o.rd}); }}'
    if m in ('AND', 'ORA', 'EOR'):
        o = Operand(ins)
        cop = {'AND': '&=', 'ORA': '|=', 'EOR': '^='}[m]
        return f'{{ {o.pre}sv_nz(c, c.a {cop} {o.rd}); }}'
    if m in ('CMP', 'CPX', 'CPY'):
        o = Operand(ins)
        r = {'CMP': 'c.a', 'CPX': 'c.x', 'CPY': 'c.y'}[m]
        return f'{{ {o.pre}sv_cmp(c, {r}, {o.rd}); }}'
    if m == 'BIT':
        o = Operand(ins)
        if mode == 'imm':
            return f'c.fz = (c.a & {o.rd}) == 0;'
        return f'{{ {o.pre}sv_bit(c, {o.rd}); }}'
    if m in ('ASL', 'LSR', 'ROL', 'ROR'):
        fn = 'sv_' + m.lower()
        if mode == 'acc':
            return f'c.a = {fn}(c, c.a);'
        o = Operand(ins)
        return f'{{ {o.pre}uint8_t v = {o.rd}; {o.wr(f"{fn}(c, v)")} }}'
    if m in ('INC', 'DEC'):
        d = '+' if m == 'INC' else '-'
        if mode == 'acc':
            return f'sv_nz(c, c.a = (uint8_t)(c.a {d} 1));'
        o = Operand(ins)
        return f'{{ {o.pre}uint8_t v = {o.rd}; {o.wr(f"sv_nz(c, (uint8_t)(v {d} 1))")} }}'
    if m in ('TSB', 'TRB'):
        o = Operand(ins)
        return f'{{ {o.pre}uint8_t v = {o.rd}; {o.wr(f"sv_{m.lower()}(c, v)")} }}'
    if m[:3] in ('RMB', 'SMB'):
        b = int(m[3])
        z = f'c.ram[{h(ins.arg, 2)}]'
        return f'{z} &= (uint8_t)~{1 << b};' if m[:3] == 'RMB' else f'{z} |= {1 << b};'
    simple = {
        'PHA': 'sv_push(c, c.a);', 'PHX': 'sv_push(c, c.x);', 'PHY': 'sv_push(c, c.y);',
        'PHP': 'sv_push(c, sv_p(c) | 0x10);',
        'PLA': 'sv_nz(c, c.a = sv_pull(c));', 'PLX': 'sv_nz(c, c.x = sv_pull(c));',
        'PLY': 'sv_nz(c, c.y = sv_pull(c));', 'PLP': 'sv_setp(c, sv_pull(c)); sv_recalc(c);',
        'TAX': 'sv_nz(c, c.x = c.a);', 'TAY': 'sv_nz(c, c.y = c.a);',
        'TXA': 'sv_nz(c, c.a = c.x);', 'TYA': 'sv_nz(c, c.a = c.y);',
        'TSX': 'sv_nz(c, c.x = c.s);', 'TXS': 'c.s = c.x;',
        'INX': 'sv_nz(c, ++c.x);', 'INY': 'sv_nz(c, ++c.y);',
        'DEX': 'sv_nz(c, --c.x);', 'DEY': 'sv_nz(c, --c.y);',
        'CLC': 'c.fc = 0;', 'SEC': 'c.fc = 1;', 'CLI': 'c.fi = 0; sv_recalc(c);',
        'SEI': 'c.fi = 1;', 'CLD': 'c.fd = 0;', 'SED': 'c.fd = 1;', 'CLV': 'c.fv = 0;',
        'NOP': ';',
    }
    if m in simple:
        return simple[m]
    raise ValueError(f'{ins.pc:04X}: {ins}')


class Body:
    """One discovered function as C++ (a list of lines)."""

    def __init__(self, prog, f, comments=True):
        self.p, self.f, self.comments = prog, f, comments
        self.win = f.slot != FIXED
        self.bank = f.slot
        self.sites = {}

    def _count(self, k):
        self.sites[k] = self.sites.get(k, 0) + 1

    def _bankcheck(self, at):
        """After something that may have switched the bank: leave if it did."""
        if not self.win:
            return ''
        return f' if (c.bank != {self.bank}) {{ sv_call(c, {h(at)}); return; }}'

    def _poll(self, at):
        return f'SV_POLL(c, {h(at)});' + self._bankcheck(at)

    def _local(self, t):
        """Is t an instruction of this function, read from the same bytes?"""
        return t in self.f.code and slot_of(t, self.f.slot) is not None

    def _goto(self, t, src):
        poll = self._poll(t) + ' ' if t <= src else ''
        if self._local(t):
            return f'{poll}goto L_{t:04X};'
        self._count('dynamic jump')
        return f'{poll}sv_call(c, {h(t)}); return;'

    def _callee(self, t):
        ts = slot_of(t, self.f.slot)
        key = (ts, t)
        if ts is not None and key in self.p.funcs:
            return f'{fname(ts, t)}(c);'
        self._count('dynamic call')
        return f'sv_call(c, {h(t)});'

    def insn(self, a):
        """(C++ text, whether control goes on to a + size)."""
        ins = self.f.code[a]
        op = ins.op
        m = op.mnem
        cyc = CYC[op.code]
        nxt = (a + op.size) & 0xFFFF
        tick = f'c.cyc += {cyc}; '
        if m in COND or m[:3] in ('BBR', 'BBS'):
            t = ins.target()
            if m in COND:
                cond = COND[m]
            else:
                b = int(m[3])
                bit = f'(c.ram[{h(ins.arg & 0xFF, 2)}] >> {b} & 1)'
                cond = bit if m[:3] == 'BBS' else f'!{bit}'
            return f'{tick}if ({cond}) {{ c.cyc++; {self._goto(t, a)} }}', True
        if m == 'BRA':
            return tick + self._goto(ins.target(), a), False
        if m == 'JMP':
            if op.mode == 'abs':
                return tick + self._goto(ins.arg, a), False
            self._count('indirect jump')
            if op.mode == 'ind':
                ea = f'sv_rd16(c, {h(ins.arg)})'
            else:
                ea = f'sv_rd16(c, (uint16_t)({h(ins.arg)} + c.x))'
            cases = ' '.join(f'case {h(t)}: goto L_{t:04X};'
                             for t in self.f.indirect.get(a, []) if self._local(t))
            return (f'{tick}{{ const uint16_t t = {ea}; '
                    f'if (t <= {h(a)}) {{ SV_POLL(c, t);{self._bankcheck_t()} }} '
                    f'switch (t) {{ {cases} default: sv_call(c, t); return; }} }}'), False
        if m == 'JSR':
            self._count('call')
            ret = (a + 3) & 0xFFFF
            push = (f'sv_push(c, {h((a + 2) >> 8 & 0xFF, 2)}); '
                    f'sv_push(c, {h((a + 2) & 0xFF, 2)});')
            return (f'{self._poll(a)} c.cyc += 6; {push} '
                    f'{{ const uint8_t s0 = c.s + 2; {self._callee(ins.arg)} '
                    f'if (c.resume != {h(ret)} || c.s != s0) return; }}'
                    + self._bankcheck(ret)), True
        if m == 'RTS':
            return (f'{tick}{{ const uint8_t lo = sv_pull(c), hi = sv_pull(c); '
                    f'c.resume = (uint16_t)((lo | hi << 8) + 1); c.rti = 0; return; }}'), False
        if m == 'RTI':
            return (f'{tick}sv_setp(c, sv_pull(c)); '
                    f'{{ const uint8_t lo = sv_pull(c), hi = sv_pull(c); '
                    f'c.resume = (uint16_t)(lo | hi << 8); }} c.rti = 1; sv_recalc(c); return;'), False
        if m == 'WAI':
            return f'{tick}sv_wai(c); {self._poll(nxt)}', True
        if m == 'STP':
            return f'{tick}c.stopped = 1; throw SvStop();', False
        text = tick + stmt(ins)
        if self.win and m in ('STA', 'STX', 'STY', 'STZ', 'TSB', 'TRB') \
                and op.mode == 'abs' and (ins.arg & 0xE000) == 0x2000 \
                and (ins.arg & 0x1FFF) == 0x26:
            text += self._bankcheck(nxt)
        return text, True

    def _bankcheck_t(self):
        if not self.win:
            return ''
        return f' if (c.bank != {self.bank}) {{ sv_call(c, t); return; }}'

    def emit(self):
        f = self.f
        order = sorted(f.code)
        lines = [f'void {f.name}(Sv& c) {{']
        if order[0] != f.entry:
            lines.append(f'    goto L_{f.entry:04X};')
        for n, a in enumerate(order):
            text, on = self.insn(a)
            if self.comments:
                text += f'  // {a:04X} {f.code[a]}'
            lines.append(f'L_{a:04X}:')
            lines.append('    ' + text)
            ins = f.code[a]
            nxt = (a + ins.op.size) & 0xFFFF
            following = order[n + 1] if n + 1 < len(order) else None
            if on and nxt != following:
                if self._local(nxt):
                    lines.append(f'    goto L_{nxt:04X};')
                else:
                    self._count('dynamic fall-through')
                    lines.append(f'    sv_call(c, {h(nxt)}); return;')
        lines.append('}')
        return lines
