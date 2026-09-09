// main.cpp - NPC 的仿真环境
// 指令和数据都放在 C++ 侧的物理内存 pmem 中,
// RTL 通过 DPI-C (pmem_read/pmem_write) 访问,
// NPC 执行到 ebreak 时通过 DPI-C 通知本环境结束仿真
//
// 用法: ./npc [镜像文件.bin]
//   不带参数时加载内置的测试程序;
//   带参数时把 bin 文件读入物理内存 (bin 按 AM 约定链接在 0x80000000 处)
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <verilated.h>
#include "svdpi.h"
#include "VNPC.h"
#include "difftest.h"

// 物理内存 (difftest.cpp 中的参考模型REF也访问它, 故定义为全局的)
uint8_t pmem[PMEM_SIZE] = {};

static VNPC *top = nullptr;
static bool halt_flag = false; // NPC 执行到 ebreak 后置位
static uint32_t halt_pc = 0;   // 执行 ebreak 时的 PC

// 客户机地址 -> 宿主机内存
static uint8_t *guest_to_host(uint32_t addr) {
  return &pmem[addr - MBASE];
}

// 地址范围检查: 4字节访问不能越过内存末尾 (支持非对齐访问)
static bool in_pmem(uint32_t addr) {
  return addr >= MBASE && addr - MBASE <= PMEM_SIZE - 4;
}

// 从addr开始连续4个字节组装成32位小端数据
// 约定存储器是字节编址的, 支持非对齐访问, 不再按字对齐
static uint32_t host_read(uint32_t addr) {
  uint8_t *p = guest_to_host(addr);
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// 越界访问只提示一次, 避免刷屏 (复位前 pc 尚未初始化, 可能产生越界的取指)
static void out_of_bound(uint32_t addr) {
  static bool warned = false;
  if (!warned) {
    warned = true;
    printf("NPC: 访问了物理内存之外的地址 0x%08x (第一次, 之后不再提示)\n", addr);
  }
}

// ==================== RTC时钟行为模型 ====================
// 返回从仿真开始所经过的时间, 单位为微秒.
// 用宿主机的墙上时间模拟: 仿真里的1微秒 = 真实的1微秒,
// 这样客户程序看到的时钟流速和真实世界一致

#include <sys/time.h>

static unsigned long long boot_us = 0; // 仿真开始时刻的墙上时间 (微秒)

static unsigned long long now_us() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (unsigned long long)tv.tv_sec * 1000000ull + tv.tv_usec;
}
uint64_t cycle_count=0;
unsigned long long get_time() {
  // RTC需返回微秒: 微秒 = 周期数 / 每微秒周期数(=名义主频的MHz数)
  // 名义主频100MHz => 每100个周期是1微秒; 若按1GHz算则改成 / 1000ull
  return cycle_count / 40ull; // 100MHz
}
// ==================== DPI-C 接口 ====================

// 读取从地址`raddr`开始的4个字节 (小端序, 支持非对齐)
extern "C" int pmem_read(int raddr) {
  uint32_t addr = (uint32_t)raddr;

  // UART行为模型: 读状态寄存器 = 查询串口是否就绪
  // 设备速度由随机数模拟: 12.5%的概率读出1(就绪), 其余情况读出0(未就绪)
  // 程序必须查询到就绪后才能输出字符, 否则字符会丢失
  if (addr == UART_STAT) {
    return (rand() & 0x7) == 0 ? 1 : 0;
  }
  else if (addr == RTC_ADDR)    { return get_time() & 0xffffffff; }
  // 读出时钟的高32位
  else if (addr == RTC_ADDR_HI) { return get_time() >> 32; }

  if (!in_pmem(addr)) {
    out_of_bound(addr);
    return 0;
  }
  return (int)host_read(addr);
}

// 从地址`waddr`开始的4个字节, 按写掩码`wmask`写入`wdata`的对应字节
// `wmask`中每比特表示`wdata`中1个字节的掩码,
// 如`wmask = 0x3`代表只写入最低2个字节, 内存中的其它字节保持不变
// 字节地址连续, 因此天然支持非对齐的sw/sh
extern "C" void pmem_write(int waddr, int wdata, char wmask) {
  // UART行为模型: 往串口数据寄存器写入 = 输出一个字符
  if ((uint32_t)waddr == UART_BASE) {
    fputc(wdata & 0xff, stderr);
    return;
  }

  uint32_t addr = (uint32_t)waddr;
  if (!in_pmem(addr)) {
    out_of_bound(addr);
    return;
  }
  uint8_t *h = guest_to_host(addr);
  for (int i = 0; i < 4; i++) {
    if (wmask & (1 << i)) {
      h[i] = (wdata >> (8 * i)) & 0xff;
    }
  }
}

// NPC 执行到 ebreak: 通知仿真环境结束仿真
extern "C" void ebreak(int pc) {
  halt_flag = true;
  halt_pc = (uint32_t)pc;
}

// NPC本周期是否提交(执行)了一条指令 (difftest.cpp中封装, 内部处理DPI作用域)
// 支持SimpleBus后NPC每2个周期才执行1条指令(1拍等待取指),
// DiffTest只应在提交指令的那一拍与REF对比, 否则REF会跑快一倍

// ==================== 仿真框架 ====================

static void single_cycle() {
  // 每半个时钟周期推进一次仿真时间, 让波形文件有时间轴
  // (否则所有信号都堆在t=0, gtkwave里没法看)
  top->contextp()->timeInc(1);
  top->clk = 0;
  top->eval();
  top->contextp()->timeInc(1);
  top->clk = 1;
  top->eval();
}

static void reset(int n) {
  top->rst = 1;
  while (n-- > 0) {
    single_cycle();
  }
  top->rst = 0;
}

// NPC 的内置测试程序, 在未指定镜像文件时使用
// (ebreak 的编码 0x00100073 通过查阅 RISC-V 手册得到)
static const uint32_t img[] = {
    0x12345537, // 00: lui   a0, 0x12345
    0x67850513, // 04: addi  a0, a0, 0x678   -> a0 = 0x12345678
    0x000005b7, // 08: lui   a1, 0x0
    0x00a585b3, // 0c: add   a1, a1, a0      -> a1 = 0x12345678
    0x00b52023, // 10: sw    a1, 0(a0)       -> M[0x12345678] = 0x12345678
    0x00052603, // 14: lw    a2, 0(a0)       -> a2 = 0x12345678
    0x00054683, // 18: lbu   a3, 0(a0)       -> a3 = 0x78
    0x00d500a3, // 1c: sb    a3, 1(a0)       -> M[0x12345679] = 0x78
    0x00052703, // 20: lw    a4, 0(a0)       -> a4 = 0x12347878
    0x00100073, // 24: ebreak                -> 结束仿真
};

// 从镜像文件读入物理内存, 返回读入的字节数
static long load_img_from_file(const char *img_file) {
  FILE *fp = fopen(img_file, "rb");
  if (fp == nullptr) {
    printf("NPC: 无法打开镜像文件 '%s'\n", img_file);
    exit(1);
  }

  fseek(fp, 0, SEEK_END);
  long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);

  assert(size > 0 && (uint64_t)size <= PMEM_SIZE);
  size_t n = fread(pmem, 1, size, fp);
  assert(n == (size_t)size);
  fclose(fp);

  printf("NPC: 已从 '%s' 读入 %ld 字节镜像到物理内存 0x%08x\n", img_file, size,
         MBASE);
  return size;
}

static void load_img(int argc, char **argv) {
  if (argc >= 2) {
    load_img_from_file(argv[1]); // 命令行指定了镜像文件
    return;
  }

  // 未指定镜像文件, 使用内置程序
  assert(sizeof(img) <= PMEM_SIZE);
  memcpy(pmem, img, sizeof(img));
  printf("NPC: 未指定镜像文件, 加载内置程序 (%zu 字节, %zu 条指令)\n",
         sizeof(img), sizeof(img) / sizeof(img[0]));
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  Verilated::traceEverOn(true); // 允许记录波形 (WAVE=1编译时NPC.v中的$dumpvars才会生效)

  boot_us = now_us(); // 时钟行为模型的计时起点

  load_img(argc, argv);

  top = new VNPC;

  // 初始化DiffTest: 缓存DPI作用域, 把参考模型REF的初始状态
  // 设置成和NPC复位后一致 (pc = MBASE, 寄存器全部为0)
  difftest_init();

  reset(10);

  // 不停地进行仿真, 直到程序执行 ebreak 指令为止;
  // NPC每提交一条指令, 就与参考模型REF对比一次状态 (DiffTest)
  uint64_t cycles = 0;
  uint64_t insts = 0;
  int diff_err = 0;
  while (!halt_flag) {
    single_cycle();
    cycles++;
    cycle_count++;
    if (npc_committed()) { // 只有真正执行了指令的这一拍才对比
      insts++;
      if (difftest_step() != 0) { // NPC与REF状态不一致, 停止仿真
        diff_err = 1;
        break;
      }
    }
  }

  int code = 0;
  if (diff_err) {
    // DiffTest发现不一致: 以非0退出码结束, 让Makefile捕捉到错误
    printf("NPC: HIT BAD TRAP (DiffTest发现执行结果不一致, 共执行 %llu 条指令)\n",
           (unsigned long long)insts);
    code = 1;
  } else {
    printf("NPC: hit ebreak at PC = 0x%08x, 共执行 %llu 条指令 / %llu 个周期\n",
           halt_pc, (unsigned long long)insts, (unsigned long long)cycles);

    // 约定: 程序在执行ebreak前把结束状态写入a0(x10)
    // 0表示程序正确结束, 非0表示程序发生错误
    code = difftest_read_gpr(10);
    if (code == 0) {
      printf("NPC: HIT GOOD TRAP (a0 = 0, 程序正确结束)\n");
    } else {
      printf("NPC: HIT BAD TRAP at PC = 0x%08x (a0 = %d, 程序发生错误)\n",
             halt_pc, code);
    }
  }

  top->final();
  delete top;
  return code; // 把结束状态作为仿真进程的退出码, 便于脚本判断
}
