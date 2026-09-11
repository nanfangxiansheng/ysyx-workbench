// main.cpp - ysyxSoC 仿真环境
// 顶层为SimTop (ysyxSoC/ready-to-run/minirv/ElaborateTop.v),
// NPC作为SoC中的处理器核, 取指从Flash(0x3000_0000)开始,
// 存储器和外设访问都经过SoC的总线.
//
// clock用于控制SoC的时钟, cpuClock用于控制NPC的时钟,
// 当前用相同的输入驱动二者.
//
// 用法: ./npc [镜像文件.bin]
//   镜像会被读入仿真侧的Flash数组, NPC通过SoC总线从Flash取指
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <verilated.h>
#include "VSimTop.h"
#include "svdpi.h"
#include "difftest.h"
#include <nvboard.h>

// auto_bind.cpp (由constr/npc.nxdc经auto_pin_bind.py生成):
// 把SimTop的引脚绑定到NVBoard的串口终端等部件
extern void nvboard_bind_all_pins(VSimTop *top);

// difftest.cpp引用的物理内存 (暂未使用, 保留符号以便链接)
uint8_t pmem[PMEM_SIZE] = {};

static VSimTop *top = nullptr;
static bool halt_flag = false; // NPC 执行到 ebreak 后置位
static uint32_t halt_pc = 0;   // 执行 ebreak 时的 PC

// NPC 执行到 ebreak: 通知仿真环境结束仿真
extern "C" void ebreak(int pc) {
  halt_flag = true;
  halt_pc = (uint32_t)pc;
}

// NPC.v导出: 读取当前PC (心跳调试用, 需先svSetScope)
extern "C" int get_pc();

// ==================== Flash行为模型 ====================
// SoC的flash是SPI接口的NOR flash: flash.v按SPI时序移位读出,
// 最终通过DPI调用flash_read(addr, data)取回一个字.
// addr是24位的flash内偏移 (16MB空间), data按小端序返回4字节
#define FLASH_SIZE (16 * 1024 * 1024)
static uint8_t flash_mem[FLASH_SIZE] = {};

extern "C" void flash_read(int32_t addr, int32_t *data) {
  uint32_t offset = (uint32_t)addr;
  assert(offset + 3 < FLASH_SIZE); // 按4字节读, 不能越过Flash末尾
  *data = (int32_t)((uint32_t)flash_mem[offset] |
                    (uint32_t)flash_mem[offset + 1] << 8 |
                    (uint32_t)flash_mem[offset + 2] << 16 |
                    (uint32_t)flash_mem[offset + 3] << 24);
}

// 从镜像文件读入Flash, 返回读入的字节数
static long load_flash_from_file(const char *img_file) {
  FILE *fp = fopen(img_file, "rb");
  if (fp == nullptr) {
    printf("NPC: 无法打开镜像文件 '%s'\n", img_file);
    exit(1);
  }

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  assert(size > 0 && (uint64_t)size <= FLASH_SIZE);
  size_t n = fread(flash_mem, 1, size, fp);
  assert(n == (size_t)size);
  fclose(fp);

  printf("NPC: 已从 '%s' 读入 %ld 字节镜像到Flash\n", img_file, size);
  return size;
}

// ==================== 仿真框架 ====================

static void single_cycle() {
  // clock驱动SoC, cpuClock驱动NPC, 当前同源同相
  top->contextp()->timeInc(1);
  top->clock = 0;
  top->cpuClock = 0;
  top->eval();
  top->contextp()->timeInc(1);
  top->clock = 1;
  top->cpuClock = 1;
  top->eval();
}

static void reset(int n) {
  top->reset = 1;
  while (n-- > 0) {
    single_cycle();
  }
  top->reset = 0;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  Verilated::traceEverOn(true); // 允许记录波形 (WAVE=1编译时生效)
  setvbuf(stdout, NULL, _IONBF, 0); // 无缓冲输出, 便于观察长时间仿真

  top = new VSimTop;

  // 必须先绑定引脚再初始化NVBoard:
  // nvboard_init()构造UART组件时会缓存pin_array中的引脚指针,
  // 若之后才nvboard_bind_pin, 组件读到的还是旧指针, 串口终端收不到任何数据
  nvboard_bind_all_pins(top);

  // 初始化NVBoard虚拟开发板(串口终端等部件在此创建)
  nvboard_init();
  // UART的RX线空闲电平为高; 之后由NVBoard驱动(在串口终端中敲键即有输入),
  // 复位期间RX为0会被UART当成起始位
  top->externalPins_uart0_rx = 1;

  // 当前不关心其他端口, 给固定值
  top->coreSel = 0;
  top->externalPins_mygpio_in = 0;

  // 把镜像读入Flash, NPC复位后将从Flash中取出第一条指令
  if (argc < 2) {
    printf("NPC: 用法: %s <镜像文件.bin>\n", argv[0]);
    return 1;
  }
  load_flash_from_file(argv[1]);

  reset(100); // 复位至少100个周期

  // 不停地进行仿真, 直到程序执行 ebreak 指令为止.
  // 若程序陷入死循环(如hello输出后j自旋), 由超时强制结束
  uint64_t cycles = 0;
  while (!halt_flag) {
    single_cycle();
    nvboard_update(); // 每个仿真周期刷新一次虚拟外设(内部有按帧节流)
    cycles++;
    if (cycles > 2000000000ull) { // 防止无限仿真
      printf("NPC: 超过20亿周期仍未停机, 强制结束\n");
      break;
    }
  }

  int code = 0;
  if (!halt_flag) {
    printf("NPC: 仿真超时结束 (共 %llu 个周期)\n", (unsigned long long)cycles);
  } else {
    printf("NPC: hit ebreak at PC = 0x%08x, 共 %llu 个周期\n", halt_pc,
           (unsigned long long)cycles);
  }

  top->final();
  delete top;
  nvboard_quit(); // 关闭NVBoard窗口
  return code;
}
