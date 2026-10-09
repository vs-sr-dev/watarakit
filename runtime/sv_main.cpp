// The recompiled game's executable.
//
//   sv ROM.sv [--frames N] [--snap F1,F2,..] [--out DIR]   headless, the monkey pad
//   sv ROM.sv --interp ...        the interpreter alone (the reference)
//   sv ROM.sv --lockstep ...      native and interpreter side by side: the
//                                 machine state compared at every frame
//   sv ROM.sv --window [--scale 4]   in a window, with the keyboard and sound (SDL2)
//   sv ROM.sv --wav FILE ...      headless, the sound written to a WAV
//
// Keys: arrows, Z = A, X = B, Shift = Select, Enter = Start, Esc quits.
#include "sv.h"
#include "sv_audio.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifdef SV_SDL
#include <SDL2/SDL.h>
#endif

// ---- the monkey: the same pad as tracer/svtrace.c ----
static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static uint8_t monkey(int f) {
    static uint8_t held = 0;
    if (f < 180) return 0xFF;
    int ph = (f - 180) % 300;
    if (ph < 6) return (uint8_t)~0x80;
    if (ph >= 40 && ph < 46) return (uint8_t)~0x20;
    if (ph >= 80 && ph < 86) return (uint8_t)~0x80;
    if (ph % 15 == 0) {
        static const uint8_t dirs[] = { 0, 1, 2, 4, 8, 1 | 8, 2 | 8, 1 | 4, 2 | 4 };
        held = dirs[rnd() % 9];
        if (rnd() % 100 < 40) held |= 0x20;
        if (rnd() % 100 < 25) held |= 0x10;
    }
    return (uint8_t)~held;
}

struct Opts {
    std::string rom, out = ".", wav;
    long frames = 3600;
    bool interp = false, lockstep = false, window = false, quiet = false;
    int scale = 4;
    std::vector<long> snaps;
};
static Opts O;
static Sv* g_ref = nullptr;      // the interpreter, in lockstep
static SvAudio g_audio;
static std::vector<int16_t> g_samples;
static long g_mismatch = -1;

static void write_pgm(const Sv& c, const std::string& path) {
    static const uint8_t pal[4] = { 0xE0, 0xB9, 0x54, 0x12 };
    uint8_t pix[160 * 160];
    sv_render(c, pix);
    for (auto& p : pix) p = pal[p];
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P5\n160 160\n255\n");
    std::fwrite(pix, 1, sizeof pix, f);
    std::fclose(f);
}

static void diff(const Sv& a, const Sv& b) {
    std::fprintf(stderr, "  interp: A=%02X X=%02X Y=%02X S=%02X P=%02X bank=%u cyc=%llu\n",
                 a.a, a.x, a.y, a.s, sv_p(a), a.bank, (unsigned long long)a.cyc);
    std::fprintf(stderr, "  native: A=%02X X=%02X Y=%02X S=%02X P=%02X bank=%u cyc=%llu\n",
                 b.a, b.x, b.y, b.s, sv_p(b), b.bank, (unsigned long long)b.cyc);
    int n = 0;
    for (int i = 0; i < 0x2000 && n < 12; i++)
        if (a.ram[i] != b.ram[i]) { std::fprintf(stderr, "  ram[%04X] %02X / %02X\n", i, a.ram[i], b.ram[i]); n++; }
    for (int i = 0; i < 0x2000 && n < 16; i++)
        if (a.vram[i] != b.vram[i]) { std::fprintf(stderr, "  vram[%04X] %02X / %02X\n", i, a.vram[i], b.vram[i]); n++; }
    for (int i = 0; i < 0x40; i++)
        if (a.reg[i] != b.reg[i]) std::fprintf(stderr, "  reg[%02X] %02X / %02X\n", i, a.reg[i], b.reg[i]);
}

static void frame_common(Sv& c) {
    long f = (long)c.frame;
    for (long s : O.snaps)
        if (s == f) write_pgm(c, O.out + "/f" + std::to_string(f) + ".pgm");
    if (f >= O.frames) throw SvStop();
}

static void hook_headless(Sv& c) {
    long f = (long)c.frame;
    if (g_ref) {
        // the reference runs to the same frame event, then the two are compared
        g_ref->joy = c.joy;
        while ((long)g_ref->frame < f)
            if (!sv_flat_frame(*g_ref)) break;
        if (sv_hash(*g_ref) != sv_hash(c)) {
            std::fprintf(stderr, "lockstep: the machines differ at frame %ld\n", f);
            diff(*g_ref, c);
            g_mismatch = f;
            throw SvStop();
        }
    }
    if (c.snd_log) g_audio.frame(c, g_samples);
    c.joy = monkey((int)f);
    if (g_ref) g_ref->joy = c.joy;
    frame_common(c);
}

#ifdef SV_SDL
static SDL_Window* win; static SDL_Renderer* ren; static SDL_Texture* tex;
static SDL_AudioDeviceID adev;
static std::chrono::steady_clock::time_point t_next;
static void hook_window(Sv& c) {
    static const uint32_t pal[4] = { 0xFFE0E0E0, 0xFFB9B9B9, 0xFF545454, 0xFF121212 };
    uint8_t pix[160 * 160]; uint32_t rgba[160 * 160];
    sv_render(c, pix);
    for (int i = 0; i < 160 * 160; i++) rgba[i] = pal[pix[i]];
    SDL_UpdateTexture(tex, nullptr, rgba, 160 * 4);
    SDL_RenderClear(ren); SDL_RenderCopy(ren, tex, nullptr, nullptr); SDL_RenderPresent(ren);
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)) throw SvStop();
    const uint8_t* k = SDL_GetKeyboardState(nullptr);
    uint8_t j = 0;
    if (k[SDL_SCANCODE_RIGHT]) j |= 1;
    if (k[SDL_SCANCODE_LEFT]) j |= 2;
    if (k[SDL_SCANCODE_DOWN]) j |= 4;
    if (k[SDL_SCANCODE_UP]) j |= 8;
    if (k[SDL_SCANCODE_X]) j |= 0x10;
    if (k[SDL_SCANCODE_Z]) j |= 0x20;
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) j |= 0x40;
    if (k[SDL_SCANCODE_RETURN]) j |= 0x80;
    c.joy = (uint8_t)~j;
    g_samples.clear();
    g_audio.frame(c, g_samples);
    if (adev) {
        // keep about three frames queued: drop a frame's sound if far ahead
        uint32_t queued = SDL_GetQueuedAudioSize(adev), frame = (uint32_t)g_samples.size() * 2;
        if (queued < frame * 6)
            SDL_QueueAudio(adev, g_samples.data(), frame);
    }
    t_next += std::chrono::microseconds(1000000 / SV_FPS);
    std::this_thread::sleep_until(t_next);
}
#endif

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--frames") O.frames = std::atol(val().c_str());
        else if (a == "--interp") O.interp = true;
        else if (a == "--lockstep") O.lockstep = true;
        else if (a == "--window") O.window = true;
        else if (a == "--quiet") O.quiet = true;
        else if (a == "--scale") O.scale = std::atoi(val().c_str());
        else if (a == "--out") O.out = val();
        else if (a == "--wav") O.wav = val();
        else if (a == "--snap") {
            std::string s = val(); size_t p = 0;
            while (p < s.size()) { O.snaps.push_back(std::atol(s.c_str() + p));
                                   p = s.find(',', p); if (p == std::string::npos) break; p++; }
        } else O.rom = a;
    }
    if (O.rom.empty()) { std::fprintf(stderr, "sv ROM.sv [--frames N] [--interp|--lockstep|--window]\n"); return 1; }
    FILE* f = std::fopen(O.rom.c_str(), "rb");
    if (!f) { std::perror(O.rom.c_str()); return 1; }
    std::vector<uint8_t> rom;
    { uint8_t buf[65536]; size_t n; while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) rom.insert(rom.end(), buf, buf + n); }
    std::fclose(f);

    sv_register_all();
    static Sv c, ref;
    sv_load(c, rom.data(), (uint32_t)rom.size());
    sv_reset(c);
    c.joy = monkey(0);
    c.natives = !O.interp;
    c.frame_hook = hook_headless;
    c.snd_log = !O.wav.empty();
    if (O.lockstep) {
        sv_load(ref, rom.data(), (uint32_t)rom.size());
        sv_reset(ref);
        ref.joy = c.joy;
        ref.natives = false;
        ref.flat_stop = true;
        g_ref = &ref;
    }
#ifdef SV_SDL
    if (O.window) {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
        SDL_AudioSpec want{}, have{};
        want.freq = g_audio.rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
        adev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (adev) SDL_PauseAudioDevice(adev, 0);
        c.snd_log = true;
        win = SDL_CreateWindow("watarakit", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               160 * O.scale, 160 * O.scale, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 160, 160);
        c.frame_hook = hook_window;
        c.joy = 0xFF;
        t_next = std::chrono::steady_clock::now();
    }
#endif
    auto t0 = std::chrono::steady_clock::now();
    try {
        if (O.interp) {
            c.flat_stop = false;
            for (;;) if (!sv_flat_frame(c)) break;
        } else sv_run(c);
    } catch (SvStop&) {}
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    write_pgm(c, O.out + "/last.pgm");
    if (!O.wav.empty()) sv_write_wav(O.wav.c_str(), g_samples, g_audio.rate);
    if (!O.quiet) {
        uint64_t total = c.cyc ? c.cyc : 1;
        std::printf("frames %llu  %.2fs (%.0fx)  hash %016llx\n", (unsigned long long)c.frame, secs,
                    c.frame / (double)SV_FPS / (secs > 0 ? secs : 1e-9),
                    (unsigned long long)sv_hash(c));
        std::printf("interpreted insns %llu  native calls %llu  fallbacks %llu  unwinds %llu  "
                    "max depth %d  nmi %llu irq %llu  cycles %llu\n",
                    (unsigned long long)c.n_interp, (unsigned long long)c.n_native_calls,
                    (unsigned long long)c.n_fallback, (unsigned long long)c.n_unwind, c.max_depth,
                    (unsigned long long)c.n_nmi, (unsigned long long)c.n_irq, (unsigned long long)total);
        if (O.lockstep)
            std::printf(g_mismatch < 0 ? "lockstep: identical for %llu frames\n"
                                       : "lockstep: MISMATCH at frame %llu\n",
                        (unsigned long long)(g_mismatch < 0 ? c.frame : g_mismatch));
    }
    return g_mismatch >= 0 ? 2 : 0;
}
