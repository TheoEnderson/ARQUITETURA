#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define MEM_SIZE (32 * 1024)
#define BASE_ADDR 0x80000000u

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

    // Macro de log: escreve no stdout e no arquivo de saída
    #define LOG(...) do { printf(__VA_ARGS__); fprintf(output, __VA_ARGS__); } while(0)

    // Memória e regs
    uint8_t* mem = (uint8_t*)malloc(MEM_SIZE);
    if (!mem) {
        fprintf(stderr, "erro: malloc MEM_SIZE\n");
        return 1;
    }
    memset(mem, 0, MEM_SIZE);

    uint32_t x[32] = {0};
    uint32_t pc = BASE_ADDR;

    // Loader simples: linhas "@<hexaddr>" e linhas com bytes "AA BB CC ..."
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
                // avança p
                while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
                while (*p == ' ' || *p == '\t') p++;
                load_addr++;
            }
        }
        // reposiciona arquivo de entrada para possíveis leituras futuras (não usadas)
        fseek(input, 0, SEEK_SET);
    }

    printf("--------------------------------------------------------------------------------\n");

    uint8_t running = 1;
    while (running) {
        // Fetch LE seguro
        if (pc < BASE_ADDR || pc + 3u < BASE_ADDR || (pc - BASE_ADDR + 3u) >= MEM_SIZE) {
            LOG("error: PC fora da memória em pc=0x%08X\n", pc);
            break;
        }
        uint32_t idx = pc - BASE_ADDR;
        uint32_t instruction =  (uint32_t)mem[idx]
                              | ((uint32_t)mem[idx+1] << 8)
                              | ((uint32_t)mem[idx+2] << 16)
                              | ((uint32_t)mem[idx+3] << 24);

        // Decode comum
        uint8_t  opcode =  instruction & 0x7F;
        uint8_t  rd     = (instruction >>  7) & 0x1F;
        uint8_t  funct3 = (instruction >> 12) & 0x07;
        uint8_t  rs1    = (instruction >> 15) & 0x1F;
        uint8_t  rs2    = (instruction >> 20) & 0x1F;
        uint8_t  funct7 = (instruction >> 25) & 0x7F;

        // Imediato I genérico
        uint32_t imm12u = instruction >> 20;
        int32_t  imm12  = (imm12u & 0x800) ? (int32_t)(imm12u | 0xFFFFF000) : (int32_t)imm12u;

        // Próximo PC padrão
        uint32_t pc_next = pc + 4;

        switch (opcode) {
            // -------------------- Tipo R (inclui Extensão M) --------------------
            case 0b0110011: {
                if (funct7 == 0b0000000 && funct3 == 0b000) { // ADD
                    x[rd] = x[rs1] + x[rs2];
                    LOG("add x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0100000 && funct3 == 0b000) { // SUB
                    x[rd] = x[rs1] - x[rs2];
                    LOG("sub x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b111) { // AND
                    x[rd] = x[rs1] & x[rs2];
                    LOG("and x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b110) { // OR
                    x[rd] = x[rs1] | x[rs2];
                    LOG("or  x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b100) { // XOR
                    x[rd] = x[rs1] ^ x[rs2];
                    LOG("xor x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b001) { // SLL
                    x[rd] = x[rs1] << (x[rs2] & 0x1F);
                    LOG("sll x%d, x%d, x%d -> x%d = 0x%08X (sh=%d)\n", rd, rs1, rs2, rd, x[rd], x[rs2] & 0x1F);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b101) { // SRL
                    x[rd] = (uint32_t)x[rs1] >> (x[rs2] & 0x1F);
                    LOG("srl x%d, x%d, x%d -> x%d = 0x%08X (sh=%d)\n", rd, rs1, rs2, rd, x[rd], x[rs2] & 0x1F);
                }
                else if (funct7 == 0b0100000 && funct3 == 0b101) { // SRA
                    x[rd] = ((int32_t)x[rs1]) >> (x[rs2] & 0x1F);
                    LOG("sra x%d, x%d, x%d -> x%d = 0x%08X (sh=%d)\n", rd, rs1, rs2, rd, x[rd], x[rs2] & 0x1F);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b010) { // SLT
                    x[rd] = ((int32_t)x[rs1] < (int32_t)x[rs2]) ? 1u : 0u;
                    LOG("slt x%d, x%d, x%d -> x%d = %u\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b011) { // SLTU
                    x[rd] = ((uint32_t)x[rs1] < (uint32_t)x[rs2]) ? 1u : 0u;
                    LOG("sltu x%d, x%d, x%d -> x%d = %u\n", rd, rs1, rs2, rd, x[rd]);
                }
                // ------ Extensão M ------
                else if (funct7 == 0b0000001 && funct3 == 0b000) { // MUL
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    int64_t p = (int64_t)a * (int64_t)b;
                    x[rd] = (uint32_t)p;
                    LOG("mul x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b001) { // MULH
                    int64_t a = (int64_t)(int32_t)x[rs1];
                    int64_t b = (int64_t)(int32_t)x[rs2];
                    int64_t prod = a * b;
                    x[rd] = (uint32_t)((uint64_t)prod >> 32);
                    LOG("mulh x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b010) { // MULHSU
                    int64_t  a = (int64_t)(int32_t)x[rs1];
                    uint64_t b = (uint64_t)x[rs2];
                    __int128 prod = (__int128)a * (__int128)b;
                    x[rd] = (uint32_t)(((__int128)prod >> 32) & 0xFFFFFFFF);
                    LOG("mulhsu x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b011) { // MULHU
                    uint64_t a = (uint64_t)x[rs1], b = (uint64_t)x[rs2];
                    uint64_t prod = a * b;
                    x[rd] = (uint32_t)(prod >> 32);
                    LOG("mulhu x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b100) { // DIV
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    uint32_t res;
                    if (b == 0) res = 0xFFFFFFFFu;
                    else if (a == INT32_MIN && b == -1) res = (uint32_t)INT32_MIN;
                    else res = (uint32_t)(a / b);
                    x[rd] = res;
                    LOG("div x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b101) { // DIVU
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = (b == 0) ? 0xFFFFFFFFu : (a / b);
                    LOG("divu x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b110) { // REM
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    uint32_t res;
                    if (b == 0) res = (uint32_t)a;
                    else if (a == INT32_MIN && b == -1) res = 0;
                    else res = (uint32_t)(a % b);
                    x[rd] = res;
                    LOG("rem x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b111) { // REMU
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = (b == 0) ? a : (a % b);
                    LOG("remu x%d, x%d, x%d -> x%d = 0x%08X\n", rd, rs1, rs2, rd, x[rd]);
                }
                else {
                    LOG("error: R-type desconhecido em pc=0x%08X\n", pc);
                    running = 0;
                }
                break;
            }

            // -------------------- Tipo I (ALU imediato) --------------------
            case 0b0010011: {
                if (funct3 == 0b000) { // ADDI
                    x[rd] = x[rs1] + imm12;
                    LOG("addi x%d, x%d, %d -> x%d = 0x%08X\n", rd, rs1, imm12, rd, x[rd]);
                }
                else if (funct3 == 0b111) { // ANDI
                    x[rd] = x[rs1] & (uint32_t)imm12;
                    LOG("andi x%d, x%d, 0x%X -> x%d = 0x%08X\n", rd, rs1, imm12, rd, x[rd]);
                }
                else if (funct3 == 0b110) { // ORI
                    x[rd] = x[rs1] | (uint32_t)imm12;
                    LOG("ori  x%d, x%d, 0x%X -> x%d = 0x%08X\n", rd, rs1, imm12, rd, x[rd]);
                }
                else if (funct3 == 0b100) { // XORI
                    x[rd] = x[rs1] ^ (uint32_t)imm12;
                    LOG("xori x%d, x%d, 0x%X -> x%d = 0x%08X\n", rd, rs1, imm12, rd, x[rd]);
                }
                else if (funct3 == 0b010) { // SLTI
                    x[rd] = ((int32_t)x[rs1] < imm12) ? 1u : 0u;
                    LOG("slti x%d, x%d, %d -> x%d = %u\n", rd, rs1, imm12, rd, x[rd]);
                }
                else if (funct3 == 0b011) { // SLTIU
                    x[rd] = ((uint32_t)x[rs1] < (uint32_t)imm12) ? 1u : 0u;
                    LOG("sltiu x%d, x%d, %d -> x%d = %u\n", rd, rs1, imm12, rd, x[rd]);
                }
                else if (funct3 == 0b001) { // SLLI
                    uint32_t shamt = imm12u & 0x1F;
                    uint32_t f7 = (imm12u >> 5) & 0x7F;
                    if (f7 == 0b0000000) {
                        x[rd] = x[rs1] << shamt;
                        LOG("slli x%d, x%d, %u -> x%d = 0x%08X\n", rd, rs1, shamt, rd, x[rd]);
                    } else {
                        LOG("Erro: funct7 inválido em SLLI (0x%X)\n", f7);
                    }
                }
                else if (funct3 == 0b101) { // SRLI/SRAI
                    uint32_t shamt = imm12u & 0x1F;
                    uint32_t f7 = (imm12u >> 5) & 0x7F;
                    if (f7 == 0b0000000) { // SRLI
                        x[rd] = (uint32_t)x[rs1] >> shamt;
                        LOG("srli x%d, x%d, %u -> x%d = 0x%08X\n", rd, rs1, shamt, rd, x[rd]);
                    } else if (f7 == 0b0100000) { // SRAI
                        x[rd] = ((int32_t)x[rs1]) >> shamt;
                        LOG("srai x%d, x%d, %u -> x%d = 0x%08X\n", rd, rs1, shamt, rd, x[rd]);
                    } else {
                        LOG("Erro: funct7 inválido em SRLI/SRAI (0x%X)\n", f7);
                    }
                }
                else {
                    LOG("error: I-type ALU desconhecido em pc=0x%08X\n", pc);
                    running = 0;
                }
                break;
            }

            // -------------------- Loads (Tipo I) --------------------
            case 0b0000011: {
                uint32_t addr = x[rs1] + imm12;
                uint32_t aidx = addr - BASE_ADDR;

                if (funct3 == 0b000) { // LB
                    uint8_t b = mem[aidx];
                    x[rd] = (uint32_t)(int8_t)b;
                    LOG("lb  x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%08X\n", rd, imm12, rs1, addr, rd, x[rd]);
                }
                else if (funct3 == 0b001) { // LH
                    uint16_t h = (uint16_t)mem[aidx] | ((uint16_t)mem[aidx+1] << 8);
                    x[rd] = (uint32_t)(int16_t)h;
                    LOG("lh  x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%08X\n", rd, imm12, rs1, addr, rd, x[rd]);
                }
                else if (funct3 == 0b010) { // LW
                    uint32_t w =  (uint32_t)mem[aidx]
                                | ((uint32_t)mem[aidx+1] << 8)
                                | ((uint32_t)mem[aidx+2] << 16)
                                | ((uint32_t)mem[aidx+3] << 24);
                    x[rd] = w;
                    LOG("lw  x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%08X\n", rd, imm12, rs1, addr, rd, x[rd]);
                }
                else if (funct3 == 0b100) { // LBU
                    uint8_t b = mem[aidx];
                    x[rd] = (uint32_t)b;
                    LOG("lbu x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%02X\n", rd, imm12, rs1, addr, rd, x[rd]);
                }
                else if (funct3 == 0b101) { // LHU
                    uint16_t h = (uint16_t)mem[aidx] | ((uint16_t)mem[aidx+1] << 8);
                    x[rd] = (uint32_t)h;
                    LOG("lhu x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%04X\n", rd, imm12, rs1, addr, rd, x[rd]);
                }
                else {
                    LOG("error: load desconhecido em pc=0x%08X\n", pc);
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
                uint32_t aidx = addr - BASE_ADDR;

                if (funct3 == 0b000) { // SB
                    mem[aidx] = (uint8_t)(x[rs2] & 0xFF);
                    LOG("sb  x%d, %d(x%d) [addr=0x%08X] <- 0x%02X\n", rs2, immS, rs1, addr, x[rs2] & 0xFF);
                }
                else if (funct3 == 0b001) { // SH
                    uint16_t h = (uint16_t)(x[rs2] & 0xFFFF);
                    mem[aidx]     = (uint8_t)(h & 0xFF);
                    mem[aidx + 1] = (uint8_t)((h >> 8) & 0xFF);
                    LOG("sh  x%d, %d(x%d) [addr=0x%08X] <- 0x%04X\n", rs2, immS, rs1, addr, h);
                }
                else if (funct3 == 0b010) { // SW
                    uint32_t w = x[rs2];
                    mem[aidx]     = (uint8_t)( w        & 0xFF);
                    mem[aidx + 1] = (uint8_t)((w >> 8)  & 0xFF);
                    mem[aidx + 2] = (uint8_t)((w >> 16) & 0xFF);
                    mem[aidx + 3] = (uint8_t)((w >> 24) & 0xFF);
                    LOG("sw  x%d, %d(x%d) [addr=0x%08X] <- 0x%08X\n", rs2, immS, rs1, addr, w);
                }
                else {
                    LOG("error: store desconhecido em pc=0x%08X\n", pc);
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
                int32_t immB = (imm13u & 0x1000) ? (int32_t)(imm13u | 0xFFFFE000) : (int32_t)imm13u;

                uint32_t target = pc + immB;

                if (funct3 == 0b000) { // BEQ
                    int taken = (x[rs1] == x[rs2]);
                    pc_next = taken ? target : pc_next;
                    LOG("beq x%d,x%d,%d -> %s (pc=0x%08X)\n", rs1, rs2, immB, taken ? "taken" : "not taken", taken ? target : (pc + 4));
                }
                else if (funct3 == 0b001) { // BNE
                    int taken = (x[rs1] != x[rs2]);
                    pc_next = taken ? target : pc_next;
                    LOG("bne x%d,x%d,%d -> %s (pc=0x%08X)\n", rs1, rs2, immB, taken ? "taken" : "not taken", taken ? target : (pc + 4));
                }
                else if (funct3 == 0b100) { // BLT
                    int taken = ((int32_t)x[rs1] < (int32_t)x[rs2]);
                    pc_next = taken ? target : pc_next;
                    LOG("blt x%d,x%d,%d -> %s (pc=0x%08X)\n", rs1, rs2, immB, taken ? "taken" : "not taken", taken ? target : (pc + 4));
                }
                else if (funct3 == 0b101) { // BGE
                    int taken = ((int32_t)x[rs1] >= (int32_t)x[rs2]);
                    pc_next = taken ? target : pc_next;
                    LOG("bge x%d,x%d,%d -> %s (pc=0x%08X)\n", rs1, rs2, immB, taken ? "taken" : "not taken", taken ? target : (pc + 4));
                }
                else if (funct3 == 0b110) { // BLTU
                    int taken = ((uint32_t)x[rs1] < (uint32_t)x[rs2]);
                    pc_next = taken ? target : pc_next;
                    LOG("bltu x%d,x%d,%d -> %s (pc=0x%08X)\n", rs1, rs2, immB, taken ? "taken" : "not taken", taken ? target : (pc + 4));
                }
                else if (funct3 == 0b111) { // BGEU
                    int taken = ((uint32_t)x[rs1] >= (uint32_t)x[rs2]);
                    pc_next = taken ? target : pc_next;
                    LOG("bgeu x%d,x%d,%d -> %s (pc=0x%08X)\n", rs1, rs2, immB, taken ? "taken" : "not taken", taken ? target : (pc + 4));
                }
                else {
                    LOG("error: branch desconhecido em pc=0x%08X\n", pc);
                    running = 0;
                }
                break;
            }

            // -------------------- JALR (Tipo I, controle de fluxo) --------------------
            case 0b1100111: {
                if (funct3 == 0b000) {
                    uint32_t ret = pc + 4;
                    uint32_t target = (x[rs1] + imm12) & ~1u;
                    x[rd] = ret;
                    pc_next = target;
                    LOG("jalr x%d, %d(x%d) -> pc=0x%08X, x%d=0x%08X\n", rd, imm12, rs1, target, rd, x[rd]);
                } else {
                    LOG("error: jalr funct3!=000 em pc=0x%08X\n", pc);
                    running = 0;
                }
                break;
            }

            // -------------------- JAL (Tipo J) --------------------
            case 0b1101111: {
                uint32_t imm_20    = (instruction >> 31) & 0x1;
                uint32_t imm_10_1  = (instruction >> 21) & 0x3FF;
                uint32_t imm_11    = (instruction >> 20) & 0x1;
                uint32_t imm_19_12 = (instruction >> 12) & 0xFF;

                uint32_t imm21u = (imm_20 << 20) | (imm_19_12 << 12) | (imm_11 << 11) | (imm_10_1 << 1);
                int32_t  immJ   = (imm21u & 0x00100000) ? (int32_t)(imm21u | 0xFFE00000) : (int32_t)imm21u;

                uint32_t ret = pc + 4;
                uint32_t target = pc + immJ;
                x[rd] = ret;
                pc_next = target;

                LOG("jal x%d, %d -> pc=0x%08X, x%d=0x%08X\n", rd, immJ, target, rd, x[rd]);
                break;
            }

            // -------------------- LUI (Tipo U) --------------------
            case 0b0110111: {
                uint32_t imm20 = instruction & 0xFFFFF000;
                x[rd] = imm20;
                LOG("lui x%d, 0x%05X -> x%d = 0x%08X\n", rd, imm20 >> 12, rd, x[rd]);
                break;
            }

            // -------------------- AUIPC (Tipo U) --------------------
            case 0b0010111: {
                uint32_t imm20 = instruction & 0xFFFFF000;
                x[rd] = pc + imm20;
                LOG("auipc x%d, 0x%05X -> x%d = 0x%08X (pc=0x%08X)\n", rd, imm20 >> 12, rd, x[rd], pc);
                break;
            }

            // -------------------- System: ECALL / EBREAK --------------------
            case 0b1110011: {
                uint32_t imm_sys = instruction >> 20;
                if (funct3 == 0b000 && imm_sys == 0x001) { // EBREAK
                    LOG("ebreak -> execução interrompida\n");
                    running = 0;
                } else if (funct3 == 0b000 && imm_sys == 0x000) { // ECALL
                    LOG("ecall -> chamada de sistema\n");
                    // Sem tratar SBI/ABI aqui; apenas loga.
                } else {
                    LOG("error: system desconhecido em pc=0x%08X\n", pc);
                    running = 0;
                }
                break;
            }

            default:
                LOG("error: unknown instruction opcode at pc = 0x%08X (opcode=0x%02X)\n", pc, opcode);
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
