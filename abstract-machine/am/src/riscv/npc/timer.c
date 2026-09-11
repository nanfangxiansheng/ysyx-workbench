#include <am.h>

// NPC没有RTC设备, 时钟源用mcycle/mcycleh (64位周期计数器, 每周期+1).
// 时间 = 周期数 / 频率. 将来在真实芯片上运行时, 把此值改成处理器的
// 实际工作频率(Hz)并重新编译即可; 仿真环境中没有频率概念, 该值按
// 仿真的墙钟速率校准(实测约35万周期/秒), 使程序读到的时钟接近真实时间.
#define SIM_CPU_FREQ_HZ 80000  // 实测本机独占运行约8万周期/秒

static uint32_t read_mcycle() {
  uint32_t val;
  asm volatile("csrr %0, mcycle" : "=r"(val));
  return val;
}

static uint32_t read_mcycleh() {
  uint32_t val;
  asm volatile("csrr %0, mcycleh" : "=r"(val));
  return val;
}

void __am_timer_init() {
}

void __am_timer_uptime(AM_TIMER_UPTIME_T *uptime) {
  // 64位计数器跨两次CSR读取: 先读hi, 再读lo, 最后重读hi.
  // 两次hi不一致说明期间低32位恰好进位, 重试即可 (标准的高-低-高读法)
  uint32_t hi, lo;
  do {
    hi = read_mcycleh();
    lo = read_mcycle();
  } while (hi != read_mcycleh());

  // 先乘1e6再除, 避免整除截断 (周期数×1e6在64位下不会溢出)
  uptime->us = (((uint64_t)hi << 32) | lo) * 1000000ull / SIM_CPU_FREQ_HZ;
}

void __am_timer_rtc(AM_TIMER_RTC_T *rtc) {
  rtc->second = 0;
  rtc->minute = 0;
  rtc->hour   = 0;
  rtc->day    = 0;
  rtc->month  = 0;
  rtc->year   = 1900;
}
