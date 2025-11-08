// RV32I+M minimal simulator (46+ instruções) – estilo de trace próximo ao do professor
// Compilar: gcc -O2 -std=c11 sim.c -o sim

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// ---------- Config ----------
enum { MEM_SIZE = 32 * 1024 };
static const uint32_t MEM_BASE = 0x80000000u;

// ---------- Utils ----------
static inline uint32_t sext32_12(uint32_t v){ return (v & 0x800)? (0xFFFFF000u | v) : v; }
static inline uint32_t sext32_13(uint32_t v){ return (v & 0x1000)? (0xFFFFE000u | v) : v; }
static inline uint32_t sext32_8 (uint32_t v){ return (v & 0x80 )? (0xFFFFFF00u | v) : v; }
static inline uint32_t sext32_16(uint32_t v){ return (v & 0x8000)? (0xFFFF0000u | v) : v; }
static inline int64_t  s64(int32_t v) { return (int64_t)v; }
static inline uint64_t u64(uint32_t v){ return (uint64_t)v; }

static inline uint32_t load_u32(const uint8_t* m, uint32_t base, uint32_t addr){
    uint32_t i = addr - base; return ((uint32_t)m[i]) | ((uint32_t)m[i+1]<<8) | ((uint32_t)m[i+2]<<16) | ((uint32_t)m[i+3]<<24);
}
static inline uint32_t load_u16(const uint8_t* m, uint32_t base, uint32_t addr){
    uint32_t i = addr - base; return ((uint32_t)m[i]) | ((uint32_t)m[i+1]<<8);
}
static inline uint32_t load_u8 (const uint8_t* m, uint32_t base, uint32_t addr){
    uint32_t i = addr - base; return (uint32_t)m[i];
}
static inline void store_u32(uint8_t* m, uint32_t base, uint32_t addr, uint32_t v){
    uint32_t i = addr - base; m[i]=v; m[i+1]=v>>8; m[i+2]=v>>16; m[i+3]=v>>24;
}
static inline void store_u16(uint8_t* m, uint32_t base, uint32_t addr, uint32_t v){
    uint32_t i = addr - base; m[i]=v; m[i+1]=v>>8;
}
static inline void store_u8 (uint8_t* m, uint32_t base, uint32_t addr, uint32_t v){
    uint32_t i = addr - base; m[i]=v;
}

// ---------- Carregar imagem ----------
static int load_hex_image(FILE* in, uint8_t* mem, uint32_t base, uint32_t memsz){
    char tok[256]; uint32_t cur = base;
    while (fscanf(in, " %255s", tok) == 1){
        if (tok[0]=='@'){
            unsigned a=0; if (sscanf(tok+1, "%x", &a)!=1) return -1; cur = a;
        } else {
            unsigned b=0; size_t L=strlen(tok); if (tok[L-1]==',') tok[L-1]='\0';
            if (sscanf(tok, "%x", &b)!=1) return -2;
            uint32_t idx = cur - base; if (idx < memsz) mem[idx]=(uint8_t)(b&0xFF); cur++;
        }
    }
    return 0;
}

// ---------- DIV/M regras do spec ----------
static inline uint32_t rv32_div(int32_t a, int32_t b){
    if (b==0) return 0xFFFFFFFFu;
    if (a==INT32_MIN && b==-1) return (uint32_t)INT32_MIN;
    return (uint32_t)(a / b);
}
static inline uint32_t rv32_rem(int32_t a, int32_t b){
    if (b==0) return (uint32_t)a;
    if (a==INT32_MIN && b==-1) return 0u;
    return (uint32_t)(a % b);
}
static inline uint32_t rv32_divu(uint32_t a, uint32_t b){
    if (b==0) return 0xFFFFFFFFu;
    return a / b;
}
static inline uint32_t rv32_remu(uint32_t a, uint32_t b){
    if (b==0) return a;
    return a % b;
}
static inline uint32_t mulh_ss(int32_t a, int32_t b){ return (uint32_t)((s64(a)*s64(b))>>32); }
static inline uint32_t mulh_su(int32_t a, uint32_t b){ return (uint32_t)((s64(a)*u64(b))>>32); }
static inline uint32_t mulh_uu(uint32_t a, uint32_t b){ return (uint32_t)((u64(a)*u64(b))>>32); }

// ---------- Main ----------
int main(int argc, char** argv){
    if (argc < 2){
        fprintf(stderr,"uso: %s <input.hex>\n", argv[0]);
        return 1;
    }

    printf("--------------------------------------------------------------------------------\n");
    for (uint32_t i=0;i<(uint32_t)argc;i++) printf("argv[%u] = %s\n", i, argv[i]);

    FILE* in = fopen(argv[1],"r"); if (!in){ fprintf(stderr,"erro: não abriu %s\n", argv[1]); return 1; }
    uint8_t* mem = (uint8_t*)malloc(MEM_SIZE); if(!mem){ fprintf(stderr,"erro: malloc\n"); fclose(in); return 1; }
    memset(mem,0,MEM_SIZE);
    if (load_hex_image(in, mem, MEM_BASE, MEM_SIZE)!=0){ fprintf(stderr,"erro: parsing do input\n"); free(mem); fclose(in); return 1; }
    fclose(in);

    uint32_t x[32]={0};
    const char* xn[32]={"zero","ra","sp","gp","tp","t0","t1","t2","s0","s1","a0","a1","a2","a3","a4","a5","a6","a7","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11","t3","t4","t5","t6"};
    uint32_t pc = MEM_BASE;

    printf("--------------------------------------------------------------------------------\n");

    uint8_t run=1;
    while (run){
        uint32_t instr = load_u32(mem, MEM_BASE, pc);

        uint8_t  op     =  instr & 0x7F;
        uint8_t  rd     = (instr>>7) & 0x1F;
        uint8_t  f3     = (instr>>12)& 0x07;
        uint8_t  rs1    = (instr>>15)& 0x1F;
        uint8_t  rs2    = (instr>>20)& 0x1F;
        uint8_t  f7     = (instr>>25)& 0x7F;

        uint32_t imm_i  = (instr>>20)&0xFFF;
        uint32_t simm_i = sext32_12(imm_i);
        uint32_t uimm20 = instr & 0xFFFFF000;

        uint32_t imm_s  = ((instr>>25)<<5) | ((instr>>7)&0x1F);
        uint32_t simm_s = (imm_s & 0x800)? (0xFFFFF000u|imm_s):imm_s;

        uint32_t imm_b =
            ((instr>>31)<<12) |
            (((instr>>25)&0x3F)<<5) |
            (((instr>>8)&0x0F)<<1) |
            (((instr>>7)&0x01)<<11);
        uint32_t simm_b = sext32_13(imm_b);

        // J-type imm (para JAL)
        uint32_t imm_j =
            ((instr>>31)<<19) |
            (((instr>>12)&0xFF)<<11) |
            (((instr>>20)&0x01)<<10) |
            ((instr>>21)&0x3FF);
        uint32_t simm_j = (imm_j>>19)? (0xFFF00000u | imm_j) : imm_j;

        switch(op){
            // ---------------- U-type ----------------
            case 0b0110111: { // LUI
                uint32_t res = uimm20;
                printf("0x%08x:lui    %s,0x%05x        %s=0x%08x\n",
                       pc, xn[rd], uimm20>>12, xn[rd], res);
                if (rd) x[rd]=res;
            } break;

            case 0b0010111: { // AUIPC
                uint32_t res = pc + uimm20;
                printf("0x%08x:auipc  %s,0x%05x          %s=0x%08x+0x%08x=0x%08x\n",
                       pc, xn[rd], uimm20>>12, xn[rd], pc, uimm20, res);
                if (rd) x[rd]=res;
            } break;

            // ---------------- J-type ----------------
            case 0b1101111: { // JAL
                uint32_t tgt = pc + (simm_j<<1);
                printf("0x%08x:jal    %s,0x%05x        pc=0x%08x,%s=0x%08x\n",
                       pc, xn[rd], (uint32_t)(simm_j & 0xFFFFF), tgt, xn[rd], pc+4);
                if (rd) x[rd]=pc+4;
                pc = tgt - 4;
            } break;

            // ---------------- I-type: JALR/OP-IMM/LOAD/SYSTEM ----------------
            case 0b1100111: { // JALR
                if (f3==0b000){
                    uint32_t tgt = (x[rs1] + (uint32_t)simm_i) & ~1u;
                    printf("0x%08x:jalr   %s,%s,0x%03x       pc=0x%08x,%s=0x%08x\n",
                           pc, xn[rd], xn[rs1], imm_i, tgt, xn[rd], pc+4);
                    if (rd) x[rd]=pc+4;
                    pc = tgt - 4;
                }
            } break;

            case 0b0010011: { // OP-IMM
                uint32_t shamt = imm_i & 0x1F;
                switch (f3){
                    case 0b000: { // ADDI
                        uint32_t res = (uint32_t)((int32_t)x[rs1] + (int32_t)simm_i);
                        printf("0x%08x:addi   %s,%s,0x%03x         %s=0x%08x+0x%08x=0x%08x\n",
                               pc, xn[rd], xn[rs1], imm_i, xn[rd], x[rs1], (uint32_t)simm_i, res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b010: { // SLTI (signed)
                        uint32_t res = ((int32_t)x[rs1] < (int32_t)simm_i)? 1u:0u;
                        printf("0x%08x:slti   %s,%s,0x%03x         %s=%d\n",
                               pc, xn[rd], xn[rs1], imm_i, xn[rd], (int)res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b011: { // SLTIU (unsigned)
                        uint32_t res = (x[rs1] < (uint32_t)simm_i)? 1u:0u;
                        printf("0x%08x:sltiu  %s,%s,0x%03x         %s=%d\n",
                               pc, xn[rd], xn[rs1], imm_i, xn[rd], (int)res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b100: { // XORI
                        uint32_t res = x[rs1] ^ (uint32_t)simm_i;
                        printf("0x%08x:xori   %s,%s,0x%03x         %s=0x%08x\n",
                               pc, xn[rd], xn[rs1], imm_i, xn[rd], res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b110: { // ORI
                        uint32_t res = x[rs1] | (uint32_t)simm_i;
                        printf("0x%08x:ori    %s,%s,0x%03x         %s=0x%08x\n",
                               pc, xn[rd], xn[rs1], imm_i, xn[rd], res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b111: { // ANDI
                        uint32_t res = x[rs1] & (uint32_t)simm_i;
                        printf("0x%08x:andi   %s,%s,0x%03x         %s=0x%08x\n",
                               pc, xn[rd], xn[rs1], imm_i, xn[rd], res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b001: { // SLLI
                        uint32_t res = x[rs1] << shamt;
                        printf("0x%08x:slli   %s,%s,%u        %s=0x%08x<<%u=0x%08x\n",
                               pc, xn[rd], xn[rs1], shamt, xn[rd], x[rs1], shamt, res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b101: { // SRLI/SRAI
                        if ((imm_i & 0x400)==0){ // SRLI
                            uint32_t res = x[rs1] >> shamt;
                            printf("0x%08x:srli   %s,%s,%u        %s=0x%08x>>%u=0x%08x\n",
                                   pc, xn[rd], xn[rs1], shamt, xn[rd], x[rs1], shamt, res);
                            if (rd) x[rd]=res;
                        } else { // SRAI
                            uint32_t res = (uint32_t)((int32_t)x[rs1] >> shamt);
                            printf("0x%08x:srai   %s,%s,%u        %s=(int)0x%08x>>%u=0x%08x\n",
                                   pc, xn[rd], xn[rs1], shamt, xn[rd], x[rs1], shamt, res);
                            if (rd) x[rd]=res;
                        }
                    } break;
                }
            } break;

            case 0b0000011: { // LOAD
                uint32_t addr = x[rs1] + (uint32_t)simm_i;
                switch (f3){
                    case 0b000: { // LB
                        uint32_t v = load_u8(mem, MEM_BASE, addr);
                        uint32_t res = sext32_8(v);
                        printf("0x%08x:lb     %s,0x%03x(%s)      %s=0x%08x\n",
                               pc, xn[rd], imm_i, xn[rs1], xn[rd], res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b001: { // LH
                        uint32_t v = load_u16(mem, MEM_BASE, addr);
                        uint32_t res = sext32_16(v);
                        printf("0x%08x:lh     %s,0x%03x(%s)      %s=0x%08x\n",
                               pc, xn[rd], imm_i, xn[rs1], xn[rd], res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b010: { // LW
                        uint32_t res = load_u32(mem, MEM_BASE, addr);
                        printf("0x%08x:lw     %s,0x%03x(%s)      %s=0x%08x\n",
                               pc, xn[rd], imm_i, xn[rs1], xn[rd], res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b100: { // LBU
                        uint32_t res = load_u8(mem, MEM_BASE, addr) & 0xFFu;
                        printf("0x%08x:lbu    %s,0x%03x(%s)      %s=0x%08x\n",
                               pc, xn[rd], imm_i, xn[rs1], xn[rd], res);
                        if (rd) x[rd]=res;
                    } break;
                    case 0b101: { // LHU
                        uint32_t res = load_u16(mem, MEM_BASE, addr) & 0xFFFFu;
                        printf("0x%08x:lhu    %s,0x%03x(%s)      %s=0x%08x\n",
                               pc, xn[rd], imm_i, xn[rs1], xn[rd], res);
                        if (rd) x[rd]=res;
                    } break;
                }
            } break;

            case 0b1110011: { // SYSTEM: ECALL/EBREAK
                if (f3==0 && imm_i==0){ // ECALL
                    printf("0x%08x:ecall\n", pc);
                } else if (f3==0 && imm_i==1){ // EBREAK
                    printf("0x%08x:ebreak\n", pc);
                    uint32_t prev = load_u32(mem, MEM_BASE, pc-4);
                    uint32_t next = load_u32(mem, MEM_BASE, pc+4);
                    if (prev==0x01f01013 && next==0x40705013) run=0;
                } else {
                    // CSR etc – fora do escopo do trabalho típico
                }
            } break;

            // ---------------- S-type ----------------
            case 0b0100011: { // STORE
                uint32_t addr = x[rs1] + (uint32_t)simm_s;
                switch (f3){
                    case 0b000: // SB
                        store_u8(mem, MEM_BASE, addr, x[rs2]&0xFFu);
                        printf("0x%08x:sb     %s,0x%03x(%s)      mem[0x%08x]=0x%02x\n",
                               pc, xn[rs2], (uint32_t)simm_s & 0xFFF, xn[rs1], addr, x[rs2]&0xFFu);
                        break;
                    case 0b001: // SH
                        store_u16(mem, MEM_BASE, addr, x[rs2]&0xFFFFu);
                        printf("0x%08x:sh     %s,0x%03x(%s)      mem[0x%08x]=0x%04x\n",
                               pc, xn[rs2], (uint32_t)simm_s & 0xFFF, xn[rs1], addr, x[rs2]&0xFFFFu);
                        break;
                    case 0b010: // SW
                        store_u32(mem, MEM_BASE, addr, x[rs2]);
                        printf("0x%08x:sw     %s,0x%03x(%s)      mem[0x%08x]=0x%08x\n",
                               pc, xn[rs2], (uint32_t)simm_s & 0xFFF, xn[rs1], addr, x[rs2]);
                        break;
                }
            } break;

            // ---------------- B-type ----------------
            case 0b1100011: { // BRANCH
                uint32_t tgt = pc + (simm_b<<1);
                int take = 0;
                switch (f3){
                    case 0b000: take = (x[rs1]==x[rs2]); // BEQ
                        printf("0x%08x:beq    %s,%s,0x%03x         (%s==%s)=%d->pc=0x%08x\n",
                               pc, xn[rs1], xn[rs2], (uint32_t)(simm_b&0x1FFF), xn[rs1], xn[rs2], take, take? tgt: pc+4);
                        break;
                    case 0b001: take = (x[rs1]!=x[rs2]); // BNE
                        printf("0x%08x:bne    %s,%s,0x%03x         (%s!=%s)=%d->pc=0x%08x\n",
                               pc, xn[rs1], xn[rs2], (uint32_t)(simm_b&0x1FFF), xn[rs1], xn[rs2], take, take? tgt: pc+4);
                        break;
                    case 0b100: take = ((int32_t)x[rs1] < (int32_t)x[rs2]); // BLT
                        printf("0x%08x:blt    %s,%s,0x%03x         (%s<%s)=%d->pc=0x%08x\n",
                               pc, xn[rs1], xn[rs2], (uint32_t)(simm_b&0x1FFF), xn[rs1], xn[rs2], take, take? tgt: pc+4);
                        break;
                    case 0b101: take = ((int32_t)x[rs1] >= (int32_t)x[rs2]); // BGE
                        printf("0x%08x:bge    %s,%s,0x%03x         (%s>=%s)=%d->pc=0x%08x\n",
                               pc, xn[rs1], xn[rs2], (uint32_t)(simm_b&0x1FFF), xn[rs1], xn[rs2], take, take? tgt: pc+4);
                        break;
                    case 0b110: take = (x[rs1] < x[rs2]); // BLTU
                        printf("0x%08x:bltu   %s,%s,0x%03x         (u%s<u%s)=%d->pc=0x%08x\n",
                               pc, xn[rs1], xn[rs2], (uint32_t)(simm_b&0x1FFF), xn[rs1], xn[rs2], take, take? tgt: pc+4);
                        break;
                    case 0b111: take = (x[rs1] >= x[rs2]); // BGEU
                        printf("0x%08x:bgeu   %s,%s,0x%03x         (u%s>=u%s)=%d->pc=0x%08x\n",
                               pc, xn[rs1], xn[rs2], (uint32_t)(simm_b&0x1FFF), xn[rs1], xn[rs2], take, take? tgt: pc+4);
                        break;
                }
                if (take) pc = tgt - 4;
            } break;

            // ---------------- R-type: OP/OP-M ----------------
            case 0b0110011: {
                if (f7==0b0000000){ // OP
                    switch(f3){
                        case 0b000: { // ADD/SUB
                            if (((instr>>30)&1)==0){ // ADD
                                uint32_t res = x[rs1] + x[rs2];
                                printf("0x%08x:add    %s,%s,%s          %s=0x%08x+0x%08x=0x%08x\n",
                                       pc, xn[rd], xn[rs1], xn[rs2], xn[rd], x[rs1], x[rs2], res);
                                if (rd) x[rd]=res;
                            } else { // SUB
                                uint32_t res = x[rs1] - x[rs2];
                                printf("0x%08x:sub    %s,%s,%s          %s=0x%08x-0x%08x=0x%08x\n",
                                       pc, xn[rd], xn[rs1], xn[rs2], xn[rd], x[rs1], x[rs2], res);
                                if (rd) x[rd]=res;
                            }
                        } break;
                        case 0b001: { // SLL
                            uint32_t sh = x[rs2] & 0x1F; uint32_t res = x[rs1] << sh;
                            printf("0x%08x:sll    %s,%s,%s          %s=0x%08x<<%u=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], x[rs1], sh, res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b010: { // SLT
                            uint32_t res = ((int32_t)x[rs1] < (int32_t)x[rs2])?1u:0u;
                            printf("0x%08x:slt    %s,%s,%s          %s=%u\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b011: { // SLTU
                            uint32_t res = (x[rs1] < x[rs2])?1u:0u;
                            printf("0x%08x:sltu   %s,%s,%s          %s=%u\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b100: { // XOR
                            uint32_t res = x[rs1]^x[rs2];
                            printf("0x%08x:xor    %s,%s,%s          %s=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b101: { // SRL/SRA
                            uint32_t sh = x[rs2] & 0x1F;
                            if (((instr>>30)&1)==0){ // SRL
                                uint32_t res = x[rs1] >> sh;
                                printf("0x%08x:srl    %s,%s,%s          %s=0x%08x>>%u=0x%08x\n",
                                       pc, xn[rd], xn[rs1], xn[rs2], xn[rd], x[rs1], sh, res);
                                if (rd) x[rd]=res;
                            } else { // SRA
                                uint32_t res = (uint32_t)((int32_t)x[rs1] >> sh);
                                printf("0x%08x:sra    %s,%s,%s          %s=(int)0x%08x>>%u=0x%08x\n",
                                       pc, xn[rd], xn[rs1], xn[rs2], xn[rd], x[rs1], sh, res);
                                if (rd) x[rd]=res;
                            }
                        } break;
                        case 0b110: { // OR
                            uint32_t res = x[rs1] | x[rs2];
                            printf("0x%08x:or     %s,%s,%s          %s=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b111: { // AND
                            uint32_t res = x[rs1] & x[rs2];
                            printf("0x%08x:and    %s,%s,%s          %s=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                    }
                } else if (f7==0b0000001){ // RV32M
                    switch(f3){
                        case 0b000: { // MUL
                            uint32_t res = (uint32_t)(u64(x[rs1])*u64(x[rs2]));
                            printf("0x%08x:mul    %s,%s,%s          %s=low32(0x%08x*0x%08x)=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], x[rs1], x[rs2], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b001: { // MULH
                            uint32_t res = mulh_ss((int32_t)x[rs1], (int32_t)x[rs2]);
                            printf("0x%08x:mulh   %s,%s,%s          %s=high32(ss)=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b010: { // MULHSU
                            uint32_t res = mulh_su((int32_t)x[rs1], x[rs2]);
                            printf("0x%08x:mulhsu %s,%s,%s          %s=high32(su)=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b011: { // MULHU
                            uint32_t res = mulh_uu(x[rs1], x[rs2]);
                            printf("0x%08x:mulhu  %s,%s,%s          %s=high32(uu)=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b100: { // DIV
                            uint32_t res = rv32_div((int32_t)x[rs1], (int32_t)x[rs2]);
                            printf("0x%08x:div    %s,%s,%s          %s=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b101: { // DIVU
                            uint32_t res = rv32_divu(x[rs1], x[rs2]);
                            printf("0x%08x:divu   %s,%s,%s          %s=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b110: { // REM
                            uint32_t res = rv32_rem((int32_t)x[rs1], (int32_t)x[rs2]);
                            printf("0x%08x:rem    %s,%s,%s          %s=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                        case 0b111: { // REMU
                            uint32_t res = rv32_remu(x[rs1], x[rs2]);
                            printf("0x%08x:remu   %s,%s,%s          %s=0x%08x\n",
                                   pc, xn[rd], xn[rs1], xn[rs2], xn[rd], res);
                            if (rd) x[rd]=res;
                        } break;
                    }
                } else {
                    // outros f7 não usados aqui
                }
            } break;

            // ---------------- FENCE (NOP prático para este projeto) ----------
            case 0b0001111: { // FENCE/FENCE.I
                printf("0x%08x:fence   (nop)\n", pc);
            } break;

            default:
                printf("error: unknown instruction opcode at pc = 0x%08x (op=0x%02x)\n", pc, op);
                run=0;
                break;
        }

        // x0 sempre zero
        x[0]=0;

        // avança pc
        pc += 4;

        // bounds básicos de memória de instruções (evita segfault se sair do range)
        if (pc < MEM_BASE || pc >= MEM_BASE + MEM_SIZE){
            // se quiser, trate como erro; aqui apenas encerra
            // printf("pc saiu do range: 0x%08x\n", pc);
            break;
        }
    }

    printf("--------------------------------------------------------------------------------\n");
    free(mem);
    return 0;
}
