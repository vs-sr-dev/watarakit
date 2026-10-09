// The Supervision's sound, synthesized from the register writes the
// machine logged (Sv::snd_ev), each at the cycle it happened.
//
// After MAME's svis_snd.cpp, in cycles rather than output samples:
//   $2010-$2013, $2014-$2017  two pulse channels: period (reg0 | (reg1 & 7) << 8) * 32
//                             cycles; reg2: bit 6 on, bits 4-5 duty (28, 24, 16, 9
//                             of 32), bits 0-3 volume; reg3: frames to play + 1.
//                             Channel 0 sounds right, channel 1 left.
//   $2018-$201C               4-bit samples by DMA: start, length (* 32 nibbles),
//                             rate (clock / 256 / (1 + n)), left/right, the bank its
//                             $8000-$BFFF reads come from; bit 7 of $201C starts it.
//   $2028-$202A               noise: a 7- or 14-bit LFSR at clock / 256 / (1 + n),
//                             volume, frames to play, left/right, on.
// The pulse waves are drawn at four points per output sample and averaged;
// a DC blocker takes away the offset of the unipolar waves.
#pragma once
#include "sv.h"
#include <vector>

struct SvAudio {
    int rate = 44100;
    float crossfeed = 0.3f;     // 0: the console's hard left/right

    void frame(Sv& c, std::vector<int16_t>& out);   // c's events up to c.cyc, as samples

private:
    struct Pulse { uint8_t reg[4] = {0}; bool on = false; int duty = 0, vol = 0;
                   uint32_t period = 0; double phase = 0; int count = 0; } ch[2];
    struct Noise { uint8_t reg[3] = {0}; bool on = false, play = false, left = false, right = false,
                   wide = false; int vol = 0, count = 0; double period = 256, acc = 0;
                   uint32_t lfsr = 1; int value = 0; } nz;
    struct Dma { uint8_t reg[5] = {0}; bool on = false, left = false, right = false;
                 uint16_t start = 0; uint32_t size = 0, bank = 0; double pos = 0, period = 256; } dma;
    uint64_t done = 0;          // the cycle rendered up to
    double frac = 0;            // samples owed
    float dc_x[2] = {0, 0}, dc_y[2] = {0, 0};

    void write(uint8_t r, uint8_t v);
    void decrement();
    void render(Sv& c, uint64_t to, std::vector<int16_t>& out);
    uint8_t dma_byte(Sv& c, uint16_t a);
};

// a WAV writer for the headless runs
void sv_write_wav(const char* path, const std::vector<int16_t>& s, int rate);
