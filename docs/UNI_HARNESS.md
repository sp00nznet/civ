# `uni_civ.py` — headless Unicorn harness for the original CIV.EXE

A scriptable, no-GUI way to run the **original** Civilization binary and observe
its behavior + memory as ground truth for the static recomp. Modeled on the bolo
recomp's `tools/uni_*.py`. Lives at `tools/recomp/uni_civ.py`.

Run:
```
python tools/recomp/uni_civ.py [--trace] [--max N] [--stop-open NAME] [--snap-open NAME]
```
(use `python -u` to see output live — Unicorn+Python hooks buffer stdout otherwise.)

## What it does

Loads `CIV.EXE` into a 1 MB real-mode Unicorn image and lets the program's own
code run, hooking only the DOS/BIOS services it needs:

| Layer | Status | Notes |
|-------|--------|-------|
| EXEPACK decompression | ✅ runs natively | **Must load at a high segment** (`LOAD_SEG=0x1000`). A low load segment makes EXEPACK's `dest = CS − dest_len_paragraphs` underflow → it prints *"Packed file is corrupt"* and exits. |
| MSC crt0 startup | ✅ | DOS version (30h), int vectors (25h/35h), opens CIV.EXE, device ioctls (44h) |
| DOS memory manager | ✅ | `48h` alloc (bump from a free ptr), `4Ah` resize (program shrinks its block → sets the free ptr), BIOS memsize word `0040:0013`. Getting this wrong yields the game's *"MS-DOS lied to us about how much memory was available"* / *"Allocated 1Mb of space????"* aborts. |
| Overlay **drivers** (`4B03`) | ✅ | The video/misc overlays are **separate .EXE files** — `misc.exe`, `egraphic.exe` (EGA), `mgraphic.exe` (MCGA), `tgraphic.exe` (Tandy) — loaded via DOS load-overlay. The harness reads the MZ module and applies its relocations with the supplied factor. (This is what the **video-mode 1–4 prompt** picks.) |
| MSC **INT 3Fh** overlay manager | ✅ dispatches natively | `CD 3F` is followed by a 16-bit overlay/entry **index**. The harness emulates the INT (push FLAGS/CS/IP, jump to `IVT[0x3F]`) so the game's *own* overlay manager runs — it loads overlays through the `4B03` hook and far-calls the function. Requires `25h` to actually populate the IVT. |
| BIOS keyboard (INT 16h) | ⚠ scripted | Feeds `1`,`1`,`1` (video/sound/input prompts) then space/enter to advance. Edit `KEYS` to drive further. |
| File I/O (3Dh/3Fh/42h/3Eh) | ✅ | Served from the game dir; reads/seeks/closes tracked per handle. |
| INT 10h video / 33h mouse / timer | ✗ no-op | Graphics writes go nowhere (headless); fine for memory/flow observation. |

## Where it gets to

Boots into the game and **past the text setup prompts**: loads `misc.exe`,
dispatches the first INT 3Fh overlay into the manager at `305a:2d58`, draws the
text **version screen** + **video/sound/input prompts** (visible via the text-mode
INT 10h below), answers them with tick-paced scripted keys (`1`/`1`/`1`), loads
the selected **graphics driver `Mgraphic.exe`** (MCGA) + `fonts.cv`, and reaches
the **"One Moment Please..."** loading screen — then exits 153 because the loaded
driver overruns VGA memory / can't init graphics in the headless env (next layer).

Two pieces that made the prompts work:
- **Text-mode INT 10h** (set-mode 03h, AH=02 cursor, AH=0E teletype, AH=09 write
  char+attr, AH=06 clear) writes `0xB8000`, so the harness can **dump the actual
  text screen** and see what to type. The game uses BIOS teletype, not direct
  `0xB8000` writes.
- **Tick-paced keys**: the game reads keys via **DOS char input** (AH=01/06/07/08),
  *not* INT 16h, and polls constantly — returning a key every poll floods it
  (490 k consumed, prompt never settles). Deliver one new key every `KEY_TICKS`
  BIOS ticks instead. Timer is driven by bumping `0040:006C` once per run-slice.

## Why it exists (the immediate target)

To recover how the **original** sets up the `sp299.pic` sprite-sheet decode
context — the recomp's open question (the refill handle at `DS:0x686C` resolves
to 0; the loader seems to skip `res_02013E`). `--snap-open sp299` dumps the full
image + the decode-context globals (`0x686C`, `0xE84A/4C`, `0xC19E`, `0x54D8`)
the moment sp299.pic is opened, so we can diff against the recomp.

## SOLVED: graphics-driver overrun -> boots into the real intro

The exit-153 "Overlay has overrun allocated memory" was the overlay manager's
check `cmp bx, [0x53bc]` where `[0x53bc]` is the last `DOS_alloc` query's
`(avail - 0x100)`. The driver-load free-query returned `avail = 0x100` (the heap
took everything else), so `[0x53bc]` got set to **0** and any driver size
overran. Fix: **floor the reported largest-free block at `0x1000`** so the
driver-load query yields a real block (`avail - 0x100` >= driver size); headless,
the VGA RAM above A000 is unused and `TOTAL` covers the writes. (Found by
single-step-tracing the alloc path — `--tracealloc`.)

With that, the harness loads the graphics driver (`Mgraphic.exe`), `fonts.cv`, the
sound driver (`Nsound.cvl`), then `logo.pic` / `birth0,1.pic` / `credits.txt` —
**the exact intro assets the recomp loads** — i.e. it now runs the real game's
intro headless.

## FIXED: post-credits divergence (drivers loaded above VGA)

The bad jump into zero memory traced (via `--findjump`) to a far call into
segment `ab00` — where the sound driver `Nsound.cvl` had loaded, *above* VGA
(`A000`). The avail-floor that fixed the graphics overrun had advanced `mem_free`
past `A000`, so drivers loaded into the VGA region and their relocations (by the
load segment) produced garbage. Fix: in the allocator, **reserve `0xE00` paras of
headroom on the big heap alloc** so the driver overlays load *below* `A000`, and
**cap `mem_free` at `A000`** so allocations never enter VGA. Now the graphics
driver loads at `9100`, the sound driver at `A000`, and the game runs the full
intro (logo/birth/credits) with no divergence.

## Driver placement: reclaim over-granted space

The avail-floor (needed for the overrun check) makes the game alloc a big block
for a small driver overlay, over-advancing `mem_free` toward VGA so the *next*
driver loads at/above A000 (corrupt). Fix: after each `4B03` overlay load, set
`mem_free` to just past the driver's real image (`load_seg + image + minalloc`),
so the graphics + sound drivers pack low and contiguous (`4A1C`/`4A3C`/`4BDD`),
all below A000. No more divergence.

## Screen visibility: the MCGA offscreen framebuffer

The MCGA driver renders to an **offscreen buffer at ~`0x4A000`** (just below the
loaded overlays), and never presents it to linear `A0000` (which stays a uniform
clear — no DAC/CRTC port emulation). Found via a memory-write histogram
(`--findfb`) then dumping the hot region: it shows the **"Sid Meier's
CIVILIZATION" title logo** (the buffer holds the two horizontal halves swapped).
`work/uni_screen.ppm` dumps it (override base with `--fb <hex>`); `--shots` saves
periodic frames to `work/shots/`. This is the key to navigating headless.

## Current wall: title screen won't advance to the interactive menu

The harness reaches and *renders* the title screen, but it won't transition to the
New Game menu: the game polls INT 16h AH=0 ~1M times (handled via a keyboard
buffer feeding scripted keys), yet 'n'/Enter don't advance it, and firing the
game's INT 8 timer at the title doesn't either. So the title's advance gate is
something else (a specific input event/scancode, or a state/flag). Now that the
screen is visible, the next step is to try inputs and watch `work/uni_screen.ppm`
change — then drive New Game -> setup -> the sp299 load (`--snap-open sp299`).

## (earlier) intro pacing

After credits the MCGA driver runs its per-frame loop: wait on a frame counter
`es:[0x440]` (`mgraphic 06c7`), compute timing (`0790`, reads `cs:[0x6b3/6b5]`),
and a VGA-retrace wait (`07a4`: `in 3DA; test al,8`). The `0x3DA` toggle handles
retrace; for the frame counter, the harness **bumps `es:[0x440]` when the spin is
detected** (no timer ISR fires headless) — this advances past that wait into the
frame loop, but the credits sequence still doesn't auto-advance to the title
(`king.txt`). It needs the timer paced to the game's real cadence + the right
input path for the "skip"/advance key (the intro isn't polling INT 16h). That's
the next step toward the sp299 load. Diagnostics: `--findjump`, `--spin`,
`--tracealloc`.

## TODO / next

- Drive the scripted keys through the menus (New Game → difficulty → civ) to
  reach the sp299 load; the prompt/menu key sequence needs tuning.
- Perf: the per-instruction `hook_code` (CS-transition trace) dominates runtime;
  gate it behind `--trace` so long runs to deep game state are fast.
- Optionally feed INT 10h to a tiny mode-13h framebuffer + dump PPMs for visual
  ground truth (the recomp already dumps `fb_*.ppm`).
