NAME      = npc
NPC_HOME  = $(abspath $(CURDIR))
OBJ_DIR   = $(NPC_HOME)/build
BIN       = $(OBJ_DIR)/$(NAME)

# top module of your design
# 接入ysyxSoC后, 仿真顶层为SimTop (NPC作为其中的处理器核)
VTOP      ?= SimTop

# ysyxSoC相关路径 (ysyxSoC位于workbench根目录, 与npc/平级)
YSYX_SOC  = $(NPC_HOME)/../ysyxSoC
SOC_ELAB  = $(YSYX_SOC)/ready-to-run/minirv/ElaborateTop.v
PERIP_VSRC = $(shell find $(YSYX_SOC)/perip -name "*.v" -o -name "*.sv")

VERILATOR = verilator
VM_DIR    = $(OBJ_DIR)/obj_dir
VM_BIN    = $(VM_DIR)/V$(VTOP)
VFLAGS    = --cc --exe \
            --top-module $(VTOP) \
            --Mdir $(VM_DIR) \
            -Wall -Wno-fatal

# ysyxSoC外设RTL所需的编译选项与include搜索路径
# (uart16550/spi的RTL中使用了include的宏定义文件)
VFLAGS   += --timescale "1ns/1ns" --no-timing
VFLAGS   += -D__VERILOG__ -DPDK_BEHAV -D__UART_TO_CONSOLE__
VFLAGS   += -I$(YSYX_SOC)/perip/uart16550/rtl -I$(YSYX_SOC)/perip/spi/rtl

# WAVE=1 时记录波形到 build/npc.fst, 可用 gtkwave 查看:
#   make sim WAVE=1 && gtkwave build/npc.fst
ifdef WAVE
VFLAGS   += --trace-fst +define+WAVE
endif

# source files (absolute paths, as Verilator generates a makefile)
VSRC = $(shell find $(NPC_HOME)/vsrc -name "*.v")
CSRC = $(shell find $(NPC_HOME)/csrc -name "*.c" -o -name "*.cpp" -o -name "*.cc")

# ==================== NVBoard ====================
# 默认目标仍是all (下面的auto_bind规则不能抢在all之前成为默认目标)
.DEFAULT_GOAL := all

# 约束文件描述SimTop引脚与NVBoard部件(串口终端等)的绑定关系,
# auto_pin_bind.py据此生成auto_bind.cpp (调用nvboard_bind_pin)
NXDC_FILES = $(NPC_HOME)/constr/npc.nxdc
SRC_AUTO_BIND = $(OBJ_DIR)/auto_bind.cpp

$(SRC_AUTO_BIND): $(NXDC_FILES)
	@mkdir -p $(OBJ_DIR)
	python3 $(NVBOARD_HOME)/scripts/auto_pin_bind.py $^ $@

# nvboard.mk: 编译NVBoard归档, 并向LDFLAGS追加SDL库
include $(NVBOARD_HOME)/scripts/nvboard.mk

CSRC += $(SRC_AUTO_BIND)

# 仿真程序需要链接NVBoard和SDL, 编译时需要nvboard/SDL头文件
# 注意链接顺序: nvboard.a必须放在SDL库之前, 否则归档中的SDL符号引用无法解析
VFLAGS += -CFLAGS "-I$(NVBOARD_HOME)/usr/include $(shell sdl2-config --cflags)" \
          -LDFLAGS "$(NVBOARD_ARCHIVE) $(LDFLAGS)"

.PHONY: all sim run clean

all: $(BIN)

$(BIN): $(VSRC) $(PERIP_VSRC) $(SOC_ELAB) $(CSRC) $(NVBOARD_ARCHIVE)
	@mkdir -p $(VM_DIR)
	$(VERILATOR) $(VFLAGS) $(VSRC) $(PERIP_VSRC) $(SOC_ELAB) $(CSRC)
	@# NVBoard归档只出现在-LDFLAGS中, Verilator生成的链接规则不追踪它的变化,
	@# 归档更新后必须删除链接产物强制重新链接, 否则cp会复制旧的仿真程序
	@if [ $(NVBOARD_ARCHIVE) -nt $(VM_DIR)/V$(VTOP) ]; then rm -f $(VM_DIR)/V$(VTOP); fi
	$(MAKE) -C $(VM_DIR) -f V$(VTOP).mk
	@cp $(VM_BIN) $(BIN)

# 运行仿真时加载的镜像文件, 例如:
#   make sim IMG=$(YSYX_SOC)/ready-to-run/minirv/hello-minirv-ysyxsoc.bin
# 镜像会被读入仿真侧的Flash, NPC从Flash中启动
IMG ?=

sim: $(BIN)
	$(call git_commit, "sim RTL") # DO NOT REMOVE THIS LINE!!!
	@$(BIN) $(IMG)

# AM的批量测试通过 run 目标启动仿真 (make ARCH=riscv32-npc ALL=dummy run)
run: sim

clean:
	rm -rf $(OBJ_DIR)

include ../Makefile
