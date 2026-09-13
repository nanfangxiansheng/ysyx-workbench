NAME      = npc
NPC_HOME  = $(abspath $(CURDIR))
OBJ_DIR   = $(NPC_HOME)/build
# 仿真程序按模式命名: 切换SIM时共用一个build/npc会因时间戳
# 骗过make (旧模式的二进制没有被源文件依赖触发重建)
BIN       = $(OBJ_DIR)/$(NAME)-$(SIM)

# 仿真模式选择:
#   SIM=soc (默认) = NPC集成在ysyxSoC中, 顶层SimTop, 复位PC=0x3000_0000,
#                    镜像读入仿真侧Flash, 从Flash(0x3000_0000)启动
#   SIM=npc        = 单独仿真NPC, 顶层NPC, 复位PC=0x8000_0000,
#                    镜像读入仿真侧pmem, C++侧行为级总线从设备应答访存请求,
#                    并开启DiffTest与C参考模型对比
# 用法: make SIM=npc sim IMG=xxx.bin / make sim IMG=xxx.bin
SIM       ?= soc

ifeq ($(SIM),soc)
# top module of your design
# 接入ysyxSoC后, 仿真顶层为SimTop (NPC作为其中的处理器核)
VTOP      = SimTop
# ysyxSoC的RTL只在集成模式下参与编译
SOC_VSRC  = $(PERIP_VSRC) $(SOC_ELAB)
else
VTOP      = NPC
SOC_VSRC  =
endif

# ysyxSoC相关路径 (ysyxSoC位于workbench根目录, 与npc/平级)
YSYX_SOC  = $(NPC_HOME)/../ysyxSoC
SOC_ELAB  = $(YSYX_SOC)/ready-to-run/minirv/ElaborateTop.v
PERIP_VSRC = $(shell find $(YSYX_SOC)/perip -name "*.v" -o -name "*.sv")

VERILATOR = verilator
# 两种模式的Verilator产物分开放置:
# 宏定义(-DSOC)变化不会触发Verilator重新编译C++源文件,
# 若共用一个obj_dir, 切换模式后残留的main.o会导致链接错误
VM_DIR    = $(OBJ_DIR)/obj_dir_$(SIM)
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

# 模式选择宏 (放在VFLAGS基础定义之后, 否则会被覆盖):
# Verilog侧(+define+SOC): NPC.v据此把复位PC选为0x3000_0000;
# C++侧(-DSOC): main.cpp据此选择SimTop仿真框架
ifeq ($(SIM),soc)
VFLAGS   += +define+SOC -CFLAGS "-DSOC"
endif

# WAVE=1 时记录波形到 build/npc.fst, 可用 gtkwave 查看:
#   make sim WAVE=1 && gtkwave build/npc.fst
# (+define+WAVE给Verilog侧, -DWAVE给C++侧: main.cpp据此创建tracer)
ifdef WAVE
VFLAGS   += --trace-fst -CFLAGS "-DWAVE"
endif

# source files (absolute paths, as Verilator generates a makefile)
VSRC = $(shell find $(NPC_HOME)/vsrc -name "*.v")
CSRC = $(shell find $(NPC_HOME)/csrc -name "*.c" -o -name "*.cpp" -o -name "*.cc")

# ==================== NVBoard (可选) ====================
# 默认不接入NVBoard以加快仿真; 需要虚拟外设(串口终端/LED/数码管)时:
#   make NVBOARD=1 sim IMG=xxx.bin
# 默认目标仍是all (下面的auto_bind规则不能抢在all之前成为默认目标)
.DEFAULT_GOAL := all

ifdef NVBOARD
# NVBoard的引脚约束(constr/npc.nxdc)绑定的是SimTop的引脚,
# 单独仿真NPC时没有这些端口, 无法接入
ifeq ($(SIM),npc)
$(error NVBoard仅支持SIM=soc (集成ysyxSoC)模式, 请去掉NVBOARD=1或改用SIM=soc)
endif
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

# 仿真程序需要链接NVBoard和SDL, 编译时需要nvboard/SDL头文件;
# -DNVBOARD宏开关main.cpp中的nvboard调用
# 注意链接顺序: nvboard.a必须放在SDL库之前, 否则归档中的SDL符号引用无法解析
VFLAGS += -CFLAGS "-DNVBOARD -I$(NVBOARD_HOME)/usr/include $(shell sdl2-config --cflags)" \
          -LDFLAGS "$(NVBOARD_ARCHIVE) $(LDFLAGS)"
endif

.PHONY: all sim run clean

all: $(BIN)

$(BIN): $(VSRC) $(SOC_VSRC) $(CSRC) $(if $(NVBOARD),$(NVBOARD_ARCHIVE))
	@mkdir -p $(VM_DIR)
	$(VERILATOR) $(VFLAGS) $(VSRC) $(SOC_VSRC) $(CSRC)
	@# NVBoard归档只出现在-LDFLAGS中, Verilator生成的链接规则不追踪它的变化,
	@# 归档更新后必须删除链接产物强制重新链接, 否则cp会复制旧的仿真程序
	@if [ "$(NVBOARD)" = "1" ] && [ $(NVBOARD_ARCHIVE) -nt $(VM_DIR)/V$(VTOP) ]; then rm -f $(VM_DIR)/V$(VTOP); fi
	$(MAKE) -C $(VM_DIR) -f V$(VTOP).mk
	@cp $(VM_BIN) $(BIN)

# 运行仿真时加载的镜像文件, 例如:
#   make sim IMG=$(YSYX_SOC)/ready-to-run/minirv/hello-minirv-ysyxsoc.bin
# SoC模式: 镜像被读入仿真侧的Flash, NPC从Flash中启动;
# 单独模式: 镜像被读入pmem(0x8000_0000), 不指定IMG时加载内置测试程序
IMG ?=

sim: $(BIN)
	$(call git_commit, "sim RTL") # DO NOT REMOVE THIS LINE!!!
	@$(BIN) $(IMG)

# AM的批量测试通过 run 目标启动仿真 (make ARCH=riscv32-npc ALL=dummy run)
run: sim

clean:
	rm -rf $(OBJ_DIR)

include ../Makefile
