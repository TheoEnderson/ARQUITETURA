#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

// RAM
#define RAM_BASE   0x80000000u
#define RAM_SIZE   (32u * 1024u)

// Dispositivos mapeados em memória
#define IO_KBD_ADDR   0x10000000u  // teclado
#define IO_TTY_ADDR   0x10000002u  // tela

// CSRs básicos (modo máquina)
static uint32_t csr_mstatus = 0;
static uint32_t csr_mtvec   = 0;
static uint32_t csr_mepc    = 0;
static uint32_t csr_mcause  = 0;
static uint32_t csr_mtval   = 0;

enum {
    EXC_INST_FAULT       = 1,
    EXC_ILLEGAL_INST     = 2,
    EXC_LOAD_FAULT       = 5,
    EXC_STORE_FAULT      = 7,
    EXC_ENV_CALL_M       = 11,
};

static uint32_t csr_read(uint32_t addr) {
    switch (addr) {
        case 0x300: return csr_mstatus; // mstatus
        case 0x305: return csr_mtvec;   // mtvec
        case 0x341: return csr_mepc;    // mepc
        case 0x342: return csr_mcause;  // mcause
        case 0x343: return csr_mtval;   // mtval
        default:    return 0;           // CSR não implementado -> 0
    }
}

static void csr_write(uint32_t addr, uint32_t val) {
    switch (addr) {
        case 0x300: csr_mstatus = val; break;
        case 0x305: csr_mtvec   = val; break;
        case 0x341: csr_mepc    = val; break;
        case 0x342: csr_mcause  = val; break;
        case 0x343: csr_mtval   = val; break;
        default: /* ignora */  break;
    }
}

static const char* csr_name(uint32_t addr) {
    switch (addr) {
        case 0x300: return "mstatus";
        case 0x305: return "mtvec";
        case 0x341: return "mepc";
        case 0x342: return "mcause";
        case 0x343: return "mtval";
        default:    return NULL;  // se não conhecer, volta NULL
    }
}

static void raise_exception(uint32_t cause,
                            uint32_t epc,
                            uint32_t tval,
                            uint32_t *pc_next,
                            FILE *output)
{
    // Nome da exceção igual ao do arquivo de referência
    const char *name = "unknown";
    switch (cause) {
        case EXC_INST_FAULT:   name = "instruction_fault";   break;
        case EXC_ILLEGAL_INST: name = "illegal_instruction"; break;
        case EXC_LOAD_FAULT:   name = "load_fault";          break;
        case EXC_STORE_FAULT:  name = "store_fault";         break;
        case EXC_ENV_CALL_M:   name = "environment_call";    break;
    }

    // Atualiza CSRs
    csr_mcause = cause;
    csr_mepc   = epc;
    csr_mtval  = tval;

    // Atualiza mstatus conforme trap de modo máquina
    // MIE (bit 3), MPIE (bit 7), MPP (bits 12–11)
    uint32_t m = csr_mstatus;
    uint32_t mie  = (m >> 3) & 1u;

    m &= ~((1u << 3) | (1u << 7) | (3u << 11)); // zera MIE, MPIE, MPP
    m |= (mie << 7);        // MPIE <- MIE antigo
    m |= (3u  << 11);       // MPP  <- 3 (modo máquina)

    csr_mstatus = m;

    // PC aponta para mtvec (trap handler)
    *pc_next = csr_mtvec;

    // Imprime linha de exceção
    fprintf(output,
        ">exception:%-26s cause=0x%08x,epc=0x%08x,tval=0x%08x\n",
        name, cause, epc, tval);
}

static inline const char* rname(int r) {
    static const char* x_label[32] = {
        "zero","ra","sp","gp","tp","t0","t1","t2",
        "s0","s1","a0","a1","a2","a3","a4","a5",
        "a6","a7","s2","s3","s4","s5","s6","s7",
        "s8","s9","s10","s11","t3","t4","t5","t6"
    };
    return x_label[r & 31];
}

static void out2(FILE* output,
                 uint32_t pc, const char* mnem,
                 const char* ops, const char* msg)
{
    fprintf(output, "0x%08x:%-6s %-19s %s\n", pc, mnem, ops, msg );
}

// Leitura de 8 bits (byte) em endereço de memória ou IO
static uint8_t mem_read8(uint32_t addr, uint8_t *mem, int *ok) {
    // RAM normal
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) {
        return mem[addr - RAM_BASE];
    }
    // Teclado mapeado em memória (0x10000000)
    else if (addr == IO_KBD_ADDR) {
        // Lê um byte do stdin (bloqueante). Ideal para E/S programada
        int c = fgetc(stdin);
        if (c == EOF) {
            // Sem mais dados de entrada: pode devolver 0 ou manter o último valor
            return 0;
        }
        return (uint8_t)c;
    }
    // Endereço inválido
    else {
        *ok = 0;
        return 0;
    }
}

// Leitura de 16 bits (2 bytes) – usada por LH / LHU
static uint16_t mem_read16(uint32_t addr, uint8_t *mem, int *ok) {
    uint16_t lo = mem_read8(addr,     mem, ok);
    if (!*ok) return 0;
    uint16_t hi = mem_read8(addr + 1, mem, ok);
    if (!*ok) return 0;
    return (uint16_t)(lo | (hi << 8));
}

// Leitura de 32 bits (4 bytes) – usada por LW
static uint32_t mem_read32(uint32_t addr, uint8_t *mem, int *ok) {
    uint32_t b0 = mem_read8(addr,     mem, ok);
    if (!*ok) return 0;
    uint32_t b1 = mem_read8(addr + 1, mem, ok);
    if (!*ok) return 0;
    uint32_t b2 = mem_read8(addr + 2, mem, ok);
    if (!*ok) return 0;
    uint32_t b3 = mem_read8(addr + 3, mem, ok);
    if (!*ok) return 0;

    return  (b0)
          | (b1 << 8)
          | (b2 << 16)
          | (b3 << 24);
}

// Escrita de 8 bits – usada por SB
static void mem_write8(uint32_t addr, uint8_t value,
                       uint8_t *mem, int *ok) {
    // RAM normal
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) {
        mem[addr - RAM_BASE] = value;
    }
    // Tela/terminal mapeado em memória (0x10000002)
    else if (addr == IO_TTY_ADDR) {
        // Escreve um caractere no stdout (dispositivo de saída)
        fputc((int)value, stdout);
        fflush(stdout);
    }
    // Endereço inválido
    else {
        *ok = 0;
    }
}

// Escrita de 16 bits – usada por SH
static void mem_write16(uint32_t addr, uint16_t value,
                        uint8_t *mem, int *ok) {
    mem_write8(addr,     (uint8_t)( value       & 0xFF), mem, ok);
    if (!*ok) return;
    mem_write8(addr + 1, (uint8_t)((value >> 8) & 0xFF), mem, ok);
}

// Escrita de 32 bits – usada por SW
static void mem_write32(uint32_t addr, uint32_t value,
                        uint8_t *mem, int *ok) {
    mem_write8(addr,     (uint8_t)( value        & 0xFF), mem, ok);
    if (!*ok) return;
    mem_write8(addr + 1, (uint8_t)((value >> 8)  & 0xFF), mem, ok);
    if (!*ok) return;
    mem_write8(addr + 2, (uint8_t)((value >> 16) & 0xFF), mem, ok);
    if (!*ok) return;
    mem_write8(addr + 3, (uint8_t)((value >> 24) & 0xFF), mem, ok);
}


int main(int argc, char* argv[]) {
    if (argc < 3) return 1;

    FILE* input  = fopen(argv[1], "r");
    FILE* output = fopen(argv[2], "w");
    if (!input || !output) return 1;

    uint8_t* mem = (uint8_t*)malloc(RAM_SIZE);
    if (!mem) { fclose(input); fclose(output); return 1; }
    memset(mem, 0, RAM_SIZE);

    uint32_t x[32] = {0};
    uint32_t pc = RAM_BASE;

    // loader
    {
        uint32_t load_addr = 0;
        char line[4096];
        while (fgets(line, sizeof(line), input)) {
            if (line[0] == '@') {
                unsigned addr_hex = 0;
                if (sscanf(line + 1, "%x", &addr_hex) == 1) {
                    load_addr = (uint32_t)addr_hex;
                }
                continue;
            }
            char* p = line;
            unsigned byte_val;
            while (sscanf(p, "%x", &byte_val) == 1) {
                if (load_addr >= RAM_BASE && load_addr < RAM_BASE + RAM_SIZE) {
                    mem[load_addr - RAM_BASE] = (uint8_t)byte_val;
                }
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
                while (*p == ' ' || *p == '\t') p++;
                load_addr++;
            }
        }
        fseek(input, 0, SEEK_SET);
    }

    uint8_t running = 1;
    while (running) {
        int ok = 1;
        uint32_t pc_curr = pc;
        uint32_t pc_next = pc + 4;

        uint32_t instruction = mem_read32(pc, mem, &ok);
        if (!ok) {
            raise_exception(EXC_INST_FAULT, pc_curr, 0, &pc_next, output);
            goto end_of_loop;
        }

        uint8_t  opcode =  instruction & 0x7F;
        uint8_t  rd     = (instruction >>  7) & 0x1F;
        uint8_t  funct3 = (instruction >> 12) & 0x07;
        uint8_t  rs1    = (instruction >> 15) & 0x1F;
        uint8_t  rs2    = (instruction >> 20) & 0x1F;
        uint8_t  funct7 = (instruction >> 25) & 0x7F;

        uint32_t imm12u = instruction >> 20;
        int32_t  imm12  = (imm12u & 0x800) ? (int32_t)(imm12u | 0xFFFFF000) : (int32_t)imm12u;


        switch (opcode) {

            // -------------------- Tipo R (inclui Extensão M) --------------------
            case 0b0110011: {
                if (funct7 == 0b0000000 && funct3 == 0b000) { // ADD
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = a + b;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x+0x%08x=0x%08x", rname(rd), a, b, x[rd]);
                    out2(output, pc_curr, "add", ops, msg);
                }
                else if (funct7 == 0b0100000 && funct3 == 0b000) { // SUB
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = a - b;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x-0x%08x=0x%08x", rname(rd), a, b, x[rd]);
                    out2(output, pc_curr, "sub", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b111) { // AND
                    x[rd] = x[rs1] & x[rs2];
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x&0x%08x=0x%08x", rname(rd), x[rs1], x[rs2], x[rd]);
                    out2(output, pc_curr, "and", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b110) { // OR
                    x[rd] = x[rs1] | x[rs2];
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x|0x%08x=0x%08x", rname(rd), x[rs1], x[rs2], x[rd]);
                    out2(output, pc_curr, "or", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b100) { // XOR
                    x[rd] = x[rs1] ^ x[rs2];
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x^0x%08x=0x%08x", rname(rd), x[rs1], x[rs2], x[rd]);
                    out2(output, pc_curr, "xor", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b001) { // SLL
                    uint32_t sh = x[rs2] & 0x1F;
                    uint32_t before = x[rs1];
                    x[rd] = before << sh;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x<<%u=0x%08x", rname(rd), before, sh, x[rd]);
                    out2(output, pc_curr, "sll", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b101) { // SRL
                    uint32_t sh = x[rs2] & 0x1F;
                    uint32_t before = x[rs1];
                    x[rd] = (uint32_t)before >> sh;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x>>%u=0x%08x", rname(rd), before, sh, x[rd]);
                    out2(output, pc_curr, "srl", ops, msg);
                }
                else if (funct7 == 0b0100000 && funct3 == 0b101) { // SRA
                    uint32_t sh = x[rs2] & 0x1F;
                    int32_t before = (int32_t)x[rs1];
                    x[rd] = (uint32_t)(before >> sh);
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x>>%u=0x%08x", rname(rd), (uint32_t)before, sh, x[rd]);
                    out2(output, pc_curr, "sra", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b010) { // SLT
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = ((int32_t)a < (int32_t)b) ? 1u : 0u;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=(0x%08x<0x%08x)=%u", rname(rd), a, b, x[rd]);
                    out2(output, pc_curr, "slt", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b011) { // SLTU
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = (a < b) ? 1u : 0u;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=(0x%08x<0x%08x)=%u", rname(rd), a, b, x[rd]);
                    out2(output, pc_curr, "sltu", ops, msg);
                }
                // ------ Extensão M ------
                else if (funct7 == 0b0000001 && funct3 == 0b000) { // MUL
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    int64_t p = (int64_t)a * (int64_t)b;
                    x[rd] = (uint32_t)p;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x*0x%08x=0x%08x", rname(rd), (uint32_t)a, (uint32_t)b, x[rd]);
                    out2(output, pc_curr, "mul", ops, msg);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b001) { // MULH
                    int64_t a = (int64_t)(int32_t)x[rs1];
                    int64_t b = (int64_t)(int32_t)x[rs2];
                    int64_t prod = a * b;
                    x[rd] = (uint32_t)((uint64_t)prod >> 32);
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x*0x%08x=0x%08x",
                             rname(rd), (uint32_t)(int32_t)a, (uint32_t)(int32_t)b, x[rd]);
                    out2(output, pc_curr, "mulh", ops, msg);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b010) { // MULHSU
                    int64_t  a = (int64_t)(int32_t)x[rs1];
                    uint64_t b = (uint64_t)x[rs2];
                    __int128 prod = (__int128)a * (__int128)b;
                    x[rd] = (uint32_t)((prod >> 32) & 0xFFFFFFFF);
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x*0x%08x=0x%08x",
                             rname(rd), (uint32_t)(int32_t)a, (uint32_t)b, x[rd]);
                    out2(output, pc_curr, "mulhsu", ops, msg);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b011) { // MULHU
                    uint64_t a = (uint64_t)x[rs1], b = (uint64_t)x[rs2];
                    uint64_t prod = a * b;
                    x[rd] = (uint32_t)(prod >> 32);
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x*0x%08x=0x%08x",
                             rname(rd), (uint32_t)a, (uint32_t)b, x[rd]);
                    out2(output, pc_curr, "mulhu", ops, msg);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b100) { // DIV
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    uint32_t res;
                    if (b == 0) res = 0xFFFFFFFFu;
                    else if (a == INT32_MIN && b == -1) res = (uint32_t)INT32_MIN;
                    else res = (uint32_t)(a / b);
                    x[rd] = res;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x/0x%08x=0x%08x",
                             rname(rd), (uint32_t)a, (uint32_t)b, x[rd]);
                    out2(output, pc_curr, "div", ops, msg);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b101) { // DIVU
                    uint32_t a = x[rs1], b = x[rs2];
                    uint32_t res = (b == 0) ? 0xFFFFFFFFu : (a / b);
                    x[rd] = res;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x/0x%08x=0x%08x", rname(rd), a, b, x[rd]);
                    out2(output, pc_curr, "divu", ops, msg);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b110) { // REM
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    uint32_t res;
                    if (b == 0) res = (uint32_t)a;
                    else if (a == INT32_MIN && b == -1) res = 0u;
                    else res = (uint32_t)(a % b);
                    x[rd] = res;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x%%0x%08x=0x%08x",
                             rname(rd), (uint32_t)a, (uint32_t)b, x[rd]);
                    out2(output, pc_curr, "rem", ops, msg);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b111) { // REMU
                    uint32_t a = x[rs1], b = x[rs2];
                    uint32_t res = (b == 0) ? a : (a % b);
                    x[rd] = res;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x%%0x%08x=0x%08x", rname(rd), a, b, x[rd]);
                    out2(output, pc_curr, "remu", ops, msg);
                }
                else {
                    uint32_t pc_exc = pc_curr + 4;
                    raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                    pc_next = pc_exc;
                    goto end_of_loop;
                }
                break;
            }

            // -------------------- Tipo I (ALU imediato) --------------------
            case 0b0010011: {
                if (funct3 == 0b000) { // ADDI
                    uint32_t before = x[rs1];
                    x[rd] = before + imm12;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=0x%08x+0x%08x=0x%08x",
                             rname(rd), before, (uint32_t)imm12, x[rd]);
                    out2(output, pc_curr, "addi", ops, msg);
                }
                else if (funct3 == 0b111) { // ANDI
                    uint32_t before = x[rs1];
                    x[rd] = before & (uint32_t)imm12;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=0x%08x&0x%08x=0x%08x", rname(rd), before, (uint32_t)imm12, x[rd]);
                    out2(output, pc_curr, "andi", ops, msg);
                }
                else if (funct3 == 0b110) { // ORI
                    uint32_t before = x[rs1];
                    x[rd] = before | (uint32_t)imm12;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=0x%08x|0x%08x=0x%08x", rname(rd), before, (uint32_t)imm12, x[rd]);
                    out2(output, pc_curr, "ori", ops, msg);
                }
                else if (funct3 == 0b100) { // XORI
                    uint32_t before = x[rs1];
                    x[rd] = before ^ (uint32_t)imm12;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=0x%08x^0x%08x=0x%08x", rname(rd), before, (uint32_t)imm12, x[rd]);
                    out2(output, pc_curr, "xori", ops, msg);
                }
                else if (funct3 == 0b010) { // SLTI (signed)
                    uint32_t before = x[rs1];
                    x[rd] = ((int32_t)before < imm12) ? 1u : 0u;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=(0x%08x<0x%08x)=%u",
                             rname(rd), before, (uint32_t)imm12, x[rd]);
                    out2(output, pc_curr, "slti", ops, msg);
                }
                else if (funct3 == 0b011) { // SLTIU (unsigned)
                    uint32_t before = x[rs1];
                    x[rd] = (before < (uint32_t)imm12) ? 1u : 0u;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=(0x%08x<0x%08x)=%u",
                             rname(rd), before, (uint32_t)imm12, x[rd]);
                    out2(output, pc_curr, "sltiu", ops, msg);
                }
                else if (funct3 == 0b001) { // SLLI
                    uint32_t shamt = imm12u & 0x1F;
                    uint32_t f7    = (imm12u >> 5) & 0x7F;
                    if (f7 == 0b0000000) {
                        uint32_t before = x[rs1];
                        x[rd] = before << shamt;
                        char ops[32], msg[128];
                        snprintf(ops, sizeof(ops), "%s,%s,%u", rname(rd), rname(rs1), shamt);
                        snprintf(msg, sizeof(msg), "%s=0x%08x<<%u=0x%08x", rname(rd), before, shamt, x[rd]);
                        out2(output, pc_curr, "slli", ops, msg);
                    } else {
                        uint32_t pc_exc = pc_curr + 4;
                        raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                        pc_next = pc_exc;
                        goto end_of_loop;
                    }
                }
                else if (funct3 == 0b101) { // SRLI / SRAI
                    uint32_t shamt = imm12u & 0x1F;
                    uint32_t f7    = (imm12u >> 5) & 0x7F;
                    if (f7 == 0b0000000) { // SRLI
                        uint32_t before = x[rs1];
                        x[rd] = (uint32_t)before >> shamt;
                        char ops[32], msg[128];
                        snprintf(ops, sizeof(ops), "%s,%s,%u", rname(rd), rname(rs1), shamt);
                        snprintf(msg, sizeof(msg), "%s=0x%08x>>%u=0x%08x", rname(rd), before, shamt, x[rd]);
                        out2(output, pc_curr, "srli", ops, msg);
                    } else if (f7 == 0b0100000) { // SRAI
                        int32_t before = (int32_t)x[rs1];
                        x[rd] = (uint32_t)(before >> shamt);
                        char ops[32], msg[128];
                        snprintf(ops, sizeof(ops), "%s,%s,%u", rname(rd), rname(rs1), shamt);
                        snprintf(msg, sizeof(msg), "%s=0x%08x>>%u=0x%08x", rname(rd), (uint32_t)before, shamt, x[rd]);
                        out2(output, pc_curr, "srai", ops, msg);
                    } else {
                        uint32_t pc_exc = pc_curr + 4;
                        raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                        pc_next = pc_exc;
                        goto end_of_loop;
                    }
                }
                else {
                    uint32_t pc_exc = pc_curr + 4;
                    raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                    pc_next = pc_exc;
                    goto end_of_loop;
                }
                break;
            }

            // -------------------- Loads (Tipo I) --------------------
            case 0b0000011: {
                uint32_t addr = x[rs1] + imm12;
                int ok = 1;  // vai ser zerado se o endereço for inválido

                if (funct3 == 0b000) { // LB (sign-extend)
                    uint8_t b = mem_read8(addr, mem, &ok);
                    if (!ok) {
                        raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output);
                        goto end_of_loop;  // ou um flag pra pular o "pc = pc_next" padrão
                    }
                    x[rd] = (uint32_t)(int8_t)b;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lb", ops, msg);
                }
                else if (funct3 == 0b001) { // LH (sign-extend)
                    uint16_t h = mem_read16(addr, mem, &ok);
                    if (!ok) {
                        raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output);
                        goto end_of_loop;  // ou um flag pra pular o "pc = pc_next" padrão
                    }
                    x[rd] = (uint32_t)(int16_t)h;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lh", ops, msg);
                }
                else if (funct3 == 0b010) { // LW
                    uint32_t w = mem_read32(addr, mem, &ok);
                    if (!ok) {
                        raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output);
                        goto end_of_loop;  // ou um flag pra pular o "pc = pc_next" padrão
                    }
                    x[rd] = w;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lw", ops, msg);
                }
                else if (funct3 == 0b100) { // LBU (zero-extend)
                    uint8_t b = mem_read8(addr, mem, &ok);
                    if (!ok) {
                        raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output);
                        goto end_of_loop;  // ou um flag pra pular o "pc = pc_next" padrão
                    }
                    x[rd] = (uint32_t)b;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lbu", ops, msg);
                }
                else if (funct3 == 0b101) { // LHU (zero-extend)
                    uint16_t h = mem_read16(addr, mem, &ok);
                    if (!ok) {
                        raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output);
                        goto end_of_loop;  // ou um flag pra pular o "pc = pc_next" padrão
                    }
                    x[rd] = (uint32_t)h;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lhu", ops, msg);
                }
                else {
                    uint32_t pc_exc = pc_curr + 4;
                    raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                    pc_next = pc_exc;
                    goto end_of_loop;
                }
                break;
            }

            // -------------------- Stores (Tipo S) --------------------
            case 0b0100011: {
                uint32_t imm_high = (instruction >> 25) & 0x7F;
                uint32_t imm_low  = (instruction >> 7)  & 0x1F;
                uint32_t imm12uS  = (imm_high << 5) | imm_low;
                int32_t  immS     = (imm12uS & 0x800) ? (int32_t)(imm12uS | 0xFFFFF000) : (int32_t)imm12uS;

                uint32_t addr = x[rs1] + immS;
                int ok = 1;

                if (funct3 == 0b000) { // SB
                    uint8_t b = (uint8_t)(x[rs2] & 0xFF);
                    mem_write8(addr, b, mem, &ok);
                    if (!ok) {
                        uint32_t pc_exc = pc_curr + 4;
                        raise_exception(EXC_STORE_FAULT, pc_curr, addr, &pc_exc, output);
                        pc_next = pc_exc;
                        goto end_of_loop;
                    }
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "mem[0x%08x]=0x%02x", addr, b);
                    out2(output, pc_curr, "sb", ops, msg);
                }
                else if (funct3 == 0b001) { // SH
                    uint16_t h = (uint16_t)(x[rs2] & 0xFFFF);
                    mem_write16(addr, h, mem, &ok);
                    if (!ok) {
                        uint32_t pc_exc = pc_curr + 4;
                        raise_exception(EXC_STORE_FAULT, pc_curr, addr, &pc_exc, output);
                        pc_next = pc_exc;
                        goto end_of_loop;
                    }
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "mem[0x%08x]=0x%04x", addr, h);
                    out2(output, pc_curr, "sh", ops, msg);
                }
                else if (funct3 == 0b010) { // SW
                    uint32_t w = x[rs2];
                    mem_write32(addr, w, mem, &ok);
                    if (!ok) {
                        uint32_t pc_exc = pc_curr + 4;
                        raise_exception(EXC_STORE_FAULT, pc_curr, addr, &pc_exc, output);
                        pc_next = pc_exc;
                        goto end_of_loop;
                    }
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "mem[0x%08x]=0x%08x", addr, w);
                    out2(output, pc_curr, "sw", ops, msg);
                }
                else {
                    uint32_t pc_exc = pc_curr + 4;
                    raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                    pc_next = pc_exc;
                    goto end_of_loop;
                }
                break;
            }

            // -------------------- Branches (Tipo B) --------------------
            case 0b1100011: {
                uint32_t imm_12   = (instruction >> 31) & 0x1;
                uint32_t imm_10_5 = (instruction >> 25) & 0x3F;
                uint32_t imm_4_1  = (instruction >> 8)  & 0xF;
                uint32_t imm_11   = (instruction >> 7)  & 0x1;

                uint32_t imm13u = (imm_12   << 12)
                                  | (imm_11   << 11)
                                  | (imm_10_5 << 5 )
                                  | (imm_4_1  << 1);
                int32_t  immB   = (imm13u & 0x1000) ? (int32_t)(imm13u | 0xFFFFE000) : (int32_t)imm13u;
                uint32_t target = pc_curr + immB;
                uint32_t shown_off = (imm13u >> 1) & 0xFFF;

                if (funct3 == 0b000) { // BEQ
                    int taken = (x[rs1] == x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x==0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "beq", ops, msg);
                }
                else if (funct3 == 0b001) { // BNE
                    int taken = (x[rs1] != x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x!=0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bne", ops, msg);
                }
                else if (funct3 == 0b100) { // BLT
                    int taken = ((int32_t)x[rs1] < (int32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x<0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "blt", ops, msg);
                }
                else if (funct3 == 0b101) { // BGE
                    int taken = ((int32_t)x[rs1] >= (int32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x>=0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bge", ops, msg);
                }
                else if (funct3 == 0b110) { // BLTU
                    int taken = (x[rs1] < x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x<0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bltu", ops, msg);
                }
                else if (funct3 == 0b111) { // BGEU
                    int taken = ( x[rs1] >= x[rs2] );
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x>=0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bgeu", ops, msg);
                }
                else {
                    uint32_t pc_exc = pc_curr + 4;
                    raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                    pc_next = pc_exc;
                    goto end_of_loop;
                }
                break;
            }

            // -------------------- JALR (Tipo I) --------------------
            case 0b1100111: {
                if (funct3 == 0b000) {
                    uint32_t rs1_before = x[rs1];
                    uint32_t ret = pc_curr + 4;
                    uint32_t target = (rs1_before + imm12) & ~1u;
                    x[rd] = ret;
                    char ops[32], msg[128];
                    uint32_t imm12z = (uint32_t)(imm12 & 0xFFF);
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), imm12z);
                    snprintf(msg, sizeof(msg), "pc=0x%08x+0x%08x,%s=0x%08x",
                             rs1_before, (uint32_t)imm12, rname(rd), ret);
                    out2(output, pc_curr, "jalr", ops, msg);
                    pc_next = target;
                } else {
                    uint32_t pc_exc = pc_curr + 4;
                    raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                    pc_next = pc_exc;
                    goto end_of_loop;
                }
                break;
            }

            // -------------------- JAL (Tipo J) --------------------
            case 0b1101111: {
                uint32_t imm_20    = (instruction >> 31) & 0x1;
                uint32_t imm_10_1  = (instruction >> 21) & 0x3ff;
                uint32_t imm_11    = (instruction >> 20) & 0x1;
                uint32_t imm_19_12 = (instruction >> 12) & 0xff;

                uint32_t imm21u = (imm_20 << 20) | (imm_19_12 << 12) | (imm_11 << 11) | (imm_10_1 << 1);
                int32_t  immJ   = (imm21u & 0x00100000) ? (int32_t)(imm21u | 0xffe00000) : (int32_t)imm21u;

                uint32_t ret = pc_curr + 4;
                uint32_t target = pc_curr + immJ;
                x[rd] = ret;

                char ops[32], msg[96];
                snprintf(ops, sizeof(ops), "%s,0x%05x", rname(rd), (imm21u >> 1) & 0xfffff);
                snprintf(msg, sizeof(msg), "pc=0x%08x,%s=0x%08x", target, rname(rd), ret);
                out2(output, pc_curr, "jal", ops, msg);

                pc_next = target;
                break;
            }

            // -------------------- LUI (Tipo U) --------------------
            case 0b0110111: {
                uint32_t imm20 = instruction & 0xFFFFF000;
                x[rd] = imm20;
                char ops[32], msg[64];
                snprintf(ops, sizeof(ops), "%s,0x%05x", rname(rd), (imm20 >> 12));
                snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                out2(output, pc_curr, "lui", ops, msg);
                break;
            }

            // -------------------- AUIPC (Tipo U) --------------------
            case 0b0010111: {
                uint32_t imm20 = instruction & 0xFFFFF000;
                x[rd] = pc_curr + imm20;
                char ops[32], msg[96];
                snprintf(ops, sizeof(ops), "%s,0x%05x", rname(rd), (imm20 >> 12));
                snprintf(msg, sizeof(msg), "%s=0x%08x+0x%08x=0x%08x",
                         rname(rd), pc_curr, imm20, x[rd]);
                out2(output, pc_curr, "auipc", ops, msg);
                break;
            }

            // -------------------- System (ECALL / EBREAK) --------------------
            case 0b1110011: {
                uint32_t imm_sys = instruction >> 20;

                if (funct3 == 0b000) {
                    // ECALL / EBREAK / MRET (SYSTEM "imediato")
                    if (imm_sys == 0x000) {          // ECALL
                        out2(output, pc_curr, "ecall", "", "");
                        uint32_t pc_next_exc = pc_curr + 4;
                        // environment_call from M-mode
                        raise_exception(EXC_ENV_CALL_M, pc_curr, 0, &pc_next_exc, output);
                        pc_next = pc_next_exc;
                    }
                    else if (imm_sys == 0x001) {     // EBREAK
                        out2(output, pc_curr, "ebreak", "", "");
                        running = 0;
                    }
                    else if (imm_sys == 0x302) {     // MRET
                        // Sem ops na saída
                        char msg[64];
                        // PC de retorno é mepc
                        pc_next = csr_mepc;

                        // Atualiza mstatus conforme mret
                        uint32_t m = csr_mstatus;
                        uint32_t mpie = (m >> 7) & 1u;
                        // MIE <- MPIE; MPIE <- 1; MPP <- 0
                        m &= ~((1u << 3) | (1u << 7) | (3u << 11));
                        m |= (mpie << 3);
                        m |= (1u << 7);
                        csr_mstatus = m;

                        snprintf(msg, sizeof(msg), "pc=0x%08x", pc_next);
                        out2(output, pc_curr, "mret", "", msg);
                    }
                    else {
                        uint32_t pc_exc = pc_curr + 4;
                        raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                        pc_next = pc_exc;
                        goto end_of_loop;
                    }
                }
                else {
                    // Instruções CSR registrador-registrador
                    uint32_t csr_addr = instruction >> 20;
                    uint32_t old = csr_read(csr_addr);

                    const char *csrnm = csr_name(csr_addr);
                    char csrbuf[16];
                    if (!csrnm) {
                        snprintf(csrbuf, sizeof(csrbuf), "0x%03x", csr_addr);
                        csrnm = csrbuf;
                    }

                    if (funct3 == 0b001) { // CSRRW
                        uint32_t rs1_val = x[rs1];
                        if (rd != 0) x[rd] = old;
                        csr_write(csr_addr, rs1_val);

                        char ops[32], msg[96];
                        // esperado: csrrw  zero,mtvec,t0
                        snprintf(ops, sizeof(ops), "%s,%s,%s",
                                rname(rd), csrnm, rname(rs1));
                        // esperado: zero=mtvec=0x00000000,mtvec=t0=0x80000004
                        snprintf(msg, sizeof(msg), "%s=%s=0x%08x,%s=%s=0x%08x",
                                rname(rd), csrnm, old,
                                csrnm, rname(rs1), rs1_val);

                        out2(output, pc_curr, "csrrw", ops, msg);
                    }
                    else if (funct3 == 0b010) { // CSRRS
                        uint32_t rs1_val = x[rs1];
                        uint32_t new_csr = old;
                        if (rs1 != 0) {
                            new_csr = old | rs1_val;
                            csr_write(csr_addr, new_csr);
                        }
                        if (rd != 0) x[rd] = old;

                        char ops[32], msg[160];
                        // esperado: csrrs  a0,mcause,zero
                        snprintf(ops, sizeof(ops), "%s,%s,%s",
                                rname(rd), csrnm, rname(rs1));

                        // esperado: a0=mcause=0x...,mcause|=zero=0x...|0x...=0x...
                        snprintf(msg, sizeof(msg),
                                "%s=%s=0x%08x,%s|=%s=0x%08x|0x%08x=0x%08x",
                                rname(rd), csrnm, old,
                                csrnm, rname(rs1), old, rs1_val, new_csr);

                        out2(output, pc_curr, "csrrs", ops, msg);
                    }
                    else {
                        uint32_t pc_exc = pc_curr + 4;
                        raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                        pc_next = pc_exc;
                        goto end_of_loop;
                    }
                }
                break;
            }

            default: {
                uint32_t pc_exc = pc_curr + 4;
                raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                pc_next = pc_exc;   // quem vale é este
                goto end_of_loop;
            }
        }
        end_of_loop:
        // x0 sempre zero (estado arquitetural)
        x[0] = 0;
        pc = pc_next;
    }

    fclose(input);
    fclose(output);
    free(mem);
    return 0;
}
