// LSU.v - Load-Store Unit (SimpleBus master)
// 按SimpleBus协议访问数据存储器: 存储器是同步读写的,
// 收到请求后下个周期才返回读数据/完成写入.
//
// 因此load的数据比执行拍晚1拍才回来, LSU用"延迟写回"来适配:
//   exec拍: 发出lsu_addr (load) 或完成写入 (store, wen高1拍)
//   下拍  : lsu_rdata返回, wb_en拉高, NPC在这一拍把wb_data写回寄存器堆
// store无需等待: 写操作在exec拍的时钟沿就完成了, wb_en保持低.
module LSU (
    input         clk,
    input         rst,
    input         exec_valid,  // 本拍正在执行指令 (来自NPC的inst_valid)
    input         is_load,     // EXU译码结果: load
    input         is_store,    // EXU译码结果: store
    input  [2:0]  funct3,
    input  [31:0] addr,        // EXU算好的访存地址 (load/store共用)
    input  [31:0] wdata,       // store要写入的数据
    output        wb_en,       // 写回允许: 比exec拍晚1拍, 每条load恰好高1拍
    output [31:0] wb_data      // 写回数据: 已按funct3截取/扩展
);
    // 存储器本体在C++侧, 通过DPI-C访问 (保留UART/RTC设备访问功能)
    import "DPI-C" function int  pmem_read(input int raddr);
    import "DPI-C" function void pmem_write(input int waddr, input int wdata, input byte wmask);

    // ============================================
    // SimpleBus: LSU与存储器(RAM)之间的数据访问接口
    // LSU不会同时load和store, 故读写地址合并为lsu_addr
    // ============================================
    wire [31:0] lsu_addr  = addr;
    wire [31:0] lsu_wdata = wdata;
    wire        lsu_wen;   // 写使能: 只有store指令的exec拍有效

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

    // 必须用exec_valid门控: fetch拍上EXU输出的is_store是上一条
    // store的残留信号, 不门控会把同一条store写两次
    assign lsu_wen = exec_valid && is_store;

    reg [31:0] lsu_rdata; // 存储器返回的读数据 (延迟1周期)

    // ============================================
    // 存储器数据端口: 同步读写 (时钟沿采样, 下拍返回读数据;
    // 写操作在lsu_wen有效的时钟沿完成, 每条store恰好写1次)
    // 伪代码: if (wen) M[waddr] = (wdata & wmask_full) | M[waddr] & ~wmask_full;
    // ============================================
    always @(posedge clk) begin
        if (!rst) begin
            lsu_rdata <= (!lsu_wen) ? pmem_read(lsu_addr) : 32'b0;
            if (lsu_wen) begin
                pmem_write(lsu_addr, lsu_wdata, lsu_wmask);
            end
        end
    end

    // ============================================
    // load的延迟写回握手
    // exec拍结束的时钟沿, 存储器才把数据放进lsu_rdata,
    // 因此写回必须推迟1拍: 把 (exec_valid && is_load) 寄存1拍,
    // wb_en恰好在数据返回的那一拍拉高, NPC据此写寄存器堆
    // ============================================
    reg wb_en_reg;
    always @(posedge clk or posedge rst) begin
        if (rst) wb_en_reg <= 1'b0;
        else     wb_en_reg <= exec_valid && is_load;
    end
    assign wb_en = wb_en_reg;

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
