// The machine under the recompiled code: the I/O registers, time and
// interrupts, the interpreter, and the door between native and interpreted
// code.  Hardware after MAME's svision driver (see tracer/svtrace.c).
#include "sv.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

const uint8_t SV_CYC[256] = {
7,6,2,1,5,3,5,5,3,2,2,1,6,4,6,5, 2,5,5,1,5,4,6,5,2,4,2,1,6,4,6,5,
6,6,2,1,3,3,5,5,4,2,2,1,4,4,6,5, 2,5,5,1,4,4,6,5,2,4,2,1,4,4,6,5,
6,6,2,1,3,3,5,5,3,2,2,1,3,4,6,5, 2,5,5,1,4,4,6,5,2,4,3,1,8,4,6,5,
6,6,2,1,3,3,5,5,4,2,2,1,6,4,6,5, 2,5,5,1,4,4,6,5,2,4,4,1,6,4,6,5,
3,6,2,1,3,3,3,5,2,2,2,1,4,4,4,5, 2,6,5,1,4,4,4,5,2,5,2,1,4,5,5,5,
2,6,2,1,3,3,3,5,2,2,2,1,4,4,4,5, 2,5,5,1,4,4,4,5,2,4,2,1,4,4,4,5,
2,6,2,1,3,3,5,5,2,2,2,3,4,4,6,5, 2,5,5,1,4,4,6,5,2,4,3,3,4,4,7,5,
2,6,2,1,3,3,5,5,2,2,2,1,4,4,6,5, 2,5,5,1,4,4,6,5,2,4,4,1,4,4,7,5 };

// ---- native functions, by (slot, address) ----
static std::vector<SvFn> g_fixed(0x4000, nullptr);
static std::vector<std::vector<SvFn>> g_win(8, std::vector<SvFn>(0x4000, nullptr));

void sv_register(int slot, uint16_t addr, SvFn fn) {
    if (slot < 0) g_fixed[addr & 0x3FFF] = fn;
    else g_win[slot & 7][addr & 0x3FFF] = fn;
}

SvFn sv_lookup(Sv& c, uint16_t pc) {
    if (!c.natives) return nullptr;
    if (pc >= 0xC000) return g_fixed[pc - 0xC000];
    if (pc >= 0x8000) return g_win[c.bank][pc - 0x8000];
    return nullptr;
}

// ---- I/O ----
static bool irq_line(const Sv& c) {
    return (c.timer_shot && (c.reg[0x26] & 2)) || (c.dma_finished && (c.reg[0x26] & 4));
}

void sv_recalc(Sv& c) {
    uint64_t n = c.frame_end;
    if (c.timer_on && c.timer_at < n) n = c.timer_at;
    if (c.dma_on && c.dma_at < n) n = c.dma_at;
    if (c.nmi_pending || (irq_line(c) && !c.fi)) n = 0;
    c.next = n;
}

static inline void snd_log(Sv& c, uint8_t r, uint8_t v) {
    if (c.snd_log && c.snd_n < SV_SND_EVENTS) c.snd_ev[c.snd_n++] = { c.cyc, r, v };
}

uint8_t sv_io_r(Sv& c, uint16_t a) {
    uint16_t r = a & 0x1FFF;
    uint8_t d = c.reg[r];
    switch (r) {
    case 0x20: return c.joy;
    case 0x21: return (d & ~0xF) | (c.reg[0x22] & 0xF);
    case 0x27: d &= ~3; if (c.timer_shot) d |= 1; if (c.dma_finished) d |= 2; return d;
    case 0x24: c.timer_shot = 0; break;
    case 0x25: c.dma_finished = 0; break;
    }
    return d;
}

void sv_io_w(Sv& c, uint16_t a, uint8_t v) {
    uint16_t r = a & 0x1FFF;
    c.reg[r] = v;
    if ((r >= 0x10 && r <= 0x1C) || (r >= 0x28 && r <= 0x2A)) snd_log(c, (uint8_t)r, v);
    switch (r) {
    case 0x26:
        c.bank = ((v & 0xE0) >> 5) % c.nbanks;
        c.win = c.rom + c.bank * 0x4000;
        c.next = 0;                    // the IRQ mask may have opened
        break;
    case 0x23: {
        uint32_t d = v ? v : 0x100;
        d *= (c.reg[0x26] & 0x10) ? 0x4000 : 0x100;
        c.timer_on = 1; c.timer_at = c.cyc + d;
        sv_recalc(c);
        break; }
    case 0x1C:
        if (v & 0x80) {
            uint32_t size = (c.reg[0x1A] ? c.reg[0x1A] : 0x100) * 32;
            c.dma_on = 1; c.dma_at = c.cyc + (uint64_t)size * 256 * (1 + (c.reg[0x1B] & 3));
        } else c.dma_on = 0;
        sv_recalc(c);
        break;
    }
}

// ---- events: the timer, the audio DMA, the frame ----
// Returns false when a flat interpreter is to stop here (the frame event).
static bool events(Sv& c) {
    bool go = true;
    if (c.timer_on && c.cyc >= c.timer_at) { c.timer_on = 0; c.timer_shot = 1; }
    if (c.dma_on && c.cyc >= c.dma_at) { c.dma_on = 0; c.dma_finished = 1; }
    if (c.cyc >= c.frame_end) {
        c.frame_end += SV_CYC_FRAME;
        c.frame++;
        snd_log(c, 0xFF, 0);
        if (c.reg[0x26] & 1) c.nmi_pending = 1;
        if (c.frame_hook) c.frame_hook(c);    // may throw SvStop
        if (!c.natives && c.flat_stop) go = false;
    }
    sv_recalc(c);
    return go;
}

static uint16_t take_interrupt(Sv& c, uint16_t at) {
    uint16_t vec;
    if (c.nmi_pending) { c.nmi_pending = 0; vec = 0xFFFA; c.n_nmi++; }
    else if (irq_line(c) && !c.fi) { vec = 0xFFFE; c.n_irq++; }
    else return 0;
    sv_push(c, at >> 8); sv_push(c, at & 0xFF); sv_push(c, sv_p(c));
    c.fi = 1; c.fd = 0;
    c.cyc += 7;
    sv_recalc(c);
    return sv_rd16(c, vec);
}

bool sv_poll(Sv& c, uint16_t at) {
    for (;;) {
        events(c);
        uint8_t s0 = c.s;
        uint16_t h = take_interrupt(c, at);
        if (!h) return true;
        sv_call(c, h);
        if (c.resume != at || c.s != s0) { c.n_unwind++; return false; }
    }
}

// ---- the interpreter ----
// One instruction at c.pc.  ST_SAFE: it ended at a safe point (a loop's
// backward transfer, WAI); ST_JSR: a JSR, not run yet -- the caller polls,
// then runs it with do_jsr; ST_RTS / ST_RTI: a return.
enum Step { ST_NEXT, ST_SAFE, ST_JSR, ST_RTS, ST_RTI };

enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABSX, M_ABSY, M_IND,
       M_INDX, M_INDY, M_ZPI, M_AIX, M_REL, M_ZPR };
static const uint8_t MODE[256] = {
// 0      1       2      3      4     5     6     7     8      9       A      B      C      D       E       F
M_IMM, M_INDX, M_IMM, M_IMP, M_ZP, M_ZP, M_ZP, M_ZP, M_IMP, M_IMM,  M_ACC, M_IMP, M_ABS, M_ABS,  M_ABS,  M_ZPR,
M_REL, M_INDY, M_ZPI, M_IMP, M_ZP, M_ZPX,M_ZPX,M_ZP, M_IMP, M_ABSY, M_ACC, M_IMP, M_ABS, M_ABSX, M_ABSX, M_ZPR,
M_ABS, M_INDX, M_IMM, M_IMP, M_ZP, M_ZP, M_ZP, M_ZP, M_IMP, M_IMM,  M_ACC, M_IMP, M_ABS, M_ABS,  M_ABS,  M_ZPR,
M_REL, M_INDY, M_ZPI, M_IMP, M_ZPX,M_ZPX,M_ZPX,M_ZP, M_IMP, M_ABSY, M_ACC, M_IMP, M_ABSX,M_ABSX, M_ABSX, M_ZPR,
M_IMP, M_INDX, M_IMM, M_IMP, M_ZP, M_ZP, M_ZP, M_ZP, M_IMP, M_IMM,  M_ACC, M_IMP, M_ABS, M_ABS,  M_ABS,  M_ZPR,
M_REL, M_INDY, M_ZPI, M_IMP, M_ZPX,M_ZPX,M_ZPX,M_ZP, M_IMP, M_ABSY, M_IMP, M_IMP, M_ABS, M_ABSX, M_ABSX, M_ZPR,
M_IMP, M_INDX, M_IMM, M_IMP, M_ZP, M_ZP, M_ZP, M_ZP, M_IMP, M_IMM,  M_ACC, M_IMP, M_IND, M_ABS,  M_ABS,  M_ZPR,
M_REL, M_INDY, M_ZPI, M_IMP, M_ZPX,M_ZPX,M_ZPX,M_ZP, M_IMP, M_ABSY, M_IMP, M_IMP, M_AIX, M_ABSX, M_ABSX, M_ZPR,
M_REL, M_INDX, M_IMM, M_IMP, M_ZP, M_ZP, M_ZP, M_ZP, M_IMP, M_IMM,  M_IMP, M_IMP, M_ABS, M_ABS,  M_ABS,  M_ZPR,
M_REL, M_INDY, M_ZPI, M_IMP, M_ZPX,M_ZPX,M_ZPY,M_ZP, M_IMP, M_ABSY, M_IMP, M_IMP, M_ABS, M_ABSX, M_ABSX, M_ZPR,
M_IMM, M_INDX, M_IMM, M_IMP, M_ZP, M_ZP, M_ZP, M_ZP, M_IMP, M_IMM,  M_IMP, M_IMP, M_ABS, M_ABS,  M_ABS,  M_ZPR,
M_REL, M_INDY, M_ZPI, M_IMP, M_ZPX,M_ZPX,M_ZPY,M_ZP, M_IMP, M_ABSY, M_IMP, M_IMP, M_ABSX,M_ABSX, M_ABSY, M_ZPR,
M_IMM, M_INDX, M_IMM, M_IMP, M_ZP, M_ZP, M_ZP, M_ZP, M_IMP, M_IMM,  M_IMP, M_IMP, M_ABS, M_ABS,  M_ABS,  M_ZPR,
M_REL, M_INDY, M_ZPI, M_IMP, M_ZPX,M_ZPX,M_ZPX,M_ZP, M_IMP, M_ABSY, M_IMP, M_IMP, M_ABS, M_ABSX, M_ABSX, M_ZPR,
M_IMM, M_INDX, M_IMM, M_IMP, M_ZP, M_ZP, M_ZP, M_ZP, M_IMP, M_IMM,  M_IMP, M_IMP, M_ABS, M_ABS,  M_ABS,  M_ZPR,
M_REL, M_INDY, M_ZPI, M_IMP, M_ZPX,M_ZPX,M_ZPX,M_ZP, M_IMP, M_ABSY, M_IMP, M_IMP, M_ABS, M_ABSX, M_ABSX, M_ZPR,
};

static Step step(Sv& c) {
    const uint16_t pc0 = c.pc;
    const uint8_t op = sv_rd(c, pc0);
    const int mode = MODE[op];
    uint16_t pc = pc0 + 1, ea = 0;
    uint8_t zpa = 0, v;
    switch (mode) {
    case M_IMM: ea = pc++; break;
    case M_ZP: ea = sv_rd(c, pc++); break;
    case M_ZPX: ea = (uint8_t)(sv_rd(c, pc++) + c.x); break;
    case M_ZPY: ea = (uint8_t)(sv_rd(c, pc++) + c.y); break;
    case M_ABS: ea = sv_rd16(c, pc); pc += 2; break;
    case M_ABSX: ea = sv_rd16(c, pc) + c.x; pc += 2; break;
    case M_ABSY: ea = sv_rd16(c, pc) + c.y; pc += 2; break;
    case M_IND: { uint16_t p = sv_rd16(c, pc); pc += 2; ea = sv_rd16(c, p); break; }
    case M_INDX: ea = sv_zpw(c, (uint8_t)(sv_rd(c, pc++) + c.x)); break;
    case M_INDY: ea = sv_zpw(c, sv_rd(c, pc++)) + c.y; break;
    case M_ZPI: ea = sv_zpw(c, sv_rd(c, pc++)); break;
    case M_AIX: { uint16_t p = sv_rd16(c, pc) + c.x; pc += 2; ea = sv_rd16(c, p); break; }
    case M_REL: { int8_t d = (int8_t)sv_rd(c, pc++); ea = pc + d; break; }
    case M_ZPR: { zpa = sv_rd(c, pc); int8_t d = (int8_t)sv_rd(c, pc + 1); pc += 2; ea = pc + d; break; }
    }
    if (op == 0x20) {          // JSR: the caller polls first, then runs it
        return ST_JSR;
    }
    c.cyc += SV_CYC[op];
    c.n_interp++;
    Step st = ST_NEXT;
    auto br = [&](bool cond) {
        if (cond) { c.cyc++; pc = ea; if (ea <= pc0) st = ST_SAFE; }
    };
#define RMW(expr) do { if (mode == M_ACC) { v = c.a; c.a = expr; } \
                       else { v = sv_rd(c, ea); sv_wr(c, ea, expr); } } while (0)
    switch (op) {
    // loads and stores
    case 0xA9: case 0xA5: case 0xB5: case 0xAD: case 0xBD: case 0xB9: case 0xA1: case 0xB1: case 0xB2:
        sv_nz(c, c.a = sv_rd(c, ea)); break;
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE: sv_nz(c, c.x = sv_rd(c, ea)); break;
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC: sv_nz(c, c.y = sv_rd(c, ea)); break;
    case 0x85: case 0x95: case 0x8D: case 0x9D: case 0x99: case 0x81: case 0x91: case 0x92:
        sv_wr(c, ea, c.a); break;
    case 0x86: case 0x96: case 0x8E: sv_wr(c, ea, c.x); break;
    case 0x84: case 0x94: case 0x8C: sv_wr(c, ea, c.y); break;
    case 0x64: case 0x74: case 0x9C: case 0x9E: sv_wr(c, ea, 0); break;
    // ALU
    case 0x69: case 0x65: case 0x75: case 0x6D: case 0x7D: case 0x79: case 0x61: case 0x71: case 0x72:
        sv_adc(c, sv_rd(c, ea)); break;
    case 0xE9: case 0xE5: case 0xF5: case 0xED: case 0xFD: case 0xF9: case 0xE1: case 0xF1: case 0xF2:
        sv_sbc(c, sv_rd(c, ea)); break;
    case 0x29: case 0x25: case 0x35: case 0x2D: case 0x3D: case 0x39: case 0x21: case 0x31: case 0x32:
        sv_nz(c, c.a &= sv_rd(c, ea)); break;
    case 0x09: case 0x05: case 0x15: case 0x0D: case 0x1D: case 0x19: case 0x01: case 0x11: case 0x12:
        sv_nz(c, c.a |= sv_rd(c, ea)); break;
    case 0x49: case 0x45: case 0x55: case 0x4D: case 0x5D: case 0x59: case 0x41: case 0x51: case 0x52:
        sv_nz(c, c.a ^= sv_rd(c, ea)); break;
    case 0xC9: case 0xC5: case 0xD5: case 0xCD: case 0xDD: case 0xD9: case 0xC1: case 0xD1: case 0xD2:
        sv_cmp(c, c.a, sv_rd(c, ea)); break;
    case 0xE0: case 0xE4: case 0xEC: sv_cmp(c, c.x, sv_rd(c, ea)); break;
    case 0xC0: case 0xC4: case 0xCC: sv_cmp(c, c.y, sv_rd(c, ea)); break;
    case 0x89: c.fz = (c.a & sv_rd(c, ea)) == 0; break;
    case 0x24: case 0x34: case 0x2C: case 0x3C: sv_bit(c, sv_rd(c, ea)); break;
    // read-modify-write
    case 0x0A: case 0x06: case 0x16: case 0x0E: case 0x1E: RMW(sv_asl(c, v)); break;
    case 0x4A: case 0x46: case 0x56: case 0x4E: case 0x5E: RMW(sv_lsr(c, v)); break;
    case 0x2A: case 0x26: case 0x36: case 0x2E: case 0x3E: RMW(sv_rol(c, v)); break;
    case 0x6A: case 0x66: case 0x76: case 0x6E: case 0x7E: RMW(sv_ror(c, v)); break;
    case 0x1A: case 0xE6: case 0xF6: case 0xEE: case 0xFE: RMW(sv_nz(c, (uint8_t)(v + 1))); break;
    case 0x3A: case 0xC6: case 0xD6: case 0xCE: case 0xDE: RMW(sv_nz(c, (uint8_t)(v - 1))); break;
    case 0x04: case 0x0C: v = sv_rd(c, ea); sv_wr(c, ea, sv_tsb(c, v)); break;
    case 0x14: case 0x1C: v = sv_rd(c, ea); sv_wr(c, ea, sv_trb(c, v)); break;
    // branches
    case 0x10: br(!c.fn); break;
    case 0x30: br(c.fn); break;
    case 0x50: br(!c.fv); break;
    case 0x70: br(c.fv); break;
    case 0x90: br(!c.fc); break;
    case 0xB0: br(c.fc); break;
    case 0xD0: br(!c.fz); break;
    case 0xF0: br(c.fz); break;
    case 0x80: pc = ea; if (ea <= pc0) st = ST_SAFE; break;
    case 0x4C: case 0x6C: case 0x7C: pc = ea; if (ea <= pc0) st = ST_SAFE; break;
    case 0x60: {
        c.rti = 0;
        uint8_t lo = sv_pull(c), hi = sv_pull(c);
        pc = (uint16_t)((lo | hi << 8) + 1); st = ST_RTS; break; }
    case 0x40:
        c.rti = 1;
        sv_setp(c, sv_pull(c)); { uint8_t lo = sv_pull(c), hi = sv_pull(c); pc = lo | hi << 8; }
        sv_recalc(c);
        st = ST_RTI; break;
    case 0x00: {                // BRK: the IRQ vector, B set (SSSnake walks into one)
        uint16_t ret = pc0 + 2;
        sv_push(c, ret >> 8); sv_push(c, ret & 0xFF); sv_push(c, sv_p(c) | 0x10);
        c.fi = 1; c.fd = 0;
        pc = sv_rd16(c, 0xFFFE);
        break; }
    // stack
    case 0x48: sv_push(c, c.a); break;
    case 0xDA: sv_push(c, c.x); break;
    case 0x5A: sv_push(c, c.y); break;
    case 0x08: sv_push(c, sv_p(c) | 0x10); break;
    case 0x68: sv_nz(c, c.a = sv_pull(c)); break;
    case 0xFA: sv_nz(c, c.x = sv_pull(c)); break;
    case 0x7A: sv_nz(c, c.y = sv_pull(c)); break;
    case 0x28: sv_setp(c, sv_pull(c)); sv_recalc(c); break;
    // transfers and flags
    case 0xAA: sv_nz(c, c.x = c.a); break;
    case 0xA8: sv_nz(c, c.y = c.a); break;
    case 0x8A: sv_nz(c, c.a = c.x); break;
    case 0x98: sv_nz(c, c.a = c.y); break;
    case 0xBA: sv_nz(c, c.x = c.s); break;
    case 0x9A: c.s = c.x; break;
    case 0xE8: sv_nz(c, ++c.x); break;
    case 0xC8: sv_nz(c, ++c.y); break;
    case 0xCA: sv_nz(c, --c.x); break;
    case 0x88: sv_nz(c, --c.y); break;
    case 0x18: c.fc = 0; break;
    case 0x38: c.fc = 1; break;
    case 0x58: c.fi = 0; sv_recalc(c); break;
    case 0x78: c.fi = 1; break;
    case 0xD8: c.fd = 0; break;
    case 0xF8: c.fd = 1; break;
    case 0xB8: c.fv = 0; break;
    case 0xCB: c.pc = pc; sv_wai(c); st = ST_SAFE; break;
    case 0xDB: c.stopped = 1; break;
    default:
        if ((op & 0x0F) == 0x07) {          // RMBn / SMBn
            int b = (op >> 4) & 7; v = sv_rd(c, ea);
            sv_wr(c, ea, (op & 0x80) ? (v | 1 << b) : (v & ~(1 << b)));
        } else if ((op & 0x0F) == 0x0F) {   // BBRn / BBSn
            int b = (op >> 4) & 7; v = sv_rd(c, zpa);
            br((op & 0x80) ? (v >> b & 1) : !(v >> b & 1));
        }
        break;                              // the undefined NOPs
    }
    c.pc = pc;
    return st;
}

static void do_jsr(Sv& c) {
    uint16_t target = sv_rd16(c, c.pc + 1), ret = c.pc + 2;
    c.cyc += SV_CYC[0x20];
    c.n_interp++;
    sv_push(c, ret >> 8); sv_push(c, ret & 0xFF);
    c.pc = target;
}

// The nested interpreter: run from pc until an RTS or RTI takes S above
// `base` (the stack as the caller left it, its return address pushed).
// Native code takes over at every native function entry.
void sv_exec(Sv& c, uint16_t pc, int base) {
    c.pc = pc;
    c.n_fallback++;
    for (;;) {
        if (c.stopped) throw SvStop();
        if (SvFn fn = sv_lookup(c, c.pc)) {
            c.n_native_calls++;
            fn(c);
            if ((int)c.s > base) return;
            c.pc = c.resume;
            // a native RTI back into this level is a safe point, as the interpreter's is
            if (c.rti && c.cyc >= c.next && !sv_poll(c, c.pc)) {
                if ((int)c.s > base) return;
                c.pc = c.resume;
            }
            continue;
        }
        uint16_t at = c.pc;
        Step st = step(c);
        if (st == ST_JSR) {
            if (c.cyc >= c.next && !sv_poll(c, at)) {
                if ((int)c.s > base) return;
                c.pc = c.resume; continue;
            }
            c.pc = at;
            do_jsr(c);
            continue;
        }
        if (st == ST_RTS || st == ST_RTI) {
            c.resume = c.pc;
            if ((int)c.s > base) return;
            if (st == ST_RTS) continue;
            // an RTI that stays inside this level: a safe point
        }
        if (st == ST_SAFE || st == ST_RTI) {
            uint16_t to = c.pc;
            if (c.cyc >= c.next && !sv_poll(c, to)) {
                if ((int)c.s > base) return;
                c.pc = c.resume;
            }
        }
    }
}

void sv_call(Sv& c, uint16_t target) {
    if (++c.depth > c.max_depth) c.max_depth = c.depth;
    if (c.depth > 4000) { std::fprintf(stderr, "sv: call depth runaway at %04X\n", target); std::exit(3); }
    if (SvFn fn = sv_lookup(c, target)) { c.n_native_calls++; fn(c); }
    else sv_exec(c, target, c.s);
    c.depth--;
}

void sv_wai(Sv& c) {
    // sleep to the next event (the poll that follows takes the interrupt)
    while (c.cyc < c.next) c.cyc = c.next;
}

// ---- the flat interpreter: the reference, no native code, no nesting ----
// Interrupts are taken by pointing pc at the vector, at the same safe points.
static bool flat_poll(Sv& c, uint16_t at) {
    if (!events(c)) return false;          // the frame: stop before delivering
    uint16_t h = take_interrupt(c, at);
    if (h) c.pc = h;
    return true;
}

bool sv_flat_frame(Sv& c) {
    uint64_t f0 = c.frame;
    // what was pending when the last frame stopped is taken first
    if (c.cyc >= c.next) {
        uint16_t h = take_interrupt(c, c.pc);
        if (h) c.pc = h;
    }
    for (;;) {
        if (c.stopped) return false;
        uint16_t at = c.pc;
        Step st = step(c);
        if (st == ST_JSR) {
            if (c.cyc >= c.next) {
                if (!flat_poll(c, at)) { c.pc = at; return true; }
                if (c.pc != at) continue;          // went to a handler
            }
            c.pc = at;
            do_jsr(c);
            continue;
        }
        if (st == ST_SAFE || st == ST_RTI) {
            uint16_t to = c.pc;
            if (c.cyc >= c.next && !flat_poll(c, to)) return true;
        }
        (void)f0;
    }
}

// ---- the machine ----
void sv_load(Sv& c, const uint8_t* rom, uint32_t size) {
    c.rom = rom; c.romsize = size; c.nbanks = size / 0x4000;
    c.fixed = rom + (c.nbanks - 1) * 0x4000;
    c.bank = 0; c.win = rom;
}

void sv_reset(Sv& c) {
    c.pc = sv_rd16(c, 0xFFFC);
    c.s = 0xFF; c.fi = 1; c.fd = 0;
    sv_recalc(c);
}

void sv_run(Sv& c) {
    uint16_t entry = c.pc;
    sv_call(c, entry);
    for (;;) sv_exec(c, c.resume, 0x1FF);  // unwound to the top: interpret on
}

void sv_render(const Sv& c, uint8_t* pix) {
    std::memset(pix, 0, 160 * 160);
    if (!(c.reg[0x26] & 8)) return;
    int j = c.reg[2] / 4 + c.reg[3] * 0x30;
    for (int y = 0; y < 160; y++) {
        uint8_t line[168] = {0};
        int sx = 3 - (c.reg[2] & 3), ex = (c.reg[0] | 3) < 163 ? (c.reg[0] | 3) : 163;
        for (int x = sx, i = 0; x < ex; x += 4, i++) {
            uint8_t b = c.vram[(j + i) & 0x1FFF];
            for (int k = 0; k < 4; k++) { line[x + k] = b & 3; b >>= 2; }
        }
        std::memcpy(pix + y * 160, line + 3, 160);
        j += 0x30; if (j >= 0x1FE0) j = 0;
    }
}

static uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t sv_hash(const Sv& c) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, c.ram, sizeof c.ram);
    h = fnv(h, c.vram, sizeof c.vram);
    h = fnv(h, c.reg, 0x40);
    uint8_t cpu[12] = { c.a, c.x, c.y, c.s, sv_p(c), (uint8_t)c.bank, c.timer_on, c.timer_shot,
                        c.dma_on, c.dma_finished, c.nmi_pending, 0 };
    h = fnv(h, cpu, sizeof cpu);
    h = fnv(h, &c.cyc, sizeof c.cyc);
    return h;
}
