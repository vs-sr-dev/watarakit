"""A Supervision cartridge: plain ROM, 16 KB banks, no header.

The CPU sees the cartridge in two windows:

    $8000-$BFFF  the bank chosen by bits 5-7 of $2026 (modulo the bank count)
    $C000-$FFFF  always the cartridge's last bank: the vectors live there

A 32 KB cart has banks 0-1, a 64 KB cart banks 0-3; a write of 5 to the
bank bits of a 64 KB cart selects bank 1.  There is no mapper chip: the
bank latch is part of the SoC, which also means no cartridge RAM and no
battery saves -- the console has none.

    python -m watarakit.cart ROM.sv              # size, banks, vectors, fill
    python -m watarakit.cart _roms/*.sv --table  # one line per cart
"""

import argparse
import glob
import hashlib
import os
import sys

BANK = 0x4000


class Cart:
    def __init__(self, data, path=''):
        if len(data) % BANK or not data:
            raise ValueError(f'{path}: size {len(data)} is not a multiple of 16 KB')
        self.data = bytes(data)
        self.path = path
        self.nbanks = len(data) // BANK

    @classmethod
    def load(cls, path):
        with open(path, 'rb') as f:
            return cls(f.read(), path)

    @property
    def name(self):
        return os.path.splitext(os.path.basename(self.path))[0]

    def bank(self, n):
        n %= self.nbanks
        return self.data[n * BANK:(n + 1) * BANK]

    @property
    def fixed(self):
        return self.nbanks - 1

    def offset(self, addr, bank=0):
        """ROM file offset for a CPU address, or None outside the ROM."""
        if 0xC000 <= addr <= 0xFFFF:
            return self.fixed * BANK + (addr - 0xC000)
        if 0x8000 <= addr <= 0xBFFF:
            return (bank % self.nbanks) * BANK + (addr - 0x8000)
        return None

    def reader(self, bank=0):
        d = self.data

        def read(addr):
            o = self.offset(addr & 0xFFFF, bank)
            return d[o] if o is not None else 0
        return read

    def word(self, addr, bank=0):
        r = self.reader(bank)
        return r(addr) | r(addr + 1) << 8

    def vectors(self):
        return {'NMI': self.word(0xFFFA), 'RESET': self.word(0xFFFC),
                'IRQ': self.word(0xFFFE)}

    def where(self, addr, bank=0):
        """A printable location: the fixed bank is 'F', a banked page 'B<n>'."""
        if addr >= 0xC000:
            return f'F:{addr:04X}'
        if addr >= 0x8000:
            return f'B{bank % self.nbanks}:{addr:04X}'
        return f'--:{addr:04X}'

    def fill(self, n):
        """Bytes of trailing fill in bank n, and the fill byte."""
        b = self.bank(n)
        last = b[-7] if n == self.fixed else b[-1]   # skip the vectors
        end = BANK - 6 if n == self.fixed else BANK
        i = end
        while i > 0 and b[i - 1] == last:
            i -= 1
        return end - i, last

    def mirrored(self):
        """Pairs of identical banks (some 64 KB dumps carry a 32 KB game twice)."""
        out = []
        for i in range(self.nbanks):
            for j in range(i + 1, self.nbanks):
                if self.bank(i) == self.bank(j):
                    out.append((i, j))
        return out

    def md5(self):
        return hashlib.md5(self.data).hexdigest()


def main(argv=None):
    ap = argparse.ArgumentParser(prog='watarakit.cart')
    ap.add_argument('roms', nargs='+')
    ap.add_argument('--table', action='store_true')
    a = ap.parse_args(argv)
    paths = [p for g in a.roms for p in (glob.glob(g) or [g])]
    for p in paths:
        c = Cart.load(p)
        v = c.vectors()
        fills = [c.fill(i) for i in range(c.nbanks)]
        if a.table:
            fs = ' '.join(f'{n:5d}' for n, _ in fills)
            mir = ','.join(f'{i}={j}' for i, j in c.mirrored()) or '-'
            print(f'{c.name[:34]:34} {len(c.data)//1024:3d}K '
                  f'R={v["RESET"]:04X} N={v["NMI"]:04X} I={v["IRQ"]:04X} '
                  f'free[{fs}] mirror={mir}')
            continue
        print(c.name)
        print(f'  size   {len(c.data)} ({c.nbanks} banks)  md5 {c.md5()}')
        for k, x in v.items():
            print(f'  {k:6} ${x:04X}')
        for i, (n, b) in enumerate(fills):
            print(f'  bank {i}: {n} bytes of ${b:02X} at the end')
        for i, j in c.mirrored():
            print(f'  bank {i} == bank {j}')


if __name__ == '__main__':
    sys.exit(main())
