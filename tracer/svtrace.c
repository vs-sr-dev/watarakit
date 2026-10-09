/* svtrace: a Watara Supervision interpreter that measures a game for the
 * static recompiler.
 *
 * It runs a cartridge headless for N frames with a scripted "monkey" pad and
 * records what a recompiler has to know about the code that actually ran:
 *
 *   - every ROM byte executed as an opcode or operand (coverage by bank),
 *     and bytes executed both ways (overlapping instructions);
 *   - indirect jumps -- JMP (abs), JMP (abs,X) -- with their targets;
 *   - RTS that do not return to their JSR (push-and-RTS dispatch), and
 *     return addresses dropped from the stack (PLA/TXS exits);
 *   - code run from RAM or VRAM, and RAM code written after it ran
 *     (self-modifying code);
 *   - bank switches, and which ones happen from inside the banked window;
 *   - where the main program was when the NMI and IRQ arrived;
 *   - every I/O register the game reads and writes.
 *
 * The hardware follows MAME's svision driver (4 MHz W65C02, 61 Hz frame NMI,
 * the $2023 timer, the audio DMA's end-of-transfer IRQ).  The LCD DMA at
 * $2008-$200D, absent from MAME, follows Potator and is only counted.
 *
 *   svtrace ROM.sv OUTDIR [frames] [--snap f1,f2,...]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the opcode table: the same text as watarakit/w65c02.py ---- */
static const char *ROWS =
"00 BRK imm  01 ORA indx 02 NOP* imm 03 NOP* imp 04 TSB zp   05 ORA zp   06 ASL zp   07 RMB0 zp "
"08 PHP imp  09 ORA imm  0A ASL acc  0B NOP* imp 0C TSB abs  0D ORA abs  0E ASL abs  0F BBR0 zpr "
"10 BPL rel  11 ORA indy 12 ORA zpi  13 NOP* imp 14 TRB zp   15 ORA zpx  16 ASL zpx  17 RMB1 zp "
"18 CLC imp  19 ORA absy 1A INC acc  1B NOP* imp 1C TRB abs  1D ORA absx 1E ASL absx 1F BBR1 zpr "
"20 JSR abs  21 AND indx 22 NOP* imm 23 NOP* imp 24 BIT zp   25 AND zp   26 ROL zp   27 RMB2 zp "
"28 PLP imp  29 AND imm  2A ROL acc  2B NOP* imp 2C BIT abs  2D AND abs  2E ROL abs  2F BBR2 zpr "
"30 BMI rel  31 AND indy 32 AND zpi  33 NOP* imp 34 BIT zpx  35 AND zpx  36 ROL zpx  37 RMB3 zp "
"38 SEC imp  39 AND absy 3A DEC acc  3B NOP* imp 3C BIT absx 3D AND absx 3E ROL absx 3F BBR3 zpr "
"40 RTI imp  41 EOR indx 42 NOP* imm 43 NOP* imp 44 NOP* zp  45 EOR zp   46 LSR zp   47 RMB4 zp "
"48 PHA imp  49 EOR imm  4A LSR acc  4B NOP* imp 4C JMP abs  4D EOR abs  4E LSR abs  4F BBR4 zpr "
"50 BVC rel  51 EOR indy 52 EOR zpi  53 NOP* imp 54 NOP* zpx 55 EOR zpx  56 LSR zpx  57 RMB5 zp "
"58 CLI imp  59 EOR absy 5A PHY imp  5B NOP* imp 5C NOP* abs 5D EOR absx 5E LSR absx 5F BBR5 zpr "
"60 RTS imp  61 ADC indx 62 NOP* imm 63 NOP* imp 64 STZ zp   65 ADC zp   66 ROR zp   67 RMB6 zp "
"68 PLA imp  69 ADC imm  6A ROR acc  6B NOP* imp 6C JMP ind  6D ADC abs  6E ROR abs  6F BBR6 zpr "
"70 BVS rel  71 ADC indy 72 ADC zpi  73 NOP* imp 74 STZ zpx  75 ADC zpx  76 ROR zpx  77 RMB7 zp "
"78 SEI imp  79 ADC absy 7A PLY imp  7B NOP* imp 7C JMP aix  7D ADC absx 7E ROR absx 7F BBR7 zpr "
"80 BRA rel  81 STA indx 82 NOP* imm 83 NOP* imp 84 STY zp   85 STA zp   86 STX zp   87 SMB0 zp "
"88 DEY imp  89 BIT imm  8A TXA imp  8B NOP* imp 8C STY abs  8D STA abs  8E STX abs  8F BBS0 zpr "
"90 BCC rel  91 STA indy 92 STA zpi  93 NOP* imp 94 STY zpx  95 STA zpx  96 STX zpy  97 SMB1 zp "
"98 TYA imp  99 STA absy 9A TXS imp  9B NOP* imp 9C STZ abs  9D STA absx 9E STZ absx 9F BBS1 zpr "
"A0 LDY imm  A1 LDA indx A2 LDX imm  A3 NOP* imp A4 LDY zp   A5 LDA zp   A6 LDX zp   A7 SMB2 zp "
"A8 TAY imp  A9 LDA imm  AA TAX imp  AB NOP* imp AC LDY abs  AD LDA abs  AE LDX abs  AF BBS2 zpr "
"B0 BCS rel  B1 LDA indy B2 LDA zpi  B3 NOP* imp B4 LDY zpx  B5 LDA zpx  B6 LDX zpy  B7 SMB3 zp "
"B8 CLV imp  B9 LDA absy BA TSX imp  BB NOP* imp BC LDY absx BD LDA absx BE LDX absy BF BBS3 zpr "
"C0 CPY imm  C1 CMP indx C2 NOP* imm C3 NOP* imp C4 CPY zp   C5 CMP zp   C6 DEC zp   C7 SMB4 zp "
"C8 INY imp  C9 CMP imm  CA DEX imp  CB WAI imp  CC CPY abs  CD CMP abs  CE DEC abs  CF BBS4 zpr "
"D0 BNE rel  D1 CMP indy D2 CMP zpi  D3 NOP* imp D4 NOP* zpx D5 CMP zpx  D6 DEC zpx  D7 SMB5 zp "
"D8 CLD imp  D9 CMP absy DA PHX imp  DB STP imp  DC NOP* abs DD CMP absx DE DEC absx DF BBS5 zpr "
"E0 CPX imm  E1 SBC indx E2 NOP* imm E3 NOP* imp E4 CPX zp   E5 SBC zp   E6 INC zp   E7 SMB6 zp "
"E8 INX imp  E9 SBC imm  EA NOP imp  EB NOP* imp EC CPX abs  ED SBC abs  EE INC abs  EF BBS6 zpr "
"F0 BEQ rel  F1 SBC indy F2 SBC zpi  F3 NOP* imp F4 NOP* zpx F5 SBC zpx  F6 INC zpx  F7 SMB7 zp "
"F8 SED imp  F9 SBC absy FA PLX imp  FB NOP* imp FC NOP* abs FD SBC absx FE INC absx FF BBS7 zpr ";

enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABSX, M_ABSY, M_IND,
       M_INDX, M_INDY, M_ZPI, M_AIX, M_REL, M_ZPR };
static const char *MODE_NAMES[] = { "imp", "acc", "imm", "zp", "zpx", "zpy", "abs",
    "absx", "absy", "ind", "indx", "indy", "zpi", "aix", "rel", "zpr" };
static const int MODE_LEN[] = { 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 1, 1, 1, 2, 1, 2 };

static const char *MNEMS[] = { "ADC","AND","ASL","BBR","BBS","BCC","BCS","BEQ","BIT",
    "BMI","BNE","BPL","BRA","BRK","BVC","BVS","CLC","CLD","CLI","CLV","CMP","CPX",
    "CPY","DEC","DEX","DEY","EOR","INC","INX","INY","JMP","JSR","LDA","LDX","LDY",
    "LSR","NOP","ORA","PHA","PHP","PHX","PHY","PLA","PLP","PLX","PLY","RMB","ROL",
    "ROR","RTI","RTS","SBC","SEC","SED","SEI","SMB","STA","STP","STX","STY","STZ",
    "TAX","TAY","TRB","TSB","TSX","TXA","TXS","TYA","WAI", 0 };
enum { I_ADC, I_AND, I_ASL, I_BBR, I_BBS, I_BCC, I_BCS, I_BEQ, I_BIT, I_BMI, I_BNE,
       I_BPL, I_BRA, I_BRK, I_BVC, I_BVS, I_CLC, I_CLD, I_CLI, I_CLV, I_CMP, I_CPX,
       I_CPY, I_DEC, I_DEX, I_DEY, I_EOR, I_INC, I_INX, I_INY, I_JMP, I_JSR, I_LDA,
       I_LDX, I_LDY, I_LSR, I_NOP, I_ORA, I_PHA, I_PHP, I_PHX, I_PHY, I_PLA, I_PLP,
       I_PLX, I_PLY, I_RMB, I_ROL, I_ROR, I_RTI, I_RTS, I_SBC, I_SEC, I_SED, I_SEI,
       I_SMB, I_STA, I_STP, I_STX, I_STY, I_STZ, I_TAX, I_TAY, I_TRB, I_TSB, I_TSX,
       I_TXA, I_TXS, I_TYA, I_WAI };

/* WDC 65C02 base cycles (no page-cross penalties; +1 for a taken branch). */
static const uint8_t CYC[256] = {
7,6,2,1,5,3,5,5,3,2,2,1,6,4,6,5, 2,5,5,1,5,4,6,5,2,4,2,1,6,4,6,5,
6,6,2,1,3,3,5,5,4,2,2,1,4,4,6,5, 2,5,5,1,4,4,6,5,2,4,2,1,4,4,6,5,
6,6,2,1,3,3,5,5,3,2,2,1,3,4,6,5, 2,5,5,1,4,4,6,5,2,4,3,1,8,4,6,5,
6,6,2,1,3,3,5,5,4,2,2,1,6,4,6,5, 2,5,5,1,4,4,6,5,2,4,4,1,6,4,6,5,
3,6,2,1,3,3,3,5,2,2,2,1,4,4,4,5, 2,6,5,1,4,4,4,5,2,5,2,1,4,5,5,5,
2,6,2,1,3,3,3,5,2,2,2,1,4,4,4,5, 2,5,5,1,4,4,4,5,2,4,2,1,4,4,4,5,
2,6,2,1,3,3,5,5,2,2,2,3,4,4,6,5, 2,5,5,1,4,4,6,5,2,4,3,3,4,4,7,5,
2,6,2,1,3,3,5,5,2,2,2,1,4,4,6,5, 2,5,5,1,4,4,6,5,2,4,4,1,4,4,7,5 };

typedef struct { uint8_t ins, mode, bit, illegal; } OpInfo;
static OpInfo OP[256];

static void build_table(void) {
    const char *p = ROWS;
    char code[4], mn[8], md[8];
    int n;
    while (sscanf(p, "%3s %7s %7s%n", code, mn, md, &n) == 3) {
        int c = (int)strtol(code, 0, 16), i;
        OpInfo *o = &OP[c];
        size_t L = strlen(mn);
        o->illegal = mn[L - 1] == '*';
        if (o->illegal) mn[--L] = 0;
        o->bit = 0;
        if (L == 4 && mn[3] >= '0' && mn[3] <= '7') { o->bit = mn[3] - '0'; mn[3] = 0; }
        for (i = 0; MNEMS[i]; i++) if (!strcmp(MNEMS[i], mn)) break;
        if (!MNEMS[i]) { fprintf(stderr, "bad mnemonic %s\n", mn); exit(1); }
        o->ins = i;
        for (i = 0; i < 16; i++) if (!strcmp(MODE_NAMES[i], md)) break;
        if (i == 16) { fprintf(stderr, "bad mode %s\n", md); exit(1); }
        o->mode = i;
        p += n;
    }
}

/* ---- machine ---- */
#define CLOCK 4000000
#define FPS 61
#define CYC_FRAME (CLOCK / FPS)

static uint8_t *rom; static uint32_t romsize, nbanks;
static uint8_t ram[0x2000], reg[0x2000], vram[0x2000];
static uint32_t bank;               /* the bank at $8000 */
static uint8_t joy = 0xFF;
static uint64_t cycles;
static int timer_shot, dma_finished, timer_on; static uint64_t timer_at;
static int dma_on; static uint64_t dma_at;
static int nmi_pending, waiting, stopped;

static uint8_t A, X, Y, S = 0xFF, P = 0x34; static uint16_t PC;
enum { FC = 1, FZ = 2, FI = 4, FD = 8, FB = 0x10, FU = 0x20, FV = 0x40, FN = 0x80 };

/* ---- what we measure ---- */
enum { R_OP = 1, R_ARG = 2, R_READ = 4, R_JTGT = 8 };
static uint8_t *romflag;                    /* per ROM byte */
static uint8_t ramexec[0x2000], vramexec[0x2000];
static uint64_t ramexec_n, vramexec_n, smc_n, rom_w_n, open_rw_n;
static uint32_t smc_first_pc = 0xFFFFFFFF;
static uint64_t io_r[0x40], io_w[0x40], io_hi_rw;
static uint64_t bank_writes, bank_changes, bank_from_window, irq_n, nmi_n, nested_int;
static uint64_t bank_hist[8];
static uint64_t drop_ret_n, comp_rts_n, brk_n, wai_n, illegal_n, insn_n;
static uint64_t lcd_dma_n, snd_dma_n;
static int int_depth;
static uint32_t *nmi_at;    /* histogram: main PC when the NMI hit (64K) */
static uint8_t used_ops[256];

/* a JSR's mark on the stack: the slot of its high byte, and what it pushed */
static uint8_t jmark[256]; static uint16_t jval[256];

typedef struct { uint32_t site, target; uint8_t kind, used; uint32_t n; } Edge;
#define EHASH 65536
static Edge edges[EHASH]; static int nedges;
static void edge(uint32_t site, uint32_t target, uint8_t kind) {
    uint32_t h = (site * 2654435761u ^ target * 40503u ^ kind) & (EHASH - 1);
    while (edges[h].used) {
        Edge *e = &edges[h];
        if (e->site == site && e->target == target && e->kind == kind) { e->n++; return; }
        h = (h + 1) & (EHASH - 1);
    }
    if (nedges >= EHASH - 1) return;
    edges[h] = (Edge){ site, target, kind, 1, 1 }; nedges++;
}
/* a location as one number: ROM file offset, or 0x1000000|cpu address */
static uint32_t loc(uint16_t a) {
    if (a >= 0xC000) return (nbanks - 1) * 0x4000 + (a - 0xC000);
    if (a >= 0x8000) return bank * 0x4000 + (a - 0x8000);
    return 0x1000000u | a;
}
static void fmt_loc(char *s, uint32_t l) {
    if (l & 0x1000000u) sprintf(s, "RAM:%04X", l & 0xFFFF);
    else if (l / 0x4000 == nbanks - 1) sprintf(s, "F:%04X", 0xC000 + (l & 0x3FFF));
    else sprintf(s, "B%u:%04X", l / 0x4000, 0x8000 + (l & 0x3FFF));
}

static void check_irq(void);

static uint8_t rd(uint16_t a) {
    if (a < 0x2000) return ram[a];
    if (a < 0x4000) {
        uint16_t r = a & 0x1FFF;
        uint8_t d = reg[r];
        if (r < 0x40) io_r[r]++; else io_hi_rw++;
        switch (r) {
        case 0x20: return joy;
        case 0x21: return (d & ~0xF) | (reg[0x22] & 0xF);
        case 0x27: d &= ~3; if (timer_shot) d |= 1; if (dma_finished) d |= 2; return d;
        case 0x24: timer_shot = 0; check_irq(); break;
        case 0x25: dma_finished = 0; check_irq(); break;
        }
        return d;
    }
    if (a < 0x6000) return vram[a - 0x4000];
    if (a < 0x8000) { open_rw_n++; return 0; }
    uint32_t o = a >= 0xC000 ? (nbanks - 1) * 0x4000 + (a - 0xC000) : bank * 0x4000 + (a - 0x8000);
    romflag[o] |= R_READ;
    return rom[o];
}

static void lcd_dma(void) {   /* Potator's reading of $2008-$200D */
    uint16_t src = reg[8] | reg[9] << 8, dst = reg[0xA] | reg[0xB] << 8;
    uint32_t len = reg[0xC] ? reg[0xC] * 16 : 4096;
    lcd_dma_n++;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t v = rd(src + i);
        uint16_t d = dst + i;
        if (d >= 0x4000 && d < 0x6000) vram[d - 0x4000] = v;
        else if (d < 0x2000) ram[d] = v;
    }
}

static void wr(uint16_t a, uint8_t v) {
    if (a < 0x2000) {
        if (ramexec[a]) { smc_n++; if (smc_first_pc == 0xFFFFFFFF) smc_first_pc = PC; }
        ram[a] = v; return;
    }
    if (a < 0x4000) {
        uint16_t r = a & 0x1FFF;
        reg[r] = v;
        if (r < 0x40) io_w[r]++; else io_hi_rw++;
        switch (r) {
        case 0x26: {
            uint32_t nb = ((v & 0xE0) >> 5) % nbanks;
            bank_writes++;
            bank_hist[(v & 0xE0) >> 5]++;
            if (nb != bank) { bank_changes++; if (PC >= 0x8000 && PC < 0xC000) bank_from_window++; }
            bank = nb; check_irq(); break; }
        case 0x23: {
            uint32_t d = v ? v : 0x100;
            d *= (reg[0x26] & 0x10) ? 0x4000 : 0x100;
            timer_on = 1; timer_at = cycles + d; break; }
        case 0x0D: if (v & 0x80) lcd_dma(); break;
        case 0x1C:
            if (v & 0x80) {
                uint32_t size = (reg[0x1A] ? reg[0x1A] : 0x100) * 32;
                snd_dma_n++;
                dma_on = 1; dma_at = cycles + (uint64_t)size * 256 * (1 + (reg[0x1B] & 3));
            } else dma_on = 0;
            break;
        }
        return;
    }
    if (a < 0x6000) {
        if (vramexec[a - 0x4000]) smc_n++;
        vram[a - 0x4000] = v; return;
    }
    if (a < 0x8000) { open_rw_n++; return; }
    rom_w_n++;
}

static int irq_line(void) {
    return (timer_shot && (reg[0x26] & 2)) || (dma_finished && (reg[0x26] & 4));
}
static void check_irq(void) { /* level-sensitive: sampled before each instruction */ }

static void push(uint8_t v) {
    jmark[S] = 0;
    ram[0x100 + S] = v; S--;
}
static uint8_t pull(void) { S++; return ram[0x100 + S]; }
static void pull_drop_check(uint8_t newS) {
    /* stack pointer rising past a JSR's high byte without its RTS */
    for (int s = S + 1; s <= newS && s <= 0xFF; s++)
        if (jmark[s]) { jmark[s] = 0; drop_ret_n++; }
}

static void nz(uint8_t v) { P = (P & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ); }

static void adc(uint8_t v) {
    unsigned c = P & FC;
    if (P & FD) {
        int al = (A & 0x0F) + (v & 0x0F) + c;
        if (al >= 0x0A) al = ((al + 6) & 0x0F) + 0x10;
        int r = (A & 0xF0) + (v & 0xF0) + al;
        int sv = (int8_t)(A & 0xF0) + (int8_t)(v & 0xF0) + al;
        P &= ~(FV | FC);
        if (sv < -128 || sv > 127) P |= FV;
        if (r >= 0xA0) r += 0x60;
        if (r >= 0x100) P |= FC;
        A = r & 0xFF; nz(A);
    } else {
        unsigned r = A + v + c;
        P &= ~(FV | FC);
        if (~(A ^ v) & (A ^ r) & 0x80) P |= FV;
        if (r > 0xFF) P |= FC;
        A = r & 0xFF; nz(A);
    }
}
static void sbc(uint8_t v) {
    unsigned c = P & FC;
    int bin = A - v + c - 1;
    P &= ~(FV | FC);
    if ((A ^ v) & (A ^ (uint8_t)bin) & 0x80) P |= FV;
    if (bin >= 0) P |= FC;
    if (P & FD) {
        int al = (A & 0x0F) - (v & 0x0F) + c - 1;
        int r = bin;
        if (r < 0) r -= 0x60;
        if (al < 0) r -= 0x06;
        A = r & 0xFF;
    } else A = bin & 0xFF;
    nz(A);
}
static void cmp(uint8_t r, uint8_t v) {
    P = (P & ~FC) | (r >= v ? FC : 0); nz((uint8_t)(r - v));
}

static void interrupt(uint16_t vec, int is_nmi) {
    if (int_depth == 0 && nmi_at && is_nmi) nmi_at[PC]++;
    if (int_depth) nested_int++;
    push(PC >> 8); push(PC & 0xFF);
    push((P & ~FB) | FU);
    P |= FI; P &= ~FD;
    PC = rd(vec) | rd(vec + 1) << 8;
    int_depth++;
    cycles += 7;
    if (is_nmi) nmi_n++; else irq_n++;
}

static void mark_exec(uint16_t pc, int len) {
    if (pc < 0x2000) { if (!ramexec[pc]) ramexec[pc] = 1; ramexec_n++; return; }
    if (pc >= 0x4000 && pc < 0x6000) { vramexec[pc - 0x4000] = 1; vramexec_n++; return; }
    if (pc < 0x8000) return;
    for (int i = 0; i < len; i++) {
        uint16_t a = pc + i;
        if (a < 0x8000) break;
        uint32_t o = a >= 0xC000 ? (nbanks - 1) * 0x4000 + (a - 0xC000) : bank * 0x4000 + (a - 0x8000);
        romflag[o] |= i ? R_ARG : R_OP;
    }
}

static void step(void) {
    uint16_t pc0 = PC;
    uint8_t opc = rd(PC);
    const OpInfo *o = &OP[opc];
    uint16_t ea = 0; uint8_t zpa = 0, v;
    int len = 1 + MODE_LEN[o->mode];
    mark_exec(pc0, len);
    used_ops[opc] = 1;
    if (o->illegal) illegal_n++;
    insn_n++;
    PC++;
    cycles += CYC[opc];
    switch (o->mode) {
    case M_IMM: ea = PC++; break;
    case M_ZP: ea = rd(PC++); break;
    case M_ZPX: ea = (uint8_t)(rd(PC++) + X); break;
    case M_ZPY: ea = (uint8_t)(rd(PC++) + Y); break;
    case M_ABS: ea = rd(PC) | rd(PC + 1) << 8; PC += 2; break;
    case M_ABSX: ea = (rd(PC) | rd(PC + 1) << 8) + X; PC += 2; break;
    case M_ABSY: ea = (rd(PC) | rd(PC + 1) << 8) + Y; PC += 2; break;
    case M_IND: { uint16_t p = rd(PC) | rd(PC + 1) << 8; PC += 2;
                  ea = rd(p) | rd(p + 1) << 8; break; }
    case M_INDX: { uint8_t z = rd(PC++) + X; ea = ram[z] | ram[(uint8_t)(z + 1)] << 8; break; }
    case M_INDY: { uint8_t z = rd(PC++); ea = (ram[z] | ram[(uint8_t)(z + 1)] << 8) + Y; break; }
    case M_ZPI: { uint8_t z = rd(PC++); ea = ram[z] | ram[(uint8_t)(z + 1)] << 8; break; }
    case M_AIX: { uint16_t p = (rd(PC) | rd(PC + 1) << 8) + X; PC += 2;
                  ea = rd(p) | rd(p + 1) << 8; break; }
    case M_REL: { int8_t d = rd(PC++); ea = PC + d; break; }
    case M_ZPR: { zpa = rd(PC); int8_t d = rd(PC + 1); PC += 2; ea = PC + d; break; }
    }
#define BR(cond) if (cond) { PC = ea; cycles++; }
#define RMW(expr) do { if (o->mode == M_ACC) { v = A; expr; A = v; } \
                       else { v = rd(ea); expr; wr(ea, v); } } while (0)
    switch (o->ins) {
    case I_LDA: A = rd(ea); nz(A); break;
    case I_LDX: X = rd(ea); nz(X); break;
    case I_LDY: Y = rd(ea); nz(Y); break;
    case I_STA: wr(ea, A); break;
    case I_STX: wr(ea, X); break;
    case I_STY: wr(ea, Y); break;
    case I_STZ: wr(ea, 0); break;
    case I_ADC: adc(rd(ea)); break;
    case I_SBC: sbc(rd(ea)); break;
    case I_AND: A &= rd(ea); nz(A); break;
    case I_ORA: A |= rd(ea); nz(A); break;
    case I_EOR: A ^= rd(ea); nz(A); break;
    case I_CMP: cmp(A, rd(ea)); break;
    case I_CPX: cmp(X, rd(ea)); break;
    case I_CPY: cmp(Y, rd(ea)); break;
    case I_BIT: v = rd(ea);
        P = (P & ~FZ) | ((A & v) ? 0 : FZ);
        if (o->mode != M_IMM) P = (P & ~(FN | FV)) | (v & (FN | FV));
        break;
    case I_ASL: RMW({ P = (P & ~FC) | (v >> 7); v <<= 1; nz(v); }); break;
    case I_LSR: RMW({ P = (P & ~FC) | (v & 1); v >>= 1; nz(v); }); break;
    case I_ROL: RMW({ uint8_t c = P & FC; P = (P & ~FC) | (v >> 7); v = (v << 1) | c; nz(v); }); break;
    case I_ROR: RMW({ uint8_t c = P & FC; P = (P & ~FC) | (v & 1); v = (v >> 1) | (c << 7); nz(v); }); break;
    case I_INC: RMW({ v++; nz(v); }); break;
    case I_DEC: RMW({ v--; nz(v); }); break;
    case I_TSB: v = rd(ea); P = (P & ~FZ) | ((A & v) ? 0 : FZ); wr(ea, v | A); break;
    case I_TRB: v = rd(ea); P = (P & ~FZ) | ((A & v) ? 0 : FZ); wr(ea, v & ~A); break;
    case I_RMB: v = rd(ea); wr(ea, v & ~(1 << o->bit)); break;
    case I_SMB: v = rd(ea); wr(ea, v | (1 << o->bit)); break;
    case I_BBR: v = rd(zpa); BR(!(v & (1 << o->bit))); break;
    case I_BBS: v = rd(zpa); BR(v & (1 << o->bit)); break;
    case I_BPL: BR(!(P & FN)); break;
    case I_BMI: BR(P & FN); break;
    case I_BVC: BR(!(P & FV)); break;
    case I_BVS: BR(P & FV); break;
    case I_BCC: BR(!(P & FC)); break;
    case I_BCS: BR(P & FC); break;
    case I_BNE: BR(!(P & FZ)); break;
    case I_BEQ: BR(P & FZ); break;
    case I_BRA: PC = ea; break;
    case I_JMP:
        if (o->mode != M_ABS) edge(loc(pc0), loc(ea), o->mode == M_IND ? 'i' : 'x');
        if (ea >= 0x8000) romflag[loc(ea)] |= R_JTGT;
        PC = ea; break;
    case I_JSR: {
        uint16_t ret = PC - 1;
        push(ret >> 8); jmark[(uint8_t)(S + 1)] = 1; jval[(uint8_t)(S + 1)] = ret;
        push(ret & 0xFF);
        jmark[(uint8_t)(S + 2)] = 1;
        if (ea >= 0x8000) romflag[loc(ea)] |= R_JTGT;
        PC = ea; break; }
    case I_RTS: {
        uint8_t hs = S + 2;
        uint16_t ret;
        int ok = jmark[hs];
        uint16_t want = jval[hs];
        uint8_t lo = pull(), hi = pull();
        ret = (lo | hi << 8);
        jmark[hs] = 0;
        if (!ok || want != ret) { comp_rts_n++; edge(loc(pc0), loc(ret + 1), 'r'); }
        PC = ret + 1; break; }
    case I_RTI: {
        uint8_t ns = S + 3;
        pull_drop_check(ns);
        P = (pull() & ~FB) | FU; PC = pull(); PC |= pull() << 8;
        if (int_depth) int_depth--;
        break; }
    case I_BRK:
        brk_n++;
        push(PC >> 8); push(PC & 0xFF); push(P | FB | FU);
        P |= FI; P &= ~FD;
        PC = rd(0xFFFE) | rd(0xFFFF) << 8; int_depth++; break;
    case I_PHA: push(A); break;
    case I_PHX: push(X); break;
    case I_PHY: push(Y); break;
    case I_PHP: push(P | FB | FU); break;
    case I_PLA: pull_drop_check(S + 1); A = pull(); nz(A); break;
    case I_PLX: pull_drop_check(S + 1); X = pull(); nz(X); break;
    case I_PLY: pull_drop_check(S + 1); Y = pull(); nz(Y); break;
    case I_PLP: pull_drop_check(S + 1); P = (pull() & ~FB) | FU; break;
    case I_TAX: X = A; nz(X); break;
    case I_TAY: Y = A; nz(Y); break;
    case I_TXA: A = X; nz(A); break;
    case I_TYA: A = Y; nz(A); break;
    case I_TSX: X = S; nz(X); break;
    case I_TXS:
        if (X > S) pull_drop_check(X);
        else for (int s = X + 1; s <= S; s++) jmark[s] = 0;
        S = X; break;
    case I_INX: X++; nz(X); break;
    case I_INY: Y++; nz(Y); break;
    case I_DEX: X--; nz(X); break;
    case I_DEY: Y--; nz(Y); break;
    case I_CLC: P &= ~FC; break;
    case I_SEC: P |= FC; break;
    case I_CLI: P &= ~FI; break;
    case I_SEI: P |= FI; break;
    case I_CLD: P &= ~FD; break;
    case I_SED: P |= FD; break;
    case I_CLV: P &= ~FV; break;
    case I_NOP: break;
    case I_WAI: waiting = 1; wai_n++; break;
    case I_STP: stopped = 1; break;
    }
}

static void events(void) {
    if (timer_on && cycles >= timer_at) { timer_on = 0; timer_shot = 1; }
    if (dma_on && cycles >= dma_at) { dma_on = 0; dma_finished = 1; }
}

/* ---- the screen, as the LCD shows it ---- */
static void snap(const char *path) {
    static const uint8_t pal[4] = { 0xE0, 0xB9, 0x54, 0x12 };
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5\n160 160\n255\n");
    int on = reg[0x26] & 8;
    int j = reg[2] / 4 + reg[3] * 0x30;
    for (int y = 0; y < 160; y++) {
        uint8_t line[164];
        int sx = 3 - (reg[2] & 3), ex = (reg[0] | 3) < 163 ? (reg[0] | 3) : 163;
        memset(line, pal[0], sizeof line);
        if (on)
            for (int x = sx, i = 0; x < ex; x += 4, i++) {
                uint8_t b = vram[(j + i) & 0x1FFF];
                for (int k = 0; k < 4 && x + k < 164; k++) { line[x + k] = pal[b & 3]; b >>= 2; }
            }
        fwrite(line + 3, 1, 160, f);
        j += 0x30; if (j >= 0x1FE0) j = 0;
    }
    fclose(f);
}

/* ---- the monkey: a deterministic pad ---- */
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 16; }
static uint8_t monkey(int f) {
    /* active-low: R L D U B A Select Start */
    static uint8_t held = 0;
    if (f < 180) return 0xFF;
    int ph = (f - 180) % 300;
    if (ph < 6) return (uint8_t)~0x80;           /* Start */
    if (ph >= 40 && ph < 46) return (uint8_t)~0x20; /* A */
    if (ph >= 80 && ph < 86) return (uint8_t)~0x80; /* Start again */
    if (ph % 15 == 0) {
        static const uint8_t dirs[] = { 0, 1, 2, 4, 8, 1 | 8, 2 | 8, 1 | 4, 2 | 4 };
        held = dirs[rnd() % 9];
        if (rnd() % 100 < 40) held |= 0x20;
        if (rnd() % 100 < 25) held |= 0x10;
    }
    return (uint8_t)~held;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "svtrace ROM OUTDIR [frames] [--snap a,b,c]\n"); return 1; }
    const char *out = argv[2];
    int frames = argc > 3 && argv[3][0] != '-' ? atoi(argv[3]) : 3600;
    int snaps[64], nsnap = 0;
    for (int i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--snap") && i + 1 < argc) {
            char *s = argv[++i];
            while (*s && nsnap < 64) { snaps[nsnap++] = atoi(s); while (*s && *s != ',') s++; if (*s) s++; }
        }
    build_table();
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END); romsize = ftell(f); fseek(f, 0, SEEK_SET);
    rom = malloc(romsize); fread(rom, 1, romsize, f); fclose(f);
    nbanks = romsize / 0x4000;
    romflag = calloc(romsize, 1);
    nmi_at = calloc(65536, sizeof *nmi_at);

    PC = rd(0xFFFC) | rd(0xFFFD) << 8;
    uint32_t reset = PC;
    uint64_t busy_frames = 0;
    for (int fr = 0; fr < frames && !stopped; fr++) {
        joy = monkey(fr);
        uint64_t end = cycles + CYC_FRAME;
        if (reg[0x26] & 1) nmi_pending = 1;
        while (cycles < end && !stopped) {
            events();
            if (nmi_pending) { nmi_pending = 0; waiting = 0; interrupt(0xFFFA, 1); continue; }
            if (irq_line()) {
                waiting = 0;
                if (!(P & FI)) { interrupt(0xFFFE, 0); continue; }
            }
            if (waiting) {
                uint64_t t = end;
                if (timer_on && timer_at < t) t = timer_at;
                if (dma_on && dma_at < t) t = dma_at;
                cycles = t > cycles ? t : cycles + 1;
                continue;
            }
            step();
        }
        for (int i = 0; i < nsnap; i++)
            if (snaps[i] == fr) {
                char p[1024]; snprintf(p, sizeof p, "%s/f%05d.pgm", out, fr); snap(p);
            }
        (void)busy_frames;
    }
    { char p[1024]; snprintf(p, sizeof p, "%s/last.pgm", out); snap(p); }

    /* ---- the report ---- */
    char p[1024]; snprintf(p, sizeof p, "%s/cov.bin", out);
    f = fopen(p, "wb"); fwrite(romflag, 1, romsize, f); fclose(f);
    snprintf(p, sizeof p, "%s/ramexec.bin", out);
    f = fopen(p, "wb"); fwrite(ramexec, 1, sizeof ramexec, f); fclose(f);

    snprintf(p, sizeof p, "%s/report.txt", out);
    f = fopen(p, "w");
    fprintf(f, "rom %s\nsize %u banks %u reset %04X frames %d stopped %d\n",
            argv[1], romsize, nbanks, reset, frames, stopped);
    fprintf(f, "insns %llu nmi %llu irq %llu nested_int %llu brk %llu wai %llu illegal %llu\n",
            (unsigned long long)insn_n, (unsigned long long)nmi_n, (unsigned long long)irq_n,
            (unsigned long long)nested_int, (unsigned long long)brk_n,
            (unsigned long long)wai_n, (unsigned long long)illegal_n);
    for (uint32_t b = 0; b < nbanks; b++) {
        uint32_t op = 0, arg = 0, rdn = 0, both = 0;
        for (uint32_t i = b * 0x4000; i < (b + 1) * 0x4000; i++) {
            if (romflag[i] & R_OP) op++;
            if (romflag[i] & (R_OP | R_ARG)) arg++;
            if (romflag[i] & R_READ) rdn++;
            if ((romflag[i] & R_OP) && (romflag[i] & R_ARG)) both++;
        }
        fprintf(f, "bank %u code_bytes %u insn_starts %u data_read %u overlap %u\n",
                b, arg, op, rdn, both);
    }
    int ramcode = 0; for (int i = 0; i < 0x2000; i++) ramcode += ramexec[i];
    int vramcode = 0; for (int i = 0; i < 0x2000; i++) vramcode += vramexec[i];
    fprintf(f, "ramexec_bytes %d ramexec_insns %llu vramexec_bytes %d smc_writes %llu",
            ramcode, (unsigned long long)ramexec_n, vramcode, (unsigned long long)smc_n);
    if (smc_first_pc != 0xFFFFFFFF) fprintf(f, " smc_first_pc %04X", smc_first_pc);
    fprintf(f, "\nbank_writes %llu bank_changes %llu bank_change_from_window %llu\n",
            (unsigned long long)bank_writes, (unsigned long long)bank_changes,
            (unsigned long long)bank_from_window);
    fprintf(f, "bank_values");
    for (int i = 0; i < 8; i++) fprintf(f, " %d:%llu", i, (unsigned long long)bank_hist[i]);
    fprintf(f, "\ncomputed_rts %llu dropped_returns %llu rom_writes %llu open_bus %llu io_hi %llu\n",
            (unsigned long long)comp_rts_n, (unsigned long long)drop_ret_n,
            (unsigned long long)rom_w_n, (unsigned long long)open_rw_n, (unsigned long long)io_hi_rw);
    fprintf(f, "lcd_dma %llu snd_dma %llu\n", (unsigned long long)lcd_dma_n, (unsigned long long)snd_dma_n);
    fprintf(f, "io_w");
    for (int i = 0; i < 0x40; i++) if (io_w[i]) fprintf(f, " %02X:%llu", i, (unsigned long long)io_w[i]);
    fprintf(f, "\nio_r");
    for (int i = 0; i < 0x40; i++) if (io_r[i]) fprintf(f, " %02X:%llu", i, (unsigned long long)io_r[i]);
    fprintf(f, "\nops_used");
    int nops = 0; for (int i = 0; i < 256; i++) nops += used_ops[i];
    fprintf(f, " %d :", nops);
    for (int i = 0; i < 256; i++) if (used_ops[i]) fprintf(f, " %02X", i);
    /* the NMI's landing places in the main program */
    fprintf(f, "\nnmi_hits");
    {
        int distinct = 0; uint64_t tot = 0;
        for (int i = 0; i < 65536; i++) if (nmi_at[i]) { distinct++; tot += nmi_at[i]; }
        fprintf(f, " distinct %d total %llu top:", distinct, (unsigned long long)tot);
        for (int k = 0; k < 6; k++) {
            int best = -1;
            for (int i = 0; i < 65536; i++) if (nmi_at[i] && (best < 0 || nmi_at[i] > nmi_at[best])) best = i;
            if (best < 0) break;
            fprintf(f, " %04X:%u", best, nmi_at[best]);
            nmi_at[best] = 0;
        }
    }
    fprintf(f, "\n");
    for (int i = 0; i < EHASH; i++) {
        char a[32], b[32];
        if (!edges[i].used) continue;
        fmt_loc(a, edges[i].site); fmt_loc(b, edges[i].target);
        fprintf(f, "edge %c %s -> %s x%u\n", edges[i].kind, a, b, edges[i].n);
    }
    fclose(f);
    return 0;
}
