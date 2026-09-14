#!/usr/bin/env python3
"""
shots.py -- run the recomp headlessly and come back with pictures.

Every "is it rendering?" question in this project used to be answered by
launching the game, watching a window, and describing what happened. That is
not a check anyone can re-run, and it is exactly how a relift regressed the
palette without anyone noticing for a day: the framebuffer dumps were gated
behind `nzA > 50`, so a screen that never drew produced no screenshot and no
alarm. This runs the thing, captures frames whether or not they have content,
and prints what the render loop actually saw.

    py -3 tools/shots.py                       capture the default boot
    py -3 tools/shots.py --keys N              drive New Game on the way through
    py -3 tools/shots.py --secs 60             give it longer
    py -3 tools/shots.py --vs <other-civ.exe>  A/B two builds, report the divergence

The --vs lane is the one that pays for itself. It runs both binaries with the
same input and compares the per-frame render diagnostics, so "the relift broke
rendering" becomes a tick number and a field name instead of an impression.

Frames land in work/shots/<label>/ as PPMs and PNGs. Needs nothing but the
standard library; tools/ppm2png.py does the encoding.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ppm2png import read_ppm, write_png  # noqa: E402

DEFAULT_EXE = os.path.join(ROOT, 'build', 'Release', 'civ.exe')
# The game reads its data from the CWD, so the run happens in the game dir.
DEFAULT_GAMEDIR = os.environ.get(
    'CIV_GAMEDIR', r'G:\recomp\pc\civ\extracted\Civilization\civ')

DIAG = re.compile(
    r'\[RENDERDIAG\] #(\d+) vmode=0x(\w+) pal_rich=(\d+)/224 nzA=(\d+)'
    r'\(bbox ([-\d]+),([-\d]+)\.\.([-\d]+),([-\d]+) col=(\d+)\) nzC=(\d+) nzD=(\d+)')


class Frame:
    """One [RENDERDIAG] line: what the render loop saw on that tick."""
    __slots__ = ('tick', 'vmode', 'pal', 'nzA', 'bbox', 'col', 'nzC', 'nzD')

    def __init__(self, m):
        g = m.groups()
        self.tick, self.vmode = int(g[0]), int(g[1], 16)
        self.pal, self.nzA = int(g[2]), int(g[3])
        self.bbox = tuple(int(x) for x in g[4:8])
        self.col, self.nzC, self.nzD = int(g[8]), int(g[9]), int(g[10])

    def key(self):
        """The fields two builds running the same input must agree on."""
        return (self.vmode, self.pal, self.nzA, self.col, self.nzC, self.nzD)

    def __str__(self):
        return (f'tick {self.tick:4d}  vmode {self.vmode:#04x}  pal {self.pal:3d}/224  '
                f'nzA {self.nzA:6d}  col {self.col:3d}  nzC {self.nzC}  nzD {self.nzD}')


def run(exe, label, secs, keys, every, maxshots, gamedir):
    """Run one build with capture on, return (frames, shot_dir, log)."""
    shot_dir = os.path.join(ROOT, 'work', 'shots', label)
    shutil.rmtree(shot_dir, ignore_errors=True)
    os.makedirs(shot_dir, exist_ok=True)

    env = dict(os.environ)
    # Set the whole capture contract here so no caller has to remember that
    # CIV_DUMPLATEST alone is a no-op without CIV_RENDERDIAG.
    env.update(CIV_RENDERDIAG='1', CIV_SHOTS=shot_dir.replace('\\', '/'),
               CIV_SHOTS_EVERY=str(every), CIV_SHOTS_MAX=str(maxshots))
    if keys:
        env['CIV_KEYSCRIPT'] = keys

    print(f'[{label}] {exe}')
    print(f'[{label}] {secs}s, keys={keys or "(none)"}, frames every {every} ticks')
    try:
        p = subprocess.run([exe, '--gamedir', '.'], cwd=gamedir, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=secs)
        out = p.stdout.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired as e:
        # A timeout is the normal exit: the game has no way to quit itself.
        out = (e.stdout or b'').decode('utf-8', 'replace')

    log = os.path.join(shot_dir, 'run.log')
    open(log, 'w', encoding='utf-8').write(out)
    frames = [Frame(m) for m in DIAG.finditer(out)]

    for ppm in sorted(f for f in os.listdir(shot_dir) if f.endswith('.ppm')):
        src = os.path.join(shot_dir, ppm)
        w, h, rgb = read_ppm(src)
        write_png(src[:-4] + '.png', w, h, rgb)
    return frames, shot_dir, log


def summarise(label, frames, shot_dir):
    if not frames:
        print(f'[{label}] no [RENDERDIAG] output -- the render loop never ran')
        return
    best = max(frames, key=lambda f: f.nzA)
    first = next((f for f in frames if f.nzA > 0), None)
    # Text mode captures land as .txt -- counting only .png reported the
    # startup config screen, the one worth seeing, as 'no frames'.
    pngs = sorted(f for f in os.listdir(shot_dir)
                  if f.endswith('.png') or f.endswith('.txt'))
    print(f'[{label}] {len(frames)} render ticks, {len(pngs)} frames captured')
    print(f'[{label}] first frame with pixels: '
          + (str(first) if first else 'NONE -- screen stayed black'))
    print(f'[{label}] busiest frame:           {best}')
    if best.pal == 0:
        print(f'[{label}] palette never programmed (pal_rich 0/224) -- '
              f'nothing wrote the DAC')
    print(f'[{label}] frames in {os.path.relpath(shot_dir, ROOT)}')


def compare(a_frames, b_frames, a_label, b_label):
    """Report the first tick where two builds stop agreeing."""
    print(f'\n--- {a_label} vs {b_label} ---')
    for i, (x, y) in enumerate(zip(a_frames, b_frames)):
        if x.key() != y.key():
            print(f'diverge at render tick {i}:')
            print(f'  {a_label:10s} {x}')
            print(f'  {b_label:10s} {y}')
            for name, xv, yv in (('vmode', x.vmode, y.vmode), ('pal_rich', x.pal, y.pal),
                                 ('nzA', x.nzA, y.nzA), ('nzC', x.nzC, y.nzC),
                                 ('nzD', x.nzD, y.nzD)):
                if xv != yv:
                    print(f'  first differing field: {name} {xv} -> {yv}')
                    break
            return 1
    if len(a_frames) != len(b_frames):
        print(f'agree for {min(len(a_frames), len(b_frames))} ticks, then one '
              f'build stopped ({a_label}: {len(a_frames)}, {b_label}: {len(b_frames)})')
        return 1
    print(f'identical across {len(a_frames)} render ticks')
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.strip().split('\n')[0])
    ap.add_argument('--exe', default=DEFAULT_EXE)
    ap.add_argument('--vs', metavar='EXE', help='second build to compare against')
    ap.add_argument('--keys', default='', help='CIV_KEYSCRIPT, e.g. N for New Game')
    ap.add_argument('--secs', type=int, default=45)
    ap.add_argument('--every', type=int, default=4, help='render ticks between frames')
    ap.add_argument('--max', type=int, default=24, help='frames to capture')
    ap.add_argument('--gamedir', default=DEFAULT_GAMEDIR)
    a = ap.parse_args()

    if not os.path.isdir(a.gamedir):
        raise SystemExit(f'game dir not found: {a.gamedir} (set CIV_GAMEDIR)')
    for exe in filter(None, (a.exe, a.vs)):
        if not os.path.isfile(exe):
            raise SystemExit(f'not built: {exe}')

    frames, d, _ = run(a.exe, 'current', a.secs, a.keys, a.every, a.max, a.gamedir)
    summarise('current', frames, d)
    if not a.vs:
        return 0

    b_frames, bd, _ = run(a.vs, 'other', a.secs, a.keys, a.every, a.max, a.gamedir)
    summarise('other', b_frames, bd)
    return compare(frames, b_frames, 'current', 'other')


if __name__ == '__main__':
    sys.exit(main())
