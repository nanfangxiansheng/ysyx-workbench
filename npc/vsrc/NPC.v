// NPC.v - 顶层模块 (单周期NPC, 支持所有8条指令 + ebreak)
// 存储器不在RTL中实现, 而是通过DPI-C交由C++仿真环境实现,
// 这样指令和数据都可以在C++侧加载, 便于测试
module NPC (
    input  clk,
    input  rst
);

    // ============================================
    // DPI-C: RTL与C++仿真环境之间的交互
    // ============================================
    import "DPI-C" function int  pmem_read(input int raddr);
    import "DPI-C" function void pmem_write(input int waddr, input int wdata, input byte wmask);
    // NPC执行ebreak时, 通过该函数通知仿真环境结束仿真
    import "DPI-C" function void ebreak();

    // PC寄存器
    reg  [31:0] pc;
    wire [31:0] pc_next;

    // 取指阶段
    reg  [31:0] inst;

    // 译码阶段
    wire [6:0]  opcode;
    wire [4:0]  rd;
    wire [2:0]  funct3;
    wire [4:0]  rs1;
    wire [4:0]  rs2;
    wire [6:0]  funct7;
    wire [31:0] imm_i;
    wire [31:0] imm_u;
    wire [31:0] imm_s;

    // 执行阶段
    wire [4:0]  reg_waddr;
    wire [31:0] reg_wdata;
    wire        reg_wen;
    wire        is_jalr;
    wire [31:0] target_pc;
    wire        pc_sel;
    wire        is_load;
    wire        is_store;
    wire [2:0]  mem_funct3;
    wire [31:0] mem_addr;
    wire [31:0] mem_wdata;
    wire        mem_wen;

    // 寄存器文件接口
    wire [31:0] reg_rdata1;
    wire [31:0] reg_rdata2;

    // 存储器接口
    reg  [3:0]  wmask;      // 写掩码: 每比特对应1个字节
    reg  [31:0] mem_rdata;  // 从存储器(C++侧)读出的数据
    wire [31:0] load_data;

    // ============================================
    // PC更新逻辑
    // ============================================
    always @(posedge clk or posedge rst) begin
        if (rst) begin
            pc <= 32'h00000000;
        end else begin
            pc <= pc_next;
        end
    end

    // PC选择
    assign pc_next = pc_sel ? target_pc : (pc + 4);

    // ============================================
    // 取指: 指令存储器由C++实现, 通过DPI-C调用
    // pmem_read()从C++侧的物理内存中取回指令
    // ============================================
    always @(*) begin
        inst = pmem_read(pc);
    end

    // ============================================
    // ebreak: 程序执行到ebreak时, 通过DPI-C通知
    // 仿真环境结束仿真 (ebreak编码见RISC-V手册)
    // ============================================
    wire is_ebreak = (inst == 32'h00100073);

    reg ebreak_printed;
    initial begin
        ebreak_printed = 1'b0;
    end

    always @(*) begin
        if (is_ebreak && !ebreak_printed) begin
            ebreak_printed = 1'b1;
            ebreak();  // 通知C++仿真环境结束仿真
            $display("\n========================================");
            $display("NPC: 遇到ebreak指令, 仿真结束!");
            $display("最终寄存器状态:");
            regfile.print_final_regs();
            $display("========================================");
        end
    end

    // ============================================
    // 模块实例化
    // ============================================

    // 1. 译码单元 (IDU)
    IDU idu (
        .inst    (inst),
        .opcode  (opcode),
        .rd      (rd),
        .funct3  (funct3),
        .rs1     (rs1),
        .rs2     (rs2),
        .funct7  (funct7),
        .imm_i   (imm_i),
        .imm_u   (imm_u),
        .imm_s   (imm_s)
    );

    // 2. 寄存器文件 (Register File)
    RegisterFile #(
        .ADDR_WIDTH(5),
        .DATA_WIDTH(32)
    ) regfile (
        .clk   (clk),
        .wdata (final_wdata),
        .waddr (reg_waddr),
        .wen   (reg_wen),
        .raddr1(rs1),
        .raddr2(rs2),
        .rdata1(reg_rdata1),
        .rdata2(reg_rdata2)
    );

    // 3. 执行单元 (EXU)
    EXU exu (
        .rs1_data  (reg_rdata1),
        .rs2_data  (reg_rdata2),
        .imm_i     (imm_i),
        .imm_u     (imm_u),
        .imm_s     (imm_s),
        .pc        (pc),
        .opcode    (opcode),
        .funct3    (funct3),
        .funct7    (funct7),
        .rd        (rd),
        .rs1       (rs1),
        .rs2       (rs2),
        .result    (reg_wdata),
        .waddr     (reg_waddr),
        .wen       (reg_wen),
        .is_jalr   (is_jalr),
        .target_pc (target_pc),
        .pc_sel    (pc_sel),
        .is_load   (is_load),
        .is_store  (is_store),
        .mem_funct3(mem_funct3),
        .mem_addr  (mem_addr),
        .mem_wdata (mem_wdata),
        .mem_wen   (mem_wen)
    );

    // 4. 访存单元 (LSU): 负责load数据的字节选择
    LSU lsu (
        .addr     (mem_addr),
        .funct3   (mem_funct3),
        .mem_data (mem_rdata),
        .load_data(load_data)
    );

    // ============================================
    // 数据访存: 存储器由C++实现, 通过DPI-C访问
    // ============================================
    always @(*) begin
        // 写掩码: wmask中每比特对应wdata中1个字节
        case (mem_funct3)
            3'b010:  wmask = 4'b1111;                  // sw - 写入4字节
            3'b000:  wmask = 4'b0001 << mem_addr[1:0]; // sb - 只写1个字节
            default: wmask = 4'b1111;
        endcase

        if (is_load) begin
            mem_rdata = pmem_read(mem_addr);
        end else begin
            mem_rdata = 0;
        end

        if (is_store) begin
            pmem_write(mem_addr, mem_wdata, wmask);
        end
    end

    // ============================================
    // 写回数据选择: load指令写回从存储器取回的数据
    // ============================================
    wire [31:0] final_wdata;
    assign final_wdata = is_load ? load_data : reg_wdata;

    // ============================================
    // 调试: 打印每拍状态
    // 仿真的结束不再依赖固定周期数, 而是由程序
    // 中的ebreak指令决定
    // ============================================
    integer cycle_count;

    initial begin
        cycle_count = 0;
    end

    always @(posedge clk) begin
        if (!rst) begin
            cycle_count <= cycle_count + 1;
            $display("----------------------------------------");
            $display("Cycle %0d: PC = 0x%08X, inst = 0x%08X", cycle_count, pc, inst);

            if (is_load) begin
                $display("  *** LOAD: addr = 0x%08X, funct3 = %b", mem_addr, mem_funct3);
            end
            if (is_store) begin
                $display("  *** STORE: addr = 0x%08X, data = 0x%08X, wmask = %b",
                         mem_addr, mem_wdata, wmask);
            end
            if (is_jalr) begin
                $display("  *** JALR: 跳转到 0x%08X", target_pc);
            end
        end
    end

endmodule
