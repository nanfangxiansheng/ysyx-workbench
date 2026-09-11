// CSR.v - 控制状态寄存器 (Control Status Register)
// RISC-V的CSR地址空间12位(4096个), 这里按需实现用到的CSR:
//   mvendorid (0xF11): 厂商标识, 只读, 值为"ysyx"的ASCII码0x79737978
//   marchid   (0xF12): 架构标识, 只读, 值为学号数字部分22040000
//   mcycle    (0xB00): 64位周期计数器的低32位, 每周期+1
//   mcycleh   (0xB80): 上述计数器的高32位 (RV32下64位CSR拆成两个访问)
// mvendorid/marchid只读(WARL): 写入被忽略; mcycle/mcycleh可写
module CSR (
    input         clock,
    input         reset,
    input  [11:0] addr,     // CSR地址 (指令的imm[11:0]字段)
    input         wen,      // 写使能: csrrs且rs1!=x0时有效
    input  [31:0] wdata,    // 写入的数据 (rs1的值)
    output [31:0] rdata     // 读出的数据 (csrrs写入rd的旧值)
);

    localparam MVENDORID = 12'hF11;
    localparam MARCHID   = 12'hF12;
    localparam MCYCLE    = 12'hB00;
    localparam MCYCLEH   = 12'hB80;

    // 64位周期计数器: 每个时钟周期+1, 与指令是否提交无关
    // (这就是真实芯片上的时钟源: 周期数/频率 = 时间)
    reg [63:0] cycle;
    always @(posedge clock) begin
        if (reset) begin
            cycle <= 64'b0;
        end else if (wen) begin
            // csrrs写CSR的语义: 与旧值按位或, 读旧值和写入同拍原子完成.
            // mvendorid/marchid按规范只读, 写入忽略(相当于照常+1)
            case (addr)
                MCYCLE:  cycle <= {cycle[63:32], cycle[31:0]  | wdata};
                MCYCLEH: cycle <= {cycle[63:32] | wdata, cycle[31:0]};
                default: cycle <= cycle + 64'b1;
            endcase
        end else begin
            cycle <= cycle + 64'b1;
        end
    end

    // 读: 组合逻辑按地址译码
    assign rdata =
        (addr == MVENDORID) ? 32'h79737978   :  // 'y''s''y''x'
        (addr == MARCHID)   ? 32'd22040000   :  // 学号ysyx_22040000的数字部分
        (addr == MCYCLE)    ? cycle[31:0]    :
        (addr == MCYCLEH)   ? cycle[63:32]   :
        32'b0;                                  // 未实现的CSR读出0

endmodule
