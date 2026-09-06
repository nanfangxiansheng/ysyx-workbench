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
#include "VNPC.h"

// AM 程序链接在 0x80000000 处, 物理内存从这一地址开始编址 (类似 NEMU 的 MBASE)
#define MBASE 0x80000000u
#define PMEM_SIZE (128 * 1024 * 1024) // 128MB 物理内存, 与 NEMU 的 MSIZE 保持一致
static uint8_t pmem[PMEM_SIZE] = {};

static VNPC *top = nullptr;
static bool halt_flag = false; // NPC 执行到 ebreak 后置位
static uint32_t halt_pc = 0;   // 执行 ebreak 时的 PC

// 客户机地址 -> 宿主机内存
static uint8_t *guest_to_host(uint32_t addr) {
  return &pmem[addr - MBASE];
}

static bool in_pmem(uint32_t addr) {
  return addr >= MBASE && addr - MBASE < PMEM_SIZE;
}

static uint32_t host_read(uint32_t addr) {
  return *(uint32_t *)guest_to_host(addr);
}

// 越界访问只提示一次, 避免刷屏 (复位前 pc 尚未初始化, 可能产生越界的取指)
static void out_of_bound(uint32_t addr) {
  static bool warned = false;
  if (!warned) {
    warned = true;
    printf("NPC: 访问了物理内存之外的地址 0x%08x (第一次, 之后不再提示)\n", addr);
  }
}

// ==================== DPI-C 接口 ====================

// 总是读取地址为`raddr & ~0x3u`的4字节返回
extern "C" int pmem_read(int raddr) {
  uint32_t addr = raddr & ~0x3u;
  if (!in_pmem(addr)) {
    out_of_bound(addr);
    return 0;
  }
  return (int)host_read(addr);
}

// 总是往地址为`waddr & ~0x3u`的4字节按写掩码`wmask`写入`wdata`
// `wmask`中每比特表示`wdata`中1个字节的掩码,
// 如`wmask = 0x3`代表只写入最低2个字节, 内存中的其它字节保持不变
extern "C" void pmem_write(int waddr, int wdata, char wmask) {
  uint32_t addr = waddr & ~0x3u;
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

// ==================== 仿真框架 ====================

static void single_cycle() {
  top->clk = 0;
  top->eval();
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

  load_img(argc, argv);

  top = new VNPC;
  reset(10);
  int cycle_count=0;

  // 不停地进行仿真, 直到程序执行 ebreak 指令为止
  uint64_t cycles = 0;
  while (!halt_flag) {
    single_cycle();
    cycles++;
    cycle_count++;
    if (cycle_count>100){
      break;
  }
  }


  printf("NPC: hit ebreak at PC = 0x%08x, 共执行 %llu 个周期, 仿真结束\n",
         halt_pc, (unsigned long long)cycles);

  top->final();
  delete top;
  return 0;
}
