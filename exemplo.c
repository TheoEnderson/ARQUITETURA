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

    uint8_t* mem = (uint8_t*)malloc(MEM_SIZE);
    if (!mem) {
        fprintf(stderr, "erro: malloc MEM_SIZE\n");
        return 1;
    }
    memset(mem, 0, MEM_SIZE);

    uint32_t x[32] = {0};
    uint32_t pc = BASE_ADDR;

    // Loader: linhas "@<hexaddr>" seguidas por bytes hex
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
        // Fetch LE seguro
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
                if (funct7 == 0b0000000 && funct3 == 0b000) { // ADD
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = a + b;
                    printf("0x%08x:add    %s,%s,%s          %s=0x%08x+0x%08x=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2),
                           rname(rd), a, b, x[rd]);
                    fprintf(output, "0x%08x:add    %s,%s,%s          %s=0x%08x+0x%08x=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2),
                            rname(rd), a, b, x[rd]);
                }
                else if (funct7 == 0b0100000 && funct3 == 0b000) { // SUB
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = a - b;
                    printf("0x%08x:sub    %s,%s,%s          %s=0x%08x-0x%08x=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2),
                           rname(rd), a, b, x[rd]);
                    fprintf(output, "0x%08x:sub    %s,%s,%s          %s=0x%08x-0x%08x=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2),
                            rname(rd), a, b, x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b111) { // AND
                    x[rd] = x[rs1] & x[rs2];
                    printf("0x%08x:and    %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:and    %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b110) { // OR
                    x[rd] = x[rs1] | x[rs2];
                    printf("0x%08x:or     %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:or     %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b100) { // XOR
                    x[rd] = x[rs1] ^ x[rs2];
                    printf("0x%08x:xor    %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:xor    %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b001) { // SLL
                    x[rd] = x[rs1] << (x[rs2] & 0x1F);
                    printf("0x%08x:sll    %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:sll    %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b101) { // SRL
                    x[rd] = (uint32_t)x[rs1] >> (x[rs2] & 0x1F);
                    printf("0x%08x:srl    %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:srl    %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0100000 && funct3 == 0b101) { // SRA
                    x[rd] = ((int32_t)x[rs1]) >> (x[rs2] & 0x1F);
                    printf("0x%08x:sra    %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:sra    %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b010) { // SLT
                    x[rd] = ((int32_t)x[rs1] < (int32_t)x[rs2]) ? 1u : 0u;
                    printf("0x%08x:slt    %s,%s,%s          %s=%u\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:slt    %s,%s,%s          %s=%u\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b011) { // SLTU
                    x[rd] = ((uint32_t)x[rs1] < (uint32_t)x[rs2]) ? 1u : 0u;
                    printf("0x%08x:sltu   %s,%s,%s          %s=%u\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:sltu   %s,%s,%s          %s=%u\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                // ------ Extensão M ------
                else if (funct7 == 0b0000001 && funct3 == 0b000) { // MUL
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    int64_t p = (int64_t)a * (int64_t)b;
                    x[rd] = (uint32_t)p;
                    printf("0x%08x:mul    %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:mul    %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b001) { // MULH
                    int64_t a = (int64_t)(int32_t)x[rs1];
                    int64_t b = (int64_t)(int32_t)x[rs2];
                    int64_t prod = a * b;
                    x[rd] = (uint32_t)((uint64_t)prod >> 32);
                    printf("0x%08x:mulh   %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:mulh   %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b010) { // MULHSU
                    int64_t  a = (int64_t)(int32_t)x[rs1];
                    uint64_t b = (uint64_t)x[rs2];
                    __int128 prod = (__int128)a * (__int128)b;
                    x[rd] = (uint32_t)(((__int128)prod >> 32) & 0xFFFFFFFF);
                    printf("0x%08x:mulhsu %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:mulhsu %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b011) { // MULHU
                    uint64_t a = (uint64_t)x[rs1], b = (uint64_t)x[rs2];
                    uint64_t prod = a * b;
                    x[rd] = (uint32_t)(prod >> 32);
                    printf("0x%08x:mulhu  %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:mulhu  %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b100) { // DIV
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    uint32_t res = (b == 0) ? 0xFFFFFFFFu :
                                   (a == INT32_MIN && b == -1) ? (uint32_t)INT32_MIN :
                                   (uint32_t)(a / b);
                    x[rd] = res;
                    printf("0x%08x:div    %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:div    %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b101) { // DIVU
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = (b == 0) ? 0xFFFFFFFFu : (a / b);
                    printf("0x%08x:divu   %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:divu   %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b110) { // REM
                    int32_t a = (int32_t)x[rs1], b = (int32_t)x[rs2];
                    uint32_t res = (b == 0) ? (uint32_t)a :
                                   (a == INT32_MIN && b == -1) ? 0u :
                                   (uint32_t)(a % b);
                    x[rd] = res;
                    printf("0x%08x:rem    %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:rem    %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b111) { // REMU
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = (b == 0) ? a : (a % b);
                    printf("0x%08x:remu   %s,%s,%s          %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:remu   %s,%s,%s          %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), rname(rs2), rname(rd), x[rd]);
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
                    uint32_t imm12z = (uint32_t)(imm12 & 0xFFF);
                    x[rd] = rs1_before + imm12;
                    printf("0x%08x:addi   %s,%s,0x%03x         %s=0x%08x+0x%08x=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), imm12z,
                           rname(rd), rs1_before, (uint32_t)imm12, x[rd]);
                    fprintf(output, "0x%08x:addi   %s,%s,0x%03x         %s=0x%08x+0x%08x=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), imm12z,
                            rname(rd), rs1_before, (uint32_t)imm12, x[rd]);
                }
                else if (funct3 == 0b111) { // ANDI
                    x[rd] = x[rs1] & (uint32_t)imm12;
                    printf("0x%08x:andi   %s,%s,0x%03x         %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:andi   %s,%s,0x%03x         %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                }
                else if (funct3 == 0b110) { // ORI
                    x[rd] = x[rs1] | (uint32_t)imm12;
                    printf("0x%08x:ori    %s,%s,0x%03x         %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:ori    %s,%s,0x%03x         %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                }
                else if (funct3 == 0b100) { // XORI
                    x[rd] = x[rs1] ^ (uint32_t)imm12;
                    printf("0x%08x:xori   %s,%s,0x%03x         %s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:xori   %s,%s,0x%03x         %s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                }
                else if (funct3 == 0b010) { // SLTI
                    x[rd] = ((int32_t)x[rs1] < imm12) ? 1u : 0u;
                    printf("0x%08x:slti   %s,%s,0x%03x         %s=%u\n",
                           pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:slti   %s,%s,0x%03x         %s=%u\n",
                            pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                }
                else if (funct3 == 0b011) { // SLTIU
                    x[rd] = ((uint32_t)x[rs1] < (uint32_t)imm12) ? 1u : 0u;
                    printf("0x%08x:sltiu  %s,%s,0x%03x         %s=%u\n",
                           pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:sltiu  %s,%s,0x%03x         %s=%u\n",
                            pc_curr, rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF), rname(rd), x[rd]);
                }
                else if (funct3 == 0b001) { // SLLI
                    uint32_t shamt = imm12u & 0x1F;
                    uint32_t f7    = (imm12u >> 5) & 0x7F;
                    if (f7 == 0b0000000) {
                        uint32_t before = x[rs1];
                        x[rd] = before << shamt;
                        printf("0x%08x:slli   %s,%s,%u         %s=0x%08x<<%u=0x%08x\n",
                               pc_curr, rname(rd), rname(rs1), shamt, rname(rd), before, shamt, x[rd]);
                        fprintf(output, "0x%08x:slli   %s,%s,%u         %s=0x%08x<<%u=0x%08x\n",
                                pc_curr, rname(rd), rname(rs1), shamt, rname(rd), before, shamt, x[rd]);
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
                        x[rd] = (uint32_t)x[rs1] >> shamt;
                        printf("0x%08x:srli   %s,%s,%u         %s=0x%08x\n",
                               pc_curr, rname(rd), rname(rs1), shamt, rname(rd), x[rd]);
                        fprintf(output, "0x%08x:srli   %s,%s,%u         %s=0x%08x\n",
                                pc_curr, rname(rd), rname(rs1), shamt, rname(rd), x[rd]);
                    } else if (f7 == 0b0100000) { // SRAI
                        x[rd] = ((int32_t)x[rs1]) >> shamt;
                        printf("0x%08x:srai   %s,%s,%u         %s=0x%08x\n",
                               pc_curr, rname(rd), rname(rs1), shamt, rname(rd), x[rd]);
                        fprintf(output, "0x%08x:srai   %s,%s,%u         %s=0x%08x\n",
                                pc_curr, rname(rd), rname(rs1), shamt, rname(rd), x[rd]);
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
                uint32_t aidx = addr - BASE_ADDR;
                if (aidx + 3u >= MEM_SIZE) {
                    printf("0x%08x:error  load fora de memória (addr=0x%08x)\n", pc_curr, addr);
                    fprintf(output, "0x%08x:error  load fora de memória (addr=0x%08x)\n", pc_curr, addr);
                    running = 0;
                    break;
                }

                if (funct3 == 0b000) { // LB
                    uint8_t b = mem[aidx];
                    x[rd] = (uint32_t)(int8_t)b;
                    printf("0x%08x:lb     %s,0x%03x(%s)      %s=0x%08x\n",
                           pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:lb     %s,0x%03x(%s)      %s=0x%08x\n",
                            pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                }
                else if (funct3 == 0b001) { // LH
                    uint16_t h = (uint16_t)mem[aidx] | ((uint16_t)mem[aidx+1] << 8);
                    x[rd] = (uint32_t)(int16_t)h;
                    printf("0x%08x:lh     %s,0x%03x(%s)      %s=0x%08x\n",
                           pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:lh     %s,0x%03x(%s)      %s=0x%08x\n",
                            pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                }
                else if (funct3 == 0b010) { // LW
                    uint32_t w =  (uint32_t)mem[aidx]
                                | ((uint32_t)mem[aidx+1] << 8)
                                | ((uint32_t)mem[aidx+2] << 16)
                                | ((uint32_t)mem[aidx+3] << 24);
                    x[rd] = w;
                    printf("0x%08x:lw     %s,0x%03x(%s)      %s=0x%08x\n",
                           pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:lw     %s,0x%03x(%s)      %s=0x%08x\n",
                            pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                }
                else if (funct3 == 0b100) { // LBU
                    uint8_t b = mem[aidx];
                    x[rd] = (uint32_t)b;
                    printf("0x%08x:lbu    %s,0x%03x(%s)      %s=0x%08x\n",
                           pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:lbu    %s,0x%03x(%s)      %s=0x%08x\n",
                            pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                }
                else if (funct3 == 0b101) { // LHU
                    uint16_t h = (uint16_t)mem[aidx] | ((uint16_t)mem[aidx+1] << 8);
                    x[rd] = (uint32_t)h;
                    printf("0x%08x:lhu    %s,0x%03x(%s)      %s=0x%08x\n",
                           pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
                    fprintf(output, "0x%08x:lhu    %s,0x%03x(%s)      %s=0x%08x\n",
                            pc_curr, rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1), rname(rd), x[rd]);
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
                uint32_t aidx = addr - BASE_ADDR;
                if (aidx + 3u >= MEM_SIZE) {
                    printf("0x%08x:error  store fora de memória (addr=0x%08x)\n", pc_curr, addr);
                    fprintf(output, "0x%08x:error  store fora de memória (addr=0x%08x)\n", pc_curr, addr);
                    running = 0;
                    break;
                }

                if (funct3 == 0b000) { // SB
                    mem[aidx] = (uint8_t)(x[rs2] & 0xFF);
                    printf("0x%08x:sb     %s,0x%03x(%s)        mem[0x%08x]=0x%02x\n",
                           pc_curr, rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1), addr, x[rs2] & 0xFF);
                    fprintf(output, "0x%08x:sb     %s,0x%03x(%s)        mem[0x%08x]=0x%02x\n",
                            pc_curr, rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1), addr, x[rs2] & 0xFF);
                }
                else if (funct3 == 0b001) { // SH
                    uint16_t h = (uint16_t)(x[rs2] & 0xFFFF);
                    mem[aidx]     = (uint8_t)(h & 0xFF);
                    mem[aidx + 1] = (uint8_t)((h >> 8) & 0xFF);
                    printf("0x%08x:sh     %s,0x%03x(%s)        mem[0x%08x]=0x%04x\n",
                           pc_curr, rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1), addr, h);
                    fprintf(output, "0x%08x:sh     %s,0x%03x(%s)        mem[0x%08x]=0x%04x\n",
                            pc_curr, rname(rs2), (uint32_t)(imm12uS & 0xFFF), rname(rs1), addr, h);
                }
                else if (funct3 == 0b010) { // SW
                    uint32_t w = x[rs2];
                    mem[aidx]     = (uint8_t)( w        & 0xFF);
                    mem[aidx + 1] = (uint8_t)((w >> 8)  & 0xFF);
                    mem[aidx + 2] = (uint8_t)((w >> 16) & 0xFF);
                    mem[aidx + 3] = (uint8_t)((w >> 24) & 0xFF);
                    uint32_t immS_disp = (uint32_t)(imm12uS & 0xFFF);
                    printf("0x%08x:sw     %s,0x%03x(%s)        mem[0x%08x]=0x%08x\n",
                           pc_curr, rname(rs2), immS_disp, rname(rs1), addr, w);
                    fprintf(output, "0x%08x:sw     %s,0x%03x(%s)        mem[0x%08x]=0x%08x\n",
                            pc_curr, rname(rs2), immS_disp, rname(rs1), addr, w);
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

                if (funct3 == 0b000) { // BEQ
                    int taken = (x[rs1] == x[rs2]);
                    pc_next = taken ? target : pc_next;
                    uint32_t shown_off = (imm13u >> 1) & 0xFFF;
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    printf("0x%08x:beq    %s,%s,0x%03x         (0x%08x==0x%08x)=%d->pc=0x%08x\n",
                           pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                    fprintf(output, "0x%08x:beq    %s,%s,0x%03x         (0x%08x==0x%08x)=%d->pc=0x%08x\n",
                            pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                }
                else if (funct3 == 0b001) { // BNE
                    int taken = (x[rs1] != x[rs2]);
                    pc_next = taken ? target : pc_next;
                    uint32_t shown_off = (imm13u >> 1) & 0xFFF;
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    printf("0x%08x:bne    %s,%s,0x%03x         (0x%08x!=0x%08x)=%d->pc=0x%08x\n",
                           pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                    fprintf(output, "0x%08x:bne    %s,%s,0x%03x         (0x%08x!=0x%08x)=%d->pc=0x%08x\n",
                            pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                }
                else if (funct3 == 0b100) { // BLT
                    uint32_t shown_off = (imm13u >> 1) & 0xFFF;
                    int taken = ((int32_t)x[rs1] < (int32_t)x[rs2]);
                    uint32_t lhs = x[rs1], rhs = x[rs2];
                    uint32_t newpc = taken ? (pc_curr + (int32_t)immB) : (pc_curr + 4);
                    pc_next = taken ? (pc_curr + (int32_t)immB) : pc_next;
                    printf("0x%08x:blt    %s,%s,0x%03x         (0x%08x<0x%08x)=%d->pc=0x%08x\n",
                           pc_curr, rname(rs1), rname(rs2), shown_off, lhs, rhs, taken, newpc);
                    fprintf(output, "0x%08x:blt    %s,%s,0x%03x         (0x%08x<0x%08x)=%d->pc=0x%08x\n",
                            pc_curr, rname(rs1), rname(rs2), shown_off, lhs, rhs, taken, newpc);
                }
                else if (funct3 == 0b101) { // BGE
                    uint32_t shown_off = (imm13u >> 1) & 0xFFF;
                    int taken = ((int32_t)x[rs1] >= (int32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    pc_next = taken ? target : pc_next;
                    printf("0x%08x:bge    %s,%s,0x%03x         (0x%08x>=0x%08x)=%d->pc=0x%08x\n",
                           pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                    fprintf(output, "0x%08x:bge    %s,%s,0x%03x         (0x%08x>=0x%08x)=%d->pc=0x%08x\n",
                            pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                }
                else if (funct3 == 0b110) { // BLTU
                    uint32_t shown_off = (imm13u >> 1) & 0xFFF;
                    int taken = ((uint32_t)x[rs1] < (uint32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    pc_next = taken ? target : pc_next;
                    printf("0x%08x:bltu   %s,%s,0x%03x         (0x%08x<0x%08x)=%d->pc=0x%08x\n",
                           pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                    fprintf(output, "0x%08x:bltu   %s,%s,0x%03x         (0x%08x<0x%08x)=%d->pc=0x%08x\n",
                            pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                }
                else if (funct3 == 0b111) { // BGEU
                    uint32_t shown_off = (imm13u >> 1) & 0xFFF;
                    int taken = ((uint32_t)x[rs1] >= (uint32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    pc_next = taken ? target : pc_next;
                    printf("0x%08x:bgeu   %s,%s,0x%03x         (0x%08x>=0x%08x)=%d->pc=0x%08x\n",
                           pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                    fprintf(output, "0x%08x:bgeu   %s,%s,0x%03x         (0x%08x>=0x%08x)=%d->pc=0x%08x\n",
                            pc_curr, rname(rs1), rname(rs2), shown_off, x[rs1], x[rs2], taken, newpc);
                }
                else {
                    printf("0x%08x:error  branch desconhecido\n", pc_curr);
                    fprintf(output, "0x%08x:error  branch desconhecido\n", pc_curr);
                    running = 0;
                }
                break;
            }

            // -------------------- JALR (Tipo I, controle de fluxo) --------------------
            case 0b1100111: {
                if (funct3 == 0b000) {
                    uint32_t rs1_before = x[rs1];
                    uint32_t ret = pc_curr + 4;
                    uint32_t target = (rs1_before + imm12) & ~1u;
                    x[rd] = ret;
                    pc_next = target;
                    uint32_t imm12z = (uint32_t)(imm12 & 0xFFF);
                    printf("0x%08x:jalr   %s,%s,0x%03x       pc=0x%08x+0x%08x,%s=0x%08x\n",
                           pc_curr, rname(rd), rname(rs1), imm12z, rs1_before, (uint32_t)imm12, rname(rd), ret);
                    fprintf(output, "0x%08x:jalr   %s,%s,0x%03x       pc=0x%08x+0x%08x,%s=0x%08x\n",
                            pc_curr, rname(rd), rname(rs1), imm12z, rs1_before, (uint32_t)imm12, rname(rd), ret);
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
                pc_next = target;

                uint32_t shown = (imm21u >> 1) & 0xfffff; // sem bit0
                printf("0x%08x:jal    %s,0x%05x        pc=0x%08x,%s=0x%08x\n",
                       pc_curr, rname(rd), shown, target, rname(rd), ret);
                fprintf(output, "0x%08x:jal    %s,0x%05x        pc=0x%08x,%s=0x%08x\n",
                        pc_curr, rname(rd), shown, target, rname(rd), ret);
                break;
            }

            // -------------------- LUI (Tipo U) --------------------
            case 0b0110111: {
                uint32_t imm20 = instruction & 0xFFFFF000;
                x[rd] = imm20;
                printf("0x%08x:lui    %s,0x%05x          %s=0x%08x\n",
                       pc_curr, rname(rd), (imm20 >> 12), rname(rd), x[rd]);
                fprintf(output, "0x%08x:lui    %s,0x%05x          %s=0x%08x\n",
                        pc_curr, rname(rd), (imm20 >> 12), rname(rd), x[rd]);
                break;
            }

            // -------------------- AUIPC (Tipo U) --------------------
            case 0b0010111: {
                uint32_t imm20 = instruction & 0xFFFFF000;
                x[rd] = pc_curr + imm20;
                printf("0x%08x:auipc  %s,0x%05x          %s=0x%08x+0x%08x=0x%08x\n",
                       pc_curr, rname(rd), (imm20 >> 12), rname(rd), pc_curr, imm20, x[rd]);
                fprintf(output, "0x%08x:auipc  %s,0x%05x          %s=0x%08x+0x%08x=0x%08x\n",
                        pc_curr, rname(rd), (imm20 >> 12), rname(rd), pc_curr, imm20, x[rd]);
                break;
            }

            // -------------------- System: ECALL / EBREAK --------------------
            case 0b1110011: {
                uint32_t imm_sys = instruction >> 20;
                if (funct3 == 0b000 && imm_sys == 0x001) { // EBREAK
                    printf("0x%08x:ebreak\n", pc_curr);
                    fprintf(output, "0x%08x:ebreak\n", pc_curr);
                    running = 0;
                } else if (funct3 == 0b000 && imm_sys == 0x000) { // ECALL
                    printf("0x%08x:ecall\n", pc_curr);
                    fprintf(output, "0x%08x:ecall\n", pc_curr);
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
