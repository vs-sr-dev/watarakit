// The Supervision's sound (see sv_audio.h).
#include "sv_audio.h"
#include <cmath>
#include <cstdio>

static const int DUTY[4] = { 28, 24, 16, 9 };   // of 32: the time a wave is high

void SvAudio::write(uint8_t r, uint8_t v) {
    if (r >= 0x10 && r <= 0x17) {
        Pulse& p = ch[(r - 0x10) >> 2];
        int o = (r - 0x10) & 3;
        p.reg[o] = v;
        switch (o) {
        case 0: case 1: {
            uint32_t size = p.reg[0] | (p.reg[1] & 7) << 8;
            p.period = size * 32; p.phase = 0; break; }
        case 2: p.on = v & 0x40; p.duty = (v >> 4) & 3; p.vol = v & 0xF; break;
        case 3: p.count = v + 1; break;
        }
    } else if (r >= 0x18 && r <= 0x1C) {
        int o = r - 0x18;
        dma.reg[o] = v;
        switch (o) {
        case 0: case 1: dma.start = dma.reg[0] | dma.reg[1] << 8; break;
        case 2: dma.size = (v ? v : 0x100) * 32; break;
        case 3: dma.period = 256.0 * (1 + (v & 3)); dma.right = v & 4; dma.left = v & 8;
                dma.bank = (v >> 4) & 7; break;
        case 4: dma.on = v & 0x80; if (dma.on) dma.pos = 0; break;
        }
    } else if (r >= 0x28 && r <= 0x2A) {
        int o = r - 0x28;
        nz.reg[o] = v;
        switch (o) {
        case 0: nz.vol = v & 0xF; nz.period = 256.0 * (1 + (v >> 4)); break;
        case 1: nz.count = v + 1; break;
        case 2: nz.wide = v & 1; nz.play = v & 2; nz.right = v & 4; nz.left = v & 8;
                nz.on = v & 0x10; nz.lfsr = 1; break;
        }
        nz.acc = 0;
    }
}

void SvAudio::decrement() {
    for (auto& p : ch) if (p.count > 0) p.count--;
    if (nz.count > 0) nz.count--;
}

uint8_t SvAudio::dma_byte(Sv& c, uint16_t a) {
    if (a >= 0x8000 && a < 0xC000)
        return c.rom[(dma.bank * 0x4000u) % c.romsize + (a & 0x3FFF)];
    if (a >= 0xC000) return c.fixed[a & 0x3FFF];
    if (a < 0x2000) return c.ram[a];
    if (a >= 0x4000 && a < 0x6000) return c.vram[a & 0x1FFF];
    return 0;
}

void SvAudio::render(Sv& c, uint64_t to, std::vector<int16_t>& out) {
    if (to <= done) return;
    frac += (double)(to - done) * rate / SV_CLOCK;
    done = to;
    const int SUB = 4;
    const double dt = (double)SV_CLOCK / rate / SUB;
    while (frac >= 1.0) {
        frac -= 1.0;
        float L = 0, R = 0;
        for (int k = 0; k < SUB; k++) {
            for (int i = 0; i < 2; i++) {
                Pulse& p = ch[i];
                if (!p.period) continue;
                if ((p.on || p.count) && p.phase < p.period * DUTY[p.duty] / 32.0)
                    (i == 0 ? R : L) += p.vol * 256.0f;
                p.phase += dt;
                while (p.phase >= p.period) p.phase -= p.period;
            }
            if (nz.on && (nz.play || nz.count)) {
                float s = nz.value ? nz.vol * 256.0f : 0;
                if (nz.left) L += s;
                if (nz.right) R += s;
                nz.acc += dt;
                while (nz.acc >= nz.period) {
                    nz.acc -= nz.period;
                    uint32_t top = nz.wide ? 0x2000 : 0x40, tap = top >> 1;
                    nz.value = (nz.lfsr & top) != 0;
                    uint32_t fb = ((nz.lfsr & top) != 0) != ((nz.lfsr & tap) != 0);
                    nz.lfsr = ((nz.lfsr << 1) | fb) & (top * 2 - 1);
                }
            }
            if (dma.on) {
                uint32_t n = (uint32_t)dma.pos;
                uint8_t b = dma_byte(c, (uint16_t)(dma.start + n / 2));
                float s = ((n & 1) ? (b & 0xF) : (b >> 4)) * 256.0f;
                if (dma.left) L += s;
                if (dma.right) R += s;
                dma.pos += dt / dma.period;
                if (dma.pos >= dma.size) dma.on = false;
            }
        }
        L /= SUB; R /= SUB;
        float mix[2] = { L + crossfeed * R, R + crossfeed * L };
        for (int j = 0; j < 2; j++) {
            float y = mix[j] - dc_x[j] + 0.9995f * dc_y[j];
            dc_x[j] = mix[j]; dc_y[j] = y;
            float v = y * 1.6f;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            out.push_back((int16_t)std::lrint(v));
        }
    }
}

void SvAudio::frame(Sv& c, std::vector<int16_t>& out) {
    for (uint32_t i = 0; i < c.snd_n; i++) {
        const SvSndEv& e = c.snd_ev[i];
        render(c, e.cyc, out);
        if (e.reg == 0xFF) decrement();
        else write(e.reg, e.val);
    }
    c.snd_n = 0;
    render(c, c.cyc, out);
}

void sv_write_wav(const char* path, const std::vector<int16_t>& s, int rate) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    uint32_t bytes = (uint32_t)(s.size() * 2);
    std::fwrite("RIFF", 1, 4, f); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(bytes);
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
}
