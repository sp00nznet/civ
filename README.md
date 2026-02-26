# Civilization Recomp

**A static recompilation of Sid Meier's Civilization (1991) for modern Windows**

> *"I've played a lot of Civilization in my time, I can tell you."*
> — Literally everyone who has ever touched this game

---

## What Is This?

This is a fan preservation project to bring the original **Sid Meier's Civilization** —
the game that invented an entire genre, ruined millions of sleep schedules, and taught
more people about the tech tree of human progress than any history class ever could —
natively to Windows 11 via **static recompilation**.

No emulation. No DOSBox wrapper. Just pure, native, one-more-turn glory running on
modern hardware exactly as Sid intended (minus the 640K memory limit and IRQ conflicts).

**This is an unauthorized fan project. We are not affiliated with Firaxis Games,
Take-Two Interactive, or MicroProse.** We just think this game is a masterpiece that
deserves to be preserved forever, turn after turn, civilization after civilization.

### A Note About Sid Meier

Sid Meier is one of the greatest game designers who has ever lived. With *Civilization*
(1991), he and Bruce Shelley created something genuinely transcendent — a game that is
simultaneously a toy, a puzzle, a strategy engine, a history lesson, and one of the most
addictive pieces of software ever compiled. Meier's design philosophy of "interesting
decisions" produced a game where every single turn matters and every choice echoes across
millennia. The fact that we're still talking about (and playing) this game over 30 years
later speaks to the brilliance of its design.

The original *Civilization* was a product of MicroProse Software, the legendary studio
co-founded by "Wild" Bill Stealey and Sid Meier. This project exists purely out of love
and respect for their work, and a desire to ensure it remains playable forever.

---

## Why Static Recompilation?

| Approach | How It Works | Trade-offs |
|----------|-------------|------------|
| **DOSBox** | Emulates entire x86 CPU + DOS + hardware | Works but adds overhead, input lag, scaling artifacts |
| **Source Port** | Rewrite from scratch based on behavior | Massive effort, subtle differences from original |
| **Static Recomp** | Translate original machine code to C, run natively | Preserves exact original logic, runs at native speed |

Static recompilation gives us the best of all worlds: the *exact* game logic Sid wrote,
compiled fresh for modern x86-64, with a thin hardware abstraction layer replacing the
DOS/VGA/AdLib interfaces with SDL2.

---

## Binary Analysis

### CIV.EXE — The Main Executable

```
================================================================
  Sid Meier's Civilization (1991) - Binary Analysis
  Compiled with Microsoft C 5.x (1988 Runtime)
================================================================

  File:              civ.exe (305,024 bytes / 297.9 KB)
  Architecture:      16-bit x86 real mode (DOS)
  Compiler:          Microsoft C 5.x (MSC 1988 Runtime Library)
  Overlay Manager:   Microsoft C INT 3Fh

  Resident Code:     ~174 KB (loaded at startup)
  Overlay Modules:   23 modules (~124 KB demand-loaded)
  Total Code:        ~298 KB

  Entry Point:       CS:IP = 2A10:0010
  Stack:             SS:SP = 3217:0080
```

### Overlay Module Map

The game uses the **Microsoft C Overlay Manager** to fit ~298 KB of code into DOS
memory constraints. Code is divided into a resident portion (always loaded) and 23
overlay modules that are demand-loaded via `INT 3Fh` when called:

```
  Overlay   File Offset   Size      Functions   Call Sites
  -------   -----------   --------  ---------   ----------
  OVL 01    0x02B800       1.5 KB       1            1
  OVL 02    0x02BE00       7.4 KB       4            4
  OVL 03    0x02DC00       3.6 KB       6            8
  OVL 04    0x02EC00       1.6 KB       4           18
  OVL 05    0x02F400       8.3 KB       7           14
  OVL 06    0x031600      10.4 KB       5           15
  OVL 07    0x034000       8.2 KB       3            4
  OVL 08    0x036200       7.0 KB       4           10
  OVL 09    0x037E00       6.3 KB       3            5
  OVL 10    0x039800       2.2 KB       2            2
  OVL 11    0x03A200       4.4 KB       2            3
  OVL 12    0x03B400       7.8 KB       6            9
  OVL 13    0x03D400       2.9 KB       2            2
  OVL 14    0x03E000       7.9 KB       8           16
  OVL 15    0x040000       2.9 KB       2            2
  OVL 16    0x040C00       1.8 KB       1            1
  OVL 17    0x041400       4.6 KB       2            3
  OVL 18    0x042800       6.7 KB       4            5
  OVL 19    0x044400       8.2 KB       4            9
  OVL 20    0x046600       6.0 KB       3            7
  OVL 21    0x048000       1.5 KB       1           18
  OVL 22    0x048800       5.3 KB       5            8
  OVL 23    0x049E00       2.4 KB       4            4
                           --------  ---------   ----------
  Total:                  ~124 KB      83 funcs    168 calls
```

### DOS/BIOS Interface

```
  Interrupt   Service          Calls   Recompilation Target
  ---------   -------          -----   --------------------
  INT 21h     DOS API           120    Win32 API / C runtime
  INT 3Fh     MSC Overlay       168    Direct function calls (resolved at recomp time)
  INT 33h     Mouse driver        7    SDL2 mouse input
  INT 16h     Keyboard BIOS       4    SDL2 keyboard input
  INT 10h     Video BIOS          3    SDL2 / D3D11 rendering
  INT 09h     Keyboard HW IRQ     3    SDL2 event loop
  INT 08h     Timer IRQ            1    SDL2 timer / QueryPerformanceCounter
```

### Support Executables

| File | Size | Purpose |
|------|------|---------|
| `egraphic.exe` | 11,584 B | EGA (16-color) graphics driver |
| `mgraphic.exe` | 7,142 B | MCGA/VGA (256-color) graphics driver |
| `tgraphic.exe` | 9,990 B | Tandy (16-color) graphics driver |
| `misc.exe` | 980 B | Utility/launcher stub |

### Game Data Files

| Type | Count | Purpose |
|------|-------|---------|
| `.pic` | 106 | Images (title, units, terrain, cities, diplomacy, wonders) |
| `.pal` | 38 | VGA/EGA palettes (256 or 16 color, 6-bit RGB) |
| `.cvl` | 4 | Sound data (AdLib/SB/Tandy/IBM speaker) |
| `.txt` | 10 | Game text (credits, help, intro, Civilopedia entries) |
| `.cv` | 1 | Font data |
| `.map` | 1 | Saved map data |
| `.sve` | 1 | Saved game state |
| `.dta` | 1 | Hall of Fame data |

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                  Recompiled Game Code                │
│         (civ.exe → C, resident + 23 overlays)       │
├─────────────────────────────────────────────────────┤
│                  DOS Compatibility Layer             │
│    INT 21h → C runtime    Overlay mgr → direct call │
│    File I/O → stdio       Memory → malloc/heap      │
├───────────────┬───────────────┬──────────────────────┤
│   Video HAL   │   Audio HAL   │     Input HAL        │
│  VGA 320x200  │  AdLib OPL2   │  Mouse + Keyboard    │
│  256-color    │  PC Speaker   │  INT 33h/16h → SDL2  │
│  → SDL2/D3D11 │  → SDL2 Audio │                      │
├───────────────┴───────────────┴──────────────────────┤
│                     SDL2 / Win32                     │
├──────────────────────────────────────────────────────┤
│                    Windows 11 x64                    │
└──────────────────────────────────────────────────────┘
```

---

## Project Structure

```
civ/
├── README.md                    # This file
├── CMakeLists.txt               # Root build configuration (CMake 3.20+)
├── civ.syms.toml                # Exported function symbol table
├── .gitignore                   # Excludes game files and build output
├── tools/                       # Reverse engineering & analysis tools
│   ├── CMakeLists.txt
│   ├── mzparse/                 # MZ header & overlay analyzer
│   ├── ovldump/                 # Overlay module extractor
│   ├── picdecode/               # .PIC/.PAL image format analyzer
│   └── recomp/                  # Static recompilation toolchain
│       ├── decode16.py          # 16-bit x86 instruction decoder
│       ├── analyze.py           # Function boundary & call graph analyzer
│       ├── lift.py              # x86-16 to C code lifter
│       └── recomp.py            # Main recompilation driver
├── include/                     # Public headers
│   ├── recomp/
│   │   ├── cpu.h                # CPU state struct (registers, flags, memory)
│   │   └── dos_compat.h         # DOS API compatibility layer
│   ├── hal/
│   │   ├── video.h              # VGA Mode 13h emulation
│   │   ├── input.h              # Keyboard & mouse HAL
│   │   └── timer.h              # PIT timer emulation
│   └── platform/
│       └── sdl_platform.h       # SDL2 platform layer
├── src/
│   ├── main.c                   # Entry point & main game loop
│   ├── recomp/
│   │   ├── cpu.c                # CPU state management
│   │   ├── dos_compat.c         # Full INT 21h/10h/16h/33h implementation
│   │   └── startup.c            # MSC crt0 replacement
│   ├── hal/
│   │   ├── video.c              # VGA DAC palette, mode 13h, vsync
│   │   ├── input.c              # Keyboard buffer, mouse state
│   │   └── timer.c              # PIT timer tick emulation
│   └── platform/
│       └── sdl_platform.c       # SDL2 window, rendering, input events
└── RecompiledFuncs/             # Auto-generated C output (gitignored)
    ├── civ_recomp.h             # Master header (482 function declarations)
    ├── civ_recomp_000..009.c    # Recompiled game code (132K lines)
    └── civ_stubs.c              # Stub functions for unresolved symbols
```

---

## Building

### Prerequisites

- **CMake** 3.20+
- **MSVC** (Visual Studio 2022) or **MinGW-w64**
- **Python 3** (for recompilation toolchain)
- **SDL2** via [vcpkg](https://github.com/microsoft/vcpkg): `vcpkg install sdl2:x64-windows`

### Building

```bash
# Step 1: Run the recompiler (generates C files from CIV.EXE)
py -3 tools/recomp/recomp.py path/to/CIV.EXE RecompiledFuncs

# Step 2: Configure CMake with vcpkg
cmake -B build -G "Visual Studio 17 2022" -A x64 \
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

# Step 3: Build
cmake --build build --config Release
```

### Running

```bash
# Run from the game data directory
cd path/to/game/data
path/to/build/Release/civ.exe --gamedir . --scale 3
```

### Running Analysis Tools

```bash
# Analyze the MZ executable structure
build/Release/mzparse.exe path/to/civ.exe

# Extract overlay modules
build/Release/ovldump.exe path/to/civ.exe output_dir/

# Analyze .PIC image files
build/Release/picdecode.exe path/to/logo.pic
```

### Running the Recompiler

```bash
# Disassemble resident code
py -3 tools/recomp/decode16.py path/to/civ.exe --resident

# Disassemble overlay N
py -3 tools/recomp/decode16.py path/to/civ.exe --overlay 5

# Analyze function boundaries
py -3 tools/recomp/analyze.py path/to/civ.exe -symbols civ.syms.toml

# Full recompilation (generates C files)
py -3 tools/recomp/recomp.py path/to/civ.exe RecompiledFuncs
```

---

## Progress

### Phase 0 — Binary Analysis & Documentation

- [x] MZ header parsing and structure analysis
- [x] Microsoft C overlay manager identification (INT 3Fh)
- [x] Overlay module enumeration (23 modules, 83 functions)
- [x] DOS interrupt surface mapping
- [x] String table extraction (1,807 strings)
- [x] Support executable identification
- [x] Game data file inventory
- [ ] .PIC image format reverse engineering
- [ ] .CVL sound format reverse engineering
- [ ] .CV font format analysis

### Phase 1 — Recompilation Toolchain

- [x] 16-bit x86 instruction decoder (full 8086/80186 opcode coverage)
- [x] Function boundary detection (MSC 5.x prologue/epilogue patterns)
- [x] Control flow analysis and call graph extraction
- [x] x86-16 to C lifter (CPU state struct approach)
- [x] MSC overlay call resolution (INT 3Fh -> direct C function calls)
- [x] Segment:offset -> flat memory translation
- [x] Port I/O lifting (IN/OUT -> port_in8/port_out8 dispatch)
- [x] Batch compilation output (split across .c files)
- [x] Symbol table export (TOML format)
- [x] Stub generation for unresolved symbols (553 stubs)

**Recompilation Results:**
```
  Functions:     482 (329 resident + 153 overlay)
  Instructions:  106,935
  Code bytes:    280,991 (274.4 KB)
  Output:        132,585 lines of C across 10 source files + 553 stubs
  Errors:        0
```

### Phase 2 — DOS Compatibility Layer *(current)*

- [x] INT 21h replacement (file I/O: create/open/close/read/write/seek/delete)
- [x] INT 21h memory management (alloc/free/resize paragraphs)
- [x] INT 21h console I/O (char in/out, print string, input status)
- [x] INT 21h system calls (date/time, DOS version, drive/directory, interrupt vectors)
- [x] INT 10h Video BIOS (set mode, cursor, teletype, get mode)
- [x] INT 16h Keyboard BIOS (read key, check key, shift flags)
- [x] INT 33h Mouse Driver (reset, show/hide, position, range, handler)
- [x] VGA Mode 13h framebuffer (320x200, 256 colors at A000:0000)
- [x] VGA DAC palette emulation (ports 3C7/3C8/3C9 state machine)
- [x] VGA input status register (port 3DA, vsync toggle)
- [x] PIT timer emulation (ports 40h/43h, 18.2 Hz tick rate)
- [x] Port I/O dispatch (VGA, PIT, PIC, keyboard ports)
- [x] DOS path translation (game directory mapping)
- [x] SDL2 platform layer (window, renderer, streaming texture)
- [x] SDL2 event handling (keyboard scancode map, mouse, fullscreen toggle)
- [x] SDL2 VGA rendering (indexed framebuffer -> RGBA palette conversion)
- [x] MSC crt0 startup replacement (segment register initialization)
- [x] Main entry point with frame-driven game loop
- [x] **Full project compiles and links (zero errors, zero warnings)**
- [ ] Cooperative yielding for game main loop (Phase 4)
- [ ] Resolve 553 stub functions (identify C library vs game functions)

**Build Output:**
```
  civ.exe           22 KB     Main executable
  civ_recomp.lib    6.0 MB    Recompiled game code (482 functions + 553 stubs)
  civ_hal.lib       36 KB     HAL + DOS compatibility
  civ_platform.lib  13 KB     SDL2 platform layer
  SDL2.dll          1.6 MB    SDL2 runtime
```

### Phase 3 — Game Data & Audio HAL

- [ ] .PIC image loader (native format support)
- [ ] .PAL palette loader
- [ ] .CVL sound data loader
- [ ] .CV font renderer
- [ ] SDL2 audio output (AdLib OPL2 synthesis or PCM playback)

### Phase 4 — Integration & Testing

- [ ] Boot sequence (MicroProse logo → title screen)
- [ ] Menu navigation (New Game / Load / Earth / Custom)
- [ ] Game initialization (world generation, civ selection)
- [ ] Map rendering (terrain tiles, units, cities)
- [ ] City management screen
- [ ] Diplomacy screens
- [ ] Combat resolution
- [ ] Technology tree
- [ ] Wonder movies / screens
- [ ] Save/load game state
- [ ] Hall of Fame

### Phase 5 — Polish & Enhancement

- [ ] Integer scaling (1x, 2x, 3x, 4x)
- [ ] Windowed / fullscreen toggle
- [ ] Modern input improvements (scroll wheel for zoom, etc.)
- [ ] Windows 11 installer / portable build
- [ ] Gamepad support (optional)

---

## Game Data — User Supplied

**You must own a legitimate copy of Civilization.** Place the game files in a `game/`
subdirectory (gitignored). The recompiled executable loads data from this directory
at runtime.

Required files:
```
game/
├── civ.exe          # Original executable (for reference/verification)
├── *.pic            # All image files
├── *.pal            # All palette files
├── *.cvl            # Sound data
├── fonts.cv         # Font data
├── *.txt            # Game text files
├── fame.dta         # Hall of Fame
└── civil0.map       # Default map
```

---

## Technical Notes

### Why Microsoft C 5.x?

MicroProse was a Microsoft C shop in the late 80s/early 90s. The runtime signature
`MS Run-Time Library - Copyright (c) 1988, Microsoft Corp` confirms MSC 5.x, which
was the standard professional C compiler for DOS development at the time. This is
consistent with other MicroProse titles of the era.

### The Overlay Manager

With only 640 KB of conventional memory, a ~298 KB game needed help. Microsoft C's
overlay manager uses `INT 3Fh` as a software interrupt to demand-load code segments
from disk. When the resident code calls an overlaid function, the overlay manager:

1. Intercepts the `INT 3Fh` call
2. Reads the overlay number and offset from the instruction stream
3. Loads the overlay module from the EXE file into memory
4. Transfers control to the target function

For recompilation, this is actually a gift — every `INT 3Fh` call site is an explicit
inter-module function call with a known target. We resolve these statically at recomp
time into direct C function calls, eliminating the overlay manager entirely.

### The Graphics Drivers

Civilization ships with separate executables for different graphics hardware:
- `egraphic.exe` — EGA (640x350, 16 colors or 320x200, 16 colors)
- `mgraphic.exe` — MCGA/VGA (320x200, 256 colors)
- `tgraphic.exe` — Tandy 1000 (320x200, 16 colors)

The main `civ.exe` calls out to these as external processes for graphics initialization
and rendering. For the recomp, we implement the VGA (256-color) path directly.

---

## Credits

### Original Game

- **Designed by** Sid Meier with Bruce Shelley
- **Programming** by Sid Meier
- **Computer Graphics** by Larry Coones, Michael Haire, Harry Teasley, Barbara Bents,
  Todd Brizzi, Erroll Roberts, Chris Soares, Nicholas Rusko-Berger, Stacey Clark,
  Brian Martel
- **Original Music** by Jeffery L. Briggs
- **Quality Assurance** by Al Roireau, Jerry Shaffirio, Mike Corcoran, Tim Train,
  Michael Rea, Chris Clark, Michael Craighead, Nick Yuran, Paul Murphy
- **Published by** MicroProse Software (1991)

### This Project

A [sp00nznet](https://github.com/sp00nznet) fan preservation project.
Built with love, respect, and far too many turns.

---

## Legal

This is an **educational and preservation project**. No original game code or assets
are distributed in this repository. Users must supply their own legally obtained copy
of Sid Meier's Civilization.

Civilization is a trademark of Take-Two Interactive Software, Inc. / Firaxis Games.
This project is not affiliated with, endorsed by, or connected to Take-Two Interactive,
Firaxis Games, or the estate of MicroProse Software in any way.

*"Just one more turn..."*
