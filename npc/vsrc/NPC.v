// NPC.v - 顶层模块 (多周期NPC, 支持所有8条指令 + ebreak)
// 已按ysyxSoC的CPU接口命名规范(E阶段)暴露SimpleBus总线:
// 存储器和外设都在SoC侧, NPC不再包含任何存储器,
// 取指固定从0x3000_0000 (Flash)开始.
//
// 总线协议: reqValid拉高后必须保持, 直到respValid有效才能撤销
// (MemBridge会把请求转成AXI, 延迟不定; 若提前撤销请求会丢失)
module NPC (
    input         clock,
    input         reset,          // 高电平有效
    // IFU: 取指总线
    output        io_ifu_reqValid,
    output [31:0] io_ifu_addr,
    input         io_ifu_respValid,
    input  [31:0] io_ifu_rdata,
    // LSU: 数据访存总线
    output        io_lsu_reqValid,
    output [31:0] io_lsu_addr,
    output [1:0]  io_lsu_size,    // 访存宽度: 00=1B 01=2B 10=4B
    output        io_lsu_wen,
    output [31:0] io_lsu_wdata,
    output [3:0]  io_lsu_wmask,
    input         io_lsu_respValid,
    input  [31:0] io_lsu_rdata
);

    // NPC执行ebreak时, 通过该函数通知仿真环境结束仿真 (顺便把PC告诉C++侧)
    import "DPI-C" function void ebreak(input int pc);

    // PC寄存器
    reg  [31:0] pc;
    wire [31:0] pc_next;

    // 主控状态机:
    // S_FETCH: 发出取指请求(io_ifu_reqValid=1)并保持, 等到respValid
    // S_WAIT : 执行指令; 若是load/store, 发出访存请求并保持,
    //          等到io_lsu_respValid的那一拍完成写回/提交
    localparam S_FETCH = 1'b0;
    localparam S_WAIT  = 1'b1;
    reg state;

    wire in_fetch = (state == S_FETCH);
    wire in_wait  = (state == S_WAIT);

    wire inst_valid = in_wait;
    wire [31:0] inst;

    // 译码阶段 (IDU输出)
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

    // 执行阶段 (EXU输出)
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

    // load数据 (LSU字节选择/扩展后)
    wire [31:0] load_data;

    // ============================================
    // IFU总线: 取指地址驱动为pc, reqValid在整个取指期间保持,
    // 收到respValid的那一拍指令有效, 进入执行状态
    // ============================================
    assign io_ifu_reqValid = in_fetch;
    assign io_ifu_addr     = pc;
    assign inst            = io_ifu_rdata;

    // ============================================
    // LSU总线: 仅load/store的执行拍发起请求并保持,
    // 直到respValid; size由访存宽度决定(funct3低2位恰好是编码)
    // ============================================
    wire is_mem = is_load || is_store;

    assign io_lsu_reqValid = in_wait && is_mem && !io_lsu_respValid;
    assign io_lsu_addr     = mem_addr;
    assign io_lsu_size     = mem_funct3[1:0]; // 000->00(1B) 001->01(2B) 010->10(4B)
    assign io_lsu_wen      = io_lsu_reqValid && is_store;
    // 子字写的数据和掩码必须放到地址对应的字节通道上 (AXI约定):
    // 例如sb写地址0x10000003时, 数据要出现在wdata[31:24]、
    // wmask应为4'b1000, APB外设按paddr[1:0]挑选字节通道
    assign io_lsu_wdata    = mem_wdata << (8 * mem_addr[1:0]);
    assign io_lsu_wmask    = wmask << mem_addr[1:0];

    // 子字读同理: 设备(Flash/PSRAM)返回的是对齐字的字,
    // 请求地址的低2位被截掉, 因此CPU要按addr[1:0]旋转提取
    wire [31:0] lsu_rdata_aligned = io_lsu_rdata >> (8 * mem_addr[1:0]);

    // 写掩码: 每比特对应wdata中1个字节, 存储器按字节编址,
    // 从addr起连续写入, 天然支持非对齐访问
    reg [3:0] wmask;
    always @(*) begin
        case (mem_funct3)
            3'b010:  wmask = 4'b1111; // sw - 写入4字节
            3'b001:  wmask = 4'b0011; // sh - 写入2字节
            3'b000:  wmask = 4'b0001; // sb - 只写1个字节
            default: wmask = 4'b1111;
        endcase
    end
    assign io_lsu_wmask = wmask;

    // 指令提交信号: 非访存指令在exec拍完成; 访存指令要等
    // 存储器respValid的那一拍才完成(load在该拍写回)
    wire inst_done = in_wait && (!is_mem || io_lsu_respValid);

    // ============================================
    // PC更新逻辑: 只在指令完成的那一拍前进, 其余拍保持不变
    // ============================================
    always @(posedge clock or posedge reset) begin
        if (reset) begin
            pc <= 32'h3000_0000; // ysyxSoC: 复位后从Flash取指令
        end else if (inst_done) begin
            pc <= pc_next;
        end
    end

    // PC选择
    assign pc_next = pc_sel ? target_pc : (pc + 4);

    // 主控状态机: 两个状态下都"respValid无效则停留等待"
    always @(posedge clock or posedge reset) begin
        if (reset) begin
            state <= S_FETCH;
        end else begin
            case (state)
                S_FETCH: state <= io_ifu_respValid ? S_WAIT : S_FETCH;
                S_WAIT:  state <= inst_done        ? S_FETCH : S_WAIT;
                default: state <= S_FETCH;
            endcase
        end
    end

    // 向C++侧导出"本周期是否提交了指令", DiffTest据此决定是否对比
    // (state在下个时钟沿就翻转了, 事后读不到, 故寄存一拍)
    reg committed;
    always @(posedge clock or posedge reset) begin
        if (reset)     committed <= 1'b0;
        else           committed <= inst_done;
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
    // ebreak: 程序执行到ebreak时, 通过DPI-C通知仿真环境结束
    // ============================================
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
    // 只在指令完成的拍写回: 非访存指令在exec拍, load在respValid拍
    // (该拍EXU译码的仍是那条load, reg_waddr/reg_wdata恰好正确)
    RegisterFile #(
        .ADDR_WIDTH(5),
        .DATA_WIDTH(32)
    ) regfile (
        .clk   (clock),
        .wdata (is_load ? load_data : reg_wdata),
        .waddr (reg_waddr),
        .wen   (reg_wen && inst_done),
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

    // 4. 访存单元 (LSU): 负责load数据的字节选择/扩展
    LSU lsu (
        .funct3   (mem_funct3),
        .mem_data (lsu_rdata_aligned),
        .load_data(load_data)
    );

    // ============================================
    // 调试: WAVE=1编译时记录波形 (Makefile: make sim WAVE=1)
    // ============================================
    `ifdef WAVE
    initial begin
        $dumpfile("build/npc.fst");
        $dumpvars(0, NPC);
    end
    `endif

endmodule
