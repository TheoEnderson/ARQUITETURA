#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
// História que falou
#include <limits.h>


int main(int argc, char* argv[]) {
	// Outputting separator
	printf("--------------------------------------------------------------------------------\n");
	// Iterating over arguments
	for(uint32_t i = 0; i < argc; i++) {
		// Outputting argument
		printf("argv[%i] = %s\n", i, argv[i]);
	}
	// Opening input and output files using proper permissions
	FILE* input = fopen(argv[1], "r");
	FILE* output = fopen(argv[2], "w");
	// Setting memory offset to 0x80000000
	const uint32_t offset = 0x80000000;
	// Creating 32 registers initialized with zero and labels
	uint32_t x[32] = { 0 };
	const char* x_label[32] = { "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6" };
	// Creating pc register initialized with memory offset
	uint32_t pc = offset;
	// Creating 32 KiB memory for both data and instructions
	uint8_t* mem = (uint8_t*)(malloc(32 * 1024));
	
	
	// Outputting separator
	printf("--------------------------------------------------------------------------------\n");
	// Setting run condition
	uint8_t run = 1;
	// Loop while condition is true
	while(run) {
		// Reading instruction from memory (4 byte alignment)
		const uint32_t instruction = ((uint32_t*)(mem))[(pc - offset) >> 2];
		// Retrieving instruction opcode (6:0)
		const uint8_t opcode = instruction & 0b1111111;
		// Retrieving instruction fields
		const uint8_t funct7 = instruction >> 25;
		const uint16_t imm = instruction >> 20;
		const uint8_t uimm = (instruction & (0b11111 << 20)) >> 20;
		const uint8_t rs1 = (instruction & (0b11111 << 15)) >> 15;
        const uint8_t rs2 = (instruction & (0b11111 << 20)) >> 20;
		const uint8_t funct3 = (instruction & (0b111 << 12)) >> 12;
		const uint8_t rd = (instruction & (0b11111 << 7)) >> 7;
		const uint32_t imm20 = ((instruction >> 31) << 19) | (((instruction & (0b11111111 << 12)) >> 12) << 11) | (((instruction & (0b1 << 20)) >> 20) << 10) | ((instruction & (0b1111111111 << 21)) >> 21);
		// Checking instruction opcode
		switch(opcode) {
            case 0b0110011: { //Tipo R
                if(funct3 == 0b000 && funct7 == 0b0000000){
                    // ADD
                    reg[rd] = reg[rs1] + reg[rs2];
                    printf("add x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct3 == 0b000 && funct7 == 0b0100000) {
                    // SUB
                    reg[rd] = reg[rs1] - reg[rs2];
                    printf("sub x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct3 == 0b111 && funct7 == 0b0000000) {
                    // AND
                    reg[rd] = reg[rs1] & reg[rs2];
                    printf("and x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct3 == 0b110 && funct7 == 0b0000000) {
                    // OR
                    reg[rd] = reg[rs1] | reg[rs2];
                    printf("or x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct3 == 0b100 && funct7 == 0b0000000) {
                    // XOR
                    reg[rd] = reg[rs1] ^ reg[rs2];
                    printf("xor x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct3 == 0b001 && funct7 == 0b0000000) {
                    // SLL
                    reg[rd] = reg[rs1] << (reg[rs2] & 0x1F);
                    printf("sll x%d, x%d, x%d -> x%d = 0x%08X (shift = %d)\n",
                        rd, rs1, rs2, rd, reg[rd], reg[rs2] & 0x1F);
                }
                else if (funct3 == 0b101 && funct7 == 0b0000000) {
                    // SRL
                    reg[rd] = (unsigned int)reg[rs1] >> (reg[rs2] & 0x1F);
                    printf("srl x%d, x%d, x%d -> x%d = 0x%08X (shift = %d)\n",
                        rd, rs1, rs2, rd, reg[rd], reg[rs2] & 0x1F);
                }
                else if (funct3 == 0b101 && funct7 == 0b0100000) {
                    // SRA (aritmético - preserva sinal)
                    reg[rd] = ((int32_t)reg[rs1]) >> (reg[rs2] & 0x1F);
                    printf("sra x%d, x%d, x%d -> x%d = 0x%08X (shift = %d)\n",
                        rd, rs1, rs2, rd, reg[rd], reg[rs2] & 0x1F);
                }
                else if (funct3 == 0b010 && funct7 == 0b0000000) {
                    // SLT (signed comparison)
                    reg[rd] = ((int32_t)reg[rs1] < (int32_t)reg[rs2]) ? 1 : 0;
                    printf("slt x%d, x%d, x%d -> x%d = %d\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct3 == 0b011 && funct7 == 0b0000000) {
                    // SLTU (unsigned)
                    reg[rd] = ((uint32_t)reg[rs1] < (uint32_t)reg[rs2]) ? 1 : 0;
                    printf("sltu x%d, x%d, x%d -> x%d = %d\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b000) {
                    // MUL: low 32 bits de (int32_t)rs1 * (int32_t)rs2
                    int32_t a = (int32_t)reg[rs1];
                    int32_t b = (int32_t)reg[rs2];
                    int64_t p = (int64_t)a * (int64_t)b;   // produto 64 bits
                    reg[rd] = (uint32_t)(p & 0xFFFFFFFFu); // mantém só os 32 bits baixos

                    printf("mul x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b001) {
                    // MULH: signed * signed, parte alta do resultado de 64 bits
                    int64_t a = (int64_t)(int32_t)reg[rs1];
                    int64_t b = (int64_t)(int32_t)reg[rs2];
                    int64_t prod = a * b;

                    // Pega os 32 bits mais altos do produto
                    uint32_t high = (uint32_t)((prod >> 32) & 0xFFFFFFFFu);
                    reg[rd] = high;

                    printf("mulh x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b010) {
                    // MULHSU: signed * unsigned, retorna parte alta (bits 63..32)
                    int64_t  a = (int64_t)(int32_t)reg[rs1];   // sign-extend
                    uint64_t b = (uint64_t)(uint32_t)reg[rs2]; // zero-extend

                    // Produto de 64 bits é suficiente (32x32 -> 64). Para máxima segurança:
                    __int128 prod = (__int128)a * (__int128)b;

                    uint32_t high = (uint32_t)((prod >> 32) & 0xFFFFFFFF);
                    reg[rd] = high;

                    printf("mulhsu x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b011) {
                    // MULHU: unsigned * unsigned, parte alta (bits 63..32)
                    uint64_t a = (uint64_t)(uint32_t)reg[rs1];
                    uint64_t b = (uint64_t)(uint32_t)reg[rs2];
                    uint64_t prod = a * b;                  // 32x32 -> 64 bits
                    reg[rd] = (uint32_t)((prod >> 32) & 0xFFFFFFFFu);

                    printf("mulhu x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b100) {
                    // DIV (signed)
                    int32_t a = (int32_t)reg[rs1];
                    int32_t b = (int32_t)reg[rs2];
                    uint32_t res;

                    if (b == 0) {
                        res = 0xFFFFFFFFu; // -1
                    } else if (a == INT32_MIN && b == -1) {
                        res = (uint32_t)INT32_MIN; // overflow definido pela especificação
                    } else {
                        res = (uint32_t)(a / b); // divisão com sinal, trunca para zero
                    }

                    reg[rd] = res;
                    printf("div x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b101) {
                    // DIVU (unsigned)
                    uint32_t a = reg[rs1];
                    uint32_t b = reg[rs2];

                    reg[rd] = (b == 0) ? 0xFFFFFFFFu : (a / b);

                    printf("divu x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b110) {
                    // REM (signed remainder)
                    int32_t a = (int32_t)reg[rs1];
                    int32_t b = (int32_t)reg[rs2];
                    uint32_t res;

                    if (b == 0) {
                        res = (uint32_t)a;              // resto = dividendo
                    } else if (a == INT32_MIN && b == -1) {
                        res = 0;                        // caso de overflow definido
                    } else {
                        res = (uint32_t)(a % b);        // resto com sinal (segue o dividendo)
                    }

                    reg[rd] = res;
                    printf("rem x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                else if (funct7 == 0b0000001 && funct3 == 0b111) {
                    // REMU (unsigned remainder)
                    uint32_t a = reg[rs1];
                    uint32_t b = reg[rs2];

                    reg[rd] = (b == 0) ? a : (a % b);

                    printf("remu x%d, x%d, x%d -> x%d = 0x%08X\n",
                        rd, rs1, rs2, rd, reg[rd]);
                }
                // x0 sempre 0
                reg[0] = 0;
                break;
            }
            case 0b0010011: { // Tipo I (ALU imediato)
                uint32_t imm12 = instr >> 20; // bits [31:20]
                int32_t imm = (imm12 & 0x800) ? (int32_t)(imm12 | 0xFFFFF000)
                                            : (int32_t)imm12;

                if (funct3 == 0b000) {
                    // ADDI
                    reg[rd] = reg[rs1] + imm;
                    printf("addi x%d, x%d, %d -> x%d = 0x%08X\n",
                        rd, rs1, imm, rd, reg[rd]);
                }
                else if (funct3 == 0b111) {
                    // ANDI
                    reg[rd] = reg[rs1] & imm;
                    printf("andi x%d, x%d, 0x%X -> x%d = 0x%08X\n",
                        rd, rs1, imm, rd, reg[rd]);
                }
                else if (funct3 == 0b110) {
                    // ORI
                    reg[rd] = reg[rs1] | imm;
                    printf("ori x%d, x%d, 0x%X -> x%d = 0x%08X\n",
                        rd, rs1, imm, rd, reg[rd]);
                }
                else if (funct3 == 0b100) {
                    // XORI
                    reg[rd] = reg[rs1] ^ imm;
                    printf("xori x%d, x%d, 0x%X -> x%d = 0x%08X\n",
                        rd, rs1, imm, rd, reg[rd]);
                }
                else if (funct3 == 0b010) {
                    // SLTI (signed)
                    reg[rd] = ((int32_t)reg[rs1] < imm) ? 1 : 0;
                    printf("slti x%d, x%d, %d -> x%d = %d\n",
                        rd, rs1, imm, rd, reg[rd]);
                }
                else if (funct3 == 0b011) {
                    // SLTIU (unsigned)
                    reg[rd] = ((uint32_t)reg[rs1] < (uint32_t)imm) ? 1 : 0;
                    printf("sltiu x%d, x%d, %d -> x%d = %d\n",
                        rd, rs1, imm, rd, reg[rd]);
                }
                else if (funct3 == 0b001) {
                    // SLLI
                    if ((imm12 & 0xFE0) == 0x000) {
                        uint32_t shamt = imm12 & 0x1F;
                        reg[rd] = reg[rs1] << shamt;
                        printf("slli x%d, x%d, %u -> x%d = 0x%08X\n",
                            rd, rs1, shamt, rd, reg[rd]);
                    } else {
                        printf("Erro: funct7 inválido em SLLI (0x%X)\n", (imm12 >> 5) & 0x7F);
                    }
                }
                else if (funct3 == 0b101) {
                    // SRLI ou SRAI
                    uint32_t funct7 = (imm12 >> 5) & 0x7F;
                    uint32_t shamt = imm12 & 0x1F;

                    if (funct7 == 0b0000000) {
                        // SRLI (lógico)
                        reg[rd] = (uint32_t)reg[rs1] >> shamt;
                        printf("srli x%d, x%d, %u -> x%d = 0x%08X\n",
                            rd, rs1, shamt, rd, reg[rd]);
                    }
                    else if (funct7 == 0b0100000) {
                        // SRAI (aritmético)
                        reg[rd] = ((int32_t)reg[rs1]) >> shamt;
                        printf("srai x%d, x%d, %u -> x%d = 0x%08X\n",
                            rd, rs1, shamt, rd, reg[rd]);
                    }
                    else {
                        printf("Erro: funct7 inválido em SRLI/SRAI (0x%X)\n", funct7);
                    }
                }

                reg[0] = 0; // x0 sempre 0
                break;
            }
            case 0b0000011: { // Loads (Tipo I)
                uint32_t imm12 = instr >> 20; // bits [31:20]
                int32_t imm = (imm12 & 0x800) ? (int32_t)(imm12 | 0xFFFFF000)
                                            : (int32_t)imm12;
                uint32_t addr = (uint32_t)(reg[rs1] + imm);

                if (funct3 == 0b000) {
                    // LB (signed)
                    uint8_t byte = memory[addr];
                    reg[rd] = (uint32_t)(int8_t)byte;
                    printf("lb x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%08X\n",
                        rd, imm, rs1, addr, rd, reg[rd]);
                }
                else if (funct3 == 0b001) {
                    // LH (signed)
                    uint16_t half = memory[addr] | (memory[addr + 1] << 8);
                    reg[rd] = (uint32_t)(int16_t)half;
                    printf("lh x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%08X\n",
                        rd, imm, rs1, addr, rd, reg[rd]);
                }
                else if (funct3 == 0b010) {
                    // LW (signed 32 bits)
                    uint32_t w =  (uint32_t)memory[addr]
                                | ((uint32_t)memory[addr + 1] << 8)
                                | ((uint32_t)memory[addr + 2] << 16)
                                | ((uint32_t)memory[addr + 3] << 24);
                    reg[rd] = w;
                    printf("lw x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%08X\n",
                        rd, imm, rs1, addr, rd, reg[rd]);
                }
                else if (funct3 == 0b100) {
                    // LBU: load byte unsigned (zero-extend)
                    uint8_t byte = memory[addr];
                    reg[rd] = (uint32_t)byte;
                    printf("lbu x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%02X\n",
                        rd, imm, rs1, addr, rd, reg[rd]);
                }
                else if (funct3 == 0b101) {
                    // LHU (unsigned)
                    uint16_t half = memory[addr] | (memory[addr + 1] << 8);
                    reg[rd] = (uint32_t)half;  // zero-extend
                    printf("lhu x%d, %d(x%d) [addr=0x%08X] -> x%d = 0x%04X\n",
                        rd, imm, rs1, addr, rd, reg[rd]);
                }

                reg[0] = 0; // x0 sempre 0
                break;
            }
            case 0b0100011: { // Stores (Tipo S)
                // Monta imm[11:0] a partir de [31:25] e [11:7]
                uint32_t imm_high = (instr >> 25) & 0x7F;  // bits 31..25
                uint32_t imm_low  = (instr >> 7)  & 0x1F;  // bits 11..7
                uint32_t imm12u   = (imm_high << 5) | imm_low;
                int32_t  imm      = (imm12u & 0x800) ? (int32_t)(imm12u | 0xFFFFF000)
                                                    : (int32_t)imm12u;

                uint32_t addr = (uint32_t)(reg[rs1] + imm);

                if (funct3 == 0b000) {
                    // SB
                    memory[addr] = (uint8_t)(reg[rs2] & 0xFF);
                    printf("sb x%d, %d(x%d) [addr=0x%08X] <- 0x%02X\n",
                        rs2, imm, rs1, addr, reg[rs2] & 0xFF);
                }
                else if (funct3 == 0b001) {
                    // SH
                    uint16_t half = (uint16_t)(reg[rs2] & 0xFFFF);
                    memory[addr]     = (uint8_t)(half & 0xFF);
                    memory[addr + 1] = (uint8_t)((half >> 8) & 0xFF);
                    printf("sh x%d, %d(x%d) [addr=0x%08X] <- 0x%04X\n",
                        rs2, imm, rs1, addr, half);
                }
                else if (funct3 == 0b010) {
                    // SW: store word (32 bits), little-endian
                    // (opcional) checagens:
                    // if (addr + 3 >= MEM_SIZE) { printf("Erro: SW fora da memória\n"); break; }
                    // if (addr & 0x3) { printf("Aviso: SW desalinhado em 0x%08X\n", addr); }

                    uint32_t w = (uint32_t)reg[rs2];
                    memory[addr]     = (uint8_t)( w        & 0xFF);
                    memory[addr + 1] = (uint8_t)((w >> 8)  & 0xFF);
                    memory[addr + 2] = (uint8_t)((w >> 16) & 0xFF);
                    memory[addr + 3] = (uint8_t)((w >> 24) & 0xFF);

                    printf("sw x%d, %d(x%d) [addr=0x%08X] <- 0x%08X\n",
                        rs2, imm, rs1, addr, w);
                }

                reg[0] = 0; // x0 sempre 0
                break;
            }
            case 0b1100011: { // Branches (Tipo B)
                // Reconstrói imm de 13 bits (inclui o bit zero implícito)
                uint32_t imm_12   = (instr >> 31) & 0x1;
                uint32_t imm_10_5 = (instr >> 25) & 0x3F;
                uint32_t imm_4_1  = (instr >> 8)  & 0xF;
                uint32_t imm_11   = (instr >> 7)  & 0x1;

                uint32_t imm13u = (imm_12   << 12) |
                                (imm_11   << 11) |
                                (imm_10_5 << 5 ) |
                                (imm_4_1  << 1 );   // bit 0 é 0

                int32_t imm = (imm13u & 0x1000) ? (int32_t)(imm13u | 0xFFFFE000)
                                                : (int32_t)imm13u;

                uint32_t target = (uint32_t)(pc + imm);

                if (funct3 == 0b000) {
                    // BEQ: branch if equal
                    int taken = (reg[rs1] == reg[rs2]);
                    if (taken) {
                        pc = target;
                    } else {
                        pc += 4; // se o seu loop já faz pc+=4 no fetch, remova esta linha
                    }
                    printf("beq x%d, x%d, %d -> %s (pc=0x%08X)\n",
                        rs1, rs2, imm, taken ? "taken" : "not taken", pc);
                }
                else if (funct3 == 0b001) {
                    // BNE
                    int taken = (reg[rs1] != reg[rs2]);
                    if (taken) pc = target;
                    else       pc += 4;
                    printf("bne x%d, x%d, %d -> %s (pc=0x%08X)\n",
                        rs1, rs2, imm, taken ? "taken" : "not taken", pc);
                }
                else if (funct3 == 0b100) {
                    // BLT (signed)
                    int taken = ((int32_t)reg[rs1] < (int32_t)reg[rs2]);
                    pc = taken ? target : pc + 4;
                    printf("blt x%d, x%d, %d -> %s (pc=0x%08X)\n",
                        rs1, rs2, imm, taken ? "taken" : "not taken", pc);
                }
                else if (funct3 == 0b101) {
                    // BGE (signed)
                    int taken = ((int32_t)reg[rs1] >= (int32_t)reg[rs2]);
                    pc = taken ? target : pc + 4;
                    printf("bge x%d, x%d, %d -> %s (pc=0x%08X)\n",
                        rs1, rs2, imm, taken ? "taken" : "not taken", pc);
                }
                else if (funct3 == 0b110) {
                    // BLTU (unsigned)
                    int taken = ((uint32_t)reg[rs1] < (uint32_t)reg[rs2]);
                    pc = taken ? target : pc + 4;
                    printf("bltu x%d, x%d, %d -> %s (pc=0x%08X)\n",
                        rs1, rs2, imm, taken ? "taken" : "not taken", pc);
                }
                else if (funct3 == 0b111) {
                    // BGEU (unsigned)
                    int taken = ((uint32_t)reg[rs1] >= (uint32_t)reg[rs2]);
                    pc = taken ? target : pc + 4;
                    printf("bgeu x%d, x%d, %d -> %s (pc=0x%08X)\n",
                        rs1, rs2, imm, taken ? "taken" : "not taken", pc);
                }

                reg[0] = 0; // x0 sempre 0
                break;
            }
            case 0b1100111: { // JALR (Jump And Link Register)
                // Extrai imediato de 12 bits (bits [31:20])
                uint32_t imm12 = instr >> 20;
                int32_t imm = (imm12 & 0x800) ? (int32_t)(imm12 | 0xFFFFF000)
                                            : (int32_t)imm12;

                if (funct3 == 0b000) {
                    // JALR
                    uint32_t return_addr = pc + 4;               // endereço de retorno
                    uint32_t target = (reg[rs1] + imm) & ~1U;    // bit 0 zerado

                    reg[rd] = return_addr;                       // salva endereço de retorno
                    pc = target;                                 // desvia para o destino

                    printf("jalr x%d, %d(x%d) -> pc=0x%08X, x%d=0x%08X\n",
                        rd, imm, rs1, pc, rd, reg[rd]);
                }

                reg[0] = 0; // x0 sempre 0
                break;
            }
            case 0b1101111: { // JAL (Jump And Link) - Formato J
                // Reconstrói o imediato J-type (21 bits com bit 0 = 0)
                uint32_t imm_20    = (instr >> 31) & 0x1;
                uint32_t imm_10_1  = (instr >> 21) & 0x3FF; // bits 30..21
                uint32_t imm_11    = (instr >> 20) & 0x1;   // bit 20
                uint32_t imm_19_12 = (instr >> 12) & 0xFF;  // bits 19..12

                uint32_t imm21u = (imm_20    << 20) |
                                (imm_19_12 << 12) |
                                (imm_11    << 11) |
                                (imm_10_1  << 1);  // bit 0 implícito = 0

                int32_t imm = (imm21u & 0x00100000) ? (int32_t)(imm21u | 0xFFE00000)
                                                    : (int32_t)imm21u;

                uint32_t ret = pc + 4;          // endereço de retorno
                uint32_t target = (uint32_t)(pc + imm);

                reg[rd] = ret;                   // rd recebe pc+4
                pc = target;                     // salta para pc + imm

                printf("jal x%d, %d -> pc=0x%08X, x%d=0x%08X\n",
                    rd, imm, pc, rd, reg[rd]);

                reg[0] = 0; // x0 sempre 0
                break;
            }
            case 0b0110111: { // LUI (Load Upper Immediate)
                // Extrai imm[31:12] e desloca 12 bits à esquerda
                uint32_t imm20 = instr & 0xFFFFF000;
                reg[rd] = imm20;

                printf("lui x%d, 0x%05X -> x%d = 0x%08X\n",
                    rd, imm20 >> 12, rd, reg[rd]);

                reg[0] = 0; // x0 sempre 0
                break;
            }
            case 0b0010111: { // AUIPC (Add Upper Immediate to PC)
                // Extrai imm[31:12] e desloca 12 bits à esquerda
                uint32_t imm20 = instr & 0xFFFFF000;
                reg[rd] = pc + imm20;

                printf("auipc x%d, 0x%05X -> x%d = 0x%08X (pc=0x%08X)\n",
                    rd, imm20 >> 12, rd, reg[rd], pc);

                reg[0] = 0; // x0 sempre 0
                break;
            }
            case 0b1110011: { // System (ECALL / EBREAK)
                uint32_t imm12 = instr >> 20; // bits [31:20]

                if (funct3 == 0b000 && imm12 == 0x001) {
                    // EBREAK
                    printf("ebreak -> execução interrompida\n");
                    running = 0; // variável de controle do loop principal
                }
                else if (funct3 == 0b000 && imm12 == 0x000) {
                    // ECALL (opcional)
                    printf("ecall -> chamada de sistema\n");
                }

                break;
            }
			default:
				// Outputting error message
				printf("error: unknown instruction opcode at pc = 0x%08x\n", pc);
				// Halting simulation
				run = 0;
		}
		// Incrementing pc by 4
		pc = pc + 4;
	}
	// Closing input and output files
	fclose(input);
	fclose(output);
	// Outputting separator
	printf("--------------------------------------------------------------------------------\n");
	// Returning success status
	return 0;
}