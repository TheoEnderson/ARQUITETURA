#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define MEM_SIZE   (32 * 1024)
#define BASE_ADDR  0x80000000u

static inline const char* rname(int r) {
    static const char* x_label[32] = {
        "zero","ra","sp","gp","tp","t0","t1","t2",
        "s0","s1","a0","a1","a2","a3","a4","a5",
        "a6","a7","s2","s3","s4","s5","s6","s7",
        "s8","s9","s10","s11","t3","t4","t5","t6"
    };
    return x_label[r & 31];
}

// Helper para padronizar o alinhamento do output (tela + arquivo)
static void out2(FILE* output,
                 uint32_t pc, const char* mnem,
                 const char* ops, const char* msg)
{
    // %-6s  = mnem em 6 colunas
    // %-18s = operandos em 18 colunas
    printf ( "0x%08x:%-6s %-18s %s\n", pc, mnem, ops, msg );
    fprintf(output, "0x%08x:%-6s %-18s %s\n", pc, mnem, ops, msg );
}

int main(int argc, char* argv[]) {
    printf("--------------------------------------------------------------------------------\n");

    if (argc < 3) {
        fprintf(stderr, "uso: %s <input> <output>\n", argv[0]);
        return 1;
    }
    for (uint32_t i = 0; i < (uint32_t)argc; i++) {
        printf("argv[%u] = %s\n", i, argv[i]);
    }

    FILE* input  = fopen(argv[1], "r");
    FILE* output = fopen(argv[2], "w");
    if (!input || !output) {
        fprintf(stderr, "erro: não foi possível abrir arquivos\n");
        return 1;
    }

    // Memória e registradores
    uint8_t* mem = (uint8_t*)malloc(MEM_SIZE);
    if (!mem) {
        fprintf(stderr, "erro: malloc MEM_SIZE\n");
        return 1;
    }
    memset(mem, 0, MEM_SIZE);

    uint32_t x[32] = {0};
    uint32_t pc = BASE_ADDR;

    // Loader: linhas com "@<hexaddr>" seguidas por bytes hex
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
                if (load_addr >= BASE_ADDR && load_addr < BASE_ADDR + MEM_SIZE) {
                    mem[load_addr - BASE_ADDR] = (uint8_t)byte_val;
                }
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
                while (*p == ' ' || *p == '\t') p++;
                load_addr++;
            }
        }
        fseek(input, 0, SEEK_SET);
    }

    printf("--------------------------------------------------------------------------------\n");

    uint8_t running = 1;
    while (running) {
        // Fetch LE
        if (pc < BASE_ADDR || (pc - BASE_ADDR + 3u) >= MEM_SIZE) {
            printf("error: PC fora da memória em pc=0x%08x\n", pc);
            fprintf(output, "error: PC fora da memória em pc=0x%08x\n", pc);
            break;
        }
        uint32_t idx = pc - BASE_ADDR;
        uint32_t instruction =  (uint32_t)mem[idx]
                              | ((uint32_t)mem[idx+1] << 8)
                              | ((uint32_t)mem[idx+2] << 16)
                              | ((uint32_t)mem[idx+3] << 24);

        // Decode
        uint8_t  opcode =  instruction & 0x7F;
        uint8_t  rd     = (instruction >>  7) & 0x1F;
        uint8_t  funct3 = (instruction >> 12) & 0x07;
        uint8_t  rs1    = (instruction >> 15) & 0x1F;
        uint8_t  rs2    = (instruction >> 20) & 0x1F;
        uint8_t  funct7 = (instruction >> 25) & 0x7F;

        uint32_t imm12u = instruction >> 20;
        int32_t  imm12  = (imm12u & 0x800) ? (int32_t)(imm12u | 0xFFFFF000) : (int32_t)imm12u;

        uint32_t pc_curr = pc;
        uint32_t pc_next = pc + 4;

        switch (opcode) {

            // -------------------- Tipo R (inclui Extensão M) --------------------
            case 0b0110011: {
                // Lógicos / aritméticos básicos
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
                    uint32_t a = x[rs1] & x[rs2];
                    x[rd] = a;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "and", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b110) { // OR
                    uint32_t a = x[rs1] | x[rs2];
                    x[rd] = a;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "or", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b100) { // XOR
                    uint32_t a = x[rs1] ^ x[rs2];
                    x[rd] = a;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "xor", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b001) { // SLL
                    uint32_t sh = x[rs2] & 0x1F;
                    x[rd] = x[rs1] << sh;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "sll", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b101) { // SRL
                    uint32_t sh = x[rs2] & 0x1F;
                    x[rd] = (uint32_t)x[rs1] >> sh;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "srl", ops, msg);
                }
                else if (funct7 == 0b0100000 && funct3 == 0b101) { // SRA
                    uint32_t sh = x[rs2] & 0x1F;
                    x[rd] = ((int32_t)x[rs1]) >> sh;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "sra", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b010) { // SLT
                    x[rd] = ((int32_t)x[rs1] < (int32_t)x[rs2]) ? 1u : 0u;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=%u", rname(rd), x[rd]);
                    out2(output, pc_curr, "slt", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b011) { // SLTU
                    x[rd] = ((uint32_t)x[rs1] < (uint32_t)x[rs2]) ? 1u : 0u;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=%u", rname(rd), x[rd]);
                    out2(output, pc_curr, "sltu", ops, msg);
                }
                // ------ Extensão M ------
                else if (funct7 == 0b0000001 && funct3 == 0b000) { // MUL
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    int64_t p = (int64_t)a * (int64_t)b;
                    x[rd] = (uint32_t)p;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x*0x%08x=0x%08x",
                             rname(rd), (uint32_t)a, (uint32_t)b, x[rd]);
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
                else if (funct7 == 0b0000001 && funct3 == 0b100) { // DIV (signed)
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
                else if (funct7 == 0b0000001 && funct3 == 0b110) { // REM (signed)
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
                    printf("0x%08x:error  R-type desconhecido\n", pc_curr);
                    fprintf(output, "0x%08x:error  R-type desconhecido\n", pc_curr);
                    running = 0;
                }
                break;
            }

            // -------------------- Tipo I (ALU imediato) --------------------
            case 0b0010011: {
                if (funct3 == 0b000) { // ADDI
                    uint32_t rs1_before = x[rs1];
                    x[rd] = rs1_before + imm12;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=0x%08x+0x%08x=0x%08x",
                             rname(rd), rs1_before, (uint32_t)imm12, x[rd]);
                    out2(output, pc_curr, "addi", ops, msg);
                }
                else if (funct3 == 0b111) { // ANDI
                    x[rd] = x[rs1] & (uint32_t)imm12;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "andi", ops, msg);
                }
                else if (funct3 == 0b110) { // ORI
                    x[rd] = x[rs1] | (uint32_t)imm12;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "ori", ops, msg);
                }
                else if (funct3 == 0b100) { // XORI
                    x[rd] = x[rs1] ^ (uint32_t)imm12;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                    out2(output, pc_curr, "xori", ops, msg);
                }
                else if (funct3 == 0b010) { // SLTI
                    x[rd] = ((int32_t)x[rs1] < imm12) ? 1u : 0u;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=%u", rname(rd), x[rd]);
                    out2(output, pc_curr, "slti", ops, msg);
                }
                else if (funct3 == 0b011) { // SLTIU
                    x[rd] = ((uint32_t)x[rs1] < (uint32_t)imm12) ? 1u : 0u;
                    char ops[32], msg[64];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=%u", rname(rd), x[rd]);
                    out2(output, pc_curr, "sltiu", ops, msg);
                }
                else if (funct3 == 0b001) { // SLLI
                    uint32_t shamt = imm12u & 0x1F;
                    uint32_t f7    = (imm12u >> 5) & 0x7F;
                    if (f7 == 0b0000000) {
                        uint32_t before = x[rs1];
                        x[rd] = before << shamt;
                        char ops[32], msg[96];
                        snprintf(ops, sizeof(ops), "%s,%s,%u", rname(rd), rname(rs1), shamt);
                        snprintf(msg, sizeof(msg), "%s=0x%08x<<%u=0x%08x",
                                 rname(rd), before, shamt, x[rd]);
                        out2(output, pc_curr, "slli", ops, msg);
                    } else {
                        printf("0x%08x:error  funct7 inválido em SLLI (0x%02x)\n", pc_curr, f7);
                        fprintf(output, "0x%08x:error  funct7 inválido em SLLI (0x%02x)\n", pc_curr, f7);
                        running = 0;
                    }
                }
                else if (funct3 == 0b101) { // SRLI / SRAI
                    uint32_t shamt = imm12u & 0x1F;
                    uint32_t f7    = (imm12u >> 5) & 0x7F;
                    if (f7 == 0b0000000) { // SRLI
                        uint32_t before = x[rs1];
                        x[rd] = (uint32_t)before >> shamt;
                        char ops[32], msg[64];
                        snprintf(ops, sizeof(ops), "%s,%s,%u", rname(rd), rname(rs1), shamt);
                        snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                        out2(output, pc_curr, "srli", ops, msg);
                    } else if (f7 == 0b0100000) { // SRAI
                        int32_t before = (int32_t)x[rs1];
                        x[rd] = (uint32_t)(before >> shamt);
                        char ops[32], msg[64];
                        snprintf(ops, sizeof(ops), "%s,%s,%u", rname(rd), rname(rs1), shamt);
                        snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                        out2(output, pc_curr, "srai", ops, msg);
                    } else {
                        printf("0x%08x:error  funct7 inválido em SRLI/SRAI (0x%02x)\n", pc_curr, f7);
                        fprintf(output, "0x%08x:error  funct7 inválido em SRLI/SRAI (0x%02x)\n", pc_curr, f7);
                        running = 0;
                    }
                }
                else {
                    printf("0x%08x:error  I-type ALU desconhecido\n", pc_curr);
                    fprintf(output, "0x%08x:error  I-type ALU desconhecido\n", pc_curr);
                    running = 0;
                }
                break;
            }

            // -------------------- Loads (Tipo I) --------------------
            case 0b0000011: {
                uint32_t addr = x[rs1] + imm12;
                if (addr < BASE_ADDR || (addr - BASE_ADDR + 3u) >= MEM_SIZE) {
                    printf("0x%08x:error  load fora de memória (addr=0x%08x)\n", pc_curr, addr);
                    fprintf(output, "0x%08x:error  load fora de memória (addr=0x%08x)\n", pc_curr, addr);
                    running = 0;
                    break;
                }
                uint32_t aidx = addr - BASE_ADDR;

                if (funct3 == 0b000) { // LB
                    uint8_t b = mem[aidx];
                    x[rd] = (uint32_t)(int8_t)b;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%02x", rname(rd), addr, b);
                    out2(output, pc_curr, "lb", ops, msg);
                }
                else if (funct3 == 0b001) { // LH
                    uint16_t h = (uint16_t)mem[aidx] | ((uint16_t)mem[aidx+1] << 8);
                    x[rd] = (uint32_t)(int16_t)h;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%04x", rname(rd), addr, h);
                    out2(output, pc_curr, "lh", ops, msg);
                }
                else if (funct3 == 0b010) { // LW
                    uint32_t w =  (uint32_t)mem[aidx]
                                | ((uint32_t)mem[aidx+1] << 8)
                                | ((uint32_t)mem[aidx+2] << 16)
                                | ((uint32_t)mem[aidx+3] << 24);
                    x[rd] = w;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, w);
                    out2(output, pc_curr, "lw", ops, msg);
                }
                else if (funct3 == 0b100) { // LBU
                    uint8_t b = mem[aidx];
                    x[rd] = (uint32_t)b;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%02x", rname(rd), addr, b);
                    out2(output, pc_curr, "lbu", ops, msg);
                }
                else if (funct3 == 0b101) { // LHU
                    uint16_t h = (uint16_t)mem[aidx] | ((uint16_t)mem[aidx+1] << 8);
                    x[rd] = (uint32_t)h;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%04x", rname(rd), addr, h);
                    out2(output, pc_curr, "lhu", ops, msg);
                }
                else {
                    printf("0x%08x:error  load desconhecido\n", pc_curr);
                    fprintf(output, "0x%08x:error  load desconhecido\n", pc_curr);
                    running = 0;
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
                if (addr < BASE_ADDR || (addr - BASE_ADDR + 3u) >= MEM_SIZE) {
                    printf("0x%08x:error  store fora de memória (addr=0x%08x)\n", pc_curr, addr);
                    fprintf(output, "0x%08x:error  store fora de memória (addr=0x%08x)\n", pc_curr, addr);
                    running = 0;
                    break;
                }
                uint32_t aidx = addr - BASE_ADDR;

                if (funct3 == 0b000) { // SB
                    uint8_t b = (uint8_t)(x[rs2] & 0xFF);
                    mem[aidx] = b;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "mem[0x%08x]=0x%02x", addr, b);
                    out2(output, pc_curr, "sb", ops, msg);
                }
                else if (funct3 == 0b001) { // SH
                    uint16_t h = (uint16_t)(x[rs2] & 0xFFFF);
                    mem[aidx]     = (uint8_t)( h        & 0xFF);
                    mem[aidx + 1] = (uint8_t)((h >> 8)  & 0xFF);
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "mem[0x%08x]=0x%04x", addr, h);
                    out2(output, pc_curr, "sh", ops, msg);
                }
                else if (funct3 == 0b010) { // SW
                    uint32_t w = x[rs2];
                    mem[aidx]     = (uint8_t)( w        & 0xFF);
                    mem[aidx + 1] = (uint8_t)((w >> 8)  & 0xFF);
                    mem[aidx + 2] = (uint8_t)((w >> 16) & 0xFF);
                    mem[aidx + 3] = (uint8_t)((w >> 24) & 0xFF);
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "mem[0x%08x]=0x%08x", addr, w);
                    out2(output, pc_curr, "sw", ops, msg);
                }
                else {
                    printf("0x%08x:error  store desconhecido\n", pc_curr);
                    fprintf(output, "0x%08x:error  store desconhecido\n", pc_curr);
                    running = 0;
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
                    int taken = ((uint32_t)x[rs1] < (uint32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x<0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bltu", ops, msg);
                }
                else if (funct3 == 0b111) { // BGEU
                    int taken = ((uint32_t)x[rs1] >= (uint32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x>=0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bgeu", ops, msg);
                }
                else {
                    printf("0x%08x:error  branch desconhecido\n", pc_curr);
                    fprintf(output, "0x%08x:error  branch desconhecido\n", pc_curr);
                    running = 0;
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
                    printf("0x%08x:error  jalr funct3!=000\n", pc_curr);
                    fprintf(output, "0x%08x:error  jalr funct3!=000\n", pc_curr);
                    running = 0;
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
                if (funct3 == 0b000 && imm_sys == 0x001) { // EBREAK
                    out2(output, pc_curr, "ebreak", "", "");
                    running = 0;
                } else if (funct3 == 0b000 && imm_sys == 0x000) { // ECALL (opcional)
                    out2(output, pc_curr, "ecall", "", "");
                } else {
                    printf("0x%08x:error  system desconhecido\n", pc_curr);
                    fprintf(output, "0x%08x:error  system desconhecido\n", pc_curr);
                    running = 0;
                }
                break;
            }

            default:
                printf("0x%08x:error  opcode desconhecido (0x%02x)\n", pc_curr, opcode);
                fprintf(output, "0x%08x:error  opcode desconhecido (0x%02x)\n", pc_curr, opcode);
                running = 0;
                break;
        }

        // x0 é sempre zero
        x[0] = 0;
        // Avança PC
        pc = pc_next;
    }

    fclose(input);
    fclose(output);
    printf("--------------------------------------------------------------------------------\n");
    return 0;
}
