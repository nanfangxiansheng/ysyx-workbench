// main.cpp - NPC 的仿真环境
// 指令和数据都放在 C++ 侧的物理内存 pmem 中,
// RTL 通过 DPI-C (pmem_read/pmem_write) 访问,
// NPC 执行到 ebreak 时通过 DPI-C 通知本环境结束仿真
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <verilated.h>
#include "VNPC.h"

#define PMEM_SIZE (8 * 1024) // 8KB 物理内存
static uint8_t pmem[PMEM_SIZE] = {};

static VNPC *top = nullptr;
static bool halt_flag = false; // NPC 执行到 ebreak 后置位

// 客户机地址 -> 宿主机内存 (地址环回映射, 类似 NEMU)
static uint8_t *guest_to_host(uint32_t addr) {
  return &pmem[addr & (PMEM_SIZE - 1)];
}

static uint32_t host_read(uint32_t addr) {
  return *(uint32_t *)guest_to_host(addr);
}

// ==================== DPI-C 接口 ====================

// 总是读取地址为`raddr & ~0x3u`的4字节返回
extern "C" int pmem_read(int raddr) {
  uint32_t addr = raddr & ~0x3u;
  return (int)host_read(addr);
}

// 总是往地址为`waddr & ~0x3u`的4字节按写掩码`wmask`写入`wdata`
// `wmask`中每比特表示`wdata`中1个字节的掩码,
// 如`wmask = 0x3`代表只写入最低2个字节, 内存中的其它字节保持不变
extern "C" void pmem_write(int waddr, int wdata, char wmask) {
  uint32_t addr = waddr & ~0x3u;
  uint8_t *h = guest_to_host(addr);
  for (int i = 0; i < 4; i++) {
    if (wmask & (1 << i)) {
      h[i] = (wdata >> (8 * i)) & 0xff;
    }
  }
}

// NPC 执行到 ebreak: 通知仿真环境结束仿真
extern "C" void ebreak() {
  halt_flag = true;
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

// NPC 的测试程序, 手动放入存储器 M 中
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

static void load_img() {
  assert(sizeof(img) <= PMEM_SIZE);
  memcpy(pmem, img, sizeof(img));
  printf("NPC: 已加载镜像到物理内存 (%zu 字节, %zu 条指令)\n", sizeof(img),
         sizeof(img) / sizeof(img[0]));
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  load_img();

  top = new VNPC;
  reset(10);

  // 不停地进行仿真, 直到程序执行 ebreak 指令为止
  uint64_t cycles = 0;
  while (!halt_flag) {
    single_cycle();
    cycles++;
  }

  printf("NPC: hit ebreak at PC = 0x%02x, 共执行 %llu 个周期, 仿真结束\n",
         (unsigned)(0x4 * (sizeof(img) / sizeof(img[0]) - 1)),
         (unsigned long long)cycles);

  top->final();
  delete top;
  return 0;
}
