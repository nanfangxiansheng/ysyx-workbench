// LSU.v - Load-Store Unit (SimpleBus master, 带reqValid/respValid握手)
// 按SimpleBus协议访问数据存储器: 存储器是同步读写的,
// 收到请求后下个周期才返回读数据/完成写入.
//
// 握手约定:
//   reqValid : LSU只在有真正访存需求的拍 (load/store的exec拍) 拉高,
//              其余拍不发请求, 避免无用的请求占据存储器
//   respValid: 存储器在请求的下拍拉高, 指示回复有效
//              (对load表示rdata有效, 对store表示写入已完成)
//
// load的数据比执行拍晚1拍才回来, LSU用"延迟写回"来适配:
//   exec拍: 拉高reqValid并发地址 (load) 或完成写入 (store)
//   下拍  : respValid拉高, wb_en随之拉高, NPC在这一拍把wb_data写回寄存器堆
module LSU (
    input         clk,
    input         rst,
    input         exec_valid,  // 本拍正在执行指令 (来自NPC的inst_valid)
    input         is_load,     // EXU译码结果: load
    input         is_store,    // EXU译码结果: store
    input  [2:0]  funct3,
    input  [31:0] addr,        // EXU算好的访存地址 (load/store共用)
    input  [31:0] wdata,       // store要写入的数据
    output        wb_en,       // 写回允许: 跟随respValid, 每条load恰好高1拍
    output [31:0] wb_data      // 写回数据: 已按funct3截取/扩展
);
    // 存储器本体在C++侧, 通过DPI-C访问 (保留UART/RTC设备访问功能)
    import "DPI-C" function int  pmem_read(input int raddr);
    import "DPI-C" function void pmem_write(input int waddr, input int wdata, input byte wmask);

    // ============================================
    // SimpleBus: LSU与存储器(RAM)之间的数据访问接口
    // LSU不会同时load和store, 故读写地址合并为lsu_addr.
    // 处理器只在有真正访存需求的拍拉高reqValid, 避免无用的
    // 请求一直占据存储器; 存储器用respValid指示回复何时有效
    // ============================================
    wire        lsu_reqValid; // 本拍有真正的访存请求 (load/store的exec拍)
    wire [31:0] lsu_addr  = addr;
    wire [31:0] lsu_wdata = wdata;
    wire        lsu_wen;      // 写使能
    reg         lsu_respValid; // 存储器的回复有效 (比reqValid晚1拍)

    // 只有load/store的exec拍才有真正的访存需求; 其余拍EXU输出
    // 的是上一条指令的残留信号, 发出去的请求全是无用的
    assign lsu_reqValid = exec_valid && (is_load || is_store);

    // 写掩码: 每比特对应wdata中1个字节; 存储器按字节编址,
    // 从addr起连续写入, 天然支持非对齐访问
    reg  [3:0]  lsu_wmask;
    always @(*) begin
        case (funct3)
            3'b010:  lsu_wmask = 4'b1111; // sw - 写入4字节
            3'b001:  lsu_wmask = 4'b0011; // sh - 写入2字节
            3'b000:  lsu_wmask = 4'b0001; // sb - 只写1个字节
            default: lsu_wmask = 4'b1111;
        endcase
    end

    // wen由reqValid门控: 没有请求时必须无效, 否则会误写存储器
    assign lsu_wen = lsu_reqValid && is_store;

    reg [31:0] lsu_rdata; // 存储器返回的读数据 (延迟1周期)

    // ============================================
    // 存储器数据端口: 同步读写 (reqValid有效的时钟沿采样,
    // 下拍返回读数据, 并用respValid指示回复有效;
    // 对写操作respValid表示写入已完成)
    // 伪代码: if (wen) M[waddr] = (wdata & wmask_full) | M[waddr] & ~wmask_full;
    // ============================================
    always @(posedge clk) begin
        if (!rst) begin
            lsu_rdata <= (lsu_reqValid && !lsu_wen) ? pmem_read(lsu_addr) : 32'b0;
            if (lsu_reqValid && lsu_wen) begin
                pmem_write(lsu_addr, lsu_wdata, lsu_wmask);
            end
            lsu_respValid <= lsu_reqValid;
        end
    end

    // ============================================
    // load的延迟写回握手
    // respValid有效的那一拍, lsu_rdata就是刚返回的load数据,
    // 此时EXU译码的仍是那条load (is_load=1), 据此产生写回.
    // 与固定延迟时代的区别: 写回不再依赖"恰好晚1拍"的计时,
    // 而是跟着respValid走 -- 存储器延迟变化时只需等待它
    // (对store, respValid只表示写入已完成, 不产生写回)
    // ============================================
    assign wb_en = lsu_respValid && is_load;

    // 加载数据处理: 存储器按字节编址且lsu_rdata就是从addr开始的
    // 连续字节流(小端序), 只需提取/扩展低字节, 无需旋转对齐.
    // wb_en有效的那一拍, EXU译码的仍是那条load, funct3没变
    reg [31:0] wb_data_reg;
    always @(*) begin
        case (funct3)
            3'b010:  wb_data_reg = lsu_rdata;                              // lw - 32位
            3'b000:  wb_data_reg = {{24{lsu_rdata[7]}},  lsu_rdata[7:0]};  // lb - 符号扩展
            3'b100:  wb_data_reg = {24'b0,               lsu_rdata[7:0]};  // lbu - 零扩展
            3'b001:  wb_data_reg = {{16{lsu_rdata[15]}}, lsu_rdata[15:0]}; // lh - 符号扩展
            3'b101:  wb_data_reg = {16'b0,               lsu_rdata[15:0]}; // lhu - 零扩展
            default: wb_data_reg = 32'b0;
        endcase
    end
    assign wb_data = wb_data_reg;

endmodule
