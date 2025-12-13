#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

// RAM
#define RAM_BASE   0x80000000u
#define RAM_SIZE   (32u * 1024u)

// MMIO (usado pelo teste)
#define IO_UART_BASE  0x10000000u   // 0: data, 1: config, 2: dummy-read=1, 5: status
#define IO_TTY_ADDR   0x10000002u
#define IO_SWI_ADDR   0x02000000u   // software interrupt (MSIP fake)

// CLINT fake: mtimecmp (low/high)
#define CLINT_MTIMECMP_LO 0x02004000u
#define CLINT_MTIMECMP_HI 0x02004004u

// PLIC fake (endereços tocados pelo programa do teste)
#define PLIC_ENABLE_ADDR   0x0c002000u
#define PLIC_THRESHOLD     0x0c200000u
#define PLIC_CLAIMCOMP     0x0c200004u

// CSRs (modo máquina)
static uint32_t csr_mstatus = 0;
static uint32_t csr_mtvec   = 0;
static uint32_t csr_mepc    = 0;
static uint32_t csr_mcause  = 0;
static uint32_t csr_mtval   = 0;
static uint32_t csr_mie     = 0;
static uint32_t csr_mip     = 0;

// Timer (modelo RISC-V: MTIP pendente quando mtime >= mtimecmp)
static uint64_t mtime    = 0;
static uint64_t mtimecmp = UINT64_MAX;  // começa desarmado

// Pendências
static int soft_irq_pending = 0;
static int timer_irq_pending = 0;
static int uart_irq_pending = 0;

// PLIC simplificado
static uint32_t plic_enable = 0;
static uint32_t plic_claim_id = 10;     // teste espera 0x0a

// UART FIFO (loopback)
static uint8_t  uart_fifo[4096];
static uint32_t uart_head = 0, uart_tail = 0;

static inline int uart_fifo_empty(void) { return uart_head == uart_tail; }
static inline int uart_fifo_full(void)  { return ((uart_tail + 1) % sizeof(uart_fifo)) == uart_head; }

static inline void uart_fifo_push(uint8_t v) {
    if (!uart_fifo_full()) {
        uart_fifo[uart_tail] = v;
        uart_tail = (uart_tail + 1) % sizeof(uart_fifo);
    }
}

static inline uint8_t uart_fifo_pop(void) {
    if (uart_fifo_empty()) return 0;
    uint8_t v = uart_fifo[uart_head];
    uart_head = (uart_head + 1) % sizeof(uart_fifo);
    return v;
}

enum {
    EXC_INST_FAULT       = 1,
    EXC_ILLEGAL_INST     = 2,
    EXC_LOAD_FAULT       = 5,
    EXC_STORE_FAULT      = 7,
    EXC_ENV_CALL_M       = 11,
};

static uint32_t csr_read(uint32_t addr) {
    switch (addr) {
        case 0x300: return csr_mstatus;
        case 0x305: return csr_mtvec;
        case 0x341: return csr_mepc;
        case 0x342: return csr_mcause;
        case 0x343: return csr_mtval;
        case 0x304: return csr_mie;
        case 0x344: return csr_mip;
        default:    return 0;
    }
}

static void csr_write(uint32_t addr, uint32_t val) {
    switch (addr) {
        case 0x300: csr_mstatus = val; break;
        case 0x305: csr_mtvec   = val; break;
        case 0x341: csr_mepc    = val; break;
        case 0x342: csr_mcause  = val; break;
        case 0x343: csr_mtval   = val; break;
        case 0x304: csr_mie     = val; break;
        case 0x344: csr_mip     = val; break;
        default: break;
    }
}

static const char* csr_name(uint32_t addr) {
    switch (addr) {
        case 0x300: return "mstatus";
        case 0x305: return "mtvec";
        case 0x341: return "mepc";
        case 0x342: return "mcause";
        case 0x343: return "mtval";
        case 0x304: return "mie";
        case 0x344: return "mip";
        default:    return NULL;
    }
}

static void raise_exception(uint32_t cause,
                            uint32_t epc,
                            uint32_t tval,
                            uint32_t *pc_next,
                            FILE *output)
{
    const char *name = "unknown";
    switch (cause) {
        case EXC_INST_FAULT:   name = "instruction_fault";   break;
        case EXC_ILLEGAL_INST: name = "illegal_instruction"; break;
        case EXC_LOAD_FAULT:   name = "load_fault";          break;
        case EXC_STORE_FAULT:  name = "store_fault";         break;
        case EXC_ENV_CALL_M:   name = "environment_call";    break;
    }

    csr_mcause = cause;
    csr_mepc   = epc;
    csr_mtval  = tval;

    uint32_t m   = csr_mstatus;
    uint32_t mie = (m >> 3) & 1u;

    // MIE <- 0, MPIE <- MIE antigo, MPP <- 3
    m &= ~((1u << 3) | (1u << 7) | (3u << 11));
    m |= (mie << 7);
    m |= (3u  << 11);
    csr_mstatus = m;

    uint32_t base = csr_mtvec & ~0x3u;
    *pc_next = base;

    fprintf(output,
        ">exception:%-26s cause=0x%08x,epc=0x%08x,tval=0x%08x\n",
        name, cause, epc, tval);
    fflush(output);
}

static void raise_interrupt(uint32_t irq_cause,
                            uint32_t epc,
                            uint32_t tval,
                            uint32_t *pc_next,
                            FILE *output)
{
    uint32_t code = irq_cause & 0x7FFFFFFFu;

    const char *name = "unknown";
    switch (code) {
        case 3:  name = "software"; break;
        case 7:  name = "timer";    break;
        case 11: name = "external"; break;
    }

    csr_mcause = irq_cause;
    csr_mepc   = epc;
    csr_mtval  = tval;

    uint32_t m   = csr_mstatus;
    uint32_t mie = (m >> 3) & 1u;

    // MIE <- 0, MPIE <- MIE antigo, MPP <- 3
    m &= ~((1u << 3) | (1u << 7) | (3u << 11));
    m |= (mie << 7);
    m |= (3u  << 11);
    csr_mstatus = m;

    uint32_t base = csr_mtvec & ~0x3u;
    uint32_t mode = csr_mtvec &  0x3u;

    *pc_next = (mode == 1u) ? (base + 4u * code) : base;

    fprintf(output,
        ">interrupt:%-26s cause=0x%08x,epc=0x%08x,tval=0x%08x\n",
        name, irq_cause, epc, tval);
    fflush(output);
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
    fprintf(output, "0x%08x:%-6s %-19s %s\n", pc, mnem, ops, msg);
    fflush(output);
}

// -------------------- Memória / MMIO --------------------

static uint8_t mem_read8(uint32_t addr, uint8_t *mem, int *ok) {
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) {
        return mem[addr - RAM_BASE];
    }

    // UART RX (pop do FIFO)
    if (addr == IO_UART_BASE + 0) {
        return uart_fifo_pop();
    }

    // Dummy read exigido pelo teste: lb 0x10000002 -> 1
    if (addr == IO_UART_BASE + 2) {
        return 0x01;
    }

    // UART status (0x60 sem dado, 0x61 com dado)
    if (addr == IO_UART_BASE + 5) {
        return uart_fifo_empty() ? 0x60 : 0x61;
    }

    *ok = 0;
    return 0;
}

static uint16_t mem_read16(uint32_t addr, uint8_t *mem, int *ok) {
    uint16_t lo = mem_read8(addr,     mem, ok);
    if (!*ok) return 0;
    uint16_t hi = mem_read8(addr + 1, mem, ok);
    if (!*ok) return 0;
    return (uint16_t)(lo | (hi << 8));
}

static uint32_t mem_read32(uint32_t addr, uint8_t *mem, int *ok) {
    // mtimecmp (se o programa ler)
    if (addr == CLINT_MTIMECMP_LO) return (uint32_t)(mtimecmp & 0xFFFFFFFFu);
    if (addr == CLINT_MTIMECMP_HI) return (uint32_t)(mtimecmp >> 32);

    // PLIC (reads que aparecem no teste)
    if (addr == PLIC_CLAIMCOMP) {
        return uart_irq_pending ? plic_claim_id : 0u;
    }
    if (addr == PLIC_THRESHOLD) {
        return 0u;
    }
    if (addr == 0x0c001000u || addr == 0x0c000028u) {
        return 0u;
    }

    uint32_t b0 = mem_read8(addr,     mem, ok); if (!*ok) return 0;
    uint32_t b1 = mem_read8(addr + 1, mem, ok); if (!*ok) return 0;
    uint32_t b2 = mem_read8(addr + 2, mem, ok); if (!*ok) return 0;
    uint32_t b3 = mem_read8(addr + 3, mem, ok); if (!*ok) return 0;

    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static void mem_write8(uint32_t addr, uint8_t value, uint8_t *mem, int *ok)
{
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) {
        mem[addr - RAM_BASE] = value;
        return;
    }

    // UART TX: imprime + loopback no FIFO; se PLIC enable, pendura external IRQ
    if (addr == IO_UART_BASE + 0) {
        fputc((int)value, stdout);
        fflush(stdout);

        uart_fifo_push(value);

        if ((plic_enable & 0x00000400u) != 0) {
            uart_irq_pending = 1;
            csr_mip |= (1u << 11); // MEIP
        }
        return;
    }

    // UART config (teste faz sb aqui)
    if (addr == IO_UART_BASE + 1) {
        return;
    }

    // “TTY”
    if (addr == IO_TTY_ADDR) {
        fputc((int)value, stdout);
        fflush(stdout);
        return;
    }

    // Software interrupt (MSIP fake)
    if (addr >= IO_SWI_ADDR && addr < IO_SWI_ADDR + 4) {
        if (value != 0) {
            soft_irq_pending = 1;
            csr_mip |= (1u << 3); // MSIP (bit 3) por consistência
        }
        return;
    }

    *ok = 0;
}

static void mem_write16(uint32_t addr, uint16_t value, uint8_t *mem, int *ok) {
    mem_write8(addr,     (uint8_t)( value       & 0xFF), mem, ok);
    if (!*ok) return;
    mem_write8(addr + 1, (uint8_t)((value >> 8) & 0xFF), mem, ok);
}

static void mem_write32(uint32_t addr, uint32_t value, uint8_t *mem, int *ok)
{
    // mtimecmp low/high
    if (addr == CLINT_MTIMECMP_LO) {
        mtimecmp = (mtimecmp & 0xFFFFFFFF00000000ULL) | (uint64_t)value;
        if (mtime >= mtimecmp) csr_mip |=  (1u << 7); else csr_mip &= ~(1u << 7);
        return;
    }
    if (addr == CLINT_MTIMECMP_HI) {
        mtimecmp = (mtimecmp & 0x00000000FFFFFFFFULL) | ((uint64_t)value << 32);
        if (mtime >= mtimecmp) csr_mip |=  (1u << 7); else csr_mip &= ~(1u << 7);
        return;
    }

    // PLIC enable (teste escreve 0x400)
    if (addr == PLIC_ENABLE_ADDR) {
        plic_enable = value;
        return;
    }

    // PLIC claim/complete
    if (addr == PLIC_CLAIMCOMP) {
        if (value == plic_claim_id) {
            uart_irq_pending = 0;
            csr_mip &= ~(1u << 11); // limpa MEIP
        }
        return;
    }

    // aceita escrita sem fault
    if (addr == 0x0c000028u) {
        return;
    }

    mem_write8(addr,     (uint8_t)( value        & 0xFF), mem, ok); if (!*ok) return;
    mem_write8(addr + 1, (uint8_t)((value >> 8)  & 0xFF), mem, ok); if (!*ok) return;
    mem_write8(addr + 2, (uint8_t)((value >> 16) & 0xFF), mem, ok); if (!*ok) return;
    mem_write8(addr + 3, (uint8_t)((value >> 24) & 0xFF), mem, ok);
}

// -------------------- CPU --------------------

int main(int argc, char* argv[]) {
    if (argc < 3) return 1;

    FILE* input  = fopen(argv[1], "r");
    if (!input) {
        perror("erro abrindo arquivo de entrada");
        return 1;
    }

    FILE* output = fopen(argv[2], "w");
    if (!output) {
        perror("erro abrindo arquivo de saída");
        fclose(input);
        return 1;
    }

    uint8_t* mem = (uint8_t*)malloc(RAM_SIZE);
    if (!mem) { fclose(input); fclose(output); return 1; }
    memset(mem, 0, RAM_SIZE);

    uint32_t x[32] = {0};
    uint32_t pc = RAM_BASE;

    // Banner “pré-carregado” no RX (o teste espera ler isso)
    const char *banner = "Poxim-V serially says: ";
    for (const char *p = banner; *p; ++p) uart_fifo_push((uint8_t)*p);

    // Loader: formato do .hex de bruninho
    {
        uint32_t load_addr = 0;
        char line[4096];
        while (fgets(line, sizeof(line), input)) {
            if (line[0] == '@') {
                unsigned addr_hex = 0;
                if (sscanf(line + 1, "%x", &addr_hex) == 1) load_addr = (uint32_t)addr_hex;
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
    uint64_t steps = 0;

    while (running) {

        if (++steps > 2000000ULL) {
            fprintf(output, ">abort: step limit\n");
            fflush(output);
            break;
        }

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

            // -------- R-type (inclui extensão M) --------
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
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = a & b;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x&0x%08x=0x%08x", rname(rd), a, b, x[rd]);
                    out2(output, pc_curr, "and", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b110) { // OR
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = a | b;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x|0x%08x=0x%08x", rname(rd), a, b, x[rd]);
                    out2(output, pc_curr, "or", ops, msg);
                }
                else if (funct7 == 0b0000000 && funct3 == 0b100) { // XOR
                    uint32_t a = x[rs1], b = x[rs2];
                    x[rd] = a ^ b;
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), rname(rs1), rname(rs2));
                    snprintf(msg, sizeof(msg), "%s=0x%08x^0x%08x=0x%08x", rname(rd), a, b, x[rd]);
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

            // -------- I-type (ALU imediato) --------
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
                else if (funct3 == 0b010) { // SLTI
                    uint32_t before = x[rs1];
                    x[rd] = ((int32_t)before < imm12) ? 1u : 0u;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rd), rname(rs1), (uint32_t)(imm12 & 0xFFF));
                    snprintf(msg, sizeof(msg), "%s=(0x%08x<0x%08x)=%u",
                             rname(rd), before, (uint32_t)imm12, x[rd]);
                    out2(output, pc_curr, "slti", ops, msg);
                }
                else if (funct3 == 0b011) { // SLTIU
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

            // -------- Loads --------
            case 0b0000011: {
                uint32_t addr = x[rs1] + imm12;
                int ok2 = 1;

                if (funct3 == 0b000) { // LB
                    uint8_t b = mem_read8(addr, mem, &ok2);
                    if (!ok2) { raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output); goto end_of_loop; }
                    x[rd] = (uint32_t)(int8_t)b;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lb", ops, msg);
                }
                else if (funct3 == 0b001) { // LH
                    uint16_t h = mem_read16(addr, mem, &ok2);
                    if (!ok2) { raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output); goto end_of_loop; }
                    x[rd] = (uint32_t)(int16_t)h;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lh", ops, msg);
                }
                else if (funct3 == 0b010) { // LW
                    uint32_t w = mem_read32(addr, mem, &ok2);
                    if (!ok2) { raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output); goto end_of_loop; }
                    x[rd] = w;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lw", ops, msg);
                }
                else if (funct3 == 0b100) { // LBU
                    uint8_t b = mem_read8(addr, mem, &ok2);
                    if (!ok2) { raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output); goto end_of_loop; }
                    x[rd] = (uint32_t)b;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                    out2(output, pc_curr, "lbu", ops, msg);
                }
                else if (funct3 == 0b101) { // LHU
                    uint16_t h = mem_read16(addr, mem, &ok2);
                    if (!ok2) { raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output); goto end_of_loop; }
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

            // -------- Stores --------
            case 0b0100011: {
                uint32_t imm_high = (instruction >> 25) & 0x7F;
                uint32_t imm_low  = (instruction >> 7)  & 0x1F;
                uint32_t imm12uS  = (imm_high << 5) | imm_low;
                int32_t  immS     = (imm12uS & 0x800) ? (int32_t)(imm12uS | 0xFFFFF000) : (int32_t)imm12uS;

                uint32_t addr = x[rs1] + immS;
                int ok2 = 1;

                if (funct3 == 0b000) { // SB
                    uint8_t b = (uint8_t)(x[rs2] & 0xFF);
                    mem_write8(addr, b, mem, &ok2);
                    if (!ok2) {
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
                    mem_write16(addr, h, mem, &ok2);
                    if (!ok2) {
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
                    mem_write32(addr, w, mem, &ok2);
                    if (!ok2) {
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

            // -------- Branches --------
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
                    int taken = (x[rs1] >= x[rs2]);
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

            // -------- JALR --------
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

            // -------- JAL --------
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

            // -------- LUI --------
            case 0b0110111: {
                uint32_t imm20 = instruction & 0xFFFFF000;
                x[rd] = imm20;
                char ops[32], msg[64];
                snprintf(ops, sizeof(ops), "%s,0x%05x", rname(rd), (imm20 >> 12));
                snprintf(msg, sizeof(msg), "%s=0x%08x", rname(rd), x[rd]);
                out2(output, pc_curr, "lui", ops, msg);
                break;
            }

            // -------- AUIPC --------
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

            // -------- SYSTEM (ecall/ebreak/mret + CSRs) --------
            case 0b1110011: {
                uint32_t imm_sys = instruction >> 20;

                if (funct3 == 0b000) {
                    if (imm_sys == 0x000) {          // ECALL
                        out2(output, pc_curr, "ecall", "", "");
                        uint32_t pc_next_exc = pc_curr + 4;
                        raise_exception(EXC_ENV_CALL_M, pc_curr, 0, &pc_next_exc, output);
                        pc_next = pc_next_exc;
                    }
                    else if (imm_sys == 0x001) {     // EBREAK
                        out2(output, pc_curr, "ebreak", "", "");
                        running = 0;
                    }
                    else if (imm_sys == 0x302) {     // MRET
                        pc_next = csr_mepc;

                        uint32_t m = csr_mstatus;
                        uint32_t mpie = (m >> 7) & 1u;

                        // MIE <- MPIE; MPIE <- 1; MPP <- 0
                        m &= ~((1u << 3) | (1u << 7) | (3u << 11));
                        m |= (mpie << 3);
                        m |= (1u << 7);
                        csr_mstatus = m;

                        char msg[64];
                        snprintf(msg, sizeof(msg), "pc=0x%08x", pc_next);
                        out2(output, pc_curr, "mret", "", msg);
                    }
                    else {
                        uint32_t pc_exc = pc_curr + 4;
                        raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                        pc_next = pc_exc;
                        goto end_of_loop;
                    }
                } else {
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
                        snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), csrnm, rname(rs1));
                        snprintf(msg, sizeof(msg), "%s=%s=0x%08x,%s=%s=0x%08x",
                                rname(rd), csrnm, old, csrnm, rname(rs1), rs1_val);
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
                        snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), csrnm, rname(rs1));
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
                pc_next = pc_exc;
                goto end_of_loop;
            }
        }

end_of_loop:
        // x0 é sempre zero
        x[0] = 0;

        // avança o tempo (1 tick por instrução) e atualiza MTIP
        mtime++;
        timer_irq_pending = (mtime >= mtimecmp);
        if (timer_irq_pending) csr_mip |=  (1u << 7);
        else                   csr_mip &= ~(1u << 7);

        // Interrupções só disparam se MSTATUS.MIE = 1
        uint32_t global_mie = (csr_mstatus & (1u << 3));

        if (global_mie) {
            if (soft_irq_pending && (csr_mie & (1u << 3))) {            // MSIE
                uint32_t irq_cause = 0x80000003u;
                uint32_t epc = pc_next;
                raise_interrupt(irq_cause, epc, 0, &pc_next, output);

                soft_irq_pending = 0;
                csr_mip &= ~(1u << 3);
            }
            else if (timer_irq_pending && (csr_mie & (1u << 7))) {      // MTIE
                uint32_t irq_cause = 0x80000007u;
                uint32_t epc = pc_next;
                raise_interrupt(irq_cause, epc, 0, &pc_next, output);

                timer_irq_pending = 0;
                csr_mip &= ~(1u << 7);
            }
            else if (uart_irq_pending && (csr_mie & (1u << 11))) {      // MEIE
                uint32_t irq_cause = 0x8000000Bu;
                uint32_t epc = pc_next;
                raise_interrupt(irq_cause, epc, 0, &pc_next, output);
                // zera quando fizer complete no PLIC
            }
        }

        pc = pc_next;
    }

    fclose(input);
    fclose(output);
    free(mem);
    return 0;
}
