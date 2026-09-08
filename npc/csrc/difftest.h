// difftest.h - DiffTest 差分测试的公共定义
#ifndef DIFFTEST_H
#define DIFFTEST_H

#include <cstdint>

// AM 程序链接在 0x80000000 处, 物理内存从这一地址开始编址 (类似 NEMU 的 MBASE)
#define MBASE 0x80000000u
#define PMEM_SIZE (128 * 1024 * 1024) // 128MB 物理内存, 与 NEMU 的 MSIZE 保持一致

// UART串口数据寄存器的地址 (内存映射I/O)
#define UART_BASE 0x10000000u
// UART串口状态寄存器的地址: 最低位为1表示就绪, 可以输出下一个字符
#define UART_STAT (UART_BASE + 4u)

// RTC实时时钟寄存器: 两次32位读拼出64位的微秒数 (内存映射I/O)
#define RTC_ADDR    0x20000000u // 低32位
#define RTC_ADDR_HI (RTC_ADDR + 4u) // 高32位

extern uint8_t pmem[PMEM_SIZE]; // 物理内存, 定义在 main.cpp

// 初始化参考模型REF: pc = MBASE, 寄存器清零, 并缓存DPI作用域
void difftest_init();

// REF执行一条指令, 然后与NPC对比PC和通用寄存器
// 一致返回0; 不一致时打印详细差异并返回非0
int difftest_step();

// 读取NPC通用寄存器的值 (供main.cpp检查a0中的结束状态)
int difftest_read_gpr(int raddr);

#endif
