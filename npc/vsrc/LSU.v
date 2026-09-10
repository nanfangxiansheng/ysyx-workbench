// LSU.v - Load-Store Unit
// 接入ysyxSoC后, SimpleBus总线上移到NPC顶层(io_lsu_*),
// LSU只负责load数据的字节选择/扩展:
// 存储器按字节编址且返回的mem_data就是从addr开始的
// 连续字节流(小端序), 因此只需提取/扩展低字节, 无需旋转对齐
module LSU (
    input  [2:0]  funct3,
    input  [31:0] mem_data,      // 来自io_lsu_rdata的4字节
    output [31:0] load_data      // 加载到寄存器的数据
);

    reg [31:0] load_data_reg;
    assign load_data = load_data_reg;

    always @(*) begin
        case (funct3)
            3'b010:  load_data_reg = mem_data;                                     // lw - 32位
            3'b000:  load_data_reg = {{24{mem_data[7]}},  mem_data[7:0]};          // lb - 符号扩展
            3'b100:  load_data_reg = {24'b0,             mem_data[7:0]};           // lbu - 零扩展
            3'b001:  load_data_reg = {{16{mem_data[15]}}, mem_data[15:0]};         // lh - 符号扩展
            3'b101:  load_data_reg = {16'b0,             mem_data[15:0]};          // lhu - 零扩展
            default: load_data_reg = 32'b0;
        endcase
    end

endmodule
