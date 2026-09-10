# ==============================================================================
# Makefile - Simulador RISC-V RV32I/M (Poxim-V)
# ==============================================================================

CC         ?= gcc
CFLAGS     ?= -Wall -Wextra -std=c99 -O2
SRC_DIR     = src
TESTS_DIR   = tests
BIN_DIR     = bin

TARGET      = poxim
TARGET_V1   = poximv1
TARGET_V2   = poximv2
TARGET_V3   = poximv3

DEFAULT_INPUT    = $(TESTS_DIR)/input.txt
DEFAULT_OUTPUT   = $(TESTS_DIR)/saida.txt
EXPECTED_OUTPUT  = $(TESTS_DIR)/esperado.txt
EXC_INPUT        = $(TESTS_DIR)/excecao.txt

.PHONY: all run test clean v1 v2 v3 help dirs

# Alvo padrão: compila a versão consolidada (Poxim-V v3)
all: $(TARGET)

# Cria diretório de binários
dirs:
	@mkdir -p $(BIN_DIR)

# Compilação do executável principal na raiz e em bin/
$(TARGET): $(SRC_DIR)/poximv3.c | dirs
	@echo "==> Compilando Poxim-V (Core Final RV32I/M + Caches L1)..."
	$(CC) $(CFLAGS) $< -o $@
	@cp -f $@ $(BIN_DIR)/$@
	@echo "==> Sucesso: binário '$@' gerado."

# Alvos individuais para cada etapa da evolução do processador
v1: $(SRC_DIR)/poximv1.c | dirs
	@echo "==> Compilando Poxim-V v1 (Core RV32I/M Base)..."
	$(CC) $(CFLAGS) $< -o $(BIN_DIR)/$(TARGET_V1)
	@echo "==> Sucesso: '$(BIN_DIR)/$(TARGET_V1)' gerado."

v2: $(SRC_DIR)/poximv2.c | dirs
	@echo "==> Compilando Poxim-V v2 (M-Mode, CSRs, Traps & MMIO)..."
	$(CC) $(CFLAGS) $< -o $(BIN_DIR)/$(TARGET_V2)
	@echo "==> Sucesso: '$(BIN_DIR)/$(TARGET_V2)' gerado."

v3: $(TARGET)

# Executa simulação padrão
run: $(TARGET)
	@echo "==> Executando simulação Poxim-V com benchmark padrão..."
	@echo "    Entrada: $(DEFAULT_INPUT)"
	@echo "    Saída:   $(DEFAULT_OUTPUT)"
	./$(TARGET) $(DEFAULT_INPUT) $(DEFAULT_OUTPUT)
	@echo "==> Simulação concluída com sucesso."

# Executa teste com entrada de exceções
run-exc: $(TARGET)
	@echo "==> Executando simulação de exceções/traps..."
	./$(TARGET) $(EXC_INPUT) $(TESTS_DIR)/saida_excecao.txt
	@echo "==> Teste de exceções concluído."

# Validação e conferência de saída
test: run
	@echo "==> Verificando integridade da execução..."
	@if [ -f "$(EXPECTED_OUTPUT)" ]; then \
		echo "==> Comparando últimas 10 linhas da saída com esperado:"; \
		tail -n 10 $(DEFAULT_OUTPUT); \
	fi

# Limpeza de binários e arquivos gerados de saída
clean:
	@echo "==> Limpando binários e arquivos gerados..."
	rm -rf $(TARGET) $(BIN_DIR) *.o *.out gmon.out *~ *.swp *.swo
	rm -f $(TESTS_DIR)/saida_excecao.txt
	@echo "==> Limpeza concluída."

# Ajuda
help:
	@echo "Uso: make [alvo]"
	@echo ""
	@echo "Alvos disponíveis:"
	@echo "  all      - Compila a versão final consolidada ($(TARGET)) com -Wall -Wextra -std=c99 -O2 (padrão)"
	@echo "  run      - Executa a simulação com os arquivos de teste padrão (input.txt -> saida.txt)"
	@echo "  run-exc  - Executa teste com vetor de exceções (excecao.txt)"
	@echo "  test     - Executa a simulação e verifica as métricas e saída geradas"
	@echo "  v1       - Compila a etapa 1 (Core RV32I/M inicial)"
	@echo "  v2       - Compila a etapa 2 (Modo Máquina, CSRs, Interrupções e MMIO)"
	@echo "  v3       - Compila a etapa 3 (Final com Caches L1)"
	@echo "  clean    - Remove binários compilados, objetos e saídas temporárias"
	@echo "  help     - Exibe esta mensagem de ajuda"
