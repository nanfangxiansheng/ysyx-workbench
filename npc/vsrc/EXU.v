// EXU.v - Execution Unit (支持 addi, jalr, jal, add, lui, auipc, lw, lbu, sw, sb)
module EXU (
    input  [31:0] rs1_data,
    input  [31:0] rs2_data,
    input  [31:0] imm_i,
    input  [31:0] imm_u,
    input  [31:0] imm_s,
    input  [31:0] imm_j,
    input  [31:0] pc,
    input  [6:0]  opcode,
    input  [2:0]  funct3,
    input  [6:0]  funct7,
    input  [4:0]  rd,
    input  [4:0]  rs1,
    input  [4:0]  rs2,
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
    output reg        mem_wen
);
    
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
        
        case (opcode)
            7'b0010011: begin  // addi 指令
                if (funct3 == 3'b000) begin
                    result = rs1_data + imm_i;
                    waddr = rd;
                    wen = (rd != 0);
                    is_jalr = 1'b0;
                    pc_sel = 1'b0;
                end
            end
            
            7'b0110011: begin  // R-type 指令 (add)
                if (funct3 == 3'b000 && funct7 == 7'b0000000) begin  // add
                    result = rs1_data + rs2_data;
                    waddr = rd;
                    wen = (rd != 0);
                    is_jalr = 1'b0;
                    pc_sel = 1'b0;
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
                // sb: 把最低字节移到wmask对应的那1个字节lane上; sw: 不移位
                mem_wdata = (funct3 == 3'b000) ? (rs2_data << (8 * mem_addr[1:0]))
                                               : rs2_data;
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
