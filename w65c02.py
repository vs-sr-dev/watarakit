"""The Supervision's CPU: a WDC 65C02 core inside the Watara SoC.

The full WDC set, with Rockwell's bit instructions (RMB/SMB/BBR/BBS) and
WAI/STP.  Opcodes the 65C02 leaves undefined execute as NOPs of 1, 2 or 3
bytes; they are marked `illegal`, since a game's code never uses them and
reaching one is the strongest sign that a walk has wandered into data.

    python -m watarakit.w65c02 ROM.sv --at FFFC --vec        # the vectors
    python -m watarakit.w65c02 ROM.sv --at E000 -n 40        # a listing
    python -m watarakit.w65c02 ROM.sv --bank 1 --at 8000     # a banked page
"""

import argparse
import sys

# Addressing modes: name -> operand byte count.
MODES = {
    'imp': 0, 'acc': 0, 'imm': 1, 'zp': 1, 'zpx': 1, 'zpy': 1,
    'abs': 2, 'absx': 2, 'absy': 2, 'ind': 2, 'indx': 1, 'indy': 1,
    'zpi': 1, 'aix': 2, 'rel': 1, 'zpr': 2,
}

_ROWS = """
00 BRK imm  01 ORA indx 02 NOP* imm 03 NOP* imp 04 TSB zp   05 ORA zp   06 ASL zp   07 RMB0 zp
08 PHP imp  09 ORA imm  0A ASL acc  0B NOP* imp 0C TSB abs  0D ORA abs  0E ASL abs  0F BBR0 zpr
10 BPL rel  11 ORA indy 12 ORA zpi  13 NOP* imp 14 TRB zp   15 ORA zpx  16 ASL zpx  17 RMB1 zp
18 CLC imp  19 ORA absy 1A INC acc  1B NOP* imp 1C TRB abs  1D ORA absx 1E ASL absx 1F BBR1 zpr
20 JSR abs  21 AND indx 22 NOP* imm 23 NOP* imp 24 BIT zp   25 AND zp   26 ROL zp   27 RMB2 zp
28 PLP imp  29 AND imm  2A ROL acc  2B NOP* imp 2C BIT abs  2D AND abs  2E ROL abs  2F BBR2 zpr
30 BMI rel  31 AND indy 32 AND zpi  33 NOP* imp 34 BIT zpx  35 AND zpx  36 ROL zpx  37 RMB3 zp
38 SEC imp  39 AND absy 3A DEC acc  3B NOP* imp 3C BIT absx 3D AND absx 3E ROL absx 3F BBR3 zpr
40 RTI imp  41 EOR indx 42 NOP* imm 43 NOP* imp 44 NOP* zp  45 EOR zp   46 LSR zp   47 RMB4 zp
48 PHA imp  49 EOR imm  4A LSR acc  4B NOP* imp 4C JMP abs  4D EOR abs  4E LSR abs  4F BBR4 zpr
50 BVC rel  51 EOR indy 52 EOR zpi  53 NOP* imp 54 NOP* zpx 55 EOR zpx  56 LSR zpx  57 RMB5 zp
58 CLI imp  59 EOR absy 5A PHY imp  5B NOP* imp 5C NOP* abs 5D EOR absx 5E LSR absx 5F BBR5 zpr
60 RTS imp  61 ADC indx 62 NOP* imm 63 NOP* imp 64 STZ zp   65 ADC zp   66 ROR zp   67 RMB6 zp
68 PLA imp  69 ADC imm  6A ROR acc  6B NOP* imp 6C JMP ind  6D ADC abs  6E ROR abs  6F BBR6 zpr
70 BVS rel  71 ADC indy 72 ADC zpi  73 NOP* imp 74 STZ zpx  75 ADC zpx  76 ROR zpx  77 RMB7 zp
78 SEI imp  79 ADC absy 7A PLY imp  7B NOP* imp 7C JMP aix  7D ADC absx 7E ROR absx 7F BBR7 zpr
80 BRA rel  81 STA indx 82 NOP* imm 83 NOP* imp 84 STY zp   85 STA zp   86 STX zp   87 SMB0 zp
88 DEY imp  89 BIT imm  8A TXA imp  8B NOP* imp 8C STY abs  8D STA abs  8E STX abs  8F BBS0 zpr
90 BCC rel  91 STA indy 92 STA zpi  93 NOP* imp 94 STY zpx  95 STA zpx  96 STX zpy  97 SMB1 zp
98 TYA imp  99 STA absy 9A TXS imp  9B NOP* imp 9C STZ abs  9D STA absx 9E STZ absx 9F BBS1 zpr
A0 LDY imm  A1 LDA indx A2 LDX imm  A3 NOP* imp A4 LDY zp   A5 LDA zp   A6 LDX zp   A7 SMB2 zp
A8 TAY imp  A9 LDA imm  AA TAX imp  AB NOP* imp AC LDY abs  AD LDA abs  AE LDX abs  AF BBS2 zpr
B0 BCS rel  B1 LDA indy B2 LDA zpi  B3 NOP* imp B4 LDY zpx  B5 LDA zpx  B6 LDX zpy  B7 SMB3 zp
B8 CLV imp  B9 LDA absy BA TSX imp  BB NOP* imp BC LDY absx BD LDA absx BE LDX absy BF BBS3 zpr
C0 CPY imm  C1 CMP indx C2 NOP* imm C3 NOP* imp C4 CPY zp   C5 CMP zp   C6 DEC zp   C7 SMB4 zp
C8 INY imp  C9 CMP imm  CA DEX imp  CB WAI imp  CC CPY abs  CD CMP abs  CE DEC abs  CF BBS4 zpr
D0 BNE rel  D1 CMP indy D2 CMP zpi  D3 NOP* imp D4 NOP* zpx D5 CMP zpx  D6 DEC zpx  D7 SMB5 zp
D8 CLD imp  D9 CMP absy DA PHX imp  DB STP imp  DC NOP* abs DD CMP absx DE DEC absx DF BBS5 zpr
E0 CPX imm  E1 SBC indx E2 NOP* imm E3 NOP* imp E4 CPX zp   E5 SBC zp   E6 INC zp   E7 SMB6 zp
E8 INX imp  E9 SBC imm  EA NOP imp  EB NOP* imp EC CPX abs  ED SBC abs  EE INC abs  EF BBS6 zpr
F0 BEQ rel  F1 SBC indy F2 SBC zpi  F3 NOP* imp F4 NOP* zpx F5 SBC zpx  F6 INC zpx  F7 SMB7 zp
F8 SED imp  F9 SBC absy FA PLX imp  FB NOP* imp FC NOP* abs FD SBC absx FE INC absx FF BBS7 zpr
"""


class Op:
    __slots__ = ('code', 'mnem', 'mode', 'size', 'illegal')

    def __init__(self, code, mnem, mode):
        self.code = code
        self.illegal = mnem.endswith('*')
        self.mnem = mnem.rstrip('*')
        self.mode = mode
        self.size = 1 + MODES[mode]


def _table():
    t = [None] * 256
    toks = _ROWS.split()
    for i in range(0, len(toks), 3):
        code = int(toks[i], 16)
        t[code] = Op(code, toks[i + 1], toks[i + 2])
    assert all(t), 'opcode table incomplete'
    return t


OPS = _table()

# Control flow classes.
BRANCHES = {'BPL', 'BMI', 'BVC', 'BVS', 'BCC', 'BCS', 'BNE', 'BEQ'} | \
           {f'BBR{i}' for i in range(8)} | {f'BBS{i}' for i in range(8)}
ENDS = {'RTS', 'RTI', 'BRA', 'JMP', 'STP', 'BRK'}   # no fall-through
# The instructions only a 65C02 has: what a 6502 programmer would not write.
CMOS_ONLY = {'STZ', 'BRA', 'PHX', 'PHY', 'PLX', 'PLY', 'TSB', 'TRB', 'WAI',
             'STP'} | {f'{m}{i}' for m in ('RMB', 'SMB', 'BBR', 'BBS')
                       for i in range(8)}
CMOS_MODES = {'zpi', 'aix'}   # (zp) and (abs,x); also INC A / DEC A, BIT #


def is_cmos(op):
    return (op.mnem in CMOS_ONLY or op.mode in CMOS_MODES
            or (op.mnem in ('INC', 'DEC') and op.mode == 'acc')
            or (op.mnem == 'BIT' and op.mode in ('imm', 'zpx', 'absx')))


class Insn:
    __slots__ = ('pc', 'op', 'arg', 'raw')

    def __init__(self, pc, op, arg, raw):
        self.pc, self.op, self.arg, self.raw = pc, op, arg, raw

    @property
    def size(self):
        return self.op.size

    def target(self):
        """The static target of a branch, JSR or JMP abs, else None."""
        m = self.op.mode
        if m == 'rel':
            return (self.pc + 2 + _s8(self.arg)) & 0xFFFF
        if m == 'zpr':
            return (self.pc + 3 + _s8(self.arg >> 8)) & 0xFFFF
        if self.op.mnem in ('JSR', 'JMP') and m == 'abs':
            return self.arg
        return None

    def __str__(self):
        return f'{self.op.mnem:<4} {fmt_operand(self)}'.rstrip()


def _s8(v):
    v &= 0xFF
    return v - 256 if v & 0x80 else v


def decode(read, pc):
    """Decode one instruction; `read(addr)` returns a byte."""
    op = OPS[read(pc)]
    n = MODES[op.mode]
    raw = bytes(read((pc + i) & 0xFFFF) for i in range(op.size))
    arg = 0
    if n == 1:
        arg = raw[1]
    elif n == 2:
        arg = raw[1] | raw[2] << 8
    return Insn(pc, op, arg, raw)


def fmt_operand(ins, names=None):
    m, a = ins.op.mode, ins.arg

    def nm(v, w=4):
        if names and v in names:
            return names[v]
        return f'${v:0{w}X}'
    if m == 'imp':
        return ''
    if m == 'acc':
        return 'A'
    if m == 'imm':
        return f'#${a:02X}'
    if m == 'zp':
        return nm(a, 2)
    if m == 'zpx':
        return nm(a, 2) + ',X'
    if m == 'zpy':
        return nm(a, 2) + ',Y'
    if m == 'abs':
        return nm(a)
    if m == 'absx':
        return nm(a) + ',X'
    if m == 'absy':
        return nm(a) + ',Y'
    if m == 'ind':
        return f'({nm(a)})'
    if m == 'indx':
        return f'({nm(a, 2)},X)'
    if m == 'indy':
        return f'({nm(a, 2)}),Y'
    if m == 'zpi':
        return f'({nm(a, 2)})'
    if m == 'aix':
        return f'({nm(a)},X)'
    if m == 'rel':
        return nm(ins.target())
    if m == 'zpr':
        return f'{nm(a & 0xFF, 2)},{nm(ins.target())}'
    raise ValueError(m)


def main(argv=None):
    from . import cart
    ap = argparse.ArgumentParser(prog='watarakit.w65c02')
    ap.add_argument('rom')
    ap.add_argument('--at', default='FFFC', help='CPU address (hex)')
    ap.add_argument('--bank', type=int, default=0,
                    help='the bank seen at $8000-$BFFF')
    ap.add_argument('-n', type=int, default=32)
    ap.add_argument('--vec', action='store_true', help='print the vectors')
    a = ap.parse_args(argv)
    c = cart.Cart.load(a.rom)
    read = c.reader(a.bank)
    if a.vec:
        for name, addr in c.vectors().items():
            print(f'{name:5} ${addr:04X}')
        return
    pc = int(a.at, 16)
    for _ in range(a.n):
        ins = decode(read, pc)
        flag = '  ; illegal' if ins.op.illegal else ''
        print(f'{c.where(pc, a.bank)}  {ins.raw.hex(" "):<9} {ins}{flag}')
        pc = (pc + ins.size) & 0xFFFF


if __name__ == '__main__':
    sys.exit(main())
