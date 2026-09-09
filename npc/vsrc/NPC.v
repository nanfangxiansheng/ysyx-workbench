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
    // NPC执行ebreak时, 通过该函数通知仿真环境结束仿真 (顺便把PC告诉C++侧)
    import "DPI-C" function void ebreak(input int pc);

    // PC寄存器
    reg  [31:0] pc;
    wire [31:0] pc_next;

    // ============================================
    // SimpleBus: IFU与存储器(ROM)之间的取指接口
    // 存储器是同步读的 (收到读请求后下个周期返回数据),
    // 因此通信协议为: master每周期发地址, slave下周期回数据
    // ============================================
    wire [31:0] ifu_raddr;   // IFU发给存储器的读地址
    reg  [31:0] ifu_rdata;   // 存储器返回的数据 (延迟1周期)

    // IFU状态机: 在不同阶段采取不同策略
    localparam IFU_IDLE = 1'b0; // 已发出pc对应的读地址, 指令尚未返回
    localparam IFU_WAIT = 1'b1; // ifu_rdata已返回, 是当前pc对应的有效指令
    reg ifu_state;

    // 指令有效信号: 只有wait状态下的ifu_rdata才对应当前的pc,
    // idle状态下总线上返回的是"上一周期"地址的数据, 不能当作指令执行
    wire inst_valid = (ifu_state == IFU_WAIT);

    // 取指阶段: inst直接来自存储器的同步读出口
    wire [31:0] inst;

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
    wire [31:0] imm_j;
    wire [31:0] imm_b;

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
    // "不执行指令"的办法: 让处理器的状态保持不变.
    // idle状态(inst_valid=0)下PC的写使能无效, PC保持原值,
    // 这样ifu_raddr也就保持不变, 等到wait状态才让PC前进
    // ============================================
    always @(posedge clk or posedge rst) begin
        if (rst) begin
            pc <= 32'h8000_0000; // AM程序链接在0x80000000处, 复位后从这里开始执行
        end else if (inst_valid) begin
            pc <= pc_next;
        end
    end

    // PC选择
    assign pc_next = pc_sel ? target_pc : (pc + 4);

    // ============================================
    // 取指地址: 协议要求每周期都通信, 因此两个状态下
    // 都把pc发给存储器 (pc只在wait状态结束的时钟沿更新)
    // ============================================
    assign ifu_raddr = pc;

    // 存储器取指端口: 同步读, 时钟沿采样当前地址, 下个周期返回数据
    always @(posedge clk) begin
        if (!rst) begin
            ifu_rdata <= pmem_read(ifu_raddr);
        end
    end

    // IFU状态机: idle发出请求并等待, wait执行返回的指令
    always @(posedge clk or posedge rst) begin
        if (rst) begin
            ifu_state <= IFU_IDLE;
        end else begin
            case (ifu_state)
                IFU_IDLE: ifu_state <= IFU_WAIT; // 下个周期指令返回
                IFU_WAIT: ifu_state <= IFU_IDLE; // 指令已执行, 取下一条
                default:  ifu_state <= IFU_IDLE;
            endcase
        end
    end

    assign inst = ifu_rdata;

    // 向C++侧导出"本周期是否提交了指令", DiffTest据此决定是否对比
    // (ifu_state在下个时钟沿就翻转了, 事后读不到, 故寄存一拍)
    reg committed;
    always @(posedge clk or posedge rst) begin
        if (rst)       committed <= 1'b0;
        else           committed <= inst_valid;
    end

    export "DPI-C" function get_committed;
    function int get_committed();
        get_committed = committed;
    endfunction

    // ============================================
    // DPI-C导出: C++侧读取当前PC (DiffTest逐条对比用)
    // ============================================
    export "DPI-C" function get_pc;
    function int get_pc();
        get_pc = pc;
    endfunction

    // ============================================
    // ebreak: 程序执行到ebreak时, 通过DPI-C通知
    // 仿真环境结束仿真 (ebreak编码见RISC-V手册)
    // ============================================
    // ebreak也要用inst_valid门控: 指令执行完后的idle周期里,
    // inst仍停留在ebreak的编码上, 不门控会触发两次
    wire is_ebreak = inst_valid && (inst == 32'h00100073);

    reg ebreak_printed;
    initial begin
        ebreak_printed = 1'b0;
    end

    always @(*) begin
        if (is_ebreak && !ebreak_printed) begin
            ebreak_printed = 1'b1;
            ebreak(pc);  // 通知C++仿真环境结束仿真
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
        .imm_s   (imm_s),
        .imm_j   (imm_j),
        .imm_b   (imm_b)
    );

    // 2. 寄存器文件 (Register File)
    RegisterFile #(
        .ADDR_WIDTH(5),
        .DATA_WIDTH(32)
    ) regfile (
        .clk   (clk),
        .wdata (final_wdata),
        .waddr (reg_waddr),
        .wen   (reg_wen && inst_valid), // idle状态下禁止写回
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
        .imm_j     (imm_j),
        .imm_b     (imm_b),
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
    // 读是纯组合的; 写必须放在时钟沿触发!
    // 若放在组合逻辑里, Verilator在clk=1的eval中更新PC后
    // 会用新指令重新稳定组合逻辑, 导致下一条store的写
    // "提前"一拍发出, 破坏store/load的时序可见性
    // ============================================
    always @(*) begin
        // 写掩码: wmask中每比特对应wdata中1个字节
        // 存储器按字节编址, 从mem_addr起连续写入, 天然支持非对齐访问
        case (mem_funct3)
            3'b010:  wmask = 4'b1111; // sw - 写入4字节
            3'b001:  wmask = 4'b0011; // sh - 写入2字节
            3'b000:  wmask = 4'b0001; // sb - 只写1个字节
            default: wmask = 4'b1111;
        endcase

        if (is_load) begin
            mem_rdata = pmem_read(mem_addr);
        end else begin
            mem_rdata = 0;
        end
    end

    // 存储器写: 时钟沿触发, 与真实硬件的同步写行为一致
    // 必须用inst_valid门控: idle状态下EXU输出的is_store是
    // 上一条指令的残留信号, 不门控会把同一条store写两次
    always @(posedge clk) begin
        if (!rst && is_store && inst_valid) begin
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

    // WAVE=1编译时记录波形 (Makefile: make sim WAVE=1)
    `ifdef WAVE
    initial begin
        $dumpfile("build/npc.fst");
        $dumpvars(0, NPC);
    end
    `endif

    always @(posedge clk) begin
        if (!rst) begin
            cycle_count <= cycle_count + 1;
            // $display("----------------------------------------");
            // $display("Cycle %0d: PC = 0x%08X, inst = 0x%08X", cycle_count, pc, inst);
            // //regfile.print_regs();

            // if (is_load) begin
            //     $display("  *** LOAD: addr = 0x%08X, funct3 = %b", mem_addr, mem_funct3);
            // end
            // if (is_store) begin
            //     $display("  *** STORE: addr = 0x%08X, data = 0x%08X, wmask = %b",
            //              mem_addr, mem_wdata, wmask);
            // end
            // if (is_jalr) begin
            //     $display("  *** JALR: 跳转到 0x%08X", target_pc);
            // end
        end
    end

endmodule
