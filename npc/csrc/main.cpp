// main.cpp - NPC 的仿真环境 (双模式, 由Makefile的SIM变量通过宏SOC选择)
//
// 定义了SOC (make sim, 默认; SIM=soc):
//   顶层为SimTop (ysyxSoC/ready-to-run/minirv/ElaborateTop.v),
//   NPC作为SoC中的处理器核, 取指从Flash(0x3000_0000)开始,
//   存储器和外设访问都经过SoC的总线.
//
// 未定义SOC (make SIM=npc sim):
//   顶层为NPC, 复位后从0x8000_0000 (pmem)开始取指;
//   NPC的SimpleBus请求由C++侧的行为级总线从设备应答
//   (pmem + UART/RTC行为模型), 并开启DiffTest与参考模型对比.
//
// 用法: ./npc [镜像文件.bin]
//   SoC模式:  镜像必须指定, 被读入仿真侧的Flash, NPC从Flash中启动
//   单独模式: 镜像被读入pmem(0x8000_0000), 不带参数时加载内置测试程序
//   单独模式下设环境变量DIFFTEST可开启DiffTest逐指令对比
//   (DIFFTEST=1 make SIM=npc sim IMG=xxx.bin, 见主函数内的说明)
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <verilated.h>
#ifdef SOC
#include "VSimTop.h"
typedef VSimTop TopModule; // 集成模式: 仿真顶层是SimTop
#else
#include "VNPC.h"
typedef VNPC TopModule; // 单独模式: 仿真顶层是NPC本身
#endif
#include "svdpi.h"
#include "difftest.h"
#ifdef NVBOARD // make NVBOARD=1 (仅SIM=soc) 时接入NVBoard虚拟外设
#include <nvboard.h>

// auto_bind.cpp (由constr/npc.nxdc经auto_pin_bind.py生成):
// 把SimTop的引脚绑定到NVBoard的串口终端等部件
extern void nvboard_bind_all_pins(VSimTop *top);
#endif

// difftest.cpp引用的物理内存:
// 单独模式下它是NPC唯一的存储器; SoC模式下暂未使用, 保留符号以便链接
uint8_t pmem[PMEM_SIZE] = {};

static TopModule *top = nullptr;
static bool halt_flag = false; // NPC 执行到 ebreak 后置位
static uint32_t halt_pc = 0;   // 执行 ebreak 时的 PC

// NPC 执行到 ebreak: 通知仿真环境结束仿真
extern "C" void ebreak(int pc) {
  halt_flag = true;
  halt_pc = (uint32_t)pc;
}

// ==================== 波形记录 (WAVE=1编译时生效) ====================
// 记录到 build/npc.fst, 可用 gtkwave 查看 (见Makefile的WAVE说明)
#ifdef WAVE
static VerilatedFstC *tfp = nullptr;

static void wave_init() {
  tfp = new VerilatedFstC;
  top->trace(tfp, 99); // 记录所有层次的信号
  tfp->open("build/npc.fst");
}

static void wave_dump() { tfp->dump(top->contextp()->time()); }
static void wave_close() { tfp->close(); }
#else
// 未开WAVE时提供空实现, 调用处无需写#ifdef
static void wave_init() {}
static void wave_dump() {}
static void wave_close() {}
#endif

#ifdef SOC
// ==================== Flash行为模型 (仅SoC模式) ====================
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

#else
// ==================== 物理内存与设备行为模型 (仅单独模式) ====================
// NPC的SimpleBus请求由本环境应答: 取指/访存都直接读写pmem,
// 特定地址路由到设备的行为模型 (UART串口/RTC时钟)

// 客户机地址 -> 宿主机内存
static uint8_t *guest_to_host(uint32_t addr) {
  return &pmem[addr - MBASE];
}

// 地址范围检查: 4字节访问不能越过内存末尾 (支持非对齐访问)
static bool in_pmem(uint32_t addr) {
  return addr >= MBASE && addr - MBASE <= PMEM_SIZE - 4;
}

// 从addr开始连续4个字节组装成32位小端数据
// 存储器按字节编址, 支持非对齐访问 (按字节流看待,
// 与DiffTest参考模型的ref_load/ref_store语义一致)
static uint32_t host_read(uint32_t addr) {
  uint8_t *p = guest_to_host(addr);
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// 把data按写掩码wmask写入从addr开始的4个字节
// wmask中每比特表示data中1个字节的掩码, 字节地址连续,
// 因此天然支持非对齐的sw/sh (与NPC发出的io_lsu_wmask约定一致)
static void host_write(uint32_t addr, uint32_t data, uint8_t wmask) {
  uint8_t *h = guest_to_host(addr);
  for (int i = 0; i < 4; i++) {
    if (wmask & (1 << i)) {
      h[i] = (data >> (8 * i)) & 0xff;
    }
  }
}

// 越界访问只提示一次, 避免刷屏 (复位前 pc 尚未稳定, 可能产生越界的取指)
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

static unsigned long long get_time() {
  return now_us() - boot_us;
}

// 总线load: 设备寄存器优先, 其余落到pmem
// UART状态寄存器: 最低位为1表示串口就绪, 设备速度用随机数模拟
// (12.5%概率就绪), 程序必须查询到就绪后才能输出字符, 否则字符会丢失
static uint32_t bus_load(uint32_t addr) {
  if (addr == UART_STAT) {
    return (rand() & 0x7) == 0 ? 1 : 0;
  } else if (addr == RTC_ADDR) {
    return get_time() & 0xffffffff; // RTC低32位
  } else if (addr == RTC_ADDR_HI) {
    return get_time() >> 32; // RTC高32位
  }

  if (!in_pmem(addr)) {
    out_of_bound(addr);
    return 0;
  }
  return host_read(addr);
}

// 总线store: 写串口数据寄存器 = 输出一个字符, 其余落到pmem
static void bus_store(uint32_t addr, uint32_t data, uint8_t wmask) {
  if (addr == UART_BASE) {
    fputc(data & 0xff, stderr);
    return;
  }

  if (!in_pmem(addr)) {
    out_of_bound(addr);
    return;
  }
  host_write(addr, data, wmask);
}

// 应答时序模仿ysyxSoC的MemBridge (MemBridge.scala:102-107):
//   respValid是单拍脉冲; 取指rdata用寄存器保持到下一次取指 --
//   因为NPC的inst组合地连在io_ifu_rdata上 (NPC.v: inst = io_ifu_rdata),
//   执行拍仍要从总线上读到当前指令; 若reqValid撤销后就撤掉rdata,
//   NPC在执行拍会读到0, 表现为"pc照常前进但寄存器不写入"
static uint32_t ifu_rdata_hold = 0; // 保持的取指应答数据

// 行为级SimpleBus从设备: 每拍检查NPC的总线请求并组合应答
// (单拍存储器, 延迟为0; NPC的reqValid在应答当拍即撤销, store只执行一次)
static void bus_slave_step() {
  // 取指通道: 新请求到来时刷新保持的数据, respValid只在该拍有效
  if (top->io_ifu_reqValid) {
    ifu_rdata_hold = bus_load(top->io_ifu_addr);
    top->io_ifu_respValid = 1;
  } else {
    top->io_ifu_respValid = 0;
  }
  top->io_ifu_rdata = ifu_rdata_hold;

  // 数据访存通道
  // 存储器返回的是对齐字的字 (请求地址低2位被截掉),
  // NPC自己按addr[1:0]旋转提取子字数据 (NPC.v: lsu_rdata_aligned);
  // 写同理: NPC已把wdata/wmask移到对齐字的对应字节通道上,
  // 从设备只需按通道写整字 (与AXI/SRAM的行为一致)
  top->io_lsu_respValid = 0;
  top->io_lsu_rdata = 0;
  if (top->io_lsu_reqValid) {
    top->io_lsu_respValid = 1;
    uint32_t word_addr = top->io_lsu_addr & ~3u; // 设备侧截掉低2位
    if (top->io_lsu_wen) {
      bus_store(word_addr, top->io_lsu_wdata, top->io_lsu_wmask);
    } else {
      top->io_lsu_rdata = bus_load(word_addr);
    }
  }
}

// NPC 的内置测试程序, 在未指定镜像文件时使用
// 对load/store/寄存器写回做冒烟测试, 正确性由DiffTest逐条保证;
// 结束前把a0清零, 满足"a0=0表示程序正确结束"的约定
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
    0x00000513, // 24: addi  a0, x0, 0       -> a0 = 0, 程序正确结束
    0x00100073, // 28: ebreak                -> 结束仿真
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

// 加载镜像: 命令行指定了文件则读入, 否则使用内置测试程序
static void load_img(int argc, char **argv) {
  if (argc >= 2) {
    load_img_from_file(argv[1]);
    return;
  }

  assert(sizeof(img) <= PMEM_SIZE);
  memcpy(pmem, img, sizeof(img));
  printf("NPC: 未指定镜像文件, 加载内置程序 (%zu 字节, %zu 条指令)\n",
         sizeof(img), sizeof(img) / sizeof(img[0]));
}
#endif // SOC

// ==================== 仿真框架 ====================

#ifdef SOC
// cpuClock(NPC核)频率 = clock(总线/外设)频率 × CLK_RATIO.
// 必须是偶数(保证两个时钟占空比都是50%); CLK_RATIO=1时退化为同频同相.
//
// 注意: 当前ready-to-run的ElaborateTop在CLK_RATIO=2时会触发SoC内部
// RationalCrossing的断言 (RationalCrossing.scala:98, CPU侧计数的相位
// 与总线侧sink对不上), 故暂用1:1. 以后想提频时需要研究该crossing
// 对时钟倍率/相位的要求 (必要时用Chisel重新elaborate SoC)
#define CLK_RATIO 1

static int clk_cnt = 0; // 时钟分频计数器

static void single_cycle() {
  // 一次调用 = cpuClock一个完整周期 (NPC前进一步);
  // clock每CLK_RATIO次调用才翻转一次, 频率即cpuClock的1/CLK_RATIO
  constexpr int HALF = CLK_RATIO / 2;

  top->contextp()->timeInc(1);
  top->cpuClock = 0;                        // NPC时钟低半周期
  // CLK_RATIO=1时HALF=0会让clock恒为高, 退化为直接跟随cpuClock
  top->clock = (CLK_RATIO == 1) ? top->cpuClock : (clk_cnt >= HALF);
  top->eval();
  wave_dump();

  top->contextp()->timeInc(1);
  top->cpuClock = 1;                        // NPC时钟上升沿
  top->clock = (CLK_RATIO == 1) ? top->cpuClock : (clk_cnt >= HALF);
  top->eval();
  wave_dump();

  if (++clk_cnt == CLK_RATIO) clk_cnt = 0;
}
#else
static void single_cycle() {
  // NPC只有一个时钟, 一次调用 = 一个完整周期 (NPC前进一步)
  // 先按当前的总线请求给出组合应答, 再给出上升沿:
  // NPC在上升沿采样respValid/rdata完成取指/访存
  bus_slave_step();

  top->contextp()->timeInc(1);
  top->clock = 0;
  top->eval();
  wave_dump();

  top->contextp()->timeInc(1);
  top->clock = 1;
  top->eval();
  wave_dump();
}
#endif

// 复位n个周期 (两种模式的顶层都有高电平有效的reset端口)
static void reset(int n) {
  top->reset = 1;
  while (n-- > 0) {
    single_cycle();
  }
  top->reset = 0;
}

// 防止无限仿真: SoC模式2e9拍已足够hello类程序;
// 单独模式要跑archbench基准程序(指令量10亿条级), 放宽到2e10拍
#ifdef SOC
static constexpr uint64_t MAX_CYCLES = 2000000000ull;
#else
static constexpr uint64_t MAX_CYCLES = 20000000000ull;
#endif

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);
  Verilated::traceEverOn(true); // 允许记录波形 (WAVE=1编译时生效)
  setvbuf(stdout, NULL, _IONBF, 0); // 无缓冲输出, 便于观察长时间仿真

  top = new TopModule;
  wave_init();

#ifdef SOC
#ifdef NVBOARD
  // 必须先绑定引脚再初始化NVBoard:
  // nvboard_init()构造UART组件时会缓存pin_array中的引脚指针,
  // 若之后才nvboard_bind_pin, 组件读到的还是旧指针, 串口终端收不到任何数据
  nvboard_bind_all_pins(top);

  // 初始化NVBoard虚拟开发板(串口终端等部件在此创建)
  nvboard_init();
  // UART的RX线空闲电平为高; 之后由NVBoard驱动(在串口终端中敲键即有输入),
  // 复位期间RX为0会被UART当成起始位
  top->externalPins_uart0_rx = 1;
#endif

  // 当前不关心其他端口, 给固定值
  top->coreSel = 0;
  top->externalPins_mygpio_in = 0;

  // 把镜像读入Flash, NPC复位后将从Flash中取出第一条指令
  if (argc < 2) {
    printf("NPC: 用法: %s <镜像文件.bin> (SoC模式必须指定镜像, "
           "单独仿真请用 make SIM=npc sim)\n",
           argv[0]);
    return 1;
  }
  load_flash_from_file(argv[1]);

  reset(100); // 复位至少100个周期

  // 不停地进行仿真, 直到程序执行 ebreak 指令为止.
  // 若程序陷入死循环(如hello输出后j自旋), 由超时强制结束
  uint64_t cycles = 0;
  while (!halt_flag) {
    single_cycle();
#ifdef NVBOARD
    nvboard_update(); // 每个仿真周期刷新一次虚拟外设(内部有按帧节流)
#endif
    cycles++;
    if (cycles > MAX_CYCLES) {
      printf("NPC: 超过%llu周期仍未停机, 强制结束\n", (unsigned long long)MAX_CYCLES);
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
#ifdef NVBOARD
  nvboard_quit(); // 关闭NVBoard窗口
#endif
  wave_close();
  return code;

#else
  // ---------------- 单独仿真NPC ----------------
  boot_us = now_us(); // RTC行为模型的计时起点

  load_img(argc, argv); // 镜像读入pmem, NPC复位后从0x8000_0000取指

  // DiffTest开关 (运行时控制, 不用重新编译):
  //   DIFFTEST=1 make SIM=npc ...  开启逐指令对比
  //   默认关闭: REF是mini参考模型, 不认识csrrs等指令,
  //   而AM程序运行时会读mcycle计时器, 所以跑AM程序时开不得;
  //   跑riscv-tests等只用基础指令的程序时可以开着查错
  // difftest_init()照常调用: 它只缓存DPI作用域, 之后读a0(x10)
  // 作退出码仍然需要它
  bool difftest_on = (getenv("DIFFTEST") != nullptr);
  printf("NPC: DiffTest %s\n", difftest_on ? "已开启 (DIFFTEST)" : "未开启");
  difftest_init();

  reset(10);

  // 不停地进行仿真, 直到程序执行 ebreak 指令为止;
  // 开启DiffTest时, NPC每提交一条指令(committed寄存器拉高一拍),
  // REF也执行一条并对比: 多周期NPC不是每拍都提交,
  // 不能每拍都对比, 否则REF会多跑
  uint64_t cycles = 0;
  int diff_err = 0;
  while (!halt_flag) {
    single_cycle();
    cycles++;
    if (difftest_on && npc_committed() && difftest_step() != 0) {
      diff_err = 1; // 状态不一致, 停止仿真
      break;
    }
    if (cycles > MAX_CYCLES) {
      printf("NPC: 超过%llu周期仍未停机, 强制结束\n", (unsigned long long)MAX_CYCLES);
      break;
    }
  }

  int code;
  if (diff_err) {
    // DiffTest发现不一致: 以非0退出码结束, 让Makefile捕捉到错误
    printf("NPC: HIT BAD TRAP (DiffTest发现执行结果不一致, 共执行 %llu 个周期)\n",
           (unsigned long long)cycles);
    code = 1;
  } else if (!halt_flag) {
    printf("NPC: 仿真超时结束 (共 %llu 个周期)\n", (unsigned long long)cycles);
    code = 1;
  } else {
    printf("NPC: hit ebreak at PC = 0x%08x, 共 %llu 个周期\n", halt_pc,
           (unsigned long long)cycles);

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
  wave_close();
  return code; // 把结束状态作为仿真进程的退出码, 便于脚本判断
#endif
}
