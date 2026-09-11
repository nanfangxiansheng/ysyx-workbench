// EXU.v - Execution Unit (支持 addi, jalr, jal, beq/bne, add, M扩展乘除法, lui, auipc, lw, lbu, sw, sb)
module EXU (
    input  [31:0] rs1_data,
    input  [31:0] rs2_data,
    input  [31:0] imm_i,
    input  [31:0] imm_u,
    input  [31:0] imm_s,
    input  [31:0] imm_j,
    input  [31:0] imm_b,
    input  [31:0] pc,
    input  [6:0]  opcode,
    input  [2:0]  funct3,
    input  [6:0]  funct7,
    input  [4:0]  rd,
    input  [4:0]  rs1,
    input  [4:0]  rs2,
    input  [11:0] csr_addr,
    input  [31:0] csr_rdata,  // CSR单元按csr_addr读出的旧值
    output reg [31:0] result,
    output reg [4:0]  waddr,
    output reg        wen,
    output reg        is_jalr,
    output reg [31:0] target_pc,
    output reg        pc_sel,
    output reg        is_load,
    output reg        is_store,
    output reg [2:0]  mem_funct3,
    output reg [31:0] mem_addr,
    output reg [31:0] mem_wdata,
    output reg        mem_wen,
    output reg        is_csr   // 本指令是csrrs: NPC据此在指令完成拍写CSR
);

    // ============================================
    // M扩展(乘除法)的中间结果
    // 乘法: 把32位操作数先扩展成64位, 乘积的低/高32位分别对应mul/mulh
    // 除法: 必须先挡住除零和INT_MIN/-1溢出, 否则Verilator生成的
    //       C++代码在仿真时会真的发生除零错误
    // ============================================
    wire [63:0] a_sgn = {{32{rs1_data[31]}}, rs1_data}; // 符号扩展
    wire [63:0] b_sgn = {{32{rs2_data[31]}}, rs2_data};
    wire [63:0] a_uns = {32'b0, rs1_data};              // 零扩展
    wire [63:0] b_uns = {32'b0, rs2_data};
    wire [63:0] prod_ss = a_sgn * b_sgn;                // 有符号×有符号
    wire [63:0] prod_su = a_sgn * $signed(b_uns);       // 有符号×无符号(b_uns按位表示的值即其符号值)
    wire [63:0] prod_uu = a_uns * b_uns;                // 无符号×无符号

    wire        div_by_zero = (rs2_data == 32'b0);
    wire        div_ovf     = (rs1_data == 32'h80000000) && (rs2_data == 32'hFFFFFFFF); // INT_MIN / -1
    wire signed [31:0] a_s32 = $signed(rs1_data);
    wire signed [31:0] b_s32 = $signed(rs2_data);
    wire signed [31:0] quo_s  = (b_s32 == 0)        ? 32'sd0 :
                                div_ovf             ? -32'sd2147483648 : a_s32 / b_s32;
    wire signed [31:0] rem_s  = (b_s32 == 0)        ? a_s32 :
                                div_ovf             ? 32'sd0           : a_s32 % b_s32;
    wire [31:0] quo_u = div_by_zero ? 32'hFFFFFFFF : rs1_data / rs2_data;
    wire [31:0] rem_u = div_by_zero ? rs1_data     : rs1_data % rs2_data;

    reg br_taken; // 分支条件是否成立

    // ============================================
    // 组合逻辑：根据 opcode 执行不同指令
    // ============================================
    always @(*) begin
        // 默认值
        result = 32'b0;
        waddr = rd;
        wen = 1'b0;
        is_jalr = 1'b0;
        target_pc = 32'b0;
        pc_sel = 1'b0;  // 默认顺序执行
        is_load = 1'b0;
        is_store = 1'b0;
        mem_funct3 = 3'b0;
        mem_addr = 32'b0;
        mem_wdata = 32'b0;
        mem_wen = 1'b0;
        is_csr = 1'b0;
        
        case (opcode)
            7'b0010011: begin  // I-type运算指令 (addi及移位/比较/逻辑全家)
                waddr = rd;
                wen = (rd != 0);
                is_jalr = 1'b0;
                pc_sel = 1'b0;
                case (funct3)
                    3'b000: result = rs1_data + imm_i;                       // addi
                    3'b001: result = rs1_data << imm_i[4:0];                 // slli
                    3'b010: result = ($signed(rs1_data) < $signed(imm_i)) ? 32'd1 : 32'd0; // slti
                    3'b011: result = (rs1_data < imm_i) ? 32'd1 : 32'd0;     // sltiu
                    3'b100: result = rs1_data ^ imm_i;                       // xori
                    3'b101: begin // srli / srai
                        // 注意: 不能用三目运算符合并两个分支,
                        // 否则无符号分支会把整个表达式拖成无符号, >>>退化成>>
                        if (imm_i[10]) begin
                            result = $signed(rs1_data) >>> imm_i[4:0]; // srai
                        end else begin
                            result = rs1_data >> imm_i[4:0];           // srli
                        end
                    end
                    3'b110: result = rs1_data | imm_i;                       // ori
                    3'b111: result = rs1_data & imm_i;                       // andi
                    default: result = 32'b0;
                endcase
            end

            7'b0110011: begin  // R-type运算指令 (add/sub/移位/比较/逻辑 + M扩展乘除法)
                waddr = rd;
                wen = (rd != 0);
                is_jalr = 1'b0;
                pc_sel = 1'b0;
                if (funct7 == 7'b0000000) begin
                    case (funct3)
                        3'b000: result = rs1_data + rs2_data;                // add
                        3'b001: result = rs1_data << rs2_data[4:0];          // sll
                        3'b010: result = ($signed(rs1_data) < $signed(rs2_data)) ? 32'd1 : 32'd0; // slt
                        3'b011: result = (rs1_data < rs2_data) ? 32'd1 : 32'd0; // sltu
                        3'b100: result = rs1_data ^ rs2_data;                // xor
                        3'b101: result = rs1_data >> rs2_data[4:0];          // srl
                        3'b110: result = rs1_data | rs2_data;                // or
                        3'b111: result = rs1_data & rs2_data;                // and
                    endcase
                end
                else if (funct7 == 7'b0100000) begin
                    case (funct3)
                        3'b000: result = rs1_data - rs2_data;                // sub
                        3'b101: result = $signed(rs1_data) >>> rs2_data[4:0]; // sra
                        default: result = 32'b0;
                    endcase
                end
                else if (funct7 == 7'b0000001) begin  // M扩展: 乘除法指令
                    case (funct3)
                        3'b000: result = prod_ss[31:0];  // mul:   乘积低32位
                        3'b001: result = prod_ss[63:32]; // mulh:  有×有, 高32位
                        3'b010: result = prod_su[63:32]; // mulhsu:有×无, 高32位
                        3'b011: result = prod_uu[63:32]; // mulhu: 无×无, 高32位
                        3'b100: result = quo_s;          // div:   商
                        3'b101: result = quo_u;          // divu:  无符号商
                        3'b110: result = rem_s;          // rem:   余数
                        3'b111: result = rem_u;          // remu:  无符号余数
                    endcase
                end
            end
            
            7'b0110111: begin  // lui 指令
                result = imm_u;
                waddr = rd;
                wen = (rd != 0);
                is_jalr = 1'b0;
                pc_sel = 1'b0;
            end

            7'b0010111: begin  // auipc 指令: rd = pc + (imm << 12)
                result = pc + imm_u;  // imm_u已经是左移12位后的结果
                waddr = rd;
                wen = (rd != 0);
                is_jalr = 1'b0;
                pc_sel = 1'b0;
            end
            
            7'b0000011: begin  // 加载指令 (lw, lbu)
                is_load = 1'b1;
                is_store = 1'b0;
                mem_funct3 = funct3;
                mem_addr = rs1_data + imm_i;
                mem_wen = 1'b0;
                
                // 加载结果会在下一周期写入寄存器
                // 这里只是为了保存控制信号
                waddr = rd;
                wen = (rd != 0);
                is_jalr = 1'b0;
                pc_sel = 1'b0;
                result = 32'b0;  // 实际数据在 WBU 阶段写入
            end
            
            7'b0100011: begin  // 存储指令 (sw, sb)
                is_load = 1'b0;
                is_store = 1'b1;
                mem_funct3 = funct3;
                mem_addr = rs1_data + imm_s;  // S-type立即数
                // 存储器按字节编址: sb/sh/sw都把原始数据放在低字节,
                // 由C++侧从mem_addr起按wmask连续写入 (无需移位对齐)
                mem_wdata = rs2_data;
                mem_wen = 1'b1;
                
                waddr = 5'b0;
                wen = 1'b0;
                is_jalr = 1'b0;
                pc_sel = 1'b0;
                result = 32'b0;
            end
            
            7'b1100111: begin  // jalr 指令
                target_pc = (rs1_data + imm_i) & ~32'h1;
                result = pc + 4;
                waddr = rd;
                wen = (rd != 0);
                is_jalr = 1'b1;
                pc_sel = 1'b1;
                is_load = 1'b0;
                is_store = 1'b0;
                mem_wen = 1'b0;
            end

            7'b1101111: begin  // jal 指令: rd = pc + 4, 跳转到 pc + imm_j
                target_pc = pc + imm_j;
                result = pc + 4;
                waddr = rd;
                wen = (rd != 0);
                is_jalr = 1'b0;
                pc_sel = 1'b1;
                is_load = 1'b0;
                is_store = 1'b0;
                mem_wen = 1'b0;
            end

            7'b1100011: begin  // 条件分支指令: 条件成立则跳转到 pc + imm_b, imm_b是b类型立即数
                case (funct3)
                    3'b000: br_taken = (rs1_data == rs2_data);                              // beq
                    3'b001: br_taken = (rs1_data != rs2_data);                              // bne
                    3'b100: br_taken = ($signed(rs1_data) <  $signed(rs2_data));            // blt
                    3'b101: br_taken = ($signed(rs1_data) >= $signed(rs2_data));            // bge
                    3'b110: br_taken = (rs1_data <  rs2_data);                              // bltu
                    3'b111: br_taken = (rs1_data >= rs2_data);                              // bgeu
                    default: br_taken = 1'b0;
                endcase
                if (br_taken) begin
                    target_pc = pc + imm_b;
                    pc_sel = 1'b1;
                end
                is_load = 1'b0;
                is_store = 1'b0;
                mem_wen = 1'b0;
            end
            
            7'b1110011: begin  // SYSTEM指令: 目前只实现csrrs (funct3=010)
                if (funct3 == 3'b010) begin
                    // csrrs rd, csr, rs1: 原子地 rd = CSR旧值, CSR |= rs1
                    // 写回旧值在组合逻辑中完成, CSR写入由NPC在指令完成拍进行,
                    // 因此rd拿到的必然是写入前的旧值
                    result = csr_rdata;
                    waddr = rd;
                    wen = (rd != 0);
                    is_csr = 1'b1;
                end
                // 其余SYSTEM指令 (csrrw/csrrc/ecall等) 按"未知指令"处理
            end

            default: begin
                // 未知指令
                result = 32'b0;
                waddr = 5'b0;
                wen = 1'b0;
                is_jalr = 1'b0;
                pc_sel = 1'b0;
                is_load = 1'b0;
                is_store = 1'b0;
                mem_wen = 1'b0;
            end
        endcase
    end
    
endmodule
