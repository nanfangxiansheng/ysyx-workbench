#include <am.h>
#include <klib-macros.h>

extern char _heap_start;
int main(const char *args);

extern char _pmem_start;
#define PMEM_SIZE (128 * 1024 * 1024)
#define PMEM_END  ((uintptr_t)&_pmem_start + PMEM_SIZE)

Area heap = RANGE(&_heap_start, PMEM_END);
static const char mainargs[MAINARGS_MAX_LEN] = TOSTRING(MAINARGS_PLACEHOLDER); // defined in CFLAGS

// UART串口的寄存器地址 (内存映射I/O)
#define UART_BASE 0x10000000ul
#define UART_STAT (UART_BASE + 4)

void putch(char c) {
  volatile char *stat = (volatile char *)UART_STAT;
  volatile char *data = (volatile char *)UART_BASE;
  // 查询状态寄存器, 直到UART就绪才输出字符, 避免字符丢失
  while (*stat == 0)
    ;
  *data = c;
}

void halt(int code) {
  asm volatile("mv a0, %0; ebreak" : :"r"(code)); // 结束状态放入a0, 供仿真环境检查
  while (1);
}

void _trm_init() {
  int ret = main(mainargs);
  halt(ret);
}
