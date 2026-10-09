// The Supervision under recompiled code: the machine, the W65C02's state,
// and the helpers the generated C++ is written in.
//
// One rule holds the recompiled code and the interpreter together: both
// charge the same cycles per instruction (SV_CYC, +1 for a taken branch)
// and both take interrupts only at the same safe points --
//
//   * before a JSR, with the JSR's own address pushed;
//   * after a control transfer to an address at or below its own (a loop),
//     with the target pushed;
//   * after an RTI and after a WAI, with the next instruction pushed.
//
// So the machine state at every frame is the same whichever way the code
// ran: the lockstep self-test (sv_main --lockstep) compares it.
#pragma once
#include <cstdint>
#include <cstring>

struct Sv;
typedef void (*SvFn)(Sv&);

// A write to a sound register, stamped with its cycle, for the synthesizer
// (sv_audio): sound is output only, so it never changes what the CPU sees.
// reg 0xFF marks the frame (the LCD's VBlank counts the notes' durations down).
struct SvSndEv { uint64_t cyc; uint8_t reg, val; };
enum { SV_SND_EVENTS = 8192 };

enum { SV_CLOCK = 4000000, SV_FPS = 61, SV_CYC_FRAME = SV_CLOCK / SV_FPS };

struct Sv {
    // the CPU: each flag a byte of 0 or 1
    uint8_t a = 0, x = 0, y = 0, s = 0xFF;
    uint8_t fn = 0, fv = 0, fd = 0, fi = 1, fz = 0, fc = 0;
    uint16_t pc = 0;          // the interpreter's
    uint16_t resume = 0;      // where the last RTS / RTI went
    uint8_t rti = 0;          // ... and whether it was an RTI

    // time
    uint64_t cyc = 0, next = 0, frame_end = SV_CYC_FRAME;
    uint64_t frame = 0;

    // the machine
    uint8_t ram[0x2000], vram[0x2000], reg[0x2000];
    const uint8_t* rom = nullptr; uint32_t romsize = 0, nbanks = 0, bank = 0;
    const uint8_t* win = nullptr;     // the bank at $8000
    const uint8_t* fixed = nullptr;   // the last bank, at $C000
    uint8_t joy = 0xFF;
    uint8_t timer_on = 0, timer_shot = 0, dma_on = 0, dma_finished = 0;
    uint8_t nmi_pending = 0, stopped = 0;
    uint64_t timer_at = 0, dma_at = 0;

    // the sound registers' writes since the synthesizer last took them
    bool snd_log = false;
    uint32_t snd_n = 0;
    SvSndEv snd_ev[SV_SND_EVENTS];

    // how the code runs
    bool natives = true;      // false: the interpreter alone (the reference)
    bool flat_stop = false;   // the flat interpreter stops at the frame event
    void (*frame_hook)(Sv&) = nullptr;
    void* user = nullptr;

    // what ran
    uint64_t n_interp = 0, n_native_calls = 0, n_fallback = 0, n_unwind = 0;
    uint64_t n_nmi = 0, n_irq = 0;
    int depth = 0, max_depth = 0;

    Sv() { std::memset(ram, 0, sizeof ram); std::memset(vram, 0, sizeof vram);
           std::memset(reg, 0, sizeof reg); }
};

extern const uint8_t SV_CYC[256];

// ---- the bus ----
uint8_t sv_io_r(Sv& c, uint16_t a);
void sv_io_w(Sv& c, uint16_t a, uint8_t v);

static inline uint8_t sv_rd(Sv& c, uint16_t a) {
    switch (a >> 13) {
    case 0: return c.ram[a];
    case 1: return sv_io_r(c, a);
    case 2: return c.vram[a & 0x1FFF];
    case 3: return 0;
    case 4: case 5: return c.win[a & 0x3FFF];
    default: return c.fixed[a & 0x3FFF];
    }
}
static inline void sv_wr(Sv& c, uint16_t a, uint8_t v) {
    switch (a >> 13) {
    case 0: c.ram[a] = v; break;
    case 1: sv_io_w(c, a, v); break;
    case 2: c.vram[a & 0x1FFF] = v; break;
    default: break;            // open bus and ROM
    }
}
static inline uint16_t sv_zpw(Sv& c, uint8_t z) {
    return c.ram[z] | c.ram[(uint8_t)(z + 1)] << 8;
}
static inline uint16_t sv_rd16(Sv& c, uint16_t a) {
    return sv_rd(c, a) | sv_rd(c, (uint16_t)(a + 1)) << 8;
}

// ---- the stack ----
static inline void sv_push(Sv& c, uint8_t v) { c.ram[0x100 + c.s] = v; c.s--; }
static inline uint8_t sv_pull(Sv& c) { c.s++; return c.ram[0x100 + c.s]; }

// ---- flags and ALU ----
static inline uint8_t sv_p(const Sv& c) {   // P as pushed by an interrupt (B clear)
    return c.fn << 7 | c.fv << 6 | 0x20 | c.fd << 3 | c.fi << 2 | c.fz << 1 | c.fc;
}
static inline void sv_setp(Sv& c, uint8_t p) {
    c.fn = p >> 7; c.fv = p >> 6 & 1; c.fd = p >> 3 & 1; c.fi = p >> 2 & 1;
    c.fz = p >> 1 & 1; c.fc = p & 1;
}
static inline uint8_t sv_nz(Sv& c, uint8_t v) { c.fn = v >> 7; c.fz = v == 0; return v; }

static inline void sv_adc(Sv& c, uint8_t v) {
    unsigned cin = c.fc;
    if (c.fd) {
        int al = (c.a & 0x0F) + (v & 0x0F) + cin;
        if (al >= 0x0A) al = ((al + 6) & 0x0F) + 0x10;
        int r = (c.a & 0xF0) + (v & 0xF0) + al;
        int sv = (int8_t)(c.a & 0xF0) + (int8_t)(v & 0xF0) + al;
        c.fv = sv < -128 || sv > 127;
        if (r >= 0xA0) r += 0x60;
        c.fc = r >= 0x100;
        sv_nz(c, c.a = (uint8_t)r);
    } else {
        unsigned r = c.a + v + cin;
        c.fv = ((~(c.a ^ v) & (c.a ^ r)) >> 7) & 1;
        c.fc = r > 0xFF;
        sv_nz(c, c.a = (uint8_t)r);
    }
}
static inline void sv_sbc(Sv& c, uint8_t v) {
    unsigned cin = c.fc;
    int bin = c.a - v + cin - 1;
    c.fv = (((c.a ^ v) & (c.a ^ (uint8_t)bin)) >> 7) & 1;
    c.fc = bin >= 0;
    if (c.fd) {
        int al = (c.a & 0x0F) - (v & 0x0F) + cin - 1;
        int r = bin;
        if (r < 0) r -= 0x60;
        if (al < 0) r -= 0x06;
        c.a = (uint8_t)r;
    } else c.a = (uint8_t)bin;
    sv_nz(c, c.a);
}
static inline void sv_cmp(Sv& c, uint8_t r, uint8_t v) {
    c.fc = r >= v; sv_nz(c, (uint8_t)(r - v));
}
static inline void sv_bit(Sv& c, uint8_t v) {
    c.fz = (c.a & v) == 0; c.fn = v >> 7; c.fv = v >> 6 & 1;
}
static inline uint8_t sv_asl(Sv& c, uint8_t v) { c.fc = v >> 7; return sv_nz(c, (uint8_t)(v << 1)); }
static inline uint8_t sv_lsr(Sv& c, uint8_t v) { c.fc = v & 1; return sv_nz(c, v >> 1); }
static inline uint8_t sv_rol(Sv& c, uint8_t v) {
    uint8_t ci = c.fc; c.fc = v >> 7; return sv_nz(c, (uint8_t)(v << 1 | ci));
}
static inline uint8_t sv_ror(Sv& c, uint8_t v) {
    uint8_t ci = c.fc; c.fc = v & 1; return sv_nz(c, (uint8_t)(v >> 1 | ci << 7));
}
static inline uint8_t sv_tsb(Sv& c, uint8_t v) { c.fz = (c.a & v) == 0; return v | c.a; }
static inline uint8_t sv_trb(Sv& c, uint8_t v) { c.fz = (c.a & v) == 0; return v & ~c.a; }

// ---- time and interrupts ----
// SV_POLL is a safe point: `at` is the address an interrupt would return to.
// sv_poll returns false when the interrupt did not come back to `at` with
// the stack as it was: the native code then returns, and the callers unwind.
bool sv_poll(Sv& c, uint16_t at);
void sv_recalc(Sv& c);           // c.next from the timers, the frame and the IRQ line
#define SV_POLL(c, at) do { if ((c).cyc >= (c).next && !sv_poll((c), (at))) return; } while (0)

// ---- calls between native and interpreted code ----
void sv_call(Sv& c, uint16_t target);     // run code at target as a JSR'd routine (dynamic)
SvFn sv_lookup(Sv& c, uint16_t pc);       // the native function at (bank, pc), or null
void sv_exec(Sv& c, uint16_t pc, int base);  // interpret until an RTS/RTI lifts S above base
void sv_wai(Sv& c);
void sv_register(int slot, uint16_t addr, SvFn fn);   // slot: bank, or -1 for the fixed bank
void sv_register_all();          // generated: every native function

// ---- the machine ----
void sv_load(Sv& c, const uint8_t* rom, uint32_t size);
void sv_reset(Sv& c);
void sv_run(Sv& c);              // from the reset vector, forever (frame_hook decides)
bool sv_flat_frame(Sv& c);       // the interpreter alone, to the next frame event
void sv_render(const Sv& c, uint8_t* pix160x160);   // 0..3 per pixel
uint64_t sv_hash(const Sv& c);
struct SvStop {};                // thrown by a frame hook to end the run
