#include <am.h>
#include <klib-macros.h>

extern char _heap_start;
int main(const char *args);

extern char _pmem_start;
#define PMEM_SIZE (128 * 1024 * 1024)
#define PMEM_END  ((uintptr_t)&_pmem_start + PMEM_SIZE)

Area heap = RANGE(&_heap_start, PMEM_END);
static const char mainargs[MAINARGS_MAX_LEN] = TOSTRING(MAINARGS_PLACEHOLDER); // defined in CFLAGS

// UART16550串口的寄存器地址 (内存映射I/O, 字节编址)
#define UART_BASE 0x10000000ul
#define UART_DLL  (UART_BASE + 0) // 除数低8位 (DLAB=1时)
#define UART_DLM  (UART_BASE + 1) // 除数高8位 (DLAB=1时)
#define UART_LCR  (UART_BASE + 3) // 线路控制寄存器
#define UART_LSR  (UART_BASE + 5) // 线路状态寄存器

#define UART_LCR_8BITS 0x03 // 8位数据, 1位停止位, 无校验
#define UART_LCR_DLAB  0x80 // DLAB=1时, offset 0/1 映射为除数寄存器
#define UART_LSR_THRE  0x20 // bit5: 发送保持寄存器(队列)空

// UART控制器时钟25MHz, 目标波特率115200
// 波特率 = 时钟频率 / (16 * 除数)  =>  除数 = 25MHz / (16 * 115200) ≈ 13.56, 取13
#define UART_FREQ 25000000ul
#define UART_BAUD 115200ul
#define UART_DIV  (UART_FREQ / (16 * UART_BAUD))

static void uart_init() {
  volatile char *lcr = (volatile char *)UART_LCR;
  *lcr = UART_LCR_DLAB | UART_LCR_8BITS; // 置DLAB=1, 顺带配置8位数据
  *(volatile char *)UART_DLL = (char)(UART_DIV & 0xff);
  *(volatile char *)UART_DLM = (char)((UART_DIV >> 8) & 0xff);
  *lcr = UART_LCR_8BITS; // 清DLAB, offset 0/1 恢复为数据寄存器
}

void putch(char c) {
  volatile char *lsr = (volatile char *)UART_LSR;
  // 查询发送队列状态, 队列为空(THRE=1)时才能写入, 避免字符丢失
  while ((*lsr & UART_LSR_THRE) == 0)
    ;
  *(volatile char *)UART_BASE = c;
}

void halt(int code) {
  asm volatile("mv a0, %0; ebreak" : :"r"(code)); // 结束状态放入a0, 供仿真环境检查
  while (1);
}

void _trm_init() {
  uart_init();
  int ret = main(mainargs);
  halt(ret);
}
