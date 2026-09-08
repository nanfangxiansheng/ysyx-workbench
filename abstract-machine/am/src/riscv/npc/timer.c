#include <am.h>

// RTC实时时钟寄存器: 两次32位读拼出64位的微秒数 (内存映射I/O)
#define RTC_ADDR    0x20000000u
#define RTC_ADDR_HI (RTC_ADDR + 4u)
/*
u 是整数字面量的后缀，表示这个常量的类型是 unsigned int（无符号），而不是默认的 int。*/
void __am_timer_init() {
}

void __am_timer_uptime(AM_TIMER_UPTIME_T *uptime) {
  volatile uint32_t *rtc_lo = (volatile uint32_t *)RTC_ADDR;
  volatile uint32_t *rtc_hi = (volatile uint32_t *)RTC_ADDR_HI;
  // 先读高32位再读低32位: 若两者之间低32位恰好溢出,
  // 读到的只是"过去"的时间, 不会出现时间倒退
  uint32_t hi = *rtc_hi;
  uint32_t lo = *rtc_lo;
  uptime->us = ((uint64_t)hi << 32) | lo;
}

void __am_timer_rtc(AM_TIMER_RTC_T *rtc) {
  rtc->second = 0;
  rtc->minute = 0;
  rtc->hour   = 0;
  rtc->day    = 0;
  rtc->month  = 0;
  rtc->year   = 1900;
}
