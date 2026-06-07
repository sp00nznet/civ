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

Boots **all the way into the game's intro**: loads `misc.exe`, dispatches the
first INT 3Fh overlay call into the manager at `305a:2d58`, re-opens CIV.EXE,
opens `intro.txt`, and runs tens of millions of instructions drawing the text
screen — i.e. the real game is *executing*, not just unpacking.

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
