"""Every cartridge through the whole route: trace, recompile, build, lockstep.

    python -m watarakit.recomp.all ROMDIR [--frames 6000] [--only NAME] [--jobs N]

For each ROM: `tracer/svtrace` (6000 frames of the monkey pad) unless its
trace is there, `recomp` to build/recomp/NAME, g++, then `sv.exe
--lockstep` -- the interpreter and the native code side by side, the
machine compared at every frame -- and one line of results.
"""
import argparse
import concurrent.futures
import glob
import os
import re
import subprocess
import sys

from . import __main__ as recomp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TRACER = os.path.join(ROOT, 'tracer', 'svtrace.exe')


def key(path):
    n = os.path.basename(path).replace(' (USA, Europe).sv', '').replace('.sv', '')
    return re.sub(r'[^A-Za-z0-9]', '_', n)


def one(rom, frames, build_dir):
    k = key(rom)
    tdir = os.path.join(build_dir, 'trace', k)
    if not os.path.exists(os.path.join(tdir, 'report.txt')):
        os.makedirs(tdir, exist_ok=True)
        subprocess.run([TRACER, rom, tdir, str(frames)], check=True)
    out = os.path.join(build_dir, 'recomp', k)
    logs = []
    try:
        files = recomp.generate(rom, tdir, out, log=logs.append)
        recomp.build(out, files, log=logs.append)
    except Exception as e:                       # noqa: BLE001 -- reported per game
        return k, f'BUILD FAILED: {str(e)[:300]}', logs
    r = subprocess.run([os.path.join(out, 'sv.exe'), os.path.abspath(rom), '--lockstep',
                        '--frames', str(frames), '--out', out],
                       capture_output=True, text=True, cwd=out)
    return k, (r.stdout + r.stderr).strip(), logs


def main(argv=None):
    ap = argparse.ArgumentParser(prog='watarakit.recomp.all')
    ap.add_argument('romdir')
    ap.add_argument('--frames', type=int, default=6000)
    ap.add_argument('--only')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--build-dir', default=os.path.join(ROOT, 'build'))
    a = ap.parse_args(argv)
    roms = sorted(glob.glob(os.path.join(a.romdir, '*.sv')))
    if a.only:
        roms = [r for r in roms if a.only.lower() in r.lower()]
    ok = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for k, res, logs in ex.map(lambda r: one(r, a.frames, a.build_dir), roms):
            same = f'lockstep: identical for {a.frames} frames' in res
            ok += same
            m = re.search(r'interpreted insns (\d+).*?fallbacks (\d+)\s+unwinds (\d+)\s+max depth (\d+)', res)
            speed = re.search(r'\((\d+)x\)', res)
            info = (f'interp {int(m.group(1)):>9}  fallbacks {m.group(2):>4}  unwinds {m.group(3):>4}  '
                    f'depth {m.group(4):>3}  {speed.group(1) if speed else "?":>5}x') if m else ''
            print(f'{"OK  " if same else "FAIL"} {k:32} {info}', flush=True)
            if not same:
                for line in res.splitlines()[:8]:
                    print('       ' + line)
    print(f'{ok}/{len(roms)} identical in lockstep')
    return 0 if ok == len(roms) else 1


if __name__ == '__main__':
    sys.exit(main())
