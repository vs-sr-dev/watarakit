# Sources

watarakit is MIT-licensed (`LICENSE`) and contains no third-party code.

The hardware it models -- the memory map, the I/O registers, the timer, the
interrupts, the LCD and the sound -- follows the behaviour of MAME's
Supervision driver (`src/mame/svision/svision.cpp` and `svis_snd.cpp`, by
Peter Trauner, GPL-2.0+), read as documentation: watarakit's interpreter,
runtime and synthesizer are written independently (the sound, for one, is
computed in CPU cycles rather than per output sample). The four grey
levels are those MAME takes from Chris Covell's photographs of the LCD.

The LCD DMA at $2008-$200D, which MAME does not model, is modelled and
counted by the tracer after Potator's description; no game starts it.
