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

## TODO / next

- Drive the scripted keys through the menus (New Game → difficulty → civ) to
  reach the sp299 load; the prompt/menu key sequence needs tuning.
- Perf: the per-instruction `hook_code` (CS-transition trace) dominates runtime;
  gate it behind `--trace` so long runs to deep game state are fast.
- Optionally feed INT 10h to a tiny mode-13h framebuffer + dump PPMs for visual
  ground truth (the recomp already dumps `fb_*.ppm`).
