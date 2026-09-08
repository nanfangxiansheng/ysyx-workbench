// difftest.cpp - DiffTest 差分测试
// 把C参考模型REF (改编自 e5_exp/mini2/minirvemu4.c) 和NPC并排执行:
// NPC每执行一条指令, REF也执行一条, 然后对比双方的PC和通用寄存器,
// 第一次不一致就打印详细差异, 由main.cpp以非0退出码结束仿真.
//
// 约定:
//   REF与NPC共享同一块物理内存pmem, load/store的结果天然一致,
//   所以只需对比PC和寄存器; REF执行到ebreak后停止对比.
#include <cassert>
#include <cstdint> // INT32_MIN
#include <cstdio>
#include <cstring>
#include "svdpi.h"
#include "difftest.h"

// NPC中通过DPI-C export导出的状态读取接口
extern "C" int get_reg(int raddr); // RegisterFile.v: 读通用寄存器
extern "C" int get_pc();           // NPC.v: 读PC

// ==================== 参考模型REF的状态 ====================
static struct {
  uint32_t pc;
  uint32_t gpr[32];
  bool halt; // REF执行到ebreak后置位, 停止对比
} ref;

static svScope scope_npc = nullptr; // NPC.v中get_pc的作用域
static svScope scope_rf = nullptr;  // regfile实例中get_reg的作用域
static uint64_t difftest_count = 0; // 已对比的指令数

// 调用DPI export函数前要先切换到函数所在实例的作用域,
// Verilator才知道去读哪个实例的状态
static uint32_t npc_read_pc() {
  svSetScope(scope_npc);
  return (uint32_t)get_pc();
}

static uint32_t npc_read_gpr(int raddr) {
  svSetScope(scope_rf);
  return (uint32_t)get_reg(raddr);
}

int difftest_read_gpr(int raddr) { return npc_read_gpr(raddr); }

void difftest_init() {
  scope_npc = svGetScopeFromName("TOP.NPC");
  scope_rf = svGetScopeFromName("TOP.NPC.regfile");
  assert(scope_npc != nullptr && scope_rf != nullptr); // 层次路径不对时会失败

  ref.pc = MBASE; // 与NPC的复位PC一致
  memset(ref.gpr, 0, sizeof(ref.gpr));
  ref.halt = false;
  difftest_count = 0;
}

// ==================== REF的内存访问 (共享pmem, 小端序) ====================
static uint32_t ref_load(uint32_t addr, int len) {
  uint8_t *m = &pmem[addr - MBASE];
  uint32_t v = 0;
  for (int i = 0; i < len; i++) {
    v |= (uint32_t)m[i] << (8 * i);
  }
  return v;
}

static void ref_store(uint32_t addr, int len, uint32_t data) {
  uint8_t *m = &pmem[addr - MBASE];
  for (int i = 0; i < len; i++) {
    m[i] = (data >> (8 * i)) & 0xff;
  }
}

// 写寄存器: 作为"完美模型", 必须保证x0恒为0
static void ref_write(uint8_t rd, uint32_t v) {
  if (rd != 0) {
    ref.gpr[rd] = v;
  }
}

// ==================== 立即数提取 (带符号扩展) ====================
#define SEXT(x, n) ((int32_t)((x) << (32 - (n))) >> (32 - (n)))
#define IMM_I(inst) SEXT(((inst) >> 20) & 0xfff, 12)
#define IMM_U(inst) ((inst) & 0xfffff000)
#define IMM_S(inst) SEXT(((((inst) >> 7) & 0x1f) | (((inst) >> 25) & 0x7f) << 5), 12)
#define IMM_J(inst)                                                                 \
  SEXT(((((inst) >> 31) & 0x1) << 20) | ((((inst) >> 21) & 0x3ff) << 1) |           \
           ((((inst) >> 20) & 0x1) << 11) | ((((inst) >> 12) & 0xff) << 12), 21)
#define IMM_B(inst)                                                                  \
  SEXT(((((inst) >> 31) & 0x1) << 12) | ((((inst) >> 7) & 0x1) << 11) |              \
           ((((inst) >> 25) & 0x3f) << 5) | ((((inst) >> 8) & 0xf) << 1), 13)

// ==================== REF执行一条指令 ====================

// 设备寄存器的地址: UART状态寄存器, RTC时钟的低32位/高32位
static bool is_dev_addr(uint32_t addr) {
  return addr == UART_STAT || addr == RTC_ADDR || addr == RTC_ADDR_HI;
}

// 设备寄存器的load: 从NPC的目标寄存器rd中拷贝读出的数据,
// 并按funct3做与NPC的LSU相同的截取/扩展
// 返回false表示REF不支持该load类型 (视作DiffTest错误)
static bool ref_dev_load(uint8_t rd, uint8_t funct3) {
  uint32_t raw = npc_read_gpr(rd);
  switch (funct3) {
    case 0b000: // lb: 符号扩展
      ref_write(rd, (uint32_t)(int32_t)(int8_t)raw);
      break;
    case 0b100: // lbu: 零扩展
      ref_write(rd, raw & 0xffu);
      break;
    case 0b001: // lh: 符号扩展
      ref_write(rd, (uint32_t)(int32_t)(int16_t)raw);
      break;
    case 0b101: // lhu: 零扩展
      ref_write(rd, raw & 0xffffu);
      break;
    case 0b010: // lw
      ref_write(rd, raw);
      break;
    default:
      return false;
  }
  return true;
}

// 返回false表示REF不支持该指令 (视作DiffTest错误)
static bool ref_exec() {
  uint32_t inst = *(uint32_t *)&pmem[ref.pc - MBASE];
  uint8_t opcode = inst & 0x7f;
  uint8_t rd = (inst >> 7) & 0x1f;
  uint8_t funct3 = (inst >> 12) & 0x7;
  uint8_t rs1 = (inst >> 15) & 0x1f;
  uint8_t rs2 = (inst >> 20) & 0x1f;
  uint8_t funct7 = (inst >> 25) & 0x7f;

  switch (opcode) {
    case 0b0010011: { // I-type运算指令
      uint32_t shamt = (inst >> 20) & 0x1f;
      uint32_t v;
      switch (funct3) {
        case 0b000: // addi
          v = ref.gpr[rs1] + (uint32_t)IMM_I(inst);
          break;
        case 0b001: // slli
          v = ref.gpr[rs1] << shamt;
          break;
        case 0b010: // slti
          v = ((int32_t)ref.gpr[rs1] < (int32_t)IMM_I(inst)) ? 1 : 0;
          break;
        case 0b011: // sltiu
          v = (ref.gpr[rs1] < (uint32_t)IMM_I(inst)) ? 1 : 0;
          break;
        case 0b100: // xori
          v = ref.gpr[rs1] ^ (uint32_t)IMM_I(inst);
          break;
        case 0b101: // srli / srai
          v = ((inst >> 25) & 0x3f) == 0b0100000
                  ? (uint32_t)((int32_t)ref.gpr[rs1] >> shamt)
                  : ref.gpr[rs1] >> shamt;
          break;
        case 0b110: // ori
          v = ref.gpr[rs1] | (uint32_t)IMM_I(inst);
          break;
        case 0b111: // andi
          v = ref.gpr[rs1] & (uint32_t)IMM_I(inst);
          break;
        default:
          return false;
      }
      ref_write(rd, v);
      ref.pc += 4;
      break;
    }
    case 0b0110011: { // R-type运算指令 + M扩展乘除法
      if (funct7 == 0b0000000) { // 基础运算
        uint32_t a = ref.gpr[rs1], b = ref.gpr[rs2];
        uint32_t shamt = b & 0x1f;
        uint32_t v;
        switch (funct3) {
          case 0b000: v = a + b; break; // add
          case 0b001: v = a << shamt; break; // sll
          case 0b010: v = ((int32_t)a < (int32_t)b) ? 1 : 0; break; // slt
          case 0b011: v = (a < b) ? 1 : 0; break; // sltu
          case 0b100: v = a ^ b; break; // xor
          case 0b101: v = a >> shamt; break; // srl
          case 0b110: v = a | b; break; // or
          case 0b111: v = a & b; break; // and
          default: return false;
        }
        ref_write(rd, v);
        ref.pc += 4;
      } else if (funct7 == 0b0100000) { // sub / sra
        if (funct3 == 0b000) {
          ref_write(rd, ref.gpr[rs1] - ref.gpr[rs2]); // sub
        } else if (funct3 == 0b101) { // sra
          ref_write(rd, (uint32_t)((int32_t)ref.gpr[rs1] >> (ref.gpr[rs2] & 0x1f)));
        } else {
          return false;
        }
        ref.pc += 4;
      } else if (funct7 == 0b0000001) { // M扩展
        uint32_t a = ref.gpr[rs1], b = ref.gpr[rs2];
        int32_t sa = (int32_t)a, sb = (int32_t)b;
        switch (funct3) {
          case 0b000: // mul: 乘积低32位
            ref_write(rd, (uint32_t)((int64_t)sa * (int64_t)sb));
            break;
          case 0b001: // mulh: 有符号×有符号, 高32位
            ref_write(rd, (uint32_t)(((int64_t)sa * (int64_t)sb) >> 32));
            break;
          case 0b010: { // mulhsu: 有符号×无符号, 高32位
            int64_t p = (int64_t)sa * (int64_t)(uint64_t)b; // b零扩展后数值不变
            ref_write(rd, (uint32_t)((uint64_t)p >> 32));
            break;
          }
          case 0b011: // mulhu: 无符号×无符号, 高32位
            ref_write(rd, (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32));
            break;
          case 0b100: // div: 除零得-1, INT_MIN/-1溢出得INT_MIN
            if (sb == 0) {
              ref_write(rd, ~0u);
            } else if (sa == INT32_MIN && sb == -1) {
              ref_write(rd, 0x80000000u);
            } else {
              ref_write(rd, (uint32_t)(sa / sb));
            }
            break;
          case 0b101: // divu: 无符号除零得0xFFFFFFFF
            ref_write(rd, b == 0 ? ~0u : a / b);
            break;
          case 0b110: // rem: 除零得被除数, 溢出得0
            if (sb == 0) {
              ref_write(rd, a);
            } else if (sa == INT32_MIN && sb == -1) {
              ref_write(rd, 0);
            } else {
              ref_write(rd, (uint32_t)(sa % sb));
            }
            break;
          case 0b111: // remu
            ref_write(rd, b == 0 ? a : a % b);
            break;
        }
        ref.pc += 4;
      } else {
        return false;
      }
      break;
    }
    case 0b0110111: // lui
      ref_write(rd, IMM_U(inst));
      ref.pc += 4;
      break;
    case 0b0010111: // auipc
      ref_write(rd, ref.pc + IMM_U(inst));
      ref.pc += 4;
      break;
    case 0b0000011: { // 加载指令: lb/lh/lw/lbu/lhu
      uint32_t addr = ref.gpr[rs1] + (uint32_t)IMM_I(inst);
      if (is_dev_addr(addr)) {
        // 设备寄存器 (UART状态/RTC时钟) 的值由NPC侧的行为模型决定,
        // NPC读出的才是真实值. 此时NPC已执行完本条load (DiffTest比REF先执行),
        // 直接从NPC的目标寄存器rd中拷贝读出的数据,
        // 并按funct3做与NPC的LSU相同的截取/扩展, 保证双方读到一致的值
        if (!ref_dev_load(rd, funct3)) {
          return false;
        }
        ref.pc += 4;
        break;
      }
      switch (funct3) {
        case 0b000: // lb: 符号扩展
          ref_write(rd, (uint32_t)(int32_t)(int8_t)ref_load(addr, 1));
          break;
        case 0b001: // lh: 符号扩展
          ref_write(rd, (uint32_t)(int32_t)(int16_t)ref_load(addr, 2));
          break;
        case 0b010: // lw
          ref_write(rd, ref_load(addr, 4));
          break;
        case 0b100: // lbu
          ref_write(rd, ref_load(addr, 1));
          break;
        case 0b101: // lhu
          ref_write(rd, ref_load(addr, 2));
          break;
        default:
          return false;
      }
      ref.pc += 4;
      break;
    }
    case 0b0100011: { // 存储指令: sb/sh/sw
      uint32_t addr = ref.gpr[rs1] + (uint32_t)IMM_S(inst);
      if (addr == UART_BASE) {
        // 写入UART: NPC侧的行为模型会输出字符, REF忽略即可, 避免输出两遍
      } else if (funct3 == 0b010) {
        ref_store(addr, 4, ref.gpr[rs2]); // sw
      } else if (funct3 == 0b001) {
        ref_store(addr, 2, ref.gpr[rs2]); // sh
      } else if (funct3 == 0b000) {
        ref_store(addr, 1, ref.gpr[rs2]); // sb
      } else {
        return false;
      }
      ref.pc += 4;
      break;
    }
    case 0b1100111: { // jalr
      uint32_t target = (ref.gpr[rs1] + (uint32_t)IMM_I(inst)) & ~0x1u;
      ref_write(rd, ref.pc + 4);
      ref.pc = target;
      break;
    }
    case 0b1101111: // jal
      ref_write(rd, ref.pc + 4);
      ref.pc += (uint32_t)IMM_J(inst);
      break;
    case 0b1100011: { // 条件分支: beq/bne/blt/bge/bltu/bgeu
      bool taken;
      switch (funct3) {
        case 0b000: taken = (ref.gpr[rs1] == ref.gpr[rs2]); break; // beq
        case 0b001: taken = (ref.gpr[rs1] != ref.gpr[rs2]); break; // bne
        case 0b100: taken = ((int32_t)ref.gpr[rs1] <  (int32_t)ref.gpr[rs2]); break; // blt
        case 0b101: taken = ((int32_t)ref.gpr[rs1] >= (int32_t)ref.gpr[rs2]); break; // bge
        case 0b110: taken = (ref.gpr[rs1] <  ref.gpr[rs2]); break; // bltu
        case 0b111: taken = (ref.gpr[rs1] >= ref.gpr[rs2]); break; // bgeu
        default: return false;
      }
      ref.pc = taken ? ref.pc + (uint32_t)IMM_B(inst) : ref.pc + 4;
      break;
    }
    default:
      return false; // REF不支持的指令
  }
  return true;
}

// ==================== 对比与报告 ====================
static int report_mismatch(uint32_t prev_pc, uint32_t inst) {
  printf("\n========================================\n");
  printf("DiffTest: 第%llu条指令执行后, NPC与REF状态不一致!\n",
         (unsigned long long)difftest_count);
  printf("  出错指令: PC = 0x%08x, inst = 0x%08x\n", prev_pc, inst);
  printf("  期望(REF): PC = 0x%08x\n", ref.pc);
  printf("  实际(NPC): PC = 0x%08x\n", npc_read_pc());
  for (int i = 1; i < 32; i++) {
    uint32_t v = npc_read_gpr(i);
    if (v != ref.gpr[i]) {
      printf("  寄存器x%-2d   期望(REF) = 0x%08x, 实际(NPC) = 0x%08x\n",
             i, ref.gpr[i], v);
    }
  }
  printf("========================================\n");
  return 1;
}

int difftest_step() {
  if (ref.halt) {
    return 0;
  }

  uint32_t prev_pc = ref.pc;
  uint32_t inst = *(uint32_t *)&pmem[ref.pc - MBASE];
  if (inst == 0x00100073) { // ebreak: 双方同时停机, 不再对比
    ref.halt = true;
    return 0;
  }

  if (!ref_exec()) {
    printf("DiffTest: REF不支持PC = 0x%08x处的指令 0x%08x\n", prev_pc, inst);
    return 1;
  }
  difftest_count++;

  // 对比PC和x1..x31 (x0恒为0, 无需对比)
  if (npc_read_pc() != ref.pc) {
    return report_mismatch(prev_pc, inst);
  }
  for (int i = 1; i < 32; i++) {
    if (npc_read_gpr(i) != ref.gpr[i]) {
      return report_mismatch(prev_pc, inst);
    }
  }
  return 0;
}
