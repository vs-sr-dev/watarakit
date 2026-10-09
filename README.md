# watarakit

Watara Supervision (1992) reverse engineering and static recompilation:
the cartridge, the W65C02 inside the SoC, a tracer that measures what a
recompiler faces in every game, a W65C02-to-C++ recompiler, and a runtime
that replaces the console under the recompiled code.

Status: the static recompiler runs the whole library. All 63 cartridges of
the (USA, Europe) set (in `_roms/`, not distributed) are recompiled to C++,
built, and run frame for frame identical to the interpreter (lockstep,
6000 frames each); 45 of them never touch the interpreter at all. With
sound: the two pulse channels, the noise and the sample DMA.

## Tools

```sh
python -m watarakit.cart "watarakit/_roms/*.sv" --table      # size, vectors, free space, mirrors
python -m watarakit.w65c02 ROM.sv --vec                       # NMI / RESET / IRQ
python -m watarakit.w65c02 ROM.sv --at C000 -n 40             # disassembly, fixed bank
python -m watarakit.w65c02 ROM.sv --bank 1 --at 8000          # a banked page

gcc -O2 -o tracer/svtrace.exe tracer/svtrace.c                # msys64 ucrt64
tracer/svtrace.exe ROM.sv OUTDIR 6000 --snap 300,1500         # run headless, measure
```

`svtrace` is a small Supervision interpreter (MAME's svision driver as the
hardware reference) with a scripted pad. It writes `report.txt` (coverage
by bank, indirect jumps and their targets, RTS that do not return to their
JSR, dropped return addresses, code run from RAM, self-modifying writes,
bank switches, where the NMI landed, I/O registers used), `cov.bin` (a flag
per ROM byte: opcode, operand, read as data, jump target) and PGM frames.

## The recompiler

```sh
python -m watarakit.recomp.discover ROM.sv --trace build/trace/GAME --report   # functions
python -m watarakit.recomp ROM.sv --trace build/trace/GAME --out build/recomp/GAME --build
build/recomp/GAME/sv.exe ROM.sv --lockstep --frames 6000    # native vs interpreter, every frame
build/recomp/GAME/sv.exe ROM.sv --window --scale 4          # play: arrows, Z/X, Shift, Enter
build/recomp/GAME/sv.exe ROM.sv --frames 1800 --wav out.wav # the sound of a headless run
python -m watarakit.recomp.all _roms --jobs 6               # every cart: trace, recompile, build, lockstep
```

- `recomp/discover.py` -- functions keyed by (bank, address): the fixed bank
  at $C000, each bank of the window at $8000. Seeded by the vectors, the
  descent's JSRs, and the tracer's jump targets and indirect-jump targets.
  A JMP never becomes a C call (code reached by jumps is part of the
  function), so state machines cannot grow the C stack.
- `recomp/emit.py` -- W65C02 to C++, one function per entry, a label per
  instruction; the interpreter's cycles; JSR as a C call with the return
  checked (`c.resume` and S), so `PLA PLA` exits unwind correctly; JMP
  (abs) as a `switch` over the traced targets.
- `runtime/` -- the machine (`sv.h`, `sv_core.cpp`), an interpreter in two
  forms: flat (the reference) and nested (the fallback for whatever is not
  native, handing back to native code at every native entry), and the
  executable (`sv_main.cpp`: headless, `--interp`, `--lockstep`, `--window`
  with SDL2).
- `runtime/sv_audio.cpp` -- the sound, synthesized from the sound
  registers' writes, which the machine logs with the cycle of each: sound
  is output only, so it never takes part in what the CPU sees (the DMA's
  end-of-transfer IRQ is timed by the machine itself from the registers).
- Interrupts are taken only at safe points -- before a JSR, after a backward
  transfer, after RTI and WAI -- in both the interpreter and the native code,
  with the same cycle count. That is what makes the lockstep exact.

## The machine, briefly

| | Supervision | Game Boy (DMG) |
|---|---|---|
| CPU | W65C02 @ 4 MHz (SoC) | SM83 @ 4.19 MHz (1.05 M-cycles) |
| RAM | 8 KB at $0000 | 8 KB WRAM + 127 B HRAM |
| Video | 8 KB **framebuffer**, 2 bpp, 192x170 virtual, 160x160 shown, X/Y scroll | tiles + tilemaps + 40 sprites + window |
| Sprites | none: the CPU blits (and XORs) them into VRAM | hardware OAM |
| Sound | 2 pulse, noise, 4-bit sample DMA from ROM | 2 pulse, wave, noise |
| Cart | plain ROM, 16 KB bank at $8000 (latch in $2026), last bank at $C000 | MBC chips, cart RAM, battery |
| Interrupts | frame NMI, timer IRQ, audio-DMA IRQ | VBlank, STAT, timer, serial, joypad |

## Survey of the library (6000 frames each, monkey pad)

- All 63 games boot and reach play or a menu in the interpreter.
- Code actually run: 2.3-10 KB per game; carts are 32 or 64 KB, mostly graphics.
- No push-and-RTS dispatch anywhere. Indirect jumps: `JMP (abs)` tables in
  ~25 games, 4-38 targets each; never `JMP (abs,X)`.
- Code in RAM: only one pattern, in the Bon Treasure family (Cave Wonders,
  Climber, Juggler, Kitchen War): a `JMP abs` written to zero page and called
  with `JSR $0067` -- an indirect call.
- Bank switches are rare (0-42 in 100 s) and never made from code inside the
  banked window.
- Dropped return addresses (PLA PLA / TXS exits): Penguin Hideout, Chimera,
  Final Combat.
- The LCD DMA at $2008-$200D is never started by any game.
- Families sharing code: Bon Treasure (Cave Wonders, Climber, Juggler, Kitchen
  War, Recycle Design); Magincross / Tasac 2010 / Thunder Shooting; Balloon
  Fight / Kung-Fu Street (Sachen); Hero Hawk / John Adventure. Pyramid and
  Tasac 2010 still carry fragments of their linker's symbol table
  (`draw_sprite`, `xor_sprite`, `hori_inverse`, `pausemusic`...).

## Notes from the library

- **SSSnake walks through empty ROM on every boot.** It enables the NMI and
  selects bank 2 before it has finished clearing RAM; the first NMI calls
  $8548 in that bank, which is $FF fill. `FF FF FF` decodes as `BBS7 $FF`,
  a branch to the next instruction whatever bit 7 says, so the CPU slides
  through the fill until a `BRK` at $9001 sends it through the IRQ handler,
  and the game carries on. BRK is implemented in full for this one game.
- **An indirect call built in zero page.** The Bon Treasure games (Cave
  Wonders, Climber, Juggler, Kitchen War) write a `JMP abs` opcode ($4C) and
  a target to $67-$69 and `JSR $0067`: the only code any game runs from RAM.
- **Vectors into the window.** Penguin Hideout and Pyramid, both 32 KB
  carts, point their NMI vector into $8000-$BFFF -- safe only because they
  never select another bank.
- **What the developers left in the padding.** Pyramid and Tasac 2010 carry
  pieces of their linker's symbol table (`draw_sprite`, `xor_sprite`,
  `hori_inverse`, `pausemusic`, `replaymusic`...): the sprite routines of a
  console with no sprites. Balloon Fight carries its assembler's command
  lines (`AS LIB\IO.ASM -OLIB\IO.OBJ`) and object names (`lib\screen.obj`,
  `lib\sachen.obj`). Galaxy Fighter's free space holds a page of an x86
  disassembly listing, IBM's "Personally Developed Software" strings
  included -- a DOS PC's memory, written into the ROM image with the game.
- **The same few people.** The credit screens of Cave Wonders, Delta Hero,
  Earth Defender, Honey Bee and Jacky Lucky name the same small team again
  and again (Zhang Li and Yong Ban on graphics, Hu Xuhui and Xiao Li Wei on
  music). Their texts end in `$`, each game spelling its space as whatever
  tile its font puts there (`PRESSaSTART$`).
- **The catalogue.** Kabi Island is Bomberman; Super Kong, PacBoy & Mouse,
  Super Pang, Block Buster and Hash Blocks need no explanation; Balloon
  Fight did not even change the name.
