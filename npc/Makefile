NAME      = npc
NPC_HOME  = $(abspath $(CURDIR))
OBJ_DIR   = $(NPC_HOME)/build
BIN       = $(OBJ_DIR)/$(NAME)

# top module of your design
VTOP      ?= example

# Verilator
VERILATOR = verilator
VM_DIR    = $(OBJ_DIR)/obj_dir
VM_BIN    = $(VM_DIR)/V$(VTOP)
VFLAGS    = --cc --exe \
            --top-module $(VTOP) \
            --Mdir $(VM_DIR) \
            -Wall -Wno-fatal

# source files (absolute paths, as Verilator generates a makefile)
VSRC = $(shell find $(NPC_HOME)/vsrc -name "*.v")
CSRC = $(shell find $(NPC_HOME)/csrc -name "*.c" -o -name "*.cpp" -o -name "*.cc")

.PHONY: all sim clean

all: $(BIN)

$(BIN): $(VSRC) $(CSRC)
	@mkdir -p $(VM_DIR)
	$(VERILATOR) $(VFLAGS) $(VSRC) $(CSRC)
	$(MAKE) -C $(VM_DIR) -f V$(VTOP).mk
	@cp $(VM_BIN) $(BIN)

sim: $(BIN)
	$(call git_commit, "sim RTL") # DO NOT REMOVE THIS LINE!!!
	@$(BIN)

clean:
	rm -rf $(OBJ_DIR)

include ../Makefile
