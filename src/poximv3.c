/**
 * ============================================================================
 * Poxim-V: RISC-V RV32I/M Full Architecture Simulator with L1 Caches & MMIO
 * ============================================================================
 * 
 * Descrição do Módulo:
 *   Simulador completo de nível de produção para arquitetura RISC-V de 32 bits.
 *   Consolida o núcleo de execução inteira (RV32I) com aceleração de multiplicação
 *   e divisão por hardware (RV32M), registradores de controle e estado (CSRs),
 *   controlador de interrupções e exceções em Modo Máquina (M-Mode), emulação
 *   de periféricos mapeados em memória (MMIO) e hierarquia de memória cache L1
 *   separada (Harvard split: ICache e DCache).
 *
 * Especificações Arquiteturais:
 *   - ISA Base: RV32I (instruções aritméticas, lógicas, de salto, desvio condicional, loads/stores)
 *   - Extensão Padrão: RV32M (multiplicação MUL/MULH/MULHSU/MULHU e divisão DIV/DIVU/REM/REMU)
 *   - Modo Privilegiado: Machine Mode (M-Mode) com suporte a traps e instrução mret
 *   - Registradores CSR: mstatus, mtvec (direto/vetorado), mepc, mcause, mtval, mie, mip, contadores
 *   - Interrupções e Exceções:
 *       * Exceções síncronas: instrução ilegal, ecall, alinhamento/falha de acesso
 *       * Interrupções assíncronas: MSIP (software), MTIP (temporizador CLINT), MEIP (externa PLIC)
 *   - Subsistema de Memória Cache L1:
 *       * Arquitetura dividida: Cache de Instruções (ICache) e Cache de Dados (DCache)
 *       * Dimensões: 256 bytes por cache, blocos de 16 bytes (4 words), associativa por conjunto em 2 vias (8 sets)
 *       * Políticas: Write-through, Write-allocate no hit / No-write-allocate no miss, substituição LRU
 *       * Sincronização: Instruções FENCE e FENCE.I (invalidação de linhas de instrução)
 *       * Telemetria: Rastreamento em tempo de execução de acessos, hits, misses e taxa de acerto final
 * ============================================================================
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

// ---------------------------------------------------------------------------
// DEFINIÇÕES E CONSTANTES
// ---------------------------------------------------------------------------

// RAM
#define RAM_BASE   0x80000000u
#define RAM_SIZE   (32u * 1024u)

// MMIO 
#define IO_UART_BASE  0x10000000u  
#define IO_TTY_ADDR   0x10000002u
#define IO_SWI_ADDR   0x02000000u   

// CLINT fake: mtimecmp (low/high)
#define CLINT_MTIMECMP_LO 0x02004000u
#define CLINT_MTIMECMP_HI 0x02004004u

#define CLINT_MTIME_LO    0x0200BFF8u
#define CLINT_MTIME_HI    0x0200BFFCu

// PLIC fake 
#define PLIC_ENABLE_ADDR   0x0c002000u
#define PLIC_THRESHOLD     0x0c200000u
#define PLIC_CLAIMCOMP     0x0c200004u
#define PLIC_PRIORITY_10   0x0c000028u

// CSRs (modo máquina)
static uint32_t csr_mstatus = 0;
static uint32_t csr_mtvec   = 0;
static uint32_t csr_mepc    = 0;
static uint32_t csr_mcause  = 0;
static uint32_t csr_mtval   = 0;
static uint32_t csr_mie     = 0;
static uint32_t csr_mip     = 0;

// Timer 
static uint64_t mtime    = 0;
static uint64_t mtimecmp = 0;  

// Pendências
static int soft_irq_pending  = 0;
static int timer_irq_pending = 0;

// UART FIFO (loopback)
static uint8_t  uart_fifo[4096];
static uint32_t uart_head = 0, uart_tail = 0;

// PLIC (estado interno)
static uint32_t plic_priority_10 = 0;  
static uint32_t plic_enable      = 0;  
static uint32_t plic_threshold   = 0;  

static int plic_irq10_pending = 0;     
//static int uart_irq_pending = 0;        
static int uart_ready = 1;            
//static int uart_eos_left = 0;
//static int uart_eos_seen = 0;
//static int uart_eos_deferred = 0;
//static int uart_post_timer = 0;

static uint32_t mem_read32_raw(uint32_t addr, uint8_t *mem, int *ok);
static void mem_write32_raw(uint32_t addr, uint32_t value, uint8_t *mem, int *ok);
static void mem_write8_raw(uint32_t addr, uint8_t value, uint8_t *mem, int *ok);
static void mem_write16_raw(uint32_t addr, uint16_t value, uint8_t *mem, int *ok);

// -------------------- CACHE  --------------------

#define CACHE_SIZE_BYTES   256u
#define CACHE_BLOCK_WORDS  4u
#define CACHE_WORD_BYTES   4u
#define CACHE_BLOCK_BYTES  16u
#define CACHE_WAYS         2u
#define CACHE_LINES        (CACHE_SIZE_BYTES / CACHE_BLOCK_BYTES) // 16
#define CACHE_SETS         (CACHE_LINES / CACHE_WAYS)             // 8

typedef struct {
    uint8_t  valid;
    uint32_t tag;
    uint32_t data[CACHE_BLOCK_WORDS]; 
    uint64_t last_access_time;
} cache_line_t;

typedef struct {
    cache_line_t way[CACHE_WAYS];
    uint8_t lru; 
} cache_set_t;

typedef struct {
    cache_set_t set[CACHE_SETS];
    uint64_t accesses;
    uint64_t hits;
} cache_t;

static cache_t icache, dcache;

static inline int is_ram(uint32_t addr) {
    return (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE);
}

static inline int cacheable(uint32_t addr) {
    if (addr >= IO_UART_BASE && addr < IO_UART_BASE + 0x1000) return 0;
    if (addr >= 0x02000000u && addr < 0x0200FFFFu) return 0; // CLINT
    if (addr >= 0x0C000000u && addr < 0x0C2FFFFFu) return 0; // PLIC
    return 1;
}

static inline uint32_t block_base(uint32_t addr) { return addr & ~(CACHE_BLOCK_BYTES - 1u); }
static inline uint32_t set_index(uint32_t addr)  { return (addr >> 4) & (CACHE_SETS - 1u); } 
static inline uint32_t tag_of(uint32_t addr)     { return addr >> 7; }
static inline uint32_t word_index(uint32_t addr) { return (addr >> 2) & 3u; }

static void cache_reset(cache_t *c) {
    memset(c, 0, sizeof(*c));
}

static __attribute__((unused)) void cache_invalidate(cache_t *c, uint32_t addr) {
    if (!cacheable(addr)) return;
    
    uint32_t si = set_index(addr);
    uint32_t tg = tag_of(addr);
    cache_set_t *S = &c->set[si];
    
    for (int w = 0; w < (int)CACHE_WAYS; w++) {
        if (S->way[w].valid && S->way[w].tag == tg) {
            S->way[w].valid = 0;
        }
    }
}

static uint32_t cache_read32(cache_t *c, const char *name, uint32_t addr, uint8_t *mem, FILE *output, int *ok) {
    if (!cacheable(addr)) {
        return mem_read32_raw(addr, mem, ok);
    }

    c->accesses++;
    uint32_t si = set_index(addr);
    uint32_t tg = tag_of(addr);
    uint32_t wi = word_index(addr);
    cache_set_t *S = &c->set[si];

    // 1. Tentar encontrar HIT
    int hit_way = -1;
    for (int w = 0; w < (int)CACHE_WAYS; w++) {
        if (S->way[w].valid && S->way[w].tag == tg) {
            hit_way = w;
            break;
        }
    }

    if (hit_way >= 0) {
        c->hits++;
        S->lru = (uint8_t)(1 - hit_way);
        S->way[hit_way].last_access_time = c->accesses;

        fprintf(output, "#cache_mem:%srh    0x%08x          line=%u,age=0,id=0x%07x,block[%d]={0x%08x,0x%08x,0x%08x,0x%08x}\n",
                name, addr, si, tg, hit_way,
                S->way[hit_way].data[0], S->way[hit_way].data[1],
                S->way[hit_way].data[2], S->way[hit_way].data[3]);
        //fflush(output);
        *ok = 1;
        return S->way[hit_way].data[wi];
    }

    // 2. Processar MISS
    uint32_t diff0 = S->way[0].valid ? (uint32_t)(c->accesses - S->way[0].last_access_time) : 0;
    uint32_t age0  = (diff0 > 255) ? 255 : diff0;

    uint32_t diff1 = S->way[1].valid ? (uint32_t)(c->accesses - S->way[1].last_access_time) : 0;
    uint32_t age1  = (diff1 > 255) ? 255 : diff1;

    fprintf(output, "#cache_mem:%srm    0x%08x          line=%u,valid={%d,%d},age={%u,%u},id={0x%07x,0x%07x}\n",
            name, addr, si,
            S->way[0].valid, S->way[1].valid,
            age0, age1,
            S->way[0].tag,   S->way[1].tag);
    //fflush(output);

    // Selecionar vítima
    int victim = -1;
    for (int w = 0; w < (int)CACHE_WAYS; w++) {
        if (!S->way[w].valid) { victim = w; break; }
    }
    if (victim < 0) victim = S->lru;

    // Buscar bloco na memória
    uint32_t base = block_base(addr);
    int ok2 = 1;
    uint32_t block[CACHE_BLOCK_WORDS];
    
    for (int i = 0; i < 4; i++) {
        block[i] = mem_read32_raw(base + 4u*(uint32_t)i, mem, &ok2);
        if (!ok2) {
            *ok = 0;
            return 0;
        }
    }

    // Atualizar Cache 
    S->way[victim].valid = 1;
    S->way[victim].tag   = tg;
    S->way[victim].last_access_time = c->accesses;
    for (int i = 0; i < 4; i++) S->way[victim].data[i] = block[i];
    
    // Atualizar LRU
    S->lru = (uint8_t)(1 - victim);

    *ok = 1;
    return S->way[victim].data[wi];
}

static void dcache_write8(uint32_t addr, uint8_t value, uint8_t *mem, FILE *output, int *ok) {
    if (!cacheable(addr)) { mem_write8_raw(addr, value, mem, ok); return; }
    dcache.accesses++;

    mem_write8_raw(addr, value, mem, ok);

    uint32_t si = set_index(addr);
    uint32_t tg = tag_of(addr);
    cache_set_t *S = &dcache.set[si];

    int hit_way = -1;
    for (int w = 0; w < (int)CACHE_WAYS; w++) {
        if (S->way[w].valid && S->way[w].tag == tg) { hit_way = w; break; }
    }

    if (hit_way >= 0) {
        dcache.hits++;
        uint32_t base = block_base(addr);
        for(int i=0; i<4; i++) {
            int dummy; S->way[hit_way].data[i] = mem_read32_raw(base + i*4, mem, &dummy);
        }
        S->lru = (uint8_t)(1 - hit_way);
        S->way[hit_way].last_access_time = dcache.accesses;
        
        fprintf(output, "#cache_mem:dwh    0x%08x          line=%u,age=0,id=0x%07x,block[%d]={0x%08x,0x%08x,0x%08x,0x%08x}\n",
                addr, si, tg, hit_way, S->way[hit_way].data[0], S->way[hit_way].data[1], S->way[hit_way].data[2], S->way[hit_way].data[3]);
    } else {
        uint32_t age0 = S->way[0].valid ? (uint32_t)(dcache.accesses - S->way[0].last_access_time) : 0;
        uint32_t age1 = S->way[1].valid ? (uint32_t)(dcache.accesses - S->way[1].last_access_time) : 0;
        fprintf(output, "#cache_mem:dwm    0x%08x          line=%u,valid={%d,%d},age={%u,%u},id={0x%07x,0x%07x}\n",
                addr, si, S->way[0].valid, S->way[1].valid, age0, age1, S->way[0].tag, S->way[1].tag);
    }
}

static void dcache_write16(uint32_t addr, uint16_t value, uint8_t *mem, FILE *output, int *ok) {
    if (!cacheable(addr)) { mem_write16_raw(addr, value, mem, ok); return; }
    dcache.accesses++;
    mem_write16_raw(addr, value, mem, ok);

    uint32_t si = set_index(addr);
    uint32_t tg = tag_of(addr);
    cache_set_t *S = &dcache.set[si];

    int hit_way = -1;
    for (int w = 0; w < (int)CACHE_WAYS; w++) {
        if (S->way[w].valid && S->way[w].tag == tg) { hit_way = w; break; }
    }

    if (hit_way >= 0) {
        dcache.hits++;
        uint32_t base = block_base(addr);
        for(int i=0; i<4; i++) {
            int dummy; S->way[hit_way].data[i] = mem_read32_raw(base + i*4, mem, &dummy);
        }
        S->lru = (uint8_t)(1 - hit_way);
        S->way[hit_way].last_access_time = dcache.accesses;
        
        fprintf(output, "#cache_mem:dwh    0x%08x          line=%u,age=0,id=0x%07x,block[%d]={0x%08x,0x%08x,0x%08x,0x%08x}\n",
                addr, si, tg, hit_way, S->way[hit_way].data[0], S->way[hit_way].data[1], S->way[hit_way].data[2], S->way[hit_way].data[3]);
    } else {
        uint32_t age0 = S->way[0].valid ? (uint32_t)(dcache.accesses - S->way[0].last_access_time) : 0;
        uint32_t age1 = S->way[1].valid ? (uint32_t)(dcache.accesses - S->way[1].last_access_time) : 0;
        fprintf(output, "#cache_mem:dwm    0x%08x          line=%u,valid={%d,%d},age={%u,%u},id={0x%07x,0x%07x}\n",
                addr, si, S->way[0].valid, S->way[1].valid, age0, age1, S->way[0].tag, S->way[1].tag);
    }
}

static void dcache_write32(uint32_t addr, uint32_t value, uint8_t *mem, FILE *output, int *ok) {
    if (!cacheable(addr)) { mem_write32_raw(addr, value, mem, ok); return; }
    dcache.accesses++;
    mem_write32_raw(addr, value, mem, ok);

    uint32_t si = set_index(addr);
    uint32_t tg = tag_of(addr);
    cache_set_t *S = &dcache.set[si];

    int hit_way = -1;
    for (int w = 0; w < (int)CACHE_WAYS; w++) {
        if (S->way[w].valid && S->way[w].tag == tg) { hit_way = w; break; }
    }

    if (hit_way >= 0) {
        dcache.hits++;
        uint32_t base = block_base(addr);
        for(int i=0; i<4; i++) {
            int dummy; S->way[hit_way].data[i] = mem_read32_raw(base + i*4, mem, &dummy);
        }
        S->lru = (uint8_t)(1 - hit_way);
        S->way[hit_way].last_access_time = dcache.accesses;
        
        fprintf(output, "#cache_mem:dwh    0x%08x          line=%u,age=0,id=0x%07x,block[%d]={0x%08x,0x%08x,0x%08x,0x%08x}\n",
                addr, si, tg, hit_way, S->way[hit_way].data[0], S->way[hit_way].data[1], S->way[hit_way].data[2], S->way[hit_way].data[3]);
    } else {
        uint32_t age0 = S->way[0].valid ? (uint32_t)(dcache.accesses - S->way[0].last_access_time) : 0;
        uint32_t age1 = S->way[1].valid ? (uint32_t)(dcache.accesses - S->way[1].last_access_time) : 0;
        fprintf(output, "#cache_mem:dwm    0x%08x          line=%u,valid={%d,%d},age={%u,%u},id={0x%07x,0x%07x}\n",
                addr, si, S->way[0].valid, S->way[1].valid, age0, age1, S->way[0].tag, S->way[1].tag);
    }
}


// Códigos de Exceção
enum {
    EXC_INST_FAULT       = 1,
    EXC_ILLEGAL_INST     = 2,
    EXC_LOAD_MISALIGNED  = 4, 
    EXC_LOAD_FAULT       = 5,
    EXC_STORE_MISALIGNED = 6, 
    EXC_STORE_FAULT      = 7,
    EXC_ENV_CALL_M       = 11,
};

// ---------------------------------------------------------------------------
// FUNÇÕES AUXILIARES UART (FIFO)
// ---------------------------------------------------------------------------

static int uart_fifo_empty(void) {
    return (uart_head == uart_tail);
}

static __attribute__((unused)) void uart_fifo_push(uint8_t val) {
    uint32_t next = (uart_head + 1) % 4096;
    if (next != uart_tail) {
        uart_fifo[uart_head] = val;
        uart_head = next;
    }
}

static uint8_t uart_fifo_pop(void) {
    if (uart_head == uart_tail) return 0;
    uint8_t val = uart_fifo[uart_tail];
    uart_tail = (uart_tail + 1) % 4096;
    
    // if (uart_head == uart_tail) {
    //    uart_eos_left = 1;
    //    uart_eos_deferred = 1;
    // }
    // -------------------------
    
    return val;
}

// ---------------------------------------------------------------------------
// CSRs e EXCEÇÕES
// ---------------------------------------------------------------------------

static uint32_t csr_read(uint32_t addr) {
    switch (addr) {
        case 0x300: return csr_mstatus;
        case 0x305: return csr_mtvec;
        case 0x341: return csr_mepc;
        case 0x342: return csr_mcause;
        case 0x343: return csr_mtval;
        case 0x304: return csr_mie;
        case 0x344: return csr_mip;
        
        case 0xC00: // cycle
        case 0xC01: // time
        case 0xB00: // mcycle
        case 0xB02: // minstret
            return (uint32_t)(mtime & 0xFFFFFFFF);
            
        case 0xC80: // cycleh
        case 0xC81: // timeh
        case 0xB80: // mcycleh
        case 0xB82: // minstreth
            
            return (uint32_t)(mtime >> 32);
        

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
        case EXC_INST_FAULT:       name = "instruction_fault";        break;
        case EXC_ILLEGAL_INST:     name = "illegal_instruction";      break;
        case EXC_LOAD_MISALIGNED:  name = "load_address_misaligned";  break; 
        case EXC_LOAD_FAULT:       name = "load_fault";               break;
        case EXC_STORE_MISALIGNED: name = "store_address_misaligned"; break; 
        case EXC_STORE_FAULT:      name = "store_fault";              break;
        case EXC_ENV_CALL_M:       name = "environment_call";         break;
    }

    csr_mcause = cause;
    csr_mepc   = epc;
    csr_mtval  = tval;

    uint32_t m   = csr_mstatus;
    uint32_t mie = (m >> 3) & 1u;

    m &= ~((1u << 3) | (1u << 7) | (3u << 11));
    m |= (mie << 7);
    m |= (3u  << 11);
    csr_mstatus = m;

    uint32_t base = csr_mtvec & ~0x3u;
    if (base == 0 || !is_ram(base)) { 
        fprintf(output, ">abort: exception jump to invalid mtvec 0x%08x\n", base);
        exit(1); 
    }
    *pc_next = base;

    fprintf(output,
        ">exception:%-26s cause=0x%08x,epc=0x%08x,tval=0x%08x\n",
        name, cause, epc, tval);
    //fflush(output);
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
    //fflush(output);
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
    int ops_empty = (!ops || ops[0] == '\0');
    int msg_empty = (!msg || msg[0] == '\0');

    if (ops_empty && msg_empty) {
        fprintf(output, "0x%08x:%-6s\n", pc, mnem);
    } else {
        fprintf(output, "0x%08x:%-6s %-19s %s\n", pc, mnem, ops, msg);
    }
    //fflush(output);
}


// -------------------- Memória / MMIO --------------------

static uint8_t mem_read8_raw(uint32_t addr, uint8_t *mem, int *ok) {
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) {
        *ok = 1;
        return mem[addr - RAM_BASE];
    }

    // UART RX (pop do FIFO)
    if (addr == IO_UART_BASE + 0) {
        *ok = 1;
        return uart_fifo_pop();
    }

    // UART Status
    if (addr == IO_UART_BASE + 2) {
        *ok = 1;
        if (!uart_ready) return 0x00;
        if (uart_fifo_empty()) {
            // if (uart_eos_left > 0 || uart_post_timer) uart_eos_seen = 1;
            return 0x00; 
        }
        return 0x01;     
    }

    // UART Line Status 
    if (addr == IO_UART_BASE + 5) {
        *ok = 1; 
        return uart_fifo_empty() ? 0x60 : 0x61;
    }

    *ok = 0;
    return 0;
}

static uint16_t mem_read16_raw(uint32_t addr, uint8_t *mem, int *ok) {
    uint16_t lo = mem_read8_raw(addr,     mem, ok);
    if (!*ok) return 0;
    uint16_t hi = mem_read8_raw(addr + 1, mem, ok);
    if (!*ok) return 0;
    return (uint16_t)(lo | (hi << 8));
}

static uint32_t mem_read32_raw(uint32_t addr, uint8_t *mem, int *ok) {
    // CLINT mtime 
    if (addr == CLINT_MTIME_LO) { *ok=1; return (uint32_t)(mtime & 0xFFFFFFFFu); }
    if (addr == CLINT_MTIME_HI) { *ok=1; return (uint32_t)(mtime >> 32); }

    // mtimecmp 
    if (addr == CLINT_MTIMECMP_LO) { *ok=1; return (uint32_t)(mtimecmp & 0xFFFFFFFFu); }
    if (addr == CLINT_MTIMECMP_HI) { *ok=1; return (uint32_t)(mtimecmp >> 32); }

    // ---------- PLIC (reads) ----------
    if (addr == PLIC_CLAIMCOMP) {
        *ok = 1;
        return (csr_mip & (1u << 11)) ? 10u : 0u;
    }

    if (addr == PLIC_THRESHOLD) {
        *ok = 1;
        return plic_threshold;
    }

    if (addr == PLIC_PRIORITY_10) {
        *ok = 1;
        return plic_priority_10;
    }

    if (addr == PLIC_ENABLE_ADDR) {
        *ok = 1;
        return plic_enable;
    }

    if (addr == 0x0c001000u) {
        *ok = 1;
        return 0u;
    }

    uint32_t b0 = mem_read8_raw(addr,     mem, ok); if (!*ok) return 0;
    uint32_t b1 = mem_read8_raw(addr + 1, mem, ok); if (!*ok) return 0;
    uint32_t b2 = mem_read8_raw(addr + 2, mem, ok); if (!*ok) return 0;
    uint32_t b3 = mem_read8_raw(addr + 3, mem, ok); if (!*ok) return 0;

    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static void mem_write8_raw(uint32_t addr, uint8_t value, uint8_t *mem, int *ok)
{
    if (addr >= RAM_BASE && addr < RAM_BASE + RAM_SIZE) {
        mem[addr - RAM_BASE] = value;
        *ok = 1;
        return;
    }

    // UART TX 
    if (addr == IO_UART_BASE + 0) {
        // uart_fifo_push(value); 
        // if (uart_ready) plic_irq10_pending = 1; 
        
        putchar((char)value); 
        
        *ok = 1;
        return;
    }

    // UART config 
    if (addr == IO_UART_BASE + 1) {
        uart_ready = 1;
        *ok = 1;
        return;
    }

    // Software interrupt (MSIP fake)
    if (addr >= IO_SWI_ADDR && addr < IO_SWI_ADDR + 4u) {
        if (addr == IO_SWI_ADDR) {
            if (value != 0) csr_mip |=  (1u << 3);  // MSIP
            else {
                csr_mip &= ~(1u << 3);
            }
        }
        *ok = 1;
        return;
    }

    // CLINT mtime 
    if (addr >= CLINT_MTIME_LO && addr < (CLINT_MTIME_HI + 4u)) {
        *ok = 1;
        return;
    }

    *ok = 0;
}

static void mem_write16_raw(uint32_t addr, uint16_t value, uint8_t *mem, int *ok) {
    mem_write8_raw(addr,     (uint8_t)( value        & 0xFF), mem, ok);
    if (!*ok) return;
    mem_write8_raw(addr + 1, (uint8_t)((value >> 8)  & 0xFF), mem, ok);
}

static void mem_write32_raw(uint32_t addr, uint32_t value, uint8_t *mem, int *ok)
{
    // CLINT mtime 
    if (addr == CLINT_MTIME_LO || addr == CLINT_MTIME_HI) {
        *ok = 1;
        return; 
    }

    // mtimecmp low/high
    if (addr == CLINT_MTIMECMP_LO) {
        mtimecmp = (mtimecmp & 0xFFFFFFFF00000000ULL) | (uint64_t)value;
        if (mtime >= mtimecmp) csr_mip |=  (1u << 7); else csr_mip &= ~(1u << 7);
        *ok = 1;
        return;
    }
    if (addr == CLINT_MTIMECMP_HI) {
        mtimecmp = (mtimecmp & 0x00000000FFFFFFFFULL) | ((uint64_t)value << 32);
        if (mtime >= mtimecmp) csr_mip |=  (1u << 7); else csr_mip &= ~(1u << 7);
        *ok = 1;
        return;
    }

    // ---------- PLIC (writes) ----------
    if (addr == PLIC_PRIORITY_10) {
        plic_priority_10 = value;
        *ok = 1;
        return;
    }

    if (addr == PLIC_ENABLE_ADDR) {
        plic_enable = value;
        *ok = 1;
        return;
    }

    if (addr == PLIC_THRESHOLD) {
        plic_threshold = value;
        *ok = 1;
        return;
    }

    if (addr == PLIC_CLAIMCOMP) {
        if (value == 10u) {
            plic_irq10_pending = 0;
        }
        *ok = 1;
        return;
    }

    // fallback
    mem_write8_raw(addr,     (uint8_t)( value        & 0xFF), mem, ok); if (!*ok) return;
    mem_write8_raw(addr + 1, (uint8_t)((value >> 8)  & 0xFF), mem, ok); if (!*ok) return;
    mem_write8_raw(addr + 2, (uint8_t)((value >> 16) & 0xFF), mem, ok); if (!*ok) return;
    mem_write8_raw(addr + 3, (uint8_t)((value >> 24) & 0xFF), mem, ok);
}


// -------------------- CPU / MAIN --------------------

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
    static char output_buffer[65536];
    setvbuf(output, output_buffer, _IOFBF, sizeof(output_buffer));

    uint8_t* mem = (uint8_t*)malloc(RAM_SIZE);
    if (!mem) { fclose(input); fclose(output); return 1; }
    memset(mem, 0, RAM_SIZE);

    cache_reset(&icache);
    cache_reset(&dcache);

    mtime = 0;
    mtimecmp = 0xFFFFFFFFFFFFFFFFULL;
    uart_ready = 1;

    uint32_t x[32] = {0};
    uint32_t pc = RAM_BASE;

    //const char *banner = "Poxim-V server 0.1\n";
    //for (const char *p = banner; *p; ++p) uart_fifo_push((uint8_t)*p);

    // Loader
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

        if (++steps > 500000ULL) {
            fprintf(output, ">abort: step limit\n");
            //fflush(output);
            break;
        }

        int ok = 1;
        uint32_t pc_curr = pc;
        uint32_t pc_next = pc + 4;

        uint32_t instruction = cache_read32(&icache, "i", pc, mem, output, &ok);
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

            // -------- R-type --------
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
                    snprintf(msg, sizeof(msg), "%s=0x%08x>>>%u=0x%08x", rname(rd), (uint32_t)before, sh, x[rd]);
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

            // -------- I-type --------
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
                        snprintf(msg, sizeof(msg), "%s=0x%08x>>>%u=0x%08x", rname(rd), (uint32_t)before, shamt, x[rd]);
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

                // 1. Aciona a Cache apenas para gerar o log exigido e atualizar o Hit Rate
                if (cacheable(addr)) {
                    int dummy;
                    cache_read32(&dcache, "d", addr, mem, output, &dummy);
                }

                // 2. Extrai os bytes perfeitamente direto da RAM (suporta desalinhamento nativo!)
                const char *mnem = "load";
                if (funct3 == 0b000) {      // LB
                    x[rd] = (int32_t)(int8_t)mem_read8_raw(addr, mem, &ok2);
                    mnem = "lb";
                }
                else if (funct3 == 0b001) { // LH
                    x[rd] = (int32_t)(int16_t)mem_read16_raw(addr, mem, &ok2);
                    mnem = "lh";
                }
                else if (funct3 == 0b010) { // LW
                    x[rd] = mem_read32_raw(addr, mem, &ok2);
                    mnem = "lw";
                }
                else if (funct3 == 0b100) { // LBU
                    x[rd] = (uint32_t)mem_read8_raw(addr, mem, &ok2);
                    mnem = "lbu";
                }
                else if (funct3 == 0b101) { // LHU
                    x[rd] = (uint32_t)mem_read16_raw(addr, mem, &ok2);
                    mnem = "lhu";
                }
                else {
                    uint32_t pc_exc = pc_curr + 4;
                    raise_exception(EXC_ILLEGAL_INST, pc_curr, instruction, &pc_exc, output);
                    pc_next = pc_exc;
                    goto end_of_loop;
                }
                
                if (!ok2) { 
                    raise_exception(EXC_LOAD_FAULT, pc_curr, addr, &pc_next, output); 
                    goto end_of_loop; 
                }

                // Imprime a linha do registrador na saída
                char ops[32], msg[128];
                snprintf(ops, sizeof(ops), "%s,0x%03x(%s)", rname(rd), (uint32_t)(imm12 & 0xFFF), rname(rs1));
                snprintf(msg, sizeof(msg), "%s=mem[0x%08x]=0x%08x", rname(rd), addr, x[rd]);
                out2(output, pc_curr, mnem, ops, msg);
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
                    
                    dcache_write8(addr, b, mem, output, &ok2);
                    
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

                    dcache_write16(addr, h, mem, output, &ok2);

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
                    dcache_write32(addr, w, mem, output, &ok2);
                    
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

                int taken = 0;

                if (funct3 == 0b000) { // BEQ
                    taken = (x[rs1] == x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x==0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "beq", ops, msg);
                }
                else if (funct3 == 0b001) { // BNE
                    taken = (x[rs1] != x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x!=0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bne", ops, msg);
                }
                else if (funct3 == 0b100) { // BLT
                    taken = ((int32_t)x[rs1] < (int32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x<0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "blt", ops, msg);
                }
                else if (funct3 == 0b101) { // BGE
                    taken = ((int32_t)x[rs1] >= (int32_t)x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x>=0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bge", ops, msg);
                }
                else if (funct3 == 0b110) { // BLTU
                    taken = (x[rs1] < x[rs2]);
                    uint32_t newpc = taken ? target : (pc_curr + 4);
                    if (taken) pc_next = target;
                    char ops[32], msg[128];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%03x", rname(rs1), rname(rs2), shown_off);
                    snprintf(msg, sizeof(msg), "(0x%08x<0x%08x)=%d->pc=0x%08x",
                             x[rs1], x[rs2], taken, newpc);
                    out2(output, pc_curr, "bltu", ops, msg);
                }
                else if (funct3 == 0b111) { // BGEU
                    taken = (x[rs1] >= x[rs2]);
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
                if (taken && target == pc_curr) {
                    fprintf(output, ">halt: branch infinite loop detected at 0x%08x\n", pc_curr);
                    running = 0; 
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

                if (target == pc_curr) {
                    fprintf(output, ">halt: infinite loop detected at 0x%08x\n", pc_curr);
                    running = 0; 
                }

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
                        char ops[32] = "", msg[128] = "";
                        out2(output, pc_curr, "ebreak", ops, msg);
                        
                        int dummy;
                        cache_read32(&icache, "i", pc_curr - 4, mem, output, &dummy);
                        cache_read32(&icache, "i", pc_curr + 4, mem, output, &dummy);
                        
                        running = 0; 
                    }
                    else if (imm_sys == 0x105) {     // WFI
                        out2(output, pc_curr, "wfi", "", "");
                        
                        uint32_t timer_mie  = (csr_mie >> 7) & 1;
                        
                        if (timer_mie && mtime < mtimecmp && mtimecmp != -1ULL) {
                            mtime = mtimecmp; 
                        }
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
                    else if (funct3 == 0b011) { // CSRRC
                    uint32_t rs1_val = x[rs1];
                    uint32_t new_csr = old;
                    if (rs1 != 0) {
                        new_csr = old & ~rs1_val;
                        csr_write(csr_addr, new_csr);
                    }
                    if (rd != 0) x[rd] = old;
                    char ops[32], msg[160];
                    snprintf(ops, sizeof(ops), "%s,%s,%s", rname(rd), csrnm, rname(rs1));
                    snprintf(msg, sizeof(msg), "%s=%s=0x%08x,%s&=(~%s)=0x%08x&(~0x%08x)=0x%08x",
                             rname(rd), csrnm, old, csrnm, rname(rs1), old, rs1_val, new_csr);
                    out2(output, pc_curr, "csrrc", ops, msg);
                }
                else if (funct3 == 0b101) { // CSRRWI
                    uint32_t zimm = rs1; 
                    if (rd != 0) x[rd] = old;
                    csr_write(csr_addr, zimm);
                    char ops[32], msg[96];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%02x", rname(rd), csrnm, zimm);
                    snprintf(msg, sizeof(msg), "%s=%s=0x%08x,%s=0x%08x",
                             rname(rd), csrnm, old, csrnm, zimm);
                    out2(output, pc_curr, "csrrwi", ops, msg);
                }
                else if (funct3 == 0b110) { // CSRRSI
                    uint32_t zimm = rs1;
                    uint32_t new_csr = old;
                    if (zimm != 0) {
                        new_csr = old | zimm;
                        csr_write(csr_addr, new_csr);
                    }
                    if (rd != 0) x[rd] = old;
                    char ops[32], msg[160];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%02x", rname(rd), csrnm, zimm);
                    snprintf(msg, sizeof(msg), "%s=%s=0x%08x,%s|=0x%08x=0x%08x",
                             rname(rd), csrnm, old, csrnm, zimm, new_csr);
                    out2(output, pc_curr, "csrrsi", ops, msg);
                }
                else if (funct3 == 0b111) { // CSRRCI
                    uint32_t zimm = rs1;
                    uint32_t new_csr = old;
                    if (zimm != 0) {
                        new_csr = old & ~zimm;
                        csr_write(csr_addr, new_csr);
                    }
                    if (rd != 0) x[rd] = old;
                    char ops[32], msg[160];
                    snprintf(ops, sizeof(ops), "%s,%s,0x%02x", rname(rd), csrnm, zimm);
                    snprintf(msg, sizeof(msg), "%s=%s=0x%08x,%s&=(~0x%08x)=0x%08x",
                             rname(rd), csrnm, old, csrnm, zimm, new_csr);
                    out2(output, pc_curr, "csrrci", ops, msg);
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
            // -------- FENCE / FENCE.I --------
            case 0b0001111: {
                if (funct3 == 0b001) { // FENCE.I
                    out2(output, pc_curr, "fence.i", "", "");
                    for (uint32_t i = 0; i < CACHE_SETS; i++) {
                        icache.set[i].way[0].valid = 0;
                        icache.set[i].way[1].valid = 0;
                        icache.set[i].lru = 0; 
                    }
                } else {
                    out2(output, pc_curr, "fence", "", "");
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

        mtime += 500; 

        // Timer Check 
        if (mtime >= mtimecmp) csr_mip |=  (1u << 7);
        else                   csr_mip &= ~(1u << 7);

        // UART Check 
        if (uart_ready && !uart_fifo_empty()) {
             plic_irq10_pending = 1;
        } else {
             plic_irq10_pending = 0; 
        }
        
        int plic_uart_enabled = (plic_enable & (1u << 10));
        int plic_priority_ok  = (plic_priority_10 > plic_threshold);

        if (plic_irq10_pending && plic_uart_enabled && plic_priority_ok) {
            csr_mip |=  (1u << 11); // Liga MEIP
        } else {
            csr_mip &= ~(1u << 11); // Desliga MEIP
        }


        uint32_t global_mie = (csr_mstatus & (1u << 3));

        if (global_mie) {
            uint32_t pending_irqs = csr_mip & csr_mie;

            if (pending_irqs) {
                
                uint32_t cause = 0;
                int take = 0;

                if (pending_irqs & (1u << 11)) { // MEIP
                    cause = 0x8000000Bu;
                    take = 1;
                }
                else if (pending_irqs & (1u << 3)) { // MSIP
                    cause = 0x80000003u;
                    take = 1;
                    soft_irq_pending = 0;
                    //csr_mip &= ~(1u << 3);
                }
                else if (pending_irqs & (1u << 7)) { // MTIP
                    cause = 0x80000007u;
                    take = 1;
                    timer_irq_pending = 0;
                    //csr_mip &= ~(1u << 7);
                }

                if (take) {
                    raise_interrupt(cause, pc_next, 0, &pc_next, output);
                }
            }
        }

        pc = pc_next;
    }

    double ih = (icache.accesses ? ((double)icache.hits / (double)icache.accesses) : 0.0);
    double dh = (dcache.accesses ? ((double)dcache.hits / (double)dcache.accesses) : 0.0);

    fprintf(output, "#cache_mem:dstats                     hit=%.4f\n", dh);
    fprintf(output, "#cache_mem:istats                     hit=%.4f\n", ih);

    fflush(output);

    fclose(input);
    fclose(output);
    free(mem);
    return 0;
}