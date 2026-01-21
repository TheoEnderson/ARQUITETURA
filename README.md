# Projetos de Arquitetura de Computadores (Poxim / RISC-V)

Este repositório reúne os projetos desenvolvidos na disciplina de **Arquitetura de Computadores**, com foco na construção incremental de um **simulador RISC-V em C** (estilo “Poxim”), seguindo testes e especificações fornecidas em aula.

A ideia é evoluir por etapas: primeiro o core e o trace, depois exceções/CSR/interrupts e, por fim, uma etapa mais completa (Projeto 3).

---

## ✅ Status

- **Projeto 1:** concluído ✅  
- **Projeto 2:** concluído ✅  
- **Projeto 3:** em andamento ⏳ (ainda não iniciado / a iniciar)

---

## 🧩 Projetos

### Projeto 1 — Core RV32 + Trace
Implementação base do simulador:
- RAM e loader de programa (formato `.hex.txt`)
- Registradores `x0..x31` e PC
- Execução das instruções principais do **RV32I**
- Geração de **trace** (registro das instruções executadas + efeitos)

📁 Pasta: `projeto-01/`

---

### Projeto 2 — Exceções, CSR e Interrupções (MMIO)
Evolução do simulador para suportar recursos de “software básico”:
- **CSRs (modo máquina)**: `mstatus`, `mtvec`, `mepc`, `mcause`, `mtval`, `mie`, `mip`
- Instruções CSR: `csrrw`, `csrrs`, além de `mret`
- **Exceções**:
  - illegal instruction
  - instruction/load/store fault
  - environment call (ecall)
- **Interrupções**:
  - software (MSIP)
  - timer (MTIP, via `mtime/mtimecmp`)
  - external (MEIP, via PLIC simplificado)
- **Dispositivos MMIO**:
  - UART (TX/RX, FIFO/loopback, status)
  - CLINT (timer fake com `mtimecmp`)
  - PLIC (enable + claim/complete)

📁 Pasta: `projeto-02/`

---

### Projeto 3 — Etapa Final (Planejado)
> Ainda não implementado.

A etapa final deve consolidar e/ou expandir o simulador com requisitos finais da disciplina (dependendo do que for pedido no PDF/testes), como por exemplo:
- mais periféricos ou mapeamentos MMIO
- testes maiores e validações mais rígidas
- melhorias na fidelidade do modelo (caso solicitado)
- organização final do projeto e documentação

📁 Pasta prevista: `projeto-03/`

---

## ▶️ Como Compilar e Executar

Cada projeto pode ter seu próprio executável. Exemplo geral:

### Compilar
```bash
gcc -Wall -Wextra -O2 main.c -o poxim
